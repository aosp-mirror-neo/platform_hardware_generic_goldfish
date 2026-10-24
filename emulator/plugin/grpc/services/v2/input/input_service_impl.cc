// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "input/input_service_impl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/functional/any_invocable.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/simple_async_grpc.h"

namespace goldfish::grpc::v2 {

namespace {

using ::android::emulation::control::AbslStatusToGrpcStatus;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::SendInputEventsResponse;
using DispatchResult = IInputDispatcher::DispatchResult;

constexpr char kClientIdMetadataKey[] = "x-client-id";
// How often a call waiting for its events checks whether it was cancelled.
constexpr absl::Duration kCancellationCheckInterval = absl::Milliseconds(100);

// Returns the source ID derived from the caller's `x-client-id` header, or 0 (the human
// operator) when the header is absent.
int32_t ClientSourceId(const ::grpc::ServerContextBase& context) {
    const auto& metadata = context.client_metadata();
    const auto it = metadata.find(kClientIdMetadataKey);
    if (it == metadata.end() || it->second.empty()) {
        return 0;
    }
    // Never log raw metadata header values; hash to a numeric source_id only.
    return InputServiceImpl::DeriveSourceIdFromClientId(
            absl::string_view(it->second.data(), it->second.length()));
}

int32_t SourceOf(const InputEvent& event, int32_t client_source_id) {
    return event.input_source_id() != 0 ? event.input_source_id() : client_source_id;
}

absl::Status WithContext(const absl::Status& status, absl::string_view context) {
    return {status.code(), absl::StrCat(context, ": ", status.message())};
}

// Coordinates asynchronous callbacks (event-loop timers and dispatch
// completions) that capture a raw reactor pointer with `OnDone()`.
class CallbackGate {
  public:
    template <typename Fn>
    void RunIfOpen(Fn&& fn) ABSL_LOCKS_EXCLUDED(gate_mutex_) {
        const std::thread::id tid = std::this_thread::get_id();
        {
            absl::MutexLock gate_lock(gate_mutex_);
            if (closed_) {
                return;
            }
            active_threads_.push_back(tid);
        }
        absl::Cleanup leave = [this, tid]() ABSL_LOCKS_EXCLUDED(gate_mutex_) {
            absl::MutexLock gate_lock(gate_mutex_);
            if (auto it = std::ranges::find(active_threads_, tid); it != active_threads_.end()) {
                active_threads_.erase(it);
            }
        };
        std::forward<Fn>(fn)();
    }

    // Prevents new callbacks from starting and waits for any in-flight callback
    // on another thread to return before the caller deletes the reactor.
    void CloseAndWait() ABSL_LOCKS_EXCLUDED(gate_mutex_) {
        const std::thread::id caller = std::this_thread::get_id();
        const auto no_other_threads = [this, caller]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(gate_mutex_) {
            return std::ranges::all_of(active_threads_,
                                       [caller](std::thread::id id) { return id == caller; });
        };
        absl::MutexLock gate_lock(gate_mutex_);
        closed_ = true;
        gate_mutex_.Await(absl::Condition(&no_other_threads));
    }

  private:
    absl::Mutex gate_mutex_;
    bool closed_ ABSL_GUARDED_BY(gate_mutex_) = false;
    std::vector<std::thread::id> active_threads_ ABSL_GUARDED_BY(gate_mutex_);
};

// The results of the events one call dispatched, which their completions
// record on the dispatcher's thread.
//
// Thread safety: all methods are thread-safe.
class DispatchedEvents : public std::enable_shared_from_this<DispatchedEvents> {
  public:
    struct Failure {
        int index;
        int32_t source_id;
        absl::Status status;
    };

    // @return The completion of event `index` from `source_id`. Call it before
    //     dispatching the event.
    IInputDispatcher::DispatchCallback Track(int index, int32_t source_id)
            ABSL_LOCKS_EXCLUDED(results_mutex_) {
        absl::MutexLock results_lock(results_mutex_);
        ++pending_;
        return [self = shared_from_this(), index, source_id](DispatchResult result) {
            self->Record(index, source_id, std::move(result));
        };
    }

