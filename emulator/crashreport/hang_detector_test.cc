// Copyright 2018 The Android Open Source Project
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

#include <atomic>
#include <string>
#include <string_view>

#include "absl/base/thread_annotations.h"
#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "android/crashreport/debug.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"

namespace android::crashreport {

class HangDetectorTest : public ::testing::Test {
  public:
    HangDetectorTest() {
        mHangDetector = HangDetector::Create(
                [this](std::string_view msg) {
                    if (!mNotify.HasBeenNotified()) {
                        mHangMessage = std::string(msg);
                        mNotify.Notify();
                    }
                },
                {
                    .hang_loop_iteration_timeout = absl::Milliseconds(5),
                    .hang_check_timeout = absl::Milliseconds(1000),
                },
                mTestClock);
    }

    void TearDown() override {
        if (mHangDetector) {
            mHangDetector->Stop();
        }
    }

    bool wait_for_hang(absl::Duration timeout = absl::Milliseconds(50)) {
        return mNotify.WaitForNotificationWithTimeout(timeout);
    }

  protected:
    const HangDetector::ActivePredicate mAlwaysActive = [] { return true; };
    absl::SimulatedClock mTestClock;
    std::string mHangMessage;
    absl::Notification mNotify;
    std::unique_ptr<HangDetector> mHangDetector;
};

TEST_F(HangDetectorTest, NormalLoopNoHang) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1), mAlwaysActive);

    EXPECT_FALSE(wait_for_hang(absl::Milliseconds(20)));

    mHangDetector->RemoveWatchedLooper(*event_loop);
}

TEST_F(HangDetectorTest, HangDetectorDestroyedFirst) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1), mAlwaysActive);

    EXPECT_FALSE(wait_for_hang(absl::Milliseconds(20)));

    mHangDetector->Stop();
    mHangDetector.reset();
}

TEST_F(HangDetectorTest, BlockedLoopTriggersHang) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1), mAlwaysActive);

    // Add a hanging task
    absl::Notification hang;
    event_loop->Post([&hang] { hang.WaitForNotification(); }).IgnoreError();

    // 1st advance: initial looper check (reschedules with task running)
    auto end_time = absl::Now() + absl::Seconds(5);
    while (!mNotify.HasBeenNotified() && absl::Now() < end_time) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(10));
    }
    ASSERT_TRUE(wait_for_hang(absl::Seconds(10)));

    // Unblock the loop so that it actually terminates!
    hang.Notify();

    mHangDetector->RemoveWatchedLooper(*event_loop);

    // Wait for loop to shutdown as the hang task is referencing the hang notification which gets
    // destroyed before the loop.
    event_loop->ShutdownAndWait().IgnoreError();
}

TEST_F(HangDetectorTest, BlockedLoopIgnoredWhenVmStopped) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    // Register looper with is_vm_running predicate returning false (VM stopped)
    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1),
                                    []() { return false; });

    // Add a hanging task
    absl::Notification hang;
    event_loop->Post([&hang] { hang.WaitForNotification(); }).IgnoreError();

    // Advance clock past the hang timeout repeatedly
    for (int i = 0; i < 20; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(10));
    }

    // Verify hang callback was NOT invoked because the VM was stopped
    EXPECT_FALSE(mNotify.HasBeenNotified());

    // Unblock and clean up
    hang.Notify();
    mHangDetector->RemoveWatchedLooper(*event_loop);
    event_loop->ShutdownAndWait().IgnoreError();
}

