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
#include "goldfish/async/testing/test_event_loop.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <queue>
#include <thread>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/base/threads/thread_utils.h"

namespace goldfish::async::testing {

// The concrete implementation class, hidden entirely within this .cpp file.
class TestEventLoopImpl : public TestEventLoop {
    class TestTimer;

  public:
    explicit TestEventLoopImpl(std::string name);
    ~TestEventLoopImpl() override;

    // EventLoop Interface
    void ShutdownTimers() override;
    size_t WaitUntilIdle() override;
    std::future<absl::Status> Shutdown() override;
    bool IsOnLoopThread() const override;
    absl::Status PostImmediately(Task task, FlowId flow_id) override;
    absl::Status PostDelayed(Task task, absl::Duration delay, FlowId flow_id) override;
    ScopedTimer CreateTimer(RepeatingTask task) override;

    // TestEventLoop Interface
    void RunAll() override;
    bool RunOne() override;
    size_t RunMany(size_t count) override;
    void AdvanceClock(absl::Duration duration) override;
    size_t TaskCount() const override;
    void Pause() override;
    void Resume() override;

    void Reschedule(std::shared_ptr<TestEventLoopImpl::TestTimer> timer, absl::Duration new_delay,
                    absl::Duration new_interval);

  private:
    struct QueuedTask {
        Task task;
        FlowId flow_id;
    };

    struct ScheduledTask {
        absl::Time execution_time;
        absl::Duration interval;
        std::shared_ptr<RepeatingTask> task;

        // handle to the timer that is handed to the developer
        // we track the liveness and cancellation state here.
        std::weak_ptr<TestTimer> handle;
        FlowId flow_id = 0;

        bool operator>(const ScheduledTask& other) const {
            return execution_time > other.execution_time;
        }
    };

    class TestTimer : public Timer, public std::enable_shared_from_this<TestTimer> {
      public:
        static std::shared_ptr<TestTimer> Create(TestEventLoopImpl* loop, RepeatingTask task,
                                                 FlowId flow_id, bool pin_for_caller) {
            auto timer = std::make_shared<TestTimer>(loop, std::move(task), flow_id);
            if (pin_for_caller) {
                timer->caller_pin_ = timer;
            } else {
                timer->loop_pin_ = timer;
            }
            return timer;
        }

        TestTimer(TestEventLoopImpl* loop, RepeatingTask task, FlowId flow_id = 0)
                : loop_(loop)
                , pending_task_(std::make_shared<RepeatingTask>(std::move(task)))
                , flow_id_(flow_id) {}
        ~TestTimer() override = default;
        void Cancel() override {
            auto caller_pin = std::move(caller_pin_);
            MarkCancelled();
        }
        void MarkCancelled() {
            cancelled_ = true;
            loop_pin_.reset();
        }
        bool IsCancelled() const { return cancelled_; }
        std::shared_ptr<RepeatingTask> task() { return pending_task_; }  // NOLINT
        FlowId GetFlowId() const { return flow_id_; }
        void Schedule(absl::Duration new_delay, absl::Duration new_interval) override {
            if (cancelled_) {
                return;
            }
            loop_->Reschedule(shared_from_this(), new_delay, new_interval);
        }

      private:
        std::shared_ptr<TestTimer> caller_pin_;
        std::shared_ptr<TestTimer> loop_pin_;
        std::atomic_bool cancelled_{false};
        TestEventLoopImpl* loop_;
        std::shared_ptr<RepeatingTask> pending_task_;
        ::goldfish::async::FlowId flow_id_;
    };

    enum class Command : uint8_t { kNone, kRunOne, kRunMany, kAdvanceTime };

    void Loop();
    bool RunOneUnlocked() ABSL_NO_THREAD_SAFETY_ANALYSIS;
    void AdvanceClockUnlocked(absl::Duration duration) ABSL_NO_THREAD_SAFETY_ANALYSIS;

    bool HasCommandOrStopped() const ABSL_SHARED_LOCKS_REQUIRED(mutex_) {
        return command_ != Command::kNone || stop_;
    }
    bool IsCommandNone() const ABSL_SHARED_LOCKS_REQUIRED(mutex_) {
        return command_ == Command::kNone;
    }
    bool IsQueueIdle() const ABSL_SHARED_LOCKS_REQUIRED(mutex_) { return queue_is_idle_; }

    std::thread thread_;
    std::thread::id thread_id_;
    mutable absl::Mutex mutex_;
    bool stop_ ABSL_GUARDED_BY(mutex_){false};

    // post queue
    std::deque<QueuedTask> tasks_ ABSL_GUARDED_BY(mutex_);

