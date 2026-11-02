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

// This file provides an implementation of the EventLoop interface based on the
// main event loop provided by the QEMU emulator. It allows scheduling tasks
// (timers, immediate callbacks) on the main QEMU thread from anywhere in the
// application.
//
// The QemuEventLoopImpl is a singleton, accessible via `getQemuEventLoop()`.
// It's crucial to call `initializeQemuEventLoop()` from the main QEMU thread
// at startup before any other thread attempts to use the loop. This sets a
// thread-local flag that allows `isOnLoopThread()` to work correctly.

#include "goldfish/async/qemu_event_loop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/perfetto/perfetto_categories.h"
#include "goldfish/qemu/qemubh.h"

extern "C" {

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "migration/snapshot.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"


// Windows workarounds
#ifdef shutdown
#undef shutdown
#endif
#ifdef close
#undef close
#endif
// IWYU pragma: end_keep
// clang-format on
}

namespace goldfish::async {
namespace {

using goldfish::async::LooperStatusEvent;
using goldfish::qemu::MakeQemuBh;
using goldfish::qemu::QEMUBHPtr;

// An implementation of the EventLoop interface that is backed by the QEMU main
// event loop. This allows scheduling work on the main QEMU thread.
class QemuEventLoopImpl : public goldfish::async::QemuEventLoop {
  public:
    // --- QemuTimer Implementation ---
    // An implementation of the Timer interface that wraps a QEMUTimer.
    //
    // This class is responsible for managing the lifetime of cancellable delayed
    // and repeating tasks scheduled on the QemuEventLoopImpl.
    class QemuTimer : public EventLoop::Timer, public std::enable_shared_from_this<QemuTimer> {
        struct Private {};

      public:
        enum class Ownership : uint8_t {
            kCallerScoped,  // Returned via ScopedTimer; disarms on false and stays reschedulable.
            kLoopManaged,   // Internal PostDelayed one-shot; self-cancels after firing.
        };

        static std::shared_ptr<QemuTimer> Create(QemuEventLoopImpl* loop, RepeatingTask task,
                                                 FlowId flow_id, Ownership ownership) {
            auto timer = std::make_shared<QemuTimer>(loop, std::move(task), flow_id, ownership,
                                                     Private());
            if (ownership == Ownership::kCallerScoped) {
                timer->caller_pin_ = timer;
            }
            if (!loop->TryRegisterTimer(timer)) {
                timer->cancelled_.store(true);
            }
            return timer;
        }

        QemuTimer(QemuEventLoopImpl* loop, EventLoop::RepeatingTask task, FlowId flow_id,
                  Ownership ownership, Private)
                : event_loop_(loop)
                , task_(std::move(task))
                , flow_id_(flow_id)
                , ownership_(ownership) {
            DCHECK(event_loop_);
        }

        void Cancel() override {
            auto caller_pin = std::move(caller_pin_);
            if (cancelled_.exchange(true)) {
                return;
            }
            if (event_loop_->IsOnLoopThread()) {
                DoCancel();
                return;
            }
            if (event_loop_->GetState() == LooperStatusEvent::State::kNotStarted ||
                !event_loop_->HasStartedDraining()) {
                DoCancel();
                return;
            }
            bool under_bql = false;
            if (bql_locked()) {
                under_bql = true;
                bql_unlock();
            }

            if (!event_loop_->PostAndWait([this]() { DoCancel(); }).ok()) {
                DoCancel();
            }

            if (under_bql) {
                bql_lock();
            }
        }

