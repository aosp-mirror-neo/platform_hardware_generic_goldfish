// Copyright (C) 2025 The Android Open Source Project
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <type_traits>

#include "absl/functional/any_invocable.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "android/crashreport/thread.h"
#include "android/status/flatten.h"
#include "goldfish/async/looper_breadcrumb_tracker.h"
#include "goldfish/eventing/event_sources.h"

namespace goldfish::async {

using android::base::eventing::CallbackEventSource;

/**
 * @brief Event that is fired when the state of the looper changes.
 * Observers can subscribe to this event to be notified of the looper's
 * lifecycle.
 */
struct LooperStatusEvent {
    enum class State : uint8_t {
        kNotStarted,    // The loop has not yet been started.
        kRunning,       // The loop is actively processing events.
        kPaused,        // The loop is temporarily paused and not processing events.
        kShuttingDown,  // A graceful shutdown has been initiated.
        kFinished,      // The loop has finished execution.
    };

    State state;
};

template <typename Sink>
void AbslStringify(Sink& sink, const LooperStatusEvent& event) {
    switch (event.state) {
    case LooperStatusEvent::State::kNotStarted:
        sink.Append("NOT_STARTED");
        break;
    case LooperStatusEvent::State::kRunning:
        sink.Append("RUNNING");
        break;
    case LooperStatusEvent::State::kPaused:
        sink.Append("PAUSED");
        break;
    case LooperStatusEvent::State::kShuttingDown:
        sink.Append("SHUTTING_DOWN");
        break;
    case LooperStatusEvent::State::kFinished:
        sink.Append("FINISHED");
        break;
    }
}

/**
 * @brief An abstract interface for an event loop.
 *
 * This allows application code to depend on the concept of an event loop
 * without being tied to a specific implementation like libuv or asio.
 */
class EventLoop : public CallbackEventSource<LooperStatusEvent> {
  public:
    /**
     * @brief A move-only, type-erased unit of work to be executed.
     *
     * Using absl::AnyInvocable allows the EventLoop to accept any callable,
     * including move-only lambdas (e.g., those capturing a std::unique_ptr),
     * without the overhead or copy restrictions of std::function.
     */
    using Task = absl::AnyInvocable<void()>;
    /**
     * @brief A repeating task that returns whether it should be rescheduled.
     *
     * The task is executed repeatedly until it returns false, or until it is cancelled.
     */
    using RepeatingTask = absl::AnyInvocable<bool()>;

    struct TimerDeleter;

    /**
     * @brief An opaque handle to a scheduled task.
     *
     * Allows for the scheduling and cancellation of a delayed or repeating task.
     */
    class Timer {
      protected:
        friend struct EventLoop::TimerDeleter;

        /**
         * @brief Cancels the scheduled task.
         * If the task has already run or been cancelled, this is a no-op.
         */
        virtual void Cancel() = 0;

      public:
        virtual ~Timer() = default;

        /**
         * @brief (Re)Schedules a timer with a new delay and interval.
         *
         * This method updates both the delay for the next execution and the
         * subsequent interval for the timer. This does nothing if the timer has
         * already been cancelled. Likewise if the event loop has shutdown.
         *
         * @param new_delay The new delay before the next execution.
         * @param new_interval The new interval for subsequent executions. When
         * set to absl::ZeroDuration(), the timer will not repeat.
         */
        virtual void Schedule(absl::Duration new_delay, absl::Duration new_interval) = 0;

        /**
         * @brief (Re)Schedules a timer with a new delay.
         *
         * This method updates the delay for the next execution. This does
         * nothing if the timer has already been cancelled. Likewise if the
         * event loop has shutdown.
         *
         * @param new_delay The new delay before the next execution.
         */
        void Schedule(absl::Duration new_delay) { Schedule(new_delay, absl::ZeroDuration()); }
    };

    struct TimerDeleter {
        void operator()(Timer* t) const { t->Cancel(); }
    };

    using ScopedTimer = std::unique_ptr<Timer, TimerDeleter>;

    /**
     * @brief Construct a new EventLoop object with a name.
     * @param name The name of the event loop.
     */
    explicit EventLoop(std::string name)
            : name_(std::move(name)), tracker_(std::make_unique<LooperBreadcrumbTracker>(name_)) {}

    virtual ~EventLoop() = default;

    /**
     * @brief Gets the name of the event loop.
     * @return The name of the event loop.
     */
    const std::string& GetName() const { return name_; }

    absl::Status ShutdownAndWait(absl::Duration timeout = absl::ZeroDuration()) {
        auto future = Shutdown();
        if (timeout != absl::ZeroDuration()) {
            if (future.wait_for(absl::ToChronoNanoseconds(timeout)) != std::future_status::ready) {
                return absl::DeadlineExceededError(
                        "Loop shutdown did not return a result within the deadline");
            }
        }
        return future.get();
    }