    // scheduled things
    std::vector<ScheduledTask> scheduled_tasks_ ABSL_GUARDED_BY(mutex_);
    absl::Time now_ ABSL_GUARDED_BY(mutex_);
    Command command_ ABSL_GUARDED_BY(mutex_) = Command::kNone;
    absl::Duration time_advance_ ABSL_GUARDED_BY(mutex_){absl::ZeroDuration()};
    size_t run_count_ ABSL_GUARDED_BY(mutex_) = 0;
    size_t tasks_actually_run_ ABSL_GUARDED_BY(mutex_) = 0;
    size_t tasks_processed_ ABSL_GUARDED_BY(mutex_) = 0;
    bool queue_is_idle_ ABSL_GUARDED_BY(mutex_) = true;
};

// --- Factory Function ---
std::unique_ptr<TestEventLoop> TestEventLoop::Create(std::string name) {
    return std::make_unique<TestEventLoopImpl>(std::move(name));
}

// --- TestEventLoopImpl Implementation ---
TestEventLoopImpl::TestEventLoopImpl(std::string name)
        : TestEventLoop(std::move(name)), now_(absl::Now()) {
    std::promise<void> thread_started_promise;
    auto thread_started_future = thread_started_promise.get_future();
    thread_ = std::thread([this, &thread_started_promise]() {
        thread_id_ = std::this_thread::get_id();
        SetState(LooperStatusEvent::State::kRunning);
        thread_started_promise.set_value();
        Loop();
    });
    thread_started_future.wait();
}

TestEventLoopImpl::~TestEventLoopImpl() {
    if (GetState() != LooperStatusEvent::State::kShuttingDown) {
        ShutdownAndWait().IgnoreError();
    }
    {
        absl::MutexLock lock(&mutex_);
        stop_ = true;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void TestEventLoopImpl::ShutdownTimers() {
    absl::MutexLock lock(&mutex_);
    for (auto& task : scheduled_tasks_) {
        if (auto handle = task.handle.lock()) {
            handle->MarkCancelled();
        }
    }
    scheduled_tasks_.clear();
}

size_t TestEventLoopImpl::WaitUntilIdle() {
    absl::MutexLock lock(&mutex_);
    mutex_.Await(absl::Condition(this, &TestEventLoopImpl::IsQueueIdle));
    return tasks_processed_;
}

std::future<absl::Status> TestEventLoopImpl::Shutdown() {
    SetState(LooperStatusEvent::State::kShuttingDown);
    std::promise<absl::Status> promise;
    promise.set_value(absl::OkStatus());
    absl::MutexLock lock(&mutex_);
    tasks_.clear();
    for (auto& task : scheduled_tasks_) {
        if (auto handle = task.handle.lock()) {
            handle->MarkCancelled();
        }
    }
    scheduled_tasks_.clear();

    return promise.get_future();
}

bool TestEventLoopImpl::IsOnLoopThread() const {
    return std::this_thread::get_id() == thread_id_;
}

absl::Status TestEventLoopImpl::PostImmediately(Task task, FlowId flow_id) {
    if (GetState() == LooperStatusEvent::State::kShuttingDown) {
        LOG(ERROR) << "Loop is shutting down.";
        return absl::UnavailableError("test loop is shutting down");
    }
    absl::MutexLock lock(&mutex_);
    queue_is_idle_ = false;
    tasks_.push_back(QueuedTask{std::move(task), flow_id});
    return absl::OkStatus();
}

absl::Status TestEventLoopImpl::PostDelayed(Task task, absl::Duration delay, FlowId flow_id) {
    if (GetState() == LooperStatusEvent::State::kShuttingDown) {
        LOG(ERROR) << "Loop is shutting down.";
        return absl::UnavailableError("test loop is shutting down");
    }
    auto timer = TestTimer::Create(
            this,
            [task = std::move(task)]() mutable {
                task();
                return false;
            },
            flow_id, /*pin_for_caller=*/false);
    timer->Schedule(delay, absl::ZeroDuration());
    return absl::OkStatus();
}

size_t TestEventLoopImpl::TaskCount() const {
    absl::MutexLock lock(&mutex_);
    return tasks_.size();
}

EventLoop::ScopedTimer TestEventLoopImpl::CreateTimer(RepeatingTask task) {
    auto timer = TestTimer::Create(this, std::move(task), 0, /*pin_for_caller=*/true);
    return ScopedTimer(timer.get());
}

void TestEventLoopImpl::Reschedule(std::shared_ptr<TestTimer> timer, absl::Duration new_delay,
                                   absl::Duration new_interval) {
    absl::MutexLock lock(&mutex_);
    auto it = std::ranges::find_if(scheduled_tasks_, [&](const ScheduledTask& task) {
        auto handle = task.handle.lock();
        return handle && handle.get() == timer.get();
    });

    if (it != scheduled_tasks_.end()) {
        it->execution_time = now_ + new_delay;
        it->interval = new_interval;
        std::ranges::make_heap(scheduled_tasks_, std::greater<>{});
    } else {
        scheduled_tasks_.push_back(
                {now_ + new_delay, new_interval, timer->task(), timer, timer->GetFlowId()});
        std::ranges::push_heap(scheduled_tasks_, std::greater<>{});
    }
}

void TestEventLoopImpl::RunAll() {
    while (RunMany(std::numeric_limits<size_t>::max())) {
    }
}

bool TestEventLoopImpl::RunOne() {
    absl::MutexLock lock(&mutex_);
    command_ = Command::kRunOne;
    tasks_actually_run_ = 0;
    mutex_.Await(absl::Condition(this, &TestEventLoopImpl::IsCommandNone));
    return tasks_actually_run_ > 0;
}

size_t TestEventLoopImpl::RunMany(size_t count) {
    absl::MutexLock lock(&mutex_);
    command_ = Command::kRunMany;
    run_count_ = count;
    tasks_actually_run_ = 0;
    mutex_.Await(absl::Condition(this, &TestEventLoopImpl::IsCommandNone));
    return tasks_actually_run_;
}

void TestEventLoopImpl::AdvanceClock(absl::Duration duration) {
    absl::MutexLock lock(&mutex_);
    command_ = Command::kAdvanceTime;
    time_advance_ = duration;
    mutex_.Await(absl::Condition(this, &TestEventLoopImpl::IsCommandNone));
}

void TestEventLoopImpl::Pause() {
    if (GetState() == LooperStatusEvent::State::kRunning) {
        SetState(LooperStatusEvent::State::kPaused);
    }
}

void TestEventLoopImpl::Resume() {
    if (GetState() == LooperStatusEvent::State::kPaused ||
        GetState() == LooperStatusEvent::State::kNotStarted) {
        SetState(LooperStatusEvent::State::kRunning);
    }
}

void TestEventLoopImpl::Loop() {
    android::base::ThreadUtils::SetCurrentThreadName(GetName());
    absl::MutexLock lock(&mutex_);
    while (!stop_) {
        mutex_.Await(absl::Condition(this, &TestEventLoopImpl::HasCommandOrStopped));
        if (stop_) break;

        // Note, we have the mutex here.
        switch (command_) {
        case Command::kRunOne:
            tasks_actually_run_ = RunOneUnlocked() ? 1 : 0;
            break;
        case Command::kRunMany:
            for (size_t i = 0; i < run_count_; ++i) {
                if (RunOneUnlocked()) {
                    tasks_actually_run_++;
                } else {
                    break;  // No more tasks to run
                }
            }
            break;
        case Command::kAdvanceTime:
            AdvanceClockUnlocked(time_advance_);
            break;
        case Command::kNone:
            break;
        }

        command_ = Command::kNone;
    }
    SetState(LooperStatusEvent::State::kFinished);
}

bool TestEventLoopImpl::RunOneUnlocked() ABSL_NO_THREAD_SAFETY_ANALYSIS {
    // we have the mutex here.
    if (tasks_.empty()) {
        return false;
    }
    QueuedTask queued = std::move(tasks_.front());
    tasks_.pop_front();
    mutex_.Unlock();
    {
        // without lock so tasks can schedule more tasks etc..
        QueuedTask queued_scoped = std::move(queued);
        if (queued_scoped.flow_id != 0 && tracker()) {
            tracker()->LogExecute(queued_scoped.flow_id);
        }
        queued_scoped.task();
    }
    mutex_.Lock();
    ++tasks_processed_;
    if (tasks_.empty()) {
        queue_is_idle_ = true;
    }
    return true;
}

void TestEventLoopImpl::AdvanceClockUnlocked(absl::Duration duration)
        ABSL_NO_THREAD_SAFETY_ANALYSIS {
    now_ += duration;
    std::vector<ScheduledTask> tasks_to_run;

    // Pop all tasks from the heap that are ready to go
    while (!scheduled_tasks_.empty() && scheduled_tasks_.front().execution_time <= now_) {
        std::ranges::pop_heap(scheduled_tasks_, std::greater<>{});
        tasks_to_run.push_back(std::move(scheduled_tasks_.back()));
        scheduled_tasks_.pop_back();
    }

    for (auto& task : tasks_to_run) {
        auto handle = task.handle.lock();
        if (!handle || handle->IsCancelled()) {
            continue;
        }

        // Run the task without a lock.
        mutex_.Unlock();
        if (task.flow_id != 0 && tracker()) {
            tracker()->LogExecute(task.flow_id);
        }
        const bool keep_repeating = (*task.task)();
        mutex_.Lock();

        // Reschedule task if needed.
        if (!keep_repeating) {
            handle->MarkCancelled();
        } else if (task.interval > absl::ZeroDuration()) {
            task.execution_time += task.interval;
            scheduled_tasks_.push_back(std::move(task));
            std::ranges::push_heap(scheduled_tasks_, std::greater<>{});
        }
    }
}

}  // namespace goldfish::async::testing