        void Schedule(absl::Duration new_delay, absl::Duration new_interval) override {
            // Because Timer::Cancel() is invoked solely by ~ScopedTimer(), a live
            // ScopedTimer is only marked cancelled_ when the loop is shutting down
            // or has finished, where EventLoop::Timer::Schedule() is a no-op.
            if (cancelled_.load()) {
                return;
            }
            const auto to_qemu_ms = [](absl::Duration d) -> int64_t {
                if (d <= absl::ZeroDuration()) {
                    return 0;
                }
                return std::max<int64_t>(1, absl::ToInt64Milliseconds(d));
            };
            const uint64_t seq = ++schedule_seq_;
            event_loop_
                    ->PostImmediately(
                            [self = shared_from_this(), seq, new_delay_ms = to_qemu_ms(new_delay),
                             new_interval_ms = to_qemu_ms(new_interval)] {
                                if (self->cancelled_.load() ||
                                    !self->event_loop_->LockActiveTimer(self.get())) {
                                    return;
                                }
                                if (seq != self->schedule_seq_.load()) {
                                    return;
                                }
                                self->active_schedule_seq_ = seq;
                                self->interval_ms_ = new_interval_ms;
                                self->next_t_ =
                                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + new_delay_ms;
                                timer_mod(&self->qemu_timer_handle_, self->next_t_);
                            },
                            /*flow_id=*/0)
                    .IgnoreError();
        }

      private:
        void DoCancel() {
            cancelled_.store(true);
            timer_del(&qemu_timer_handle_);
            std::shared_ptr<QemuTimer> loop_pin = event_loop_->UnregisterTimer(this);
            loop_pin.reset();
        }

        // C-style callback passed to QEMU.
        static void OnTimerStatic(void* opaque) {
            auto* raw = static_cast<QemuTimer*>(opaque);
            const std::shared_ptr<QemuTimer> self = raw->event_loop_->LockActiveTimer(raw);
            if (!self || self->cancelled_.load()) {
                return;
            }
            DCHECK(self->event_loop_->IsOnLoopThread())
                    << "OnTimer callback is not called from the event loop";

            self->OnTimer();
        }

        void OnTimer() {
            if (active_schedule_seq_ != schedule_seq_.load()) {
                return;
            }
            if (flow_id_ != 0 && event_loop_->tracker()) {
                event_loop_->tracker()->LogExecute(flow_id_);
            }

            if (!task_()) {
                if (ownership_ == Ownership::kLoopManaged) {
                    DoCancel();
                }
            } else if (interval_ms_ && !cancelled_.load() &&
                       active_schedule_seq_ == schedule_seq_.load()) {
                // The Qemu Timer API doesn't natively support repeating timers so we have to kick
                // it off again.
                constexpr int kStarvingWarning = 3;
                next_t_ += interval_ms_;

                const int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
                if ((now - next_t_) > (interval_ms_ * kStarvingWarning)) {
                    LOG(WARNING) << "A timer (" << this << ") is more than " << kStarvingWarning
                                 << " intervals (" << interval_ms_
                                 << "ms) behind. "
                                    "This is a sign of insufficient performance.";

                    next_t_ = now + interval_ms_;
                }

                timer_mod(&qemu_timer_handle_, next_t_);
            }
        }

        QemuEventLoopImpl* const event_loop_;
        std::shared_ptr<QemuTimer> caller_pin_;
        std::shared_ptr<QemuTimer> pinned_;  ///< prevents calling the dtor while registered
        RepeatingTask task_;
        QEMUTimer qemu_timer_handle_{};
        const FlowId flow_id_;
        int64_t next_t_ = 0;
        int64_t interval_ms_ = 0;

        // Monotonically increasing sequence number incremented on each Schedule() call.
        // Captured in the posted task to ensure that when multiple Schedule() calls
        // race or are posted in quick succession, older tasks running on the event loop
        // thread discard stale updates if superseded by a newer Schedule().
        std::atomic<uint64_t> schedule_seq_{0};

        // The sequence number of the schedule currently active on the loop backend.
        // Checked in OnTimer() to ensure an expiring timer does not fire if a newer
        // Schedule() has superseded it.
        uint64_t active_schedule_seq_ = 0;

        const Ownership ownership_;
        std::atomic<bool> cancelled_{false};

        friend QemuEventLoopImpl;
    };

