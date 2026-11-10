// Copyright (C) 2024 The Android Open Source Project
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
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/log/check.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/base/unique_handle.h"
#include "goldfish/eventing/event_source.h"
#include "goldfish/eventing/policies/pointer_handlers.h"

namespace android::base::eventing {

// Helper trait to extract the event type 'T' from any event source
template <typename EventSourceType>
struct event_source_traits;

template <template <typename, typename, typename> class Host, typename T, typename Policy,
          typename Dispatcher>
struct event_source_traits<Host<T, Policy, Dispatcher>> {
    using event_type = T;
};

template <template <typename, typename> class Host, typename T, typename Policy>
struct event_source_traits<Host<T, Policy>> {
    using event_type = T;
};

/**
 * @brief RAII wrapper for automatic callback management.
 *
 * This class automatically unregisters the callback when destroyed.
 * It works with both CallbackEventSupport and WithCallbacks classes.
 *
 * @tparam EventSystem The type of event system (CallbackEventSupport or WithCallbacks)
 * @tparam T The event type
 */
// Helper trait to determine callback signature based on event type T.
template <typename T>
struct event_callback_traits {
    using type = std::function<void(typename EventParam<T>::type)>;
};

template <>
struct event_callback_traits<void> {
    using type = std::function<void()>;
};

/**
 * @brief RAII wrapper for automatic callback management.
 *
 * This class automatically unregisters the callback when destroyed.
 * It works with both CallbackEventSupport and WithCallbacks classes.
 *
 * @tparam EventSystem The type of event system (CallbackEventSupport or WithCallbacks)
 * @tparam T The event type
 */
template <typename EventSystem, typename T>
class ScopedEventCallback {
  public:
    using EventCallback = typename event_callback_traits<T>::type;
    static constexpr size_t kInvalidId = 0;
    struct CallbackDeleter {
        struct Empty {};
        explicit CallbackDeleter(Empty) {}
        explicit CallbackDeleter(EventSystem* sys) : source(sys) {}

        void operator()(size_t id) const {
            if (id != kInvalidId) {
                DCHECK(source) << "The invariant id != kInvalid -> source is broken";
                source->RemoveCallback(id);
            }
        }

        EventSystem* source = nullptr;
    };

    using Handle = ::goldfish::base::UniqueHandle<size_t, kInvalidId, CallbackDeleter>;

    ScopedEventCallback() = default;
    ScopedEventCallback(EventSystem& system, EventCallback callback)
            : handle_(system.AddCallback(std::move(callback)), CallbackDeleter(&system)) {}

    ScopedEventCallback(ScopedEventCallback&&) noexcept = default;
    ScopedEventCallback& operator=(ScopedEventCallback&&) noexcept = default;
    ScopedEventCallback(const ScopedEventCallback&) = delete;
    ScopedEventCallback& operator=(const ScopedEventCallback&) = delete;
    ~ScopedEventCallback() = default;

    size_t GetId() const { return handle_.get(); }
    explicit operator bool() const { return handle_.ok(); }
    void Reset() { handle_.reset(); }
    size_t Release() { return handle_.release(); }

  private:
    Handle handle_;
};

/**
 * @brief A mixin that adds a modern, safe, and high-performance callback API
 * to any policy-based event source.
 *
 * @details This class inherits from the provided EventSourceType, gaining its
 * performance characteristics. It adds an ID-based callback system where each
 * callback is managed by its own dedicated internal listener.
 *
 * ### Concurrency and Lifetime Contract
 * - **Thread Safety**: Registration (`AddCallback`), unregistration (`RemoveCallback`),
 *   and event dispatch (`FireEvent`) are thread-safe and can be invoked concurrently.
 * - **Synchronous Draining**: When `RemoveCallback` is invoked (either directly or via
 *   `ScopedEventCallback` RAII destruction), it blocks until all active in-flight
 *   executions of that callback on other threads have completed. Once `RemoveCallback`
 *   returns, the callback is guaranteed never to be invoked again, and any captured
 *   resources may be safely deallocated.
 * - **No Re-entrant Self-Removal**: A registered callback MUST NOT synchronously unregister
 *   itself (e.g. by resetting its own `ScopedCallbackHandle` or calling `RemoveCallback`
 *   with its own ID) from within its own execution context. Because `RemoveCallback` waits
 *   for all in-flight executions to exit, attempting self-unregistration from inside the
 *   callback will result in a deadlock.
 *
 * @tparam EventSourceType The concrete event source class to extend (e.g.,
 * eventing::ThreadSafeEventSource<MyEvent>).
 */
template <typename EventSourceType>
class WithCallbacks : public EventSourceType {
  private:
    class InternalListener;