    // Waits until every tracked event completed. @return false if `context`
    // was cancelled first.
    bool AwaitAll(const ::grpc::ServerContext& context) const ABSL_LOCKS_EXCLUDED(results_mutex_) {
        while (true) {
            {
                absl::MutexLock results_lock(results_mutex_);
                if (results_mutex_.AwaitWithTimeout(
                            absl::Condition(this, &DispatchedEvents::AllCompleted),
                            kCancellationCheckInterval)) {
                    return true;
                }
            }
            if (context.IsCancelled()) {
                return false;
            }
        }
    }

    // Runs `on_all_completed` once every event tracked so far has completed:
    // synchronously if none are pending, else on the thread completing the last.
    void NotifyWhenAllCompleted(absl::AnyInvocable<void() &&> on_all_completed)
            ABSL_LOCKS_EXCLUDED(results_mutex_) {
        {
            absl::MutexLock results_lock(results_mutex_);
            if (pending_ > 0) {
                DCHECK(!on_all_completed_);
                on_all_completed_ = std::move(on_all_completed);
                return;
            }
        }
        std::move(on_all_completed)();
    }

    // Runs `on_first_failure` on the thread completing the first event that
    // fails, if one does. Set it before tracking any event; it runs at most
    // once, and never if a failure was already recorded.
    void NotifyOnFirstFailure(absl::AnyInvocable<void() &&> on_first_failure)
            ABSL_LOCKS_EXCLUDED(results_mutex_) {
        absl::MutexLock results_lock(results_mutex_);
        DCHECK(!first_failure_);
        on_first_failure_ = std::move(on_first_failure);
    }

    // @return The failure of the first event that failed, if any did.
    std::optional<Failure> FirstFailure() const ABSL_LOCKS_EXCLUDED(results_mutex_) {
        absl::MutexLock results_lock(results_mutex_);
        return first_failure_;
    }

    // Sets when the first and last events that reached the guest were
    // dispatched, if any did.
    void SetDispatchTimes(SendInputEventsResponse& response) const
            ABSL_LOCKS_EXCLUDED(results_mutex_) {
        absl::MutexLock results_lock(results_mutex_);
        if (first_dispatched_.has_value()) {
            response.set_first_event_dispatched_at_nanos(absl::ToUnixNanos(first_dispatched_->at));
            response.set_last_event_dispatched_at_nanos(absl::ToUnixNanos(last_dispatched_->at));
        }
    }

  private:
    struct Dispatched {
        int index;
        absl::Time at;
    };

    void Record(int index, int32_t source_id, DispatchResult result)
            ABSL_LOCKS_EXCLUDED(results_mutex_) {
        absl::AnyInvocable<void() &&> on_first_failure;
        absl::AnyInvocable<void() &&> on_all_completed;
        {
            absl::MutexLock results_lock(results_mutex_);
            --pending_;
            if (!result.status.ok()) {
                if (!first_failure_) {
                    on_first_failure = std::move(on_first_failure_);
                }
                if (!first_failure_ || index < first_failure_->index) {
                    first_failure_ = Failure{index, source_id, std::move(result.status)};
                }
            }
            if (result.dispatched_at.has_value()) {
                const Dispatched dispatched{index, *result.dispatched_at};
                if (!first_dispatched_ || index < first_dispatched_->index) {
                    first_dispatched_ = dispatched;
                }
                if (!last_dispatched_ || index > last_dispatched_->index) {
                    last_dispatched_ = dispatched;
                }
            }
            if (pending_ == 0) {
                on_all_completed = std::move(on_all_completed_);
            }
        }
        // The failure first, so a reactor idle in a read learns of it and
        // starts awaiting the rest; then the completion of the rest, if this
        // was it.
        if (on_first_failure) {
            std::move(on_first_failure)();
        }
        if (on_all_completed) {
            std::move(on_all_completed)();
        }
    }

    bool AllCompleted() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(results_mutex_) {
        return pending_ == 0;
    }