    explicit QemuEventLoopImpl(std::string name)
            : QemuEventLoop(std::move(name)), drainer_bh_(MakeQemuBh([&] { DrainQueue(); })) {
        const bool lock_bql = !bql_locked();
        if (lock_bql) {
            bql_lock();
        }
        snapshot_progress_entry_ = add_snapshot_progress_handler(&OnSnapshotProgress, this);
        if (lock_bql) {
            bql_unlock();
        }
    }

    // TODO(whollins): Clean-up usages and make this FATAL.
    ~QemuEventLoopImpl() override {
        if (snapshot_progress_entry_) {
            const bool lock_bql = !bql_locked();
            if (lock_bql) {
                bql_lock();
            }
            del_snapshot_progress_handler(snapshot_progress_entry_);
            snapshot_progress_entry_ = nullptr;
            if (lock_bql) {
                bql_unlock();
            }
        }
        LOG_IF(ERROR, !is_shutting_down_) << "Qemu loop has not been shutdown prior to destruction";
        DCHECK(active_timers_.empty());
    };

    void OnInitialDrain() {
        CompareAndSetState(LooperStatusEvent::State::kNotStarted,
                           snapshot_in_progress_.load() ? LooperStatusEvent::State::kPaused
                                                        : LooperStatusEvent::State::kRunning);
    }

    void ShutdownTimers() override;
    size_t WaitUntilIdle() override;
    std::future<absl::Status> Shutdown() override;

    bool IsOnLoopThread() const override;
    bool HasStartedDraining() const { return qemu_thread_id_.load() != std::thread::id{}; }

    ScopedTimer CreateTimer(RepeatingTask task) override;

  private:
    struct QueuedTask {
        Task task;
        FlowId flow_id;
    };

    void PostImmediatelyLocked(Task task, FlowId flow_id = 0)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(queue_mutex_);
    absl::Status PostImmediately(Task task, FlowId flow_id) override;
    absl::Status PostDelayed(Task task, absl::Duration delay, FlowId flow_id) override;

    size_t DrainQueue() {
        qemu_thread_id_ = std::this_thread::get_id();
        std::queue<QueuedTask> local_queue;
        {
            const absl::MutexLock lock(queue_mutex_);
            task_queue_.swap(local_queue);
            drainer_scheduled_ = false;
        }

        const size_t num_tasks_to_process = local_queue.size();
        TRACE_EVENT("async", "QemuEventLoop::DrainQueue", "batch_size", num_tasks_to_process);

        while (!local_queue.empty()) {
            FlowId flow_id = local_queue.front().flow_id;
            if (flow_id != 0 && tracker()) {
                tracker()->LogExecute(flow_id);
            }
            local_queue.front().task();
            local_queue.pop();
        }

        absl::MutexLock lock(queue_mutex_);
        queue_is_idle_ = task_queue_.empty();
        tasks_processed_ += num_tasks_to_process;
        return tasks_processed_;
    }

    bool TryRegisterTimer(const std::shared_ptr<QemuTimer>& t) {
        const absl::MutexLock lock(active_timers_mutex_);
        if (timers_shut_down_ || is_shutting_down_.load()) {
            return false;
        }
        timer_init_ms(&t->qemu_timer_handle_, QEMU_CLOCK_VIRTUAL, &QemuTimer::OnTimerStatic,
                      t.get());
        t->pinned_ = t;
        std::weak_ptr<QemuTimer>& existing = active_timers_[t.get()];
        DCHECK(existing.expired()) << "Tried to insert a duplicate timer";
        existing = t;
        return true;
    }

    std::shared_ptr<QemuTimer> UnregisterTimer(QemuTimer* const t) {
        const absl::MutexLock lock(active_timers_mutex_);
        active_timers_.erase(t);
        return std::move(t->pinned_);
    }

