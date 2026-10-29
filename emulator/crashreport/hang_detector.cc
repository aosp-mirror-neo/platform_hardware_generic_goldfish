// Copyright 2017 The Android Open Source Project
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

#include "android/crashreport/hang_detector.h"

#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"

#include "android/crashreport/debug.h"
#include "goldfish/async/event_loop.h"

namespace android::crashreport {

namespace {

constexpr int kMaxHangCount = 2;

class LoopWatcher {
  public:
    LoopWatcher(std::string loop_name, ::goldfish::async::EventLoop& event_loop,
                absl::Duration hang_timeout, absl::Duration hang_check_timeout,
                absl::Clock& clock, HangDetector::ActivePredicate is_active)
            : loop_name_(std::move(loop_name))
            , timeout_(hang_timeout)
            , hang_check_timeout_(hang_check_timeout)
            , clock_(clock)
            , is_active_(std::move(is_active))
            , event_loop_(event_loop)
            , timer_(event_loop.CreateTimer([this]() {
                TaskComplete();
                return true;
            }))
            , state_subscription_(android::base::eventing::MakeScopedCallback(
                      event_loop, [this](const ::goldfish::async::LooperStatusEvent& event) {
                          OnLooperStateChanged(event);
                      })) {}

    ~LoopWatcher() { CancelHangCheck(); }

    LoopWatcher(LoopWatcher&&) = delete;
    LoopWatcher& operator=(LoopWatcher&&) = delete;
    LoopWatcher(const LoopWatcher&) = delete;
    LoopWatcher& operator=(const LoopWatcher&) = delete;

    bool IsOnTheLoop(::goldfish::async::EventLoop& rhs) const { return &event_loop_ == &rhs; }

    void StartHangCheck() {
        const absl::MutexLock l(&mutex_);
        if (IsActive()) {
            ScheduleHangCheckLocked();
        }
    }

    void CancelHangCheck() {
        std::shared_ptr<::goldfish::async::EventLoop::Timer> timer;
        std::unique_ptr<android::base::eventing::ScopedEventCallback<
                ::goldfish::async::EventLoop, ::goldfish::async::LooperStatusEvent>>
                state_subscription;
        {
            const absl::MutexLock l(&mutex_);
            timer = std::move(timer_);
            state_subscription = std::move(state_subscription_);
            is_task_running_ = false;
        }

        state_subscription.reset();
        if (timer) {
            timer->Cancel();
        }
    }

    void Process(const HangDetector::HangCallback& hang_callback) {
        absl::ReleasableMutexLock l(&mutex_);
        if (!timer_) {
            // Cancelled: the loop is no longer watched, and its predicate may capture state
            // that the owner has already destroyed.
            return;
        }
        if (android::base::IsDebuggerAttached() || !IsActive()) {
            is_task_running_ = false;
            hang_count_ = 0;
            last_check_time_ = absl::InfinitePast();
            return;
        }

        const absl::Time now = clock_.TimeNow();
        if (is_task_running_) {
            // Heuristic: If the looper watcher itself took much longer than
            // mTimeout to fire again, it's possible there was a system-wide
            // sleep. In this case, don't count that as hanging.
            // Note that we are using std::chrono to make sure all the durations
            // are scaled appropriately.
            const absl::Time time_system_live_and_hanging = last_check_time_ + timeout_;
            const absl::Time time_system_sleeped_past_hang_timeout =
                    last_check_time_ + 2 * timeout_;
            if (now > time_system_live_and_hanging && now < time_system_sleeped_past_hang_timeout) {
                const absl::Duration time_passed = now - last_check_time_;
                const auto message = absl::StrCat("detected a hanging thread '", loop_name_,
                                                  "'. No response for ", time_passed);
                ++hang_count_;
                LOG(ERROR) << message;
                if (hang_count_ >= kMaxHangCount && hang_callback) {
                    l.Release();
                    hang_callback(message);
                    return;
                }
                // Start another hang check in case something happened to this previous event.
                ScheduleHangCheckLocked();
            }
        } else if (now > last_check_time_ + hang_check_timeout_) {
            hang_count_ = 0;
            ScheduleHangCheckLocked();
        }
    }

  private:
    bool IsActive() const {
        return event_loop_.GetState() == ::goldfish::async::LooperStatusEvent::State::kRunning &&
               (!is_active_ || is_active_());
    }