  public:
    using T = typename event_source_traits<EventSourceType>::event_type;
    using EventCallback = typename event_callback_traits<T>::type;
    using CallbackId = size_t;
    static constexpr CallbackId kInvalidCallbackId = 0;
    using PtrType = typename EventSourceType::Ptr;

  private:
    using ListenerMap = std::unordered_map<CallbackId, std::shared_ptr<InternalListener>>;

  public:
    /**
     * @brief A unique pointer that holds a ScopedEventCallback, ensuring the
     * callback is automatically unregistered when the handle goes out of scope.
     * This is the return type of `makeScopedCallback`.
     */
    using ScopedCallbackHandle =
            std::unique_ptr<ScopedEventCallback<WithCallbacks<EventSourceType>, T>>;

    using EventSourceType::EventSourceType;
    virtual ~WithCallbacks() {
        // Move the map under callbacks_mutex_ to synchronize with in-flight callbacks
        // that query the source (e.g. CallbackCount()), but disconnect outside the lock
        // to avoid deadlocking while waiting for them to complete.
        ListenerMap listeners_to_disconnect;
        {
            absl::MutexLock callbacks_lock(callbacks_mutex_);
            listeners_to_disconnect = std::move(listener_map_);
        }
        for (const auto& [id, listener] : listeners_to_disconnect) {
            if (listener) {
                if constexpr (eventing::is_shared_ptr_v<PtrType> ||
                              eventing::is_weak_ptr_v<PtrType>) {
                    EventSourceType::RemoveListener(listener);
                } else {
                    EventSourceType::RemoveListener(listener.get());
                }
                listener->Disconnect();
            }
        }
    }

    /**
     * @brief Adds a callback, creating a dedicated listener for it.
     * @return A unique ID for managing the callback's lifetime.
     */
    virtual CallbackId AddCallback(EventCallback callback) {
        const auto listener = std::make_shared<InternalListener>(std::move(callback));
        CallbackId id;

        {
            absl::MutexLock callbacks_lock(callbacks_mutex_);
            id = next_id_++;
            const bool inserted = listener_map_.insert({id, listener}).second;
            DCHECK(inserted);
            (void)inserted;
        }

        // Add the listener to the underlying high-performance EventSource
        if constexpr (eventing::is_shared_ptr_v<PtrType> || eventing::is_weak_ptr_v<PtrType>) {
            EventSourceType::AddListener(listener);
        } else {
            EventSourceType::AddListener(listener.get());
        }

        return id;
    }

    /**
     * @brief Removes a callback by its ID.
     *
     * Blocks until any in-flight executions of the callback on other threads
     * finish, ensuring that resources accessed by the callback can be safely
     * destroyed immediately after RemoveCallback returns.
     *
     * @warning A callback must not unregister itself (e.g. reset its own
     * ScopedCallbackHandle) from within its own callback execution; doing so
     * will deadlock.
     */
    virtual void RemoveCallback(CallbackId id) {
        if (id == kInvalidCallbackId) {
            return;
        }
        std::shared_ptr<InternalListener> listener;
        {
            absl::MutexLock callbacks_lock(callbacks_mutex_);
            auto it = listener_map_.find(id);
            if (it == listener_map_.end()) {
                return;
            }
            listener = it->second;
            listener_map_.erase(it);
        }

        // Remove the listener from the underlying EventSource and disconnect outside
        // callbacks_mutex_ to avoid deadlocking if an in-flight callback queries the source.
        if (listener) {
            if constexpr (eventing::is_shared_ptr_v<PtrType> || eventing::is_weak_ptr_v<PtrType>) {
                EventSourceType::RemoveListener(listener);
            } else {
                EventSourceType::RemoveListener(listener.get());
            }
            listener->Disconnect();
        }
    }