    std::shared_ptr<QemuTimer> LockActiveTimer(QemuTimer* const t) {
        const absl::MutexLock lock(active_timers_mutex_);
        auto it = active_timers_.find(t);
        if (it == active_timers_.end()) {
            return nullptr;
        }
        return it->second.lock();
    }

    void ShutdownTimersInternal() {
        LOG_IF(DFATAL, !IsOnLoopThread())
                << "ShutdownTimersInternal must be called from the loop thread";
        std::vector<std::shared_ptr<QemuTimer>> timers_to_cancel;
        {
            const absl::MutexLock lock(active_timers_mutex_);
            timers_shut_down_ = true;
            timers_to_cancel.reserve(active_timers_.size());
            for (const auto& [raw_ptr, weak_timer] : active_timers_) {
                if (std::shared_ptr<QemuTimer> timer = std::move(raw_ptr->pinned_)) {
                    timers_to_cancel.push_back(std::move(timer));
                }
            }
            active_timers_.clear();
        }
        for (const auto& timer : timers_to_cancel) {
            timer->cancelled_.store(true);
            timer_del(&timer->qemu_timer_handle_);
        }
    }

    static void OnSnapshotProgress(void* opaque, const QemuSnapshotProgress* progress) {
        if (!progress) {
            return;
        }
        auto* self = static_cast<QemuEventLoopImpl*>(opaque);
        self->HandleSnapshotProgress(*progress);
    }

    // Uses compare-and-set so that kPaused/kRunning never overwrite kShuttingDown or kFinished
    // set by a concurrent Shutdown().
    void HandleSnapshotProgress(const QemuSnapshotProgress& progress) {
        switch (progress.phase) {
        case QEMU_SNAPSHOT_PHASE_PREPARING:
        case QEMU_SNAPSHOT_PHASE_TRANSFERRING:
            snapshot_in_progress_.store(true);
            CompareAndSetState<LooperStatusEvent::State::kRunning,
                               LooperStatusEvent::State::kPaused>();
            break;
        case QEMU_SNAPSHOT_PHASE_COMPLETED:
        case QEMU_SNAPSHOT_PHASE_FAILED:
        case QEMU_SNAPSHOT_PHASE_NONE:
            snapshot_in_progress_.store(false);
            CompareAndSetState<LooperStatusEvent::State::kPaused,
                               LooperStatusEvent::State::kRunning>();
            break;
        }
    }

    const QEMUBHPtr drainer_bh_;
    QemuSnapshotProgressEntry* snapshot_progress_entry_{nullptr};
    std::promise<absl::Status> shutdown_complete_promise_;
    std::atomic<std::thread::id> qemu_thread_id_{std::thread::id{}};
    size_t tasks_processed_ ABSL_GUARDED_BY(queue_mutex_) = 0;
    std::queue<QueuedTask> task_queue_ ABSL_GUARDED_BY(queue_mutex_);
    // A map of raw pointers to their corresponding weak pointers for safe shutdown.
    absl::flat_hash_map<QemuTimer*, std::weak_ptr<QemuTimer>> active_timers_
            ABSL_GUARDED_BY(active_timers_mutex_);

    // Keep all bools together to minimize alignment gaps.
    std::atomic<bool> snapshot_in_progress_{false};
    std::atomic<bool> is_shutting_down_{false};
    bool drainer_scheduled_ ABSL_GUARDED_BY(queue_mutex_) = false;
    bool queue_is_idle_ ABSL_GUARDED_BY(queue_mutex_) = true;
    bool timers_shut_down_ ABSL_GUARDED_BY(active_timers_mutex_) = false;