    /**
     * @brief Cancels all outstanding timers in the event loop.
     */
    virtual void ShutdownTimers() = 0;

    /**
     * @brief Blocks the calling thread until the event loop is idle.
     *
     * This method waits until all tasks that were enqueued *before* this method
     * was called have been processed. Tasks posted while `WaitUntilIdle` is
     * executing will not be waited for by the current call.
     * @return An opaque value which changes when a task is processed.
     */
    virtual size_t WaitUntilIdle() = 0;

    /**
     * @brief Initiates a graceful shutdown of the event loop.
     *
     * This method schedules the closing of all internal handles. It returns
     * a future that will be fulfilled when all cleanup tasks are complete.
     * This should be called before the object is destroyed.
     *
     * Note: that this will cancel all outstanding timers and posted callbacks
     * once this returns no new timers are callbacks can be scheduled.
     */
    virtual std::future<absl::Status> Shutdown() = 0;

    /**
     * @brief Checks if the current thread is the one running this event loop.
     * @return true if the caller is on the event loop's thread, false otherwise.
     */
    virtual bool IsOnLoopThread() const = 0;

    // Evaluates to absl::Status for void tasks, or absl::StatusOr<std::future<T>> for tasks
    // returning T.
    template <typename F>
    using PostReturnType =
            std::conditional_t<std::is_void_v<std::invoke_result_t<std::decay_t<F>>>, absl::Status,
                               absl::StatusOr<std::future<std::invoke_result_t<std::decay_t<F>>>>>;

    // Evaluates to absl::Status for void or absl::Status tasks, absl::StatusOr<T> for
    // absl::StatusOr<T> tasks, or absl::StatusOr<T> for tasks returning a plain value T.
    template <typename F>
    using PostAndWaitReturnType =
            android::status::flatten_status_or_t<std::invoke_result_t<std::decay_t<F>>>;

    /**
     * @brief Posts a callable object for execution on the event loop.
     *
     * If the callable returns void, Post() directly delegates to PostTaskWithOptions() and returns
     * absl::Status, eliminating promise/future heap allocation and sub-state overhead.
     * If the callable returns a non-void type T, Post() returns absl::StatusOr<std::future<T>>.
     *
     * @tparam F The type of the callable object.
     * @param f The callable object to execute.
     * @param delay The duration to wait before executing the task. A delay of
     * zero executes the task as soon as possible.
     * @param options Configuration options specifying task metadata for diagnostics.
     * @return absl::Status for void tasks, or std::future<T> for value-returning tasks.
     */
    template <typename F>
    auto Post(F&& f, absl::Duration delay = absl::ZeroDuration(), PostOptions options = {})
            -> PostReturnType<F> {
        if (options.caller_pc == 0) {
            options.caller_pc = __builtin_return_address(0);
        }
        if constexpr (std::is_same_v<PostReturnType<F>, absl::Status>) {
            return PostTaskWithOptions(std::forward<F>(f), delay, options);
        } else {
            return PostWithOptions<F>(std::forward<F>(f), delay, options);
        }
    }

    /**
     * @brief Posts a callable object with a custom context label.
     *
     * This is a convenience overload of Post() that automatically sets the PostOptions
     * context label and retrieves the caller's program counter.
     *
     * @tparam F The type of the callable object.
     * @param f The callable object to execute.
     * @param delay The duration to wait before execution.
     * @param context A label describing the source or purpose of the task.
     * @return absl::Status for void tasks, or std::future<T> for value-returning tasks.
     */
    template <typename F>
    auto Post(F&& f, absl::Duration delay, std::string_view context) -> PostReturnType<F> {
        return Post(std::forward<F>(f), delay,
                    PostOptions{.caller_pc = __builtin_return_address(0), .context = context});
    }

    /**
     * @brief Posts a callable object immediately with a custom context label.
     *
     * This is a convenience overload of Post() that executes the callable as soon as
     * possible with the specified context label.
     *
     * @tparam F The type of the callable object.
     * @param f The callable object to execute.
     * @param context A label describing the source or purpose of the task.
     * @return absl::Status for void tasks, or std::future<T> for value-returning tasks.
     */
    template <typename F>
    auto Post(F&& f, std::string_view context) -> PostReturnType<F> {
        return Post(std::forward<F>(f), absl::ZeroDuration(), context);
    }

  private:
    absl::Status PostTaskWithOptions(Task task, absl::Duration delay, const PostOptions& options) {
        FlowId flow_id = 0;
        if (tracker_) {
            flow_id = tracker_->LogPost(options);
        }
        if (delay == absl::ZeroDuration()) {
            return PostImmediately(std::move(task), flow_id);
        }
        return PostDelayed(std::move(task), delay, flow_id);
    }
    template <typename F>
    auto PostWithOptions(F&& f, absl::Duration delay, const PostOptions& options)
            -> absl::StatusOr<std::future<decltype(std::forward<F>(f)())>> {
        using ReturnType = decltype(std::forward<F>(f)());
        std::promise<ReturnType> promise;
        auto future = promise.get_future();

        auto task_runner = [promise = std::move(promise), f = std::forward<F>(f)]() mutable {
            promise.set_value(f());
        };

        absl::Status s = PostTaskWithOptions(std::move(task_runner), delay, options);
        if (!s.ok()) {
            return s;
        }

        return future;
    }