    mutable absl::Mutex results_mutex_;
    // Events tracked but not yet completed.
    size_t pending_ ABSL_GUARDED_BY(results_mutex_) = 0;
    std::optional<Failure> first_failure_ ABSL_GUARDED_BY(results_mutex_);
    std::optional<Dispatched> first_dispatched_ ABSL_GUARDED_BY(results_mutex_);
    std::optional<Dispatched> last_dispatched_ ABSL_GUARDED_BY(results_mutex_);
    absl::AnyInvocable<void() &&> on_first_failure_ ABSL_GUARDED_BY(results_mutex_);
    absl::AnyInvocable<void() &&> on_all_completed_ ABSL_GUARDED_BY(results_mutex_);
};

::grpc::Status ReadOnlyError(absl::string_view rpc) {
    return {::grpc::StatusCode::PERMISSION_DENIED,
            absl::StrFormat("Cannot execute %s: InputService is running in read-only mode. "
                            "Connect to a read-write endpoint or enable input injection "
                            "permissions.",
                            rpc)};
}

}  // namespace

InputServiceImpl::InputServiceImpl(std::shared_ptr<IInputDispatcher> dispatcher,
                                   IMultiDisplay& multi_display, Options options)
        : dispatcher_(std::move(dispatcher))
        , options_(std::move(options))
        , validator_(multi_display,
                     V2InputValidator::Options{.hw_sensor_hinge = options_.hw_sensor_hinge,
                                               .hw_keyboard = options_.hw_keyboard,
                                               .hw_rotary_input = options_.hw_rotary_input,
                                               .max_touch_points = options_.max_touch_points}) {
    DCHECK(dispatcher_ != nullptr);
}

int32_t InputServiceImpl::DeriveSourceIdFromClientId(absl::string_view client_id) {
    const size_t hash = absl::HashOf(client_id);
    const int32_t source_id = static_cast<int32_t>(hash & 0x7fffffffU);
    return source_id != 0 ? source_id : 1;
}

absl::Status InputServiceImpl::ValidateInputEvent(const InputEvent& event) const {
    return validator_.Validate(event);
}

absl::Status InputServiceImpl::ValidateBatchRequest(const SendInputEventsRequest& request) const {
    std::optional<int64_t> first_ts;
    std::optional<int64_t> prev_ts;
    for (int i = 0; i < request.events_size(); ++i) {
        const InputEvent& event = request.events(i);
        if (absl::Status status = ValidateInputEvent(event); !status.ok()) {
            return WithContext(
                    status, absl::StrFormat("SendInputEvents failed validation at events[%d]", i));
        }
        if (!event.has_timestamp_nanos()) {
            continue;
        }
        const int64_t ts = event.timestamp_nanos();
        if (prev_ts.has_value() && ts < *prev_ts) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "SendInputEvents failed validation at events[%d]: timestamp_nanos '%d' is "
                    "less than previous timestamp_nanos '%d'. Event timestamps in a batch "
                    "must be monotonically non-decreasing.",
                    i, ts, *prev_ts));
        }
        first_ts = first_ts.value_or(ts);
        const absl::Duration span = absl::Nanoseconds(ts - *first_ts);
        if (span > options_.max_batch_duration) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "SendInputEvents batch duration '%s' (from timestamp_nanos '%d' to '%d') "
                    "exceeds maximum allowed batch duration '%s'. Split longer gestures across "
                    "multiple SendInputEvents requests or use StreamInputEvents.",
                    absl::FormatDuration(span), *first_ts, ts,
                    absl::FormatDuration(options_.max_batch_duration)));
        }
        prev_ts = ts;
    }
    return absl::OkStatus();
}

std::unique_ptr<DispatcherSession> InputServiceImpl::OpenSession(
        int32_t source_id, absl::Duration idle_timeout) const {
    return std::make_unique<DispatcherSession>(dispatcher_, IInputDispatcher::SessionOptions{
                                                                .human = source_id == 0,
                                                                .source_id = source_id,
                                                                .reject_down_on_held = true,
                                                                .idle_timeout = idle_timeout,
                                                            });
}

DispatcherSession& InputServiceImpl::BatchSession(int32_t source_id) {
    DCHECK_NE(source_id, 0);
    {
        absl::MutexLock batch_sessions_lock(batch_sessions_mutex_);
        if (auto it = batch_sessions_.find(source_id); it != batch_sessions_.end()) {
            return *it->second;
        }
    }
    // Opened outside the lock, as opening calls into the dispatcher. Sessions
    // are never removed, so the reference stays valid after unlocking.
    std::unique_ptr<DispatcherSession> opened =
            OpenSession(source_id, options_.held_input_idle_timeout);
    absl::MutexLock batch_sessions_lock(batch_sessions_mutex_);
    return *batch_sessions_.try_emplace(source_id, std::move(opened)).first->second;
}

void InputServiceImpl::SleepUntil(absl::Time target) const {
    const absl::Time now = options_.now_fn();
    if (target > now) {
        options_.sleep_fn(target - now);
    }
}

