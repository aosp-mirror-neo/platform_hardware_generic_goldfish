// Copyright (C) 2020 The Android Open Source Project
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

#include <functional>
#include <mutex>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/synchronization/mutex.h"
#include "google/protobuf/util/message_differencer.h"

#include "android/emulation/control/simple_async_grpc.h"
#include "goldfish/eventing/event_sources.h"

#define DEBUG_EVT 0

#if DEBUG_EVT >= 1
#define DD_EVT(fmt, ...) printf("EventSupport: %s:%d| " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define DD_EVT(...) (void)0
#endif

namespace android {
namespace emulation {
namespace control {

using android::base::eventing::CallbackEventSource;
using android::base::eventing::EventListener;
using android::base::eventing::EventParam;

/**
 * @class BaseEventStreamWriter
 * @brief A base server writer reactor that subscribes to an event source and streams incoming
 * events to a gRPC client.
 *
 * @details This class bridges internal emulator event sources (`CallbackEventSource`) with gRPC
 * asynchronous writer streams (`SimpleServerWriter`).
 *
 * ### Subclassing Contract & Thread-Safety Invariant:
 * Derived subclasses **must explicitly call `Subscribe()`** at the end of their constructor (once
 * all subclass fields are initialized and the dynamic type is complete) to begin receiving events.
 *
 * Automatic subscription in `BaseEventStreamWriter`'s constructor is intentionally avoided:
 * registering callbacks while base construction is in flight leaks `this` to concurrent event
 * dispatch threads, resulting in `vptr` data races (ctor/dtor vs virtual call) and concurrent
 * access to uninitialized subclass members.
 *
 * ### Example Usage:
 * @code
 * class MyStreamWriter : public BaseEventStreamWriter<MyProtoReply, MyInternalEvent> {
 *   public:
 *     MyStreamWriter(ChangeSupport* source, CustomConfig config)
 *         : BaseEventStreamWriter(source), config_(std::move(config)) {
 *         // Subclass is fully constructed; safe to activate subscription:
 *         Subscribe();
 *     }
 *
 *     void EventArrived(const MyInternalEvent& event) override {
 *         MyProtoReply reply = FormatReply(event, config_);
 *         Write(reply);
 *     }
 *
 *   private:
 *     CustomConfig config_;
 * };
 * @endcode
 *
 * @tparam T The type of gRPC messages to be written to the stream.
 * @tparam Event The underlying event type produced by the event source.
 */
template <class T, class Event, class Source = CallbackEventSource<Event>,
          size_t max_queue_size = 0, size_t recycle_size = 0,
          class Reactor = ::grpc::ServerWriteReactor<T>>
class BaseEventStreamWriter : public WithSimpleQueueWriter<Reactor, max_queue_size, recycle_size>,
                              EventListener<Event> {
  public:
    using ChangeSupport = Source;

    /**
     * @brief Constructs a `BaseEventStreamWriter` for the specified event source.
     *
     * @param listener Pointer to the event source instance managing event subscriptions.
     */
    explicit BaseEventStreamWriter(ChangeSupport* listener) : listener_(listener) {
        DCHECK(listener);
    }

    ~BaseEventStreamWriter() override { Unsubscribe(); }

    /**
     * @brief Overrides `SimpleServerWriter::OnDone()` to delete the writer instance when the client
     * finishes reading the stream.
     */
    void OnDone() override {
        Unsubscribe();
        delete this;
    }

    /**
     * @brief Overrides `SimpleServerWriter::OnCancel()` to handle client stream cancellations.
     *
     * @details Unregisters the event callback and synchronizes stream termination with the unified
     * `reactor_lock_`.
     */
    void OnCancel() override {
        DD_EVT("Cancelled %p", this);
        if (!this->SetCancelled()) {
            return;
        }
        Unsubscribe();
        this->Finish(grpc::Status::CANCELLED);
    }

    /**
     * @brief Finishes the stream. Thread-safe and idempotent.
     * Unsubscribes from the event source prior to finishing the gRPC stream.
     */
    template <typename... Args>
    void Finish(Args&&... args) {
        Unsubscribe();
        WithSimpleQueueWriter<Reactor, max_queue_size, recycle_size>::Finish(
                std::forward<Args>(args)...);
    }

    /**
     * @brief Default no-op handler for event delivery during base class destruction.
     */
    void EventArrived(typename EventParam<Event>::type /*event*/) override {}

  protected:
    /**
     * @brief Subscribes to the event source.
     *
     * @note **Subclass Contract**: This method MUST be invoked at the end of the derived class
     * constructor once all derived fields are initialized and the vtable is finalized.
     * It is thread-safe and idempotent.
     */
    void Subscribe() {
        absl::MutexLock lock(&this->reactor_lock_);
        if (callback_id_ == ChangeSupport::kInvalidCallbackId && listener_) {
            callback_id_ = listener_->AddCallback(
                    [this](const Event event) { this->EventArrived(event); });
        }
    }

    /**
     * @brief Unsubscribes from the event source.
     *
     * @note Thread-safe and idempotent. Automatically invoked on cancellation, completion, and
     * destruction. Calls `RemoveCallback` outside `reactor_lock_` to prevent deadlock with
     * concurrent event deliveries.
     */
    void Unsubscribe() {
        typename ChangeSupport::CallbackId id_to_remove = ChangeSupport::kInvalidCallbackId;
        {
            absl::MutexLock lock(&this->reactor_lock_);
            id_to_remove = std::exchange(callback_id_, ChangeSupport::kInvalidCallbackId);
        }
        if (id_to_remove != ChangeSupport::kInvalidCallbackId) {
            listener_->RemoveCallback(id_to_remove);
        }
    }

  private:
    typename ChangeSupport::CallbackId callback_id_ ABSL_GUARDED_BY(this->reactor_lock_){
        ChangeSupport::kInvalidCallbackId};
    ChangeSupport* const listener_;
};

/**
 * @class GenericEventStreamWriter
 * @brief A generic gRPC server event stream writer where the gRPC message type and event type are
 * identical.
 *
 * @tparam T The type of events and gRPC messages to be written to the stream.
 */
template <class T, size_t max_queue_size = 0, size_t recycle_size = 0>
class GenericEventStreamWriter
        : public BaseEventStreamWriter<T, T, CallbackEventSource<T>, max_queue_size, recycle_size> {
    using ChangeSupport = CallbackEventSource<T>;

  public:
    /**
     * @brief Constructs a `GenericEventStreamWriter` subscribed to the specified event source.
     *
     * @param listener Pointer to the `CallbackEventSource` instance.
     */
    explicit GenericEventStreamWriter(ChangeSupport* listener)
            : BaseEventStreamWriter<T, T, CallbackEventSource<T>, max_queue_size, recycle_size>(
                      listener) {
        this->Subscribe();
    }

    ~GenericEventStreamWriter() override = default;

    /**
     * @brief Invoked when an event arrives from the underlying event source. Enqueues the event for
     * gRPC writing.
     *
     * @param event The event of type `T` that has arrived.
     */
    void EventArrived(typename EventParam<T>::type event) override {
        DD_EVT("Handling %p, %s", this, event.ShortDebugString().c_str());
        this->Write(event);
    };
};

/**
 * @class UniqueEventStreamWriter
 * @brief A gRPC server event stream writer that filters out consecutive duplicate events and
 * optionally sends an immediate initial state snapshot upon connection.
 *
 * @details Designed for event sources that produce `T` messages directly and may emit spurious
 * consecutive duplicates (e.g., `A, A, B, B, B, C, C` -> `A, B, C`). If `populate_fn` is provided,
 * it is invoked synchronously during construction to deliver the initial state snapshot before
 * subscribing to `source`. When an event of type `T` arrives and passes the optional `filter_fn`,
 * it is compared against the previously forwarded message using `MessageDifferencer::Equals` and
 * written only when it differs.
 *
 * Because deduplication retains a copy of the last written `T` and runs
 * `MessageDifferencer::Equals`, prefer `UniqueEventStreamWriter` for compact control-plane
 * messages (such as `VmState` or `GrpcNotification`) and `StateStreamWriter` for large payload
 * streams (such as `ImageFrame` screenshots).
 *
 * @tparam T The gRPC Protobuf message type to write to the stream.
 * @tparam Source The event source type (defaults to `CallbackEventSource<T>`).
 * @tparam max_queue_size The maximum number of items to keep in the write queue (0 = unbounded).
 * @tparam recycle_size The maximum number of items to keep in the recycle pool (0 = disabled).
 * @tparam Reactor The underlying gRPC write reactor type.
 */
template <class T, class Source = CallbackEventSource<T>, size_t max_queue_size = 0,
          size_t recycle_size = 0, class Reactor = ::grpc::ServerWriteReactor<T>>
class UniqueEventStreamWriter final
        : public BaseEventStreamWriter<T, T, Source, max_queue_size, recycle_size, Reactor> {
  public:
    using ChangeSupport = Source;
    using PopulateStateFn = absl::AnyInvocable<void(T*)>;
    using FilterPredicate = absl::AnyInvocable<bool(const typename EventParam<T>::type&) const>;

    /**
     * @brief Constructs a `UniqueEventStreamWriter`, optionally writes an initial state snapshot,
     * and subscribes to the event source.
     *
     * @param source The event source to subscribe to.
     * @param populate_fn Optional function invoked once during construction to populate the initial
     *                    Protobuf message `T`.
     * @param filter_fn Optional predicate to filter incoming events before deduplication.
     */
    explicit UniqueEventStreamWriter(ChangeSupport* source, PopulateStateFn populate_fn = nullptr,
                                     FilterPredicate filter_fn = nullptr)
            : BaseEventStreamWriter<T, T, Source, max_queue_size, recycle_size, Reactor>(source)
            , filter_fn_(std::move(filter_fn)) {
        if (populate_fn) {
            T initial_state = this->AcquireMessage();
            populate_fn(&initial_state);
            WriteIfChanged(std::move(initial_state));
        }
        this->Subscribe();
    }

    /**
     * @brief Constructs a `UniqueEventStreamWriter`, writes a sequence of initial messages before
     * subscribing, and subscribes to the event source.
     *
     * @param source The event source to subscribe to.
     * @param initial_events Initial messages to write prior to subscribing.
     * @param filter_fn Optional predicate to filter incoming events before deduplication.
     */
    UniqueEventStreamWriter(ChangeSupport* source, std::vector<T> initial_events,
                            FilterPredicate filter_fn = nullptr)
            : BaseEventStreamWriter<T, T, Source, max_queue_size, recycle_size, Reactor>(source)
            , filter_fn_(std::move(filter_fn)) {
        for (auto& event : initial_events) {
            WriteIfChanged(std::move(event));
        }
        this->Subscribe();
    }

    ~UniqueEventStreamWriter() override { this->Unsubscribe(); }

    /**
     * @brief Invoked when an event arrives. Evaluates `filter_fn` and forwards the event only if
     * it differs from the previously written message.
     *
     * @param event The event of type `T` that has arrived.
     */
    void EventArrived(typename EventParam<T>::type event) override {
        DD_EVT("Handling %p, %s", this, event.ShortDebugString().c_str());
        if (filter_fn_ && !filter_fn_(event)) {
            return;
        }
        WriteIfChanged(event);
    }

  private:
    void WriteIfChanged(T state) {
        {
            absl::MutexLock lock(&state_lock_);
            if (has_last_state_ &&
                google::protobuf::util::MessageDifferencer::Equals(state, last_state_)) {
                return;
            }
            last_state_ = state;
            has_last_state_ = true;
        }
        this->Write(std::move(state));
    }

    FilterPredicate filter_fn_;
    absl::Mutex state_lock_;
    bool has_last_state_ ABSL_GUARDED_BY(state_lock_) = false;
    T last_state_ ABSL_GUARDED_BY(state_lock_);
};

/**
 * @class StateStreamWriter
 * @brief A gRPC server event stream writer that sends an immediate initial snapshot
 * upon connection and reactively streams state updates as events arrive.
 *
 * @details When recycling is enabled (`recycle_size > 0`), message objects passed to
 * `populate_fn` are acquired from the internal pool via `AcquireMessage()` and can (and will)
 * contain stale data from previous transmissions. It is up to `populate_fn` to properly initialize
 * the recycled object (e.g. overwriting fields or clearing stale state as needed). This behavior is
 * intentional and by design to allow reusing existing `std::string` buffers, repeated fields, and
 * allocated storage across stream updates without repetitive heap allocations.
 *
 * ### Thread Safety:
 * - `populate_fn` is invoked synchronously on the calling thread during `StateStreamWriter`
 *   construction to deliver the initial state snapshot, and subsequently invoked asynchronously
 *   on whichever thread(s) dispatch notifications from the `CallbackEventSource` via
 * `EventArrived`.
 * - `populate_fn` MUST be thread-safe with respect to any external state being read. Callers should
 *   acquire appropriate locks (e.g. state/framebuffer mutexes) inside `populate_fn` if external
 *   state can be modified concurrently.
 * - Internal queue operations and recycling via `AcquireMessage()` and `Write()` are fully
 * thread-safe and protected by `reactor_lock_`.
 *
 * ### Example `populate_fn` Usage:
 * @code
 * auto populate_fn = [&](ImageFrame* frame) {
 *     // Acquire external lock if state can be modified concurrently:
 *     absl::MutexLock lock(&display_mutex_);
 *
 *     // Scalars must be explicitly overwritten since recycled instances contain stale data:
 *     frame->set_img_width(current_width_);
 *     frame->set_img_height(current_height_);
 *     frame->set_format(ImageFormat::RGBA8888);
 *
 *     // Reuses internal std::string capacity without allocating new heap memory:
 *     frame->mutable_image_bytes()->assign(raw_pixels, byte_size);
 * };
 * @endcode
 *
 * @tparam T The gRPC Protobuf message type to write to the stream.
 * @tparam Event The underlying event type produced by the CallbackEventSource.
 * @tparam Source The event source type (defaults to `CallbackEventSource<Event>`).
 * @tparam max_queue_size The maximum number of items to keep in the write queue (0 = unbounded).
 * @tparam recycle_size The maximum number of items to keep in the recycle pool (0 = disabled).
 * @tparam Reactor The underlying gRPC write reactor type.
 * @tparam deduplicate When true, suppresses consecutive identical `T` snapshots via
 *                     `MessageDifferencer::Equals` (zero storage/runtime overhead when false).
 */
template <class T, class Event, class Source = CallbackEventSource<Event>,
          size_t max_queue_size = 0, size_t recycle_size = 0,
          class Reactor = ::grpc::ServerWriteReactor<T>, bool deduplicate = false>
class StateStreamWriter
        : public BaseEventStreamWriter<T, Event, Source, max_queue_size, recycle_size, Reactor> {
  public:
    using ChangeSupport = Source;

    /**
     * @brief Callback function type to populate a Protobuf message `T` with the current state.
     *
     * @details
     * ### Stale Data & Buffer Reuse:
     * When recycling is enabled (`recycle_size > 0`), the `T*` passed to this function can
     * (and will) contain stale data from previous transmissions. It is up to `populate_fn` to
     * properly initialize the recycled object (e.g. overwriting fields or clearing unneeded data).
     * For example, assigning scalar fields like `set_img_width()` / `set_img_height()` and
     * using `mutable_image_bytes()->assign(...)` allows reusing existing `std::string` buffer
     * capacity without incurring repetitive heap allocations.
     *
     * ### Thread Safety:
     * This callback is executed synchronously during `StateStreamWriter` construction (initial
     * snapshot) and asynchronously from event source dispatcher threads during `EventArrived()`.
     * Any shared or mutable external state accessed inside `populate_fn` must be synchronized by
     * the caller.
     */
    using PopulateStateFn = absl::AnyInvocable<void(T*) const>;
    using FilterPredicate = absl::AnyInvocable<bool(const typename EventParam<Event>::type&) const>;

    /**
     * @brief Constructs a state stream writer, writes the initial state snapshot,
     * and subscribes to the event source.
     *
     * @param source The event source to subscribe to.
     * @param populate_fn Function to populate the Protobuf message with current state.
     *                    Note: When recycling is enabled (`recycle_size > 0`), the object passed
     *                    to `populate_fn` can (and will) contain stale data and must be properly
     *                    initialized by `populate_fn` (e.g. setting `img_width`, `img_height`, and
     *                    reusing `std::string` buffers). Must be thread-safe as it is invoked
     * across constructor and event dispatch threads.
     * @param filter_fn Optional predicate to filter incoming events.
     */
    StateStreamWriter(ChangeSupport* source, PopulateStateFn populate_fn,
                      FilterPredicate filter_fn = nullptr)
            : BaseEventStreamWriter<T, Event, Source, max_queue_size, recycle_size, Reactor>(source)
            , populate_fn_(std::move(populate_fn))
            , filter_fn_(std::move(filter_fn)) {
        WriteState();
        this->Subscribe();
    }

    ~StateStreamWriter() override { this->Unsubscribe(); }

    void EventArrived(typename EventParam<Event>::type event) override {
        if (!filter_fn_ || filter_fn_(event)) {
            WriteState();
        }
    }

  private:
    struct EmptyDeduplicationState {};
    struct DeduplicationState {
        absl::Mutex dedup_mutex;
        // Invariant: has_last_state ==> last_state equals the most recent T passed to Write().
        bool has_last_state ABSL_GUARDED_BY(dedup_mutex) = false;
        T last_state ABSL_GUARDED_BY(dedup_mutex);
    };
    using DeduplicationStorage =
            std::conditional_t<deduplicate, DeduplicationState, EmptyDeduplicationState>;

    void WriteState() {
        DCHECK(populate_fn_);
        T current_state = this->AcquireMessage();
        populate_fn_(&current_state);
        if constexpr (deduplicate) {
            const absl::MutexLock dedup_lock(dedup_.dedup_mutex);
            if (dedup_.has_last_state && google::protobuf::util::MessageDifferencer::Equals(
                                                 current_state, dedup_.last_state)) {
                this->RecycleMessage(std::move(current_state));
                return;
            }
            dedup_.last_state = current_state;
            dedup_.has_last_state = true;
            this->Write(std::move(current_state));
        } else {
            this->Write(std::move(current_state));
        }
    }

    PopulateStateFn populate_fn_;
    FilterPredicate filter_fn_;
    [[no_unique_address]] DeduplicationStorage dedup_;
};

/**
 * Server state stream writer that sends an immediate initial snapshot upon connection
 * and streams subsequent updates only when the populated Protobuf message differs from
 * the previously transmitted snapshot (`MessageDifferencer::Equals`).
 *
 * Prefer `UniqueStateStreamWriter` for compact control-plane state snapshots (such as
 * display topology or system status) where multiple internal notifications may fire for
 * the same logical state, and `StateStreamWriter` for large payload streams (such as
 * `ImageFrame` screenshots). When `recycle_size > 0`, `populate_fn` must clear or
 * overwrite all fields so stale data from recycled messages does not affect equality
 * comparison; suppressed duplicate messages are automatically returned to the recycle pool.
 */
template <class T, class Event, class Source = CallbackEventSource<Event>,
          size_t max_queue_size = 0, size_t recycle_size = 0,
          class Reactor = ::grpc::ServerWriteReactor<T>>
using UniqueStateStreamWriter =
        StateStreamWriter<T, Event, Source, max_queue_size, recycle_size, Reactor,
                          /*deduplicate=*/true>;

}  // namespace control
}  // namespace emulation
}  // namespace android