    void OnLooperStateChanged(const ::goldfish::async::LooperStatusEvent& event) {
        if (event.state != ::goldfish::async::LooperStatusEvent::State::kRunning) {
            const absl::MutexLock l(&mutex_);
            is_task_running_ = false;
            hang_count_ = 0;
            last_check_time_ = absl::InfinitePast();
        }
    }

    void ScheduleHangCheckLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
        is_task_running_ = true;
        last_check_time_ = clock_.TimeNow();
        // 0 means run as soon as possible.
        if (timer_) {
            timer_->Schedule(absl::ZeroDuration());
        }
    }

    void TaskComplete() {
        const absl::MutexLock l(&mutex_);
        is_task_running_ = false;
    }

    const std::string loop_name_;
    const absl::Duration timeout_;
    const absl::Duration hang_check_timeout_;
    absl::Clock& clock_;
    const HangDetector::ActivePredicate is_active_;

    absl::Mutex mutex_;
    ::goldfish::async::EventLoop& event_loop_;
    std::shared_ptr<::goldfish::async::EventLoop::Timer> timer_ ABSL_GUARDED_BY(mutex_);
    std::unique_ptr<android::base::eventing::ScopedEventCallback<
            ::goldfish::async::EventLoop, ::goldfish::async::LooperStatusEvent>>
            state_subscription_ ABSL_GUARDED_BY(mutex_);
    bool is_task_running_ ABSL_GUARDED_BY(mutex_) = false;
    absl::Time last_check_time_ ABSL_GUARDED_BY(mutex_) = absl::InfinitePast();
    int hang_count_ ABSL_GUARDED_BY(mutex_) = 0;
};

class PredicateWatcher {
  public:
    PredicateWatcher(std::string name, HangDetector::HangPredicate check,
                     absl::Duration hang_check_timeout, absl::Clock& clock)
            : name_(std::move(name))
            , hang_check_timeout_(hang_check_timeout)
            , clock_(clock)
            , check_(std::move(check))
            , last_check_time_(clock_.TimeNow()) {}

    ~PredicateWatcher() { Cancel(); }

    PredicateWatcher(PredicateWatcher&&) = delete;
    PredicateWatcher& operator=(PredicateWatcher&&) = delete;
    PredicateWatcher(const PredicateWatcher&) = delete;
    PredicateWatcher& operator=(const PredicateWatcher&) = delete;

    void Cancel() {
        HangDetector::HangPredicate check;
        {
            const absl::MutexLock l(&mutex_);
            check = std::move(check_);
            hang_count_ = 0;
        }
    }

    void Process(const HangDetector::HangCallback& hang_callback) {
        absl::ReleasableMutexLock l(&mutex_);
        if (!check_) {
            // Cancelled: the predicate was removed, and its closure may capture state
            // that the owner has already destroyed.
            return;
        }
        if (android::base::IsDebuggerAttached()) {
            hang_count_ = 0;
            last_check_time_ = clock_.TimeNow();
            return;
        }

        const absl::Time now = clock_.TimeNow();
        if (now < last_check_time_ + hang_check_timeout_) {
            return;
        }
        last_check_time_ = now;

        const absl::Status status = check_();
        if (status.ok()) {
            hang_count_ = 0;
            return;
        }

        ++hang_count_;
        const std::string message =
                absl::StrCat("detected a hanging predicate '", name_, "': ", status.message());
        LOG(ERROR) << message;
        if (hang_count_ >= kMaxHangCount && hang_callback) {
            l.Release();
            hang_callback(message);
        }
    }

  private:
    const std::string name_;
    const absl::Duration hang_check_timeout_;
    absl::Clock& clock_;

    absl::Mutex mutex_;
    HangDetector::HangPredicate check_ ABSL_GUARDED_BY(mutex_);
    absl::Time last_check_time_ ABSL_GUARDED_BY(mutex_);
    int hang_count_ ABSL_GUARDED_BY(mutex_) = 0;
};

class HangDetectorImpl : public HangDetector {
  public:
    HangDetectorImpl(HangCallback hang_callback, Timing timing, absl::Clock& clock)
            : hang_callback_(std::move(hang_callback))
            , timing_(std::move(timing))
            , clock_(clock)
            , worker_thread_([this]() { WorkerThread(); }) {}

    ~HangDetectorImpl() override { Stop(); }