::grpc::Status InputServiceImpl::SendInputEvents(::grpc::ServerContext* context,
                                                 const SendInputEventsRequest* request,
                                                 SendInputEventsResponse* response) {
    if (options_.read_only) {
        return ReadOnlyError("SendInputEvents");
    }
    if (absl::Status status = ValidateBatchRequest(*request); !status.ok()) {
        VLOG(1) << "InputService::SendInputEvents: " << status;
        return AbslStatusToGrpcStatus(status);
    }

    const int32_t client_source_id = ClientSourceId(*context);
    // Closed when the call returns, so anonymous batches release what they left
    // held instead of sharing a persistent session across callers.
    std::unique_ptr<DispatcherSession> anonymous_session;
    auto dispatched = std::make_shared<DispatchedEvents>();
    // Host time corresponding to client timestamp 0, anchored at the first timed event.
    std::optional<absl::Time> client_epoch;
    for (int i = 0; i < request->events_size(); ++i) {
        const InputEvent& event = request->events(i);
        if (event.has_timestamp_nanos()) {
            const absl::Duration client_time = absl::Nanoseconds(event.timestamp_nanos());
            if (!client_epoch.has_value()) {
                client_epoch = options_.now_fn() - client_time;
            }
            SleepUntil(*client_epoch + client_time);
        }
        if (context->IsCancelled()) {
            return ::grpc::Status::CANCELLED;
        }
        if (dispatched->FirstFailure().has_value()) {
            break;
        }
        const int32_t source_id = SourceOf(event, client_source_id);
        DispatcherSession* session = nullptr;
        if (source_id == 0) {
            if (anonymous_session == nullptr) {
                anonymous_session = OpenSession(0);
            }
            session = anonymous_session.get();
        } else {
            session = &BatchSession(source_id);
        }
        session->Dispatch(event, dispatched->Track(i, source_id));
    }

    if (!dispatched->AwaitAll(*context)) {
        return ::grpc::Status::CANCELLED;
    }
    if (std::optional<DispatchedEvents::Failure> failure = dispatched->FirstFailure()) {
        return AbslStatusToGrpcStatus(
                WithContext(failure->status, absl::StrFormat("SendInputEvents failed at events[%d] "
                                                             "(input_source_id '%d')",
                                                             failure->index, failure->source_id)));
    }
    dispatched->SetDispatchTimes(*response);
    response->set_events_dispatched(request->events_size());
    return ::grpc::Status::OK;
}

// Immediately finishes a `ServerReadReactor` with a non-OK status (used when `StreamInputEvents`
// is rejected up front, e.g. in read-only mode).
class ErrorServerReader final : public ::grpc::ServerReadReactor<InputServiceImpl::InputEvent> {
  public:
    explicit ErrorServerReader(::grpc::Status status) { this->Finish(std::move(status)); }
    void OnDone() override { delete this; }
};

/**
 * @brief Asynchronous gRPC `ServerReadReactor` for `StreamInputEvents` with event-loop pacing.
 *
 * ### Read-Pacing & Completion State Machine
 * `::SimpleServerReader<InputEvent>` (`WithSimpleReader`) starts the first `StartRead()` upon
 * construction and automatically issues the next `StartRead()` whenever `Read()` returns `true`:
 * - **Immediate dispatch (`delay == 0`):** `Read()` validates and dispatches the event immediately
 *   and returns `true` (unless an earlier event already failed), pulling the next message.
 * - **Delayed dispatch (`delay > 0`):** Instead of sleeping on a gRPC callback thread, `Read()`
 *   schedules a one-shot timer on `options_.event_loop` (`ScheduleDelayed`) and returns `false`,
 *   pausing further gRPC reads. When the timer fires on the event-loop thread,
 *   `OnScheduledDispatch()` dispatches the event and explicitly calls `this->StartRead()` to
 *   resume reading the stream.
 * - **Half-close or dispatch failure:** Instead of blocking on `AwaitAll()`,
 *   `AwaitDispatchesAndFinish()` transitions `state_` to `kAwaitingDispatches` and registers a
 *   non-blocking `DispatchedEvents::NotifyWhenAllCompleted` callback so all tracked dispatches
 *   finish before sessions are closed and `ServerReadReactor::Finish()` is invoked. Dispatches
 *   complete on the dispatcher's loop after `Read()` returned, so the first failure also reaches
 *   the reactor through `DispatchedEvents::NotifyOnFirstFailure`; otherwise a client idle
 *   between events would not see the stream end until it sent another.
 *
 * ### Cancellation & Lifetime Safety
 * - `CallbackGate` guards all asynchronous timer and completion callbacks against `OnDone()`,
 *   preventing use-after-free if cancellation triggers `OnDone()` while a callback is in flight.
 * - `EventLoop::Timer::Cancel()` and `DispatcherSession` destruction are always executed outside
 *   `this->reactor_lock_`.
 */
