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

#pragma once

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/emulation/control/grpc_event_stream_support.h"
#include "emulator/plugin/grpc/input/dispatcher_session.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/v2_input_validator.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/display/abstract_multi_display.h"
#include "input_service.grpc.pb.h"

namespace goldfish::grpc::v2 {

using InputServiceBase =
        ::android::emulation::v2::InputService::WithCallbackMethod_StreamInputEvents<
                ::android::emulation::v2::InputService::WithCallbackMethod_StreamInputActivity<
                        ::android::emulation::v2::InputService::Service>>;

/**
 * @brief Implements the AEMU v2 InputService.
 *
 * Validates touch, mouse, stylus, keyboard, rotary axis, and IME text events,
 * dispatches them to the guest through the shared `IInputDispatcher`, and streams
 * the dispatcher's input activity to observers.
 *
 * ### Pacing & Timelines:
 * - `SendInputEvents` validates batch monotonicity and duration bounds up front,
 *   then paces events in real time against the host monotonic clock (`NowFn`)
 *   according to `timestamp_nanos` deltas.
 * - `StreamInputEvents` rebases client `timestamp_nanos` deltas onto the host
 *   clock, caps stale inter-burst gaps to prevent artificial lag, and schedules
 *   delayed dispatches asynchronously on the `EventLoop`.
 *
 * ### Sources & Sessions:
 * - An event's source is its `input_source_id`, else a deterministic non-zero ID
 *   derived from a non-empty `x-client-id` metadata header, else `0`, the human
 *   operator. Each source holds its own contacts and keys in its own
 *   `DispatcherSession`.
 * - A `StreamInputEvents` stream closes its sessions when it ends, which reports
 *   the release of what they held as synthesized `POINTER_ACTION_CANCEL` and
 *   `KEY_ACTION_UP` activity. Anonymous `SendInputEvents` events (`source_id == 0`)
 *   use a per-call session that closes when the call returns; identified sources
 *   (`source_id != 0`) share one persistent session per source across
 *   `SendInputEvents` calls and expire any input left held after
 *   `held_input_idle_timeout` of inactivity.
 * - The first event that fails to dispatch, for example with
 *   `RESOURCE_EXHAUSTED` when no touch slot is free, ends the call with its
 *   index; later events aren't dispatched.
 *
 * ### Thread Safety:
 * Thread-safe across concurrent `SendInputEvents`, `StreamInputEvents`, and
 * `StreamInputActivity` RPC invocations.
 *
 * ### Ownership:
 * Shares ownership of the dispatcher. `multi_display` must outlive the service.
 */
class InputServiceImpl final : public InputServiceBase {
  public:
    static constexpr absl::Duration kDefaultMaxBatchDuration = absl::Seconds(30);
    static constexpr absl::Duration kDefaultMaxStreamStepDuration = absl::Seconds(1);
    static constexpr absl::Duration kDefaultHeldInputIdleTimeout = absl::Seconds(60);
    static constexpr int32_t kDefaultMaxTouchPoints = V2InputValidator::kDefaultMaxTouchPoints;

    using IMultiDisplay = ::goldfish::display::IMultiDisplay;

    using InputEvent = ::android::emulation::v2::InputEvent;
    using PointerEvent = ::android::emulation::v2::PointerEvent;
    using SendInputEventsRequest = ::android::emulation::v2::SendInputEventsRequest;
    using SendInputEventsResponse = ::android::emulation::v2::SendInputEventsResponse;
    using StreamInputEventsResponse = ::android::emulation::v2::StreamInputEventsResponse;
    using StreamInputActivityRequest = ::android::emulation::v2::StreamInputActivityRequest;
    using InputActivity = ::android::emulation::v2::InputActivity;

    using ActivityEventSource = IInputDispatcher::ActivityEventSource;
    using NowFn = std::function<absl::Time()>;
    using SleepFn = std::function<void(absl::Duration)>;