TEST_F(HangDetectorTest, HangCheckSuspendedAndResetAcrossSaveVmTransition) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    std::atomic<bool> is_active{true};
    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1),
                                    [&is_active]() { return is_active.load(); });

    // Simulate VM entering SAVE_VM while event loop stops processing virtual timers
    is_active = false;
    absl::Notification hang;
    event_loop->Post([&hang] { hang.WaitForNotification(); }).IgnoreError();

    // Advance clock by 10 seconds during SAVE_VM (> 2x 1s timeout)
    for (int i = 0; i < 20; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(500));
        absl::SleepFor(absl::Milliseconds(5));
    }
    EXPECT_FALSE(mNotify.HasBeenNotified());

    // Simulate save_snapshot() calling vm_resume(RUN_STATE_RUNNING) briefly before exit teardown
    is_active = true;

    // Advance clock by 500ms (< 1s timeout). Because the inactive period reset is_task_running_ =
    // false and hang_count_ = 0, resuming starts a fresh check and must NOT fire a false-positive
    // crash!
    for (int i = 0; i < 5; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(5));
    }
    EXPECT_FALSE(mNotify.HasBeenNotified());

    hang.Notify();
    mHangDetector->RemoveWatchedLooper(*event_loop);
    event_loop->ShutdownAndWait().IgnoreError();
}

TEST_F(HangDetectorTest, ResumedVmDetectsSubsequentHang) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    std::atomic<bool> is_active{false};
    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1),
                                    [&is_active]() { return is_active.load(); });

    absl::Notification hang;
    event_loop->Post([&hang] { hang.WaitForNotification(); }).IgnoreError();

    // Resume VM while loop is blocked
    is_active = true;

    // Advance clock until hang is detected
    auto end_time = absl::Now() + absl::Seconds(5);
    while (!mNotify.HasBeenNotified() && absl::Now() < end_time) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(10));
    }
    ASSERT_TRUE(wait_for_hang(absl::Seconds(10)));

    hang.Notify();
    mHangDetector->RemoveWatchedLooper(*event_loop);
    event_loop->ShutdownAndWait().IgnoreError();
}

TEST_F(HangDetectorTest, NoHangCallbackDeadlockWhenRemovingLooper) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "This test cannot be run under a debugger";
    }

    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    absl::Notification hang_cb_called;
    absl::Notification remove_completed;

    // Start a teardown thread upfront that waits for the hang callback to fire,
    // simulating concurrent looper removal upon hang detection.
    std::thread remove_thread([&]() {
        hang_cb_called.WaitForNotification();
        event_loop->ShutdownAndWait().IgnoreError();
        remove_completed.Notify();
    });

    absl::SimulatedClock test_clock;

    // Create a HangDetector where the hang callback triggers asynchronous looper removal.
    auto hang_detector = HangDetector::Create(
            [&](std::string_view msg) {
                if (!hang_cb_called.HasBeenNotified()) {
                    hang_cb_called.Notify();
                }
            },
            {
                .hang_loop_iteration_timeout = absl::Milliseconds(5),
                .hang_check_timeout = absl::Milliseconds(100),
            },
            test_clock);

    hang_detector->AddWatchedLooper("test loop", *event_loop, absl::Milliseconds(100),
                                    mAlwaysActive);

    // Block the event loop to trigger hang detection
    absl::Notification hang;
    event_loop->Post([&hang] { hang.WaitForNotification(); }).IgnoreError();

    // 1st advance: initial check
    auto end_time = absl::Now() + absl::Seconds(5);
    while (!hang_cb_called.HasBeenNotified() && absl::Now() < end_time) {
        test_clock.AdvanceTime(absl::Milliseconds(20));
        absl::SleepFor(absl::Milliseconds(5));
    }
    ASSERT_TRUE(hang_cb_called.WaitForNotificationWithTimeout(absl::Seconds(10)));

    hang.Notify();
    ASSERT_TRUE(remove_completed.WaitForNotificationWithTimeout(absl::Seconds(10)));
    if (remove_thread.joinable()) {
        remove_thread.join();
    }
    hang_detector->Stop();
}

TEST_F(HangDetectorTest, RemoveWatchedLooperAfterStopNoCrash) {
    auto event_loop =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1), mAlwaysActive);

    // Stop the detector first (simulating early teardown)
    mHangDetector->Stop();

    // Verify calling RemoveWatchedLooper after Stop() is safe and does not crash or assert
    EXPECT_NO_FATAL_FAILURE(mHangDetector->RemoveWatchedLooper(*event_loop));

    event_loop->ShutdownAndWait().IgnoreError();
}