    // Mutexes placed at the very end.
    absl::Mutex queue_mutex_;
    absl::Mutex active_timers_mutex_;
};

// --- QemuEventLoopImpl Method Implementations ---

void QemuEventLoopImpl::ShutdownTimers() {
    if (IsOnLoopThread()) {
        ShutdownTimersInternal();
        return;
    }
    PostImmediately([this]() { ShutdownTimersInternal(); }, /*flow_id=*/0).IgnoreError();
}

size_t QemuEventLoopImpl::WaitUntilIdle() {
    if (IsOnLoopThread() || (!HasStartedDraining() && bql_locked())) {
        return DrainQueue();
    } else {
        const absl::MutexLock lock(queue_mutex_);
        if (!queue_mutex_.AwaitWithTimeout(absl::Condition(&queue_is_idle_), absl::Seconds(15))) {
            LOG(FATAL) << "Timed out waiting for queue to be idle";
        }
        return tasks_processed_;
    }
}

std::future<absl::Status> QemuEventLoopImpl::Shutdown() {
    auto do_shutdown = [this] {
        DrainQueue();
        ShutdownTimersInternal();
        SetState(LooperStatusEvent::State::kFinished);
        shutdown_complete_promise_.set_value(absl::OkStatus());
    };

    const bool on_loop_thread = IsOnLoopThread() || (!HasStartedDraining() && bql_locked());
    {
        const absl::MutexLock lock(queue_mutex_);
        if (is_shutting_down_.exchange(true)) {
            std::promise<absl::Status> promise;
            promise.set_value(absl::InvalidArgumentError("This loop has already been shutdown"));
            return promise.get_future();
        }
    }
    SetState(LooperStatusEvent::State::kShuttingDown);
    auto future = shutdown_complete_promise_.get_future();

    if (on_loop_thread) {
        do_shutdown();
    } else {
        const absl::MutexLock lock(queue_mutex_);
        PostImmediatelyLocked(do_shutdown);
    }

    return future;
}

bool QemuEventLoopImpl::IsOnLoopThread() const {
    return qemu_thread_id_ == std::this_thread::get_id();
}

void QemuEventLoopImpl::PostImmediatelyLocked(Task task, FlowId flow_id) {
    queue_is_idle_ = false;
    task_queue_.push(QueuedTask{std::move(task), flow_id});

    if (!drainer_scheduled_) {
        qemu_bh_schedule(drainer_bh_.get());
        drainer_scheduled_ = true;
    }
}

absl::Status QemuEventLoopImpl::PostImmediately(Task task, FlowId flow_id) {
    const absl::MutexLock lock(queue_mutex_);
    if (is_shutting_down_) {
        VLOG(1) << "Event loop is shutting down, not scheduling task";
        return absl::UnavailableError("QemuEventLoopImpl is shutting down");
    }

    PostImmediatelyLocked(std::move(task), flow_id);
    return absl::OkStatus();
}

absl::Status QemuEventLoopImpl::PostDelayed(Task task, absl::Duration delay, FlowId flow_id) {
    if (is_shutting_down_) {
        VLOG(1) << "Event loop is shutting down, not scheduling task";
        return absl::UnavailableError("QemuEventLoopImpl is shutting down");
    }

    // The timer will manage its own lifetime via a shared_ptr cycle that is
    // broken when the timer fires.
    auto timer = QemuTimer::Create(
            this,
            [task = std::move(task)]() mutable {
                task();
                return false;
            },
            flow_id, QemuTimer::Ownership::kLoopManaged);
    timer->Schedule(delay, absl::ZeroDuration());
    return absl::OkStatus();
}

EventLoop::ScopedTimer QemuEventLoopImpl::CreateTimer(RepeatingTask task) {
    auto timer = QemuTimer::Create(this, std::move(task), /*flow_id=*/0,
                                   QemuTimer::Ownership::kCallerScoped);
    return ScopedTimer(timer.get());
}

}  // namespace

// --- Factory Function ---
std::unique_ptr<QemuEventLoop> QemuEventLoop::Create(std::string name) {
    auto loop = std::make_unique<QemuEventLoopImpl>(std::move(name));
    loop->Post([loop_ptr = loop.get()] { loop_ptr->OnInitialDrain(); }).IgnoreError();

    return loop;
}

}  // namespace goldfish::async