class InputServiceImpl::StreamInputReader final : public ::SimpleServerReader<InputEvent> {
  public:
    StreamInputReader(InputServiceImpl& service, ::grpc::CallbackServerContext* context,
                      StreamInputEventsResponse* response)
            : service_(service)
            , context_(context)
            , client_source_id_(ClientSourceId(*context))
            , response_(response) {
        // A failure may complete while no read is in progress; end the stream
        // from there rather than waiting for the client's next event.
        dispatched_->NotifyOnFirstFailure([this, gate = gate_]() {
            gate->RunIfOpen([this]() { AwaitDispatchesAndFinish(); });
        });
    }

    bool Read(const InputEvent* event) override {
        if (dispatched_->FirstFailure().has_value()) {
            AwaitDispatchesAndFinish();
            return false;
        }

        const int32_t source_id = SourceOf(*event, client_source_id_);
        if (absl::Status status = service_.ValidateInputEvent(*event); !status.ok()) {
            int32_t failed_index = 0;
            {
                absl::MutexLock lock(this->reactor_lock_);
                failed_index = dispatched_count_;
            }
            status = WithContext(status,
                                 absl::StrFormat("StreamInputEvents failed validation at event #%d "
                                                 "(input_source_id '%d')",
                                                 failed_index, source_id));
            VLOG(1) << "InputService::StreamInputEvents: " << status;
            FinishStream(AbslStatusToGrpcStatus(status));
            return false;
        }

        absl::Duration delay = absl::ZeroDuration();
        if (event->has_timestamp_nanos()) {
            // Preserve the client's inter-event spacing, but cap each step so that a pause
            // between bursts (or a timestamp going backwards) rebases onto the host clock
            // instead of delaying dispatch.
            const absl::Time now = service_.options_.now_fn();
            absl::Time target = now;
            if (prev_client_ts_.has_value() && event->timestamp_nanos() > *prev_client_ts_) {
                const absl::Duration step =
                        std::min(absl::Nanoseconds(event->timestamp_nanos() - *prev_client_ts_),
                                 service_.options_.max_stream_step_duration);
                target = std::max(target, prev_target_time_ + step);
            }
            prev_client_ts_ = event->timestamp_nanos();
            prev_target_time_ = target;
            if (target > now) {
                delay = target - now;
            }
        }

        if (delay > absl::ZeroDuration()) {
            if (service_.options_.event_loop != nullptr) {
                return ScheduleDelayedDispatch(*event, source_id, delay);
            }
            service_.options_.sleep_fn(delay);
        }

        return DispatchAndRecord(*event, source_id);
    }

    void OnReadDone(bool ok) override {
        if (!ok) {
            AwaitDispatchesAndFinish();
            return;
        }
        ::SimpleServerReader<InputEvent>::OnReadDone(true);
    }

    void OnCancel() override {
        std::shared_ptr<::goldfish::async::EventLoop::Timer> timer_to_cancel;
        bool should_finish = false;
        {
            absl::MutexLock lock(this->reactor_lock_);
            cancelled_ = true;
            if (state_ == State::kWaitingForTimer || state_ == State::kAwaitingDispatches) {
                timer_to_cancel = std::move(pending_timer_);
                should_finish = true;
            }
        }
        if (timer_to_cancel != nullptr) {
            timer_to_cancel->Cancel();
        }
        if (should_finish) {
            FinishStream(::grpc::Status::CANCELLED);
        }
    }

    void OnDone() override {
        std::shared_ptr<::goldfish::async::EventLoop::Timer> timer_to_cancel;
        {
            absl::MutexLock lock(this->reactor_lock_);
            timer_to_cancel = std::move(pending_timer_);
        }
        if (timer_to_cancel != nullptr) {
            timer_to_cancel->Cancel();
        }
        gate_->CloseAndWait();

        StreamSessions sessions_to_close;
        {
            absl::MutexLock lock(this->reactor_lock_);
            sessions_to_close = std::move(sessions_);
            sessions_.clear();
        }
        sessions_to_close.clear();
        delete this;
    }