TEST_F(HangDetectorTest, PredicateOfRemovedLooperIsNotCalledAgain) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "Predicates are not evaluated under a debugger";
    }

    auto loop_a =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());
    auto loop_b =
            goldfish::async::ThreadedEventLoop::Create(goldfish::async::LibuvEventLoop::Create());

    // Holds the worker inside the predicate of `loop_a` while `loop_b` is removed, so the
    // worker's copy of the watcher list still contains the removed watcher.
    std::atomic<bool> armed{false};
    std::atomic<int> armed_calls{0};
    absl::Notification worker_blocked;
    absl::Notification release_worker;
    absl::Notification next_iteration;
    mHangDetector->AddWatchedLooper("a", *loop_a, absl::Seconds(1), [&]() {
        if (!armed.load()) {
            return true;
        }
        // The first armed call may come from an iteration that started before `loop_b` was
        // added; the second one comes from an iteration that includes it.
        const int call = ++armed_calls;
        if (call == 2) {
            worker_blocked.Notify();
            release_worker.WaitForNotification();
        } else if (call == 3) {
            next_iteration.Notify();
        }
        return true;
    });
    std::atomic<int> removed_predicate_calls{0};
    mHangDetector->AddWatchedLooper("b", *loop_b, absl::Seconds(1), [&]() {
        ++removed_predicate_calls;
        return true;
    });
    const absl::Cleanup cleanup = [&] {
        armed = false;
        if (!release_worker.HasBeenNotified()) {
            release_worker.Notify();
        }
        mHangDetector->RemoveWatchedLooper(*loop_a);
        mHangDetector->RemoveWatchedLooper(*loop_b);
    };

    armed = true;
    ASSERT_TRUE(worker_blocked.WaitForNotificationWithTimeout(absl::Seconds(10)));
    mHangDetector->RemoveWatchedLooper(*loop_b);
    const int calls_after_removal = removed_predicate_calls.load();

    release_worker.Notify();
    ASSERT_TRUE(next_iteration.WaitForNotificationWithTimeout(absl::Seconds(10)));

    EXPECT_EQ(removed_predicate_calls.load(), calls_after_removal);
}

TEST_F(HangDetectorTest, PausedEventLoopIsIgnored) {
    auto event_loop = goldfish::async::testing::TestEventLoop::Create();
    event_loop->Pause();

    mHangDetector->AddWatchedLooper("paused test loop", *event_loop, absl::Seconds(1),
                                    mAlwaysActive);

    for (int i = 0; i < 20; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(10));
    }

    EXPECT_FALSE(mNotify.HasBeenNotified());

    mHangDetector->RemoveWatchedLooper(*event_loop);
}

TEST_F(HangDetectorTest, PauseAndResumeEventLoopResetsInFlightCheckAndDetectsSubsequentHang) {
    auto event_loop = goldfish::async::testing::TestEventLoop::Create();

    // Register while running so an initial hang check task is scheduled at t = 0.
    mHangDetector->AddWatchedLooper("test loop", *event_loop, absl::Seconds(1), mAlwaysActive);

    // Transition the loop to kPaused before the timer completes.
    event_loop->Pause();

    // Advance clock by 10s (> 2x 1s timeout) while paused.
    for (int i = 0; i < 20; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(500));
        absl::SleepFor(absl::Milliseconds(5));
    }
    EXPECT_FALSE(mNotify.HasBeenNotified());

    // Resume the loop and advance by 500ms (< 1s timeout). Because pausing reset the in-flight
    // check, resuming must start a fresh check rather than firing a false-positive hang.
    event_loop->Resume();
    for (int i = 0; i < 5; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(5));
    }
    EXPECT_FALSE(mNotify.HasBeenNotified());

    // Continue advancing the clock while the resumed loop does not advance its timers.
    auto end_time = absl::Now() + absl::Seconds(5);
    while (!mNotify.HasBeenNotified() && absl::Now() < end_time) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(10));
    }
    ASSERT_TRUE(wait_for_hang(absl::Seconds(10)));

    mHangDetector->RemoveWatchedLooper(*event_loop);
}