    void AddWatchedLooper(std::string loop_name, ::goldfish::async::EventLoop& event_loop,
                          absl::Duration task_timeout, ActivePredicate is_active) override {
        const absl::MutexLock l(&mutex_);
        if (stopping_) {
            return;
        }
        loop_watchers_.emplace_back(std::make_shared<LoopWatcher>(
                std::move(loop_name), event_loop, task_timeout, timing_.hang_check_timeout,
                clock_, std::move(is_active)));
        loop_watchers_.back()->StartHangCheck();
    }

    void RemoveWatchedLooper(::goldfish::async::EventLoop& event_loop) override {
        std::vector<std::shared_ptr<LoopWatcher>> to_remove;
        {
            const absl::MutexLock l(&mutex_);
            if (stopping_) {
                return;
            }

            for (auto it = loop_watchers_.begin(); it != loop_watchers_.end();) {
                if ((*it)->IsOnTheLoop(event_loop)) {
                    to_remove.push_back(std::move(*it));
                    it = loop_watchers_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& lw : to_remove) {
            lw->CancelHangCheck();
        }
    }

    void AddPredicate(std::string name, HangPredicate check) override {
        if (!check) {
            return;
        }
        std::shared_ptr<PredicateWatcher> replaced;
        {
            const absl::MutexLock l(&mutex_);
            if (stopping_) {
                return;
            }
            auto it = predicate_watchers_.find(name);
            if (it != predicate_watchers_.end()) {
                replaced = std::move(it->second);
            }
            predicate_watchers_[name] = std::make_shared<PredicateWatcher>(
                    name, std::move(check), timing_.hang_check_timeout, clock_);
        }

        if (replaced) {
            replaced->Cancel();
        }
    }

    void RemovePredicate(std::string_view name) override {
        std::shared_ptr<PredicateWatcher> to_remove;
        {
            const absl::MutexLock l(&mutex_);
            if (stopping_) {
                return;
            }
            auto node = predicate_watchers_.extract(name);
            if (!node.empty()) {
                to_remove = std::move(node.mapped());
            }
        }

        if (to_remove) {
            to_remove->Cancel();
        }
    }

    void Stop() override {
        std::vector<std::shared_ptr<LoopWatcher>> loop_watchers;
        absl::flat_hash_map<std::string, std::shared_ptr<PredicateWatcher>> predicate_watchers;
        {
            const absl::MutexLock l(&mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            loop_watchers = std::move(loop_watchers_);
            predicate_watchers = std::move(predicate_watchers_);
        }

        for (auto& lw : loop_watchers) {
            lw->CancelHangCheck();
        }
        for (auto& [name, pw] : predicate_watchers) {
            pw->Cancel();
        }

        DCHECK(worker_thread_.joinable());
        worker_thread_.join();
    }

  private:
    void WorkerThread() {
        auto await = [this] ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_) { return stopping_; };
        for (;;) {
            std::vector<std::shared_ptr<LoopWatcher>> watchers;
            std::vector<std::shared_ptr<PredicateWatcher>> predicates;
            {
                const absl::MutexLock l(&mutex_);
                if (mutex_.AwaitWithTimeout(absl::Condition(&await),
                                            timing_.hang_loop_iteration_timeout)) {
                    if (stopping_) {
                        break;
                    }
                }
                watchers = loop_watchers_;
                predicates.reserve(predicate_watchers_.size());
                for (const auto& [name, pw] : predicate_watchers_) {
                    predicates.push_back(pw);
                }
            }

            for (auto&& lw : watchers) {
                lw->Process(hang_callback_);
            }
            for (auto&& pw : predicates) {
                pw->Process(hang_callback_);
            }
        }
    }

    const HangCallback hang_callback_;
    const Timing timing_;
    absl::Clock& clock_;

    std::vector<std::shared_ptr<LoopWatcher>> loop_watchers_ ABSL_GUARDED_BY(mutex_);
    absl::flat_hash_map<std::string, std::shared_ptr<PredicateWatcher>> predicate_watchers_
            ABSL_GUARDED_BY(mutex_);

    absl::Mutex mutex_;
    bool stopping_ ABSL_GUARDED_BY(mutex_) = false;

    // A separate worker thread so it's not affected if anything hangs.
    std::thread worker_thread_;
};

}  // namespace

std::unique_ptr<HangDetector> HangDetector::Create(HangCallback hang_callback, Timing timing,
                                                   absl::Clock& clock) {
    return std::make_unique<HangDetectorImpl>(std::move(hang_callback), std::move(timing), clock);
}

}  // namespace android::crashreport