    /**
     * @brief Returns the number of listeners in the underlying event source.
     */
    size_t Size() { return EventSourceType::Size(); }

    /**
     * @brief Fires an event to all listeners in the underlying event source.
     */
    void FireEvent(typename EventParam<T>::type event) { EventSourceType::FireEvent(event); }

    /**
     * @brief Returns the number of active callbacks.
     */
    size_t CallbackCount() const {
        absl::MutexLock callbacks_lock(callbacks_mutex_);
        return listener_map_.size();
    }

  private:
    // A dedicated listener that holds a single callback.
    class InternalListener : public eventing::EventListener<T> {
      public:
        explicit InternalListener(EventCallback cb) : callback_(std::move(cb)) {}

        void EventArrived(typename EventParam<T>::type event) override {
            {
                absl::MutexLock state_lock(state_mutex_);
                if (!active_) {
                    return;
                }
                ++in_flight_count_;
            }

            callback_(event);

            {
                absl::MutexLock state_lock(state_mutex_);
                --in_flight_count_;
            }
        }

        void Disconnect() {
            EventCallback callback_to_destroy;
            {
                absl::MutexLock state_lock(state_mutex_);
                active_ = false;
                state_mutex_.Await(absl::Condition(
                        +[](uint32_t* count) { return *count == 0; }, &in_flight_count_));
                callback_to_destroy = std::move(callback_);
            }
        }

      private:
        mutable absl::Mutex state_mutex_;
        bool active_ ABSL_GUARDED_BY(state_mutex_) = true;
        uint32_t in_flight_count_ ABSL_GUARDED_BY(state_mutex_) = 0;
        // Invoked outside state_mutex_ while in_flight_count_ > 0; moved out and
        // destroyed in Disconnect() once all in-flight executions complete.
        EventCallback callback_;
    };

    mutable absl::Mutex callbacks_mutex_;
    CallbackId next_id_ ABSL_GUARDED_BY(callbacks_mutex_) = 1;
    ListenerMap listener_map_ ABSL_GUARDED_BY(callbacks_mutex_);
};

/**
 * @brief Helper function to create a ScopedEventCallback
 *
 * @tparam EventSystem The type of event system
 * @tparam T The event type
 * @param system Reference to the event system
 * @param callback The callback function
 * @return A new ScopedEventCallback instance
 */
template <typename EventSystem, typename T>
auto MakeScopedCallback(EventSystem& system,
                        typename ScopedEventCallback<EventSystem, T>::EventCallback callback) {
    return std::make_unique<ScopedEventCallback<EventSystem, T>>(system, std::move(callback));
}

// Helper to deduce the event type from a lambda's signature
template <typename T>
struct function_traits;
template <typename ClassType, typename ReturnType, typename Arg>
struct function_traits<ReturnType (ClassType::*)(Arg) const> {
    using event_type = std::decay_t<Arg>;
};
template <typename ClassType, typename ReturnType>
struct function_traits<ReturnType (ClassType::*)() const> {
    using event_type = void;
};

/**
 * @brief Helper function to create a ScopedEventCallback with automatic type deduction.
 * @param system Reference to the event system
 * @param callback The callback function
 * @return A new ScopedEventCallback instance
 */
template <typename EventSystem, typename F>
auto MakeScopedCallback(EventSystem& system, F&& callback) {
    using T = typename function_traits<decltype(&F::operator())>::event_type;
    return std::make_unique<ScopedEventCallback<EventSystem, T>>(system, std::forward<F>(callback));
}

}  // namespace android::base::eventing