  public:
    /**
     * @brief Posts a task to the event loop and blocks the calling thread
     * until the task is complete.
     *
     * @warning This method MUST NOT be called from the event loop's own
     * thread, as it will cause an immediate deadlock. The application
     * will immediately exit with a FATAL warning.
     */
    template <typename F>
    auto PostAndWait(F&& task) -> PostAndWaitReturnType<F> {
        if (IsOnLoopThread()) {
            LOG(FATAL) << "postAndWait cannot be called from the event loop.";
        }
        // Fail immediately if looper has finished to avoid deadlocking
        // when no loop thread is running to process enqueued tasks.
        if (GetState() == LooperStatusEvent::State::kFinished) {
            return absl::FailedPreconditionError("Event loop has finished execution");
        }

        using ReturnType = std::invoke_result_t<std::decay_t<F>>;
        absl::Notification done;
        FlowId flow_id = 0;
        if (tracker_) {
            flow_id = tracker_->LogPost(PostOptions{.caller_pc = __builtin_return_address(0)});
        }

        if constexpr (std::is_void_v<ReturnType>) {
            auto task_runner = [&task, &done]() {
                task();
                done.Notify();
            };
            absl::Status s = PostImmediately(std::move(task_runner), flow_id);
            if (!s.ok()) {
                return s;
            }
            done.WaitForNotification();
            return absl::OkStatus();
        } else {
            // Use std::optional to reserve uninitialized storage on the caller's stack frame.
            // This avoids requiring ReturnType to be default-constructible, eliminates dynamic heap
            // allocations (unlike std::promise/future or std::unique_ptr), and avoids dummy
            // fallback error status initializations.
            std::optional<ReturnType> result;
            auto task_runner = [&task, &result, &done]() {
                result.emplace(task());
                done.Notify();
            };
            absl::Status s = PostImmediately(std::move(task_runner), flow_id);
            if (!s.ok()) {
                return s;
            }
            done.WaitForNotification();
            return std::move(result).value();
        }
    }

    /**
     * @brief Schedules a cancellable task to be executed once after a delay.
     * @param task The task to execute. Must return true to reschedule, or false to stop.
     * @return A ScopedTimer handle for scheduling and cancellation.
     */
    virtual ScopedTimer CreateTimer(RepeatingTask task) = 0;

    /**
     * @brief Schedules a cancellable task to be executed once after a delay.
     * @param task The task to execute.
     * @param delay The duration to wait before executing the task.
     * @return A ScopedTimer handle for cancellation.
     */
    ScopedTimer ScheduleDelayed(Task task, absl::Duration delay) {
        return ScheduleRepeating(
                [task = std::move(task)]() mutable {
                    task();
                    return false;
                },
                delay, absl::ZeroDuration());
    }

    /**
     * @brief Schedules a cancellable task to be executed repeatedly.
     * @param task The task to execute.
     * @param initial_delay The delay before the first execution.
     * @param interval The time between subsequent executions.
     * @return A ScopedTimer handle for cancellation.
     */
    ScopedTimer ScheduleRepeating(RepeatingTask task, absl::Duration initial_delay,
                                  absl::Duration interval) {
        auto timer = CreateTimer(std::move(task));
        if (timer) {
            timer->Schedule(initial_delay, interval);
        }
        return timer;
    };

    // Implementation specific loop.
    virtual void* GetRawLoop() {
        return nullptr;  // `nullptr` is a valid value here
    }

    /**
     * @brief Gets the current state of the event loop.
     * @return The current state.
     */
    virtual LooperStatusEvent::State GetState() const { return state_; }

    /**
     * @brief Accesses the associated looper breadcrumb tracker.
     *
     * @return A pointer to the LooperBreadcrumbTracker instance, or nullptr if disabled.
     */
    LooperBreadcrumbTracker* tracker() const { return tracker_.get(); }

  protected:
    virtual absl::Status PostImmediately(Task task, FlowId flow_id) = 0;
    virtual absl::Status PostDelayed(Task task, absl::Duration delay, FlowId flow_id) = 0;

    void SetState(LooperStatusEvent::State new_state) {
        const LooperStatusEvent::State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            FireEvent({.state = new_state});
        }
    }

  private:
    std::string name_;
    std::atomic<LooperStatusEvent::State> state_{LooperStatusEvent::State::kNotStarted};
    std::unique_ptr<LooperBreadcrumbTracker> tracker_;
};

}  // namespace goldfish::async