TEST_F(HangDetectorTest, NotStartedEventLoopIsIgnored) {
    auto event_loop = goldfish::async::LibuvEventLoop::Create();
    ASSERT_EQ(event_loop->GetState(), goldfish::async::LooperStatusEvent::State::kNotStarted);

    mHangDetector->AddWatchedLooper("unstarted loop", *event_loop, absl::Seconds(1), mAlwaysActive);

    for (int i = 0; i < 20; ++i) {
        mTestClock.AdvanceTime(absl::Milliseconds(100));
        absl::SleepFor(absl::Milliseconds(10));
    }

    EXPECT_FALSE(mNotify.HasBeenNotified());

    mHangDetector->RemoveWatchedLooper(*event_loop);
}

namespace {

class PredicateStepSynchronizer {
  public:
    int RecordEvaluation() {
        const absl::MutexLock l(&mutex_);
        return ++evaluations_;
    }

    bool AdvanceAndWaitForEvaluation(absl::SimulatedClock* clock, int target_evaluations,
                                     absl::Duration step = absl::Seconds(1),
                                     absl::Duration timeout = absl::Seconds(10)) {
        clock->AdvanceTime(step);
        const absl::MutexLock l(&mutex_);
        auto reached = [this, target_evaluations]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
            return evaluations_ >= target_evaluations;
        };
        return mutex_.AwaitWithTimeout(absl::Condition(&reached), timeout);
    }

    int Evaluations() const {
        const absl::MutexLock l(&mutex_);
        return evaluations_;
    }

  private:
    mutable absl::Mutex mutex_;
    int evaluations_ ABSL_GUARDED_BY(mutex_) = 0;
};

}  // namespace

TEST_F(HangDetectorTest, HealthyPredicateNeverFiresHangCallback) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "Predicates are not evaluated under a debugger";
    }

    PredicateStepSynchronizer sync;
    mHangDetector->AddPredicate("healthy_check", [&sync]() {
        sync.RecordEvaluation();
        return absl::OkStatus();
    });

    for (int step = 1; step <= 3; ++step) {
        ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, step));
    }
    EXPECT_EQ(sync.Evaluations(), 3);
    EXPECT_FALSE(mNotify.HasBeenNotified());

    mHangDetector->RemovePredicate("healthy_check");
}

TEST_F(HangDetectorTest, SinglePredicateFailureDoesNotTriggerHangCallback) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "Predicates are not evaluated under a debugger";
    }

    PredicateStepSynchronizer sync;
    mHangDetector->AddPredicate("single_strike", [&sync]() {
        const int eval = sync.RecordEvaluation();
        if (eval == 1) {
            return absl::DeadlineExceededError("transient stall on strike 1");
        }
        return absl::OkStatus();
    });

    // 1st check fails (strike 1 of 2), which must not fire the hang callback.
    ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, 1));
    // 2nd check succeeds, confirming the worker finished processing strike 1 without firing.
    ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, 2));
    EXPECT_FALSE(mNotify.HasBeenNotified());

    mHangDetector->RemovePredicate("single_strike");
}