    template <typename Reactor = ::grpc::ServerWriteReactor<InputActivity>>
    using InputActivityWriter = ::android::emulation::control::UniqueEventStreamWriter<
            InputActivity, ActivityEventSource, 0, 0, Reactor>;

    struct Options {
        bool hw_sensor_hinge{false};
        bool hw_keyboard{true};
        bool hw_rotary_input{false};
        bool read_only{false};
        bool supports_input_activity_observation{true};
        int32_t max_touch_points{kDefaultMaxTouchPoints};
        absl::Duration max_batch_duration{kDefaultMaxBatchDuration};
        absl::Duration max_stream_step_duration{kDefaultMaxStreamStepDuration};
        absl::Duration held_input_idle_timeout{kDefaultHeldInputIdleTimeout};
        ::goldfish::async::EventLoop* event_loop{nullptr};
        NowFn now_fn{[]() { return absl::Now(); }};
        SleepFn sleep_fn{[](absl::Duration d) { absl::SleepFor(d); }};
    };

    /**
     * @param dispatcher Applies the events; must not be null.
     * @param multi_display Validates the display IDs of pointer events.
     */
    InputServiceImpl(std::shared_ptr<IInputDispatcher> dispatcher, IMultiDisplay& multi_display,
                     Options options);

    ::grpc::Status SendInputEvents(::grpc::ServerContext* context,
                                   const SendInputEventsRequest* request,
                                   SendInputEventsResponse* response) override;

    ::grpc::ServerReadReactor<InputEvent>* StreamInputEvents(
            ::grpc::CallbackServerContext* context, StreamInputEventsResponse* response) override;

    /**
     * @brief Creates the activity reactor without the observation policy check, so tests can
     * substitute an in-memory `Reactor`.
     */
    template <typename Reactor = ::grpc::ServerWriteReactor<InputActivity>>
    auto* CreateInputActivityWriter(const StreamInputActivityRequest& request) {
        typename InputActivityWriter<Reactor>::FilterPredicate filter;
        if (request.has_input_source_id()) {
            const int32_t source_id = request.input_source_id();
            filter = [source_id](const InputActivity& activity) {
                return activity.event().input_source_id() == source_id;
            };
        }
        return new InputActivityWriter<Reactor>(&dispatcher_->activity(), /*populate_fn=*/nullptr,
                                                std::move(filter));
    }

    ::grpc::ServerWriteReactor<InputActivity>* StreamInputActivity(
            ::grpc::CallbackServerContext* context,
            const StreamInputActivityRequest* request) override;

    /**
     * @brief Deterministically derives an input_source_id for a client identifier string.
     *
     * Used when `InputEvent.input_source_id` is 0 and the caller supplies a
     * non-empty `x-client-id` metadata header. Always returns a non-zero positive
     * 32-bit integer.
     */
    static int32_t DeriveSourceIdFromClientId(absl::string_view client_id);

  private:
    class StreamInputReader;

    // The session of each source, opened when the source first dispatches.
    using SourceSessions = absl::flat_hash_map<int32_t, std::unique_ptr<DispatcherSession>>;

    absl::Status ValidateInputEvent(const InputEvent& event) const;
    absl::Status ValidateBatchRequest(const SendInputEventsRequest& request) const;
    std::unique_ptr<DispatcherSession> OpenSession(
            int32_t source_id, absl::Duration idle_timeout = absl::ZeroDuration()) const;
    // @return The session that the SendInputEvents calls of non-zero `source_id`
    //     share, which stays open as long as the service and expires held input
    //     after `held_input_idle_timeout` of inactivity.
    DispatcherSession& BatchSession(int32_t source_id) ABSL_LOCKS_EXCLUDED(batch_sessions_mutex_);
    void SleepUntil(absl::Time target) const;

    const std::shared_ptr<IInputDispatcher> dispatcher_;
    const Options options_;
    const V2InputValidator validator_;

    absl::Mutex batch_sessions_mutex_;
    SourceSessions batch_sessions_ ABSL_GUARDED_BY(batch_sessions_mutex_);
};

}  // namespace goldfish::grpc::v2