  private:
    using StreamSessions = absl::flat_hash_map<int32_t, std::shared_ptr<DispatcherSession>>;

    enum class State {
        kReading,
        kWaitingForTimer,
        kDispatchingTimer,
        kAwaitingDispatches,
        kFinished,
    };

    // Whether the stream takes no more events: it is finished, or only waits
    // for the dispatches it made before finishing. A dispatch that fails on
    // the dispatcher's loop can move it there at any time; from then on, no
    // slot is claimed, no timer scheduled, and no read started.
    bool IsEnding() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(this->reactor_lock_) {
        return state_ == State::kFinished || state_ == State::kAwaitingDispatches;
    }

    void FinishStream(::grpc::Status status) {
        StreamSessions sessions_to_close;
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (state_ == State::kFinished) {
                return;
            }
            state_ = State::kFinished;
            response_->set_events_dispatched(dispatched_count_);
            sessions_to_close = std::move(sessions_);
            sessions_.clear();
        }
        sessions_to_close.clear();
        ::grpc::ServerReadReactor<InputEvent>::Finish(std::move(status));
    }

    void AwaitDispatchesAndFinish() {
        bool cancelled = false;
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (IsEnding()) {
                return;
            }
            cancelled = cancelled_ || context_->IsCancelled();
            if (!cancelled) {
                state_ = State::kAwaitingDispatches;
            }
        }
        if (cancelled) {
            FinishStream(::grpc::Status::CANCELLED);
            return;
        }
        dispatched_->NotifyWhenAllCompleted(
                [this, gate = gate_]() { gate->RunIfOpen([this]() { FinishAfterDispatches(); }); });
    }

    void FinishAfterDispatches() {
        if (std::optional<DispatchedEvents::Failure> failure = dispatched_->FirstFailure()) {
            FinishStream(AbslStatusToGrpcStatus(WithContext(
                    failure->status, absl::StrFormat("StreamInputEvents failed at event #%d "
                                                     "(input_source_id '%d')",
                                                     failure->index, failure->source_id))));
            return;
        }
        FinishStream(::grpc::Status::OK);
    }

    // The session an event dispatches on and the index it dispatches as. No
    // session when the stream finished or, with `cancelled`, was cancelled.
    struct DispatchSlot {
        std::shared_ptr<DispatcherSession> session;
        int32_t index = 0;
        bool cancelled = false;
    };

    // Claims the next dispatch index on the session of `source_id`, opening
    // the session on first use. Opens outside `reactor_lock_`, as opening
    // calls into the dispatcher.
    DispatchSlot ClaimDispatchSlot(int32_t source_id) {
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (IsEnding()) {
                return {};
            }
            if (cancelled_) {
                return {.cancelled = true};
            }
            if (auto it = sessions_.find(source_id); it != sessions_.end()) {
                return {.session = it->second, .index = dispatched_count_++};
            }
        }

        auto opened = std::shared_ptr<DispatcherSession>(service_.OpenSession(source_id));
        DispatchSlot slot;
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (!IsEnding()) {
                if (cancelled_) {
                    slot.cancelled = true;
                } else {
                    slot.session =
                            sessions_.try_emplace(source_id, std::move(opened)).first->second;
                    slot.index = dispatched_count_++;
                }
            }
        }
        // Any unused `opened` session is destroyed here outside `reactor_lock_`.
        return slot;
    }

    bool DispatchAndRecord(InputEvent event, int32_t source_id) {
        if (dispatched_->FirstFailure().has_value()) {
            AwaitDispatchesAndFinish();
            return false;
        }

        const DispatchSlot slot = ClaimDispatchSlot(source_id);
        if (slot.session == nullptr) {
            if (slot.cancelled) {
                FinishStream(::grpc::Status::CANCELLED);
            }
            return false;
        }

        slot.session->Dispatch(event, dispatched_->Track(slot.index, source_id));
        if (dispatched_->FirstFailure().has_value()) {
            AwaitDispatchesAndFinish();
            return false;
        }
        return true;
    }

    bool ScheduleDelayedDispatch(InputEvent event, int32_t source_id, absl::Duration delay) {
        bool cancelled = false;
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (IsEnding()) {
                return false;
            }
            cancelled = cancelled_;
            if (!cancelled) {
                state_ = State::kWaitingForTimer;
            }
        }
        if (cancelled) {
            FinishStream(::grpc::Status::CANCELLED);
            return false;
        }
        // Scheduled outside `reactor_lock_`: the loop may run the timer, or
        // report a failure, before this returns, and both take the lock.
        std::shared_ptr<::goldfish::async::EventLoop::Timer> timer =
                service_.options_.event_loop->ScheduleDelayed(
                        [this, gate = gate_, event = std::move(event), source_id]() mutable {
                            gate->RunIfOpen([this, event = std::move(event), source_id]() mutable {
                                OnScheduledDispatch(std::move(event), source_id);
                            });
                        },
                        delay);
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (state_ == State::kWaitingForTimer) {
                pending_timer_ = std::move(timer);
            }
        }
        if (timer != nullptr) {
            // The stream moved on meanwhile: the timer fired, or must not.
            timer->Cancel();
        }
        return false;
    }

    void OnScheduledDispatch(InputEvent event, int32_t source_id) {
        std::shared_ptr<::goldfish::async::EventLoop::Timer> expired_timer;
        {
            absl::MutexLock lock(this->reactor_lock_);
            if (cancelled_ || state_ != State::kWaitingForTimer) {
                return;
            }
            state_ = State::kDispatchingTimer;
            expired_timer = std::move(pending_timer_);
        }
        expired_timer.reset();
        if (!DispatchAndRecord(std::move(event), source_id)) {
            return;
        }
        bool should_finish_cancelled = false;
        {
            absl::MutexLock lock(this->reactor_lock_);
            // An earlier dispatch may have failed meanwhile; the stream is then
            // ending and must not read again.
            if (IsEnding()) {
                return;
            }
            if (cancelled_) {
                should_finish_cancelled = true;
            } else {
                state_ = State::kReading;
            }
        }
        if (should_finish_cancelled) {
            FinishStream(::grpc::Status::CANCELLED);
            return;
        }
        this->StartRead();
    }

    InputServiceImpl& service_;
    ::grpc::CallbackServerContext* const context_;
    const int32_t client_source_id_;
    StreamInputEventsResponse* const response_;
    const std::shared_ptr<DispatchedEvents> dispatched_ = std::make_shared<DispatchedEvents>();
    const std::shared_ptr<CallbackGate> gate_ = std::make_shared<CallbackGate>();

    std::optional<int64_t> prev_client_ts_;
    absl::Time prev_target_time_;

    int32_t dispatched_count_ ABSL_GUARDED_BY(this->reactor_lock_){0};
    StreamSessions sessions_ ABSL_GUARDED_BY(this->reactor_lock_);
    std::shared_ptr<::goldfish::async::EventLoop::Timer> pending_timer_
            ABSL_GUARDED_BY(this->reactor_lock_);
    State state_ ABSL_GUARDED_BY(this->reactor_lock_){State::kReading};
    bool cancelled_ ABSL_GUARDED_BY(this->reactor_lock_){false};
};

::grpc::ServerReadReactor<InputServiceImpl::InputEvent>* InputServiceImpl::StreamInputEvents(
        ::grpc::CallbackServerContext* context, StreamInputEventsResponse* response) {
    if (options_.read_only) {
        return new ErrorServerReader(ReadOnlyError("StreamInputEvents"));
    }
    return new StreamInputReader(*this, context, response);
}

::grpc::ServerWriteReactor<InputServiceImpl::InputActivity>* InputServiceImpl::StreamInputActivity(
        ::grpc::CallbackServerContext* /*context*/, const StreamInputActivityRequest* request) {
    if (!options_.supports_input_activity_observation) {
        return new ::ErrorServerWriter<InputActivity>(::grpc::Status(
                ::grpc::StatusCode::PERMISSION_DENIED,
                "Cannot subscribe to StreamInputActivity: input activity observation is disabled "
                "on this endpoint. Query CapabilitiesService to check "
                "InputCapabilities.supports_input_activity_observation, or enable activity "
                "observation in the server configuration."));
    }
    return CreateInputActivityWriter<>(*request);
}

}  // namespace goldfish::grpc::v2