TEST_F(HangDetectorTest, TwoConsecutivePredicateFailuresTriggerHangCallbackWithDetail) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "Predicates are not evaluated under a debugger";
    }

    constexpr std::string_view kPredicateName = "snapshot_progress_日本語_🚀";
    constexpr std::string_view kErrorDetail =
            "Snapshot save '안드로이드_スナップショット' stalled at 45% (transferred_bytes=1024)";

    PredicateStepSynchronizer sync;
    mHangDetector->AddPredicate(std::string(kPredicateName), [&]() {
        const int eval = sync.RecordEvaluation();
        if (eval == 2) {
            EXPECT_FALSE(mNotify.HasBeenNotified());
        }
        return absl::DeadlineExceededError(kErrorDetail);
    });

    // Strike 1
    ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, 1));
    EXPECT_FALSE(mNotify.HasBeenNotified());

    // Strike 2 triggers hang_callback with the predicate name and status message.
    mTestClock.AdvanceTime(absl::Seconds(1));
    ASSERT_TRUE(wait_for_hang(absl::Seconds(10)));
    EXPECT_THAT(mHangMessage, ::testing::HasSubstr(kPredicateName));
    EXPECT_THAT(mHangMessage, ::testing::HasSubstr(kErrorDetail));

    mHangDetector->RemovePredicate(kPredicateName);
}

TEST_F(HangDetectorTest, PredicateRecoveryResetsStrikeCount) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "Predicates are not evaluated under a debugger";
    }

    PredicateStepSynchronizer sync;
    mHangDetector->AddPredicate("recoverable_check", [&]() {
        const int eval = sync.RecordEvaluation();
        if (eval == 1) {
            return absl::DeadlineExceededError("first stall (strike 1)");
        }
        if (eval == 2) {
            // Recovery resets strike count back to 0.
            return absl::OkStatus();
        }
        if (eval == 3) {
            // A subsequent single failure is strike 1 again, not strike 2.
            return absl::DeadlineExceededError("second stall after recovery (strike 1)");
        }
        EXPECT_FALSE(mNotify.HasBeenNotified());
        return absl::DeadlineExceededError("second consecutive stall after recovery (strike 2)");
    });

    ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, 1));
    ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, 2));
    ASSERT_TRUE(sync.AdvanceAndWaitForEvaluation(&mTestClock, 3));
    EXPECT_FALSE(mNotify.HasBeenNotified());

    // Now the second consecutive failure (evaluation 4) triggers the hang callback.
    mTestClock.AdvanceTime(absl::Seconds(1));
    ASSERT_TRUE(wait_for_hang(absl::Seconds(10)));
    EXPECT_THAT(mHangMessage,
                ::testing::HasSubstr("second consecutive stall after recovery (strike 2)"));

    mHangDetector->RemovePredicate("recoverable_check");
}

TEST_F(HangDetectorTest, RemovePredicatePreventsFurtherChecksAndUnregistersCleanly) {
    if (android::base::IsDebuggerAttached()) {
        GTEST_SKIP() << "Predicates are not evaluated under a debugger";
    }

    PredicateStepSynchronizer removed_sync;
    mHangDetector->AddPredicate("removable_check", [&removed_sync]() {
        removed_sync.RecordEvaluation();
        return absl::DeadlineExceededError("stalled before removal");
    });

    // Evaluate once (strike 1), then remove the predicate.
    ASSERT_TRUE(removed_sync.AdvanceAndWaitForEvaluation(&mTestClock, 1));
    mHangDetector->RemovePredicate("removable_check");

    // Register a probe predicate and advance the clock to prove the worker thread ran a full check
    // cycle without evaluating the removed predicate.
    PredicateStepSynchronizer probe_sync;
    mHangDetector->AddPredicate("probe_check", [&probe_sync]() {
        probe_sync.RecordEvaluation();
        return absl::OkStatus();
    });
    ASSERT_TRUE(probe_sync.AdvanceAndWaitForEvaluation(&mTestClock, 1));

    EXPECT_EQ(removed_sync.Evaluations(), 1);
    EXPECT_FALSE(mNotify.HasBeenNotified());

    mHangDetector->RemovePredicate("probe_check");
    mHangDetector->Stop();
    EXPECT_NO_FATAL_FAILURE(mHangDetector->RemovePredicate("probe_check"));
}

}  // namespace android::crashreport
