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

#include <atomic>
#include <chrono>
#include <future>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"

using namespace goldfish::async;
using namespace goldfish::async::testing;
using namespace std::chrono_literals;

TEST(TestEventLoop, PostSingleTask) {
    auto loop = TestEventLoop::Create();
    bool executed = false;
    loop->Post([&]() { executed = true; }).IgnoreError();
    ASSERT_FALSE(executed);
    loop->RunAll();
    ASSERT_TRUE(executed);
}

TEST(TestEventLoop, PostMultipleTasks) {
    auto loop = TestEventLoop::Create();
    int count = 0;
    loop->Post([&]() { count++; }).IgnoreError();
    loop->Post([&]() { count++; }).IgnoreError();
    loop->Post([&]() { count++; }).IgnoreError();
    ASSERT_EQ(0, count);
    loop->RunAll();
    ASSERT_EQ(3, count);
}

TEST(TestEventLoop, ScheduleDelayedTask) {
    auto loop = TestEventLoop::Create();
    bool executed = false;

    // Store the returned handle in a variable to keep it alive.
    auto timer = loop->ScheduleRepeating(
            [&]() {
                executed = true;
                return false;
            },
            absl::Milliseconds(100), absl::ZeroDuration());

    // only advance clock does something with timed task
    loop->RunAll();
    ASSERT_FALSE(executed);
    loop->AdvanceClock(absl::Milliseconds(99));
    ASSERT_FALSE(executed);
    loop->AdvanceClock(absl::Milliseconds(1));
    ASSERT_TRUE(executed);
}

TEST(TestEventLoop, ScheduleRepeatingTask) {
    auto loop = TestEventLoop::Create();
    int count = 0;
    auto timer = loop->ScheduleRepeating(
            [&]() {
                count++;
                return true;
            },
            absl::Milliseconds(100), absl::Milliseconds(50));

    loop->AdvanceClock(absl::Milliseconds(100));
    ASSERT_EQ(1, count);

    loop->AdvanceClock(absl::Milliseconds(50));
    ASSERT_EQ(2, count);

    loop->AdvanceClock(absl::Milliseconds(50));
    ASSERT_EQ(3, count);

    // no new events after cancel.
    timer.reset();
    loop->AdvanceClock(absl::Milliseconds(50));
    ASSERT_EQ(3, count);
}

TEST(TestEventLoop, StopFromCallback) {
    auto loop = TestEventLoop::Create();
    int count = 0;

    auto timer = loop->ScheduleRepeating(
            [&]() {
                ++count;
                return count < 3;
            },
            absl::Milliseconds(100), absl::Milliseconds(50));

    loop->AdvanceClock(absl::Milliseconds(100));
    ASSERT_EQ(1, count);

    loop->AdvanceClock(absl::Milliseconds(50));
    ASSERT_EQ(2, count);

    loop->AdvanceClock(absl::Milliseconds(50));
    ASSERT_EQ(3, count);

    loop->AdvanceClock(absl::Milliseconds(50));
    EXPECT_EQ(3, count);
    loop->AdvanceClock(absl::Milliseconds(50));
    EXPECT_EQ(3, count);
    loop->AdvanceClock(absl::Milliseconds(50));
    EXPECT_EQ(3, count);
}

TEST(TestEventLoop, PostAndWait) {
    auto loop = TestEventLoop::Create();
    auto future = std::async(std::launch::async,
                             [&]() { return loop->PostAndWait([]() { return 42; }); });

    while (future.wait_for(100ms) == std::future_status::timeout) {
        loop->RunAll();
    }
    auto s = future.get();
    ASSERT_THAT(s, absl_testing::IsOk());
    EXPECT_EQ(*s, 42);
}

TEST(TestEventLoop, PostAndWaitVoid) {
    auto loop = TestEventLoop::Create();
    bool executed = false;
    auto future = std::async(std::launch::async, [&]() {
        loop->PostAndWait([&]() { executed = true; }).IgnoreError();
    });
    while (future.wait_for(100ms) == std::future_status::timeout) {
        loop->RunAll();
    }
    future.get();
    ASSERT_TRUE(executed);
}

TEST(TestEventLoop, TimerCancellation) {
    auto loop = TestEventLoop::Create();
    bool executed = false;
    {
        auto timer = loop->ScheduleRepeating(
                [&]() {
                    executed = true;
                    return false;
                },
                absl::Milliseconds(100), absl::ZeroDuration());
    }
    loop->AdvanceClock(absl::Milliseconds(100));
    ASSERT_FALSE(executed);
}

TEST(TestEventLoop, TimerHandleDestructionCancels) {
    auto loop = TestEventLoop::Create();
    bool executed = false;
    {
        auto timer = loop->ScheduleRepeating(
                [&]() {
                    executed = true;
                    return false;
                },
                absl::Milliseconds(100), absl::ZeroDuration());
    }
    // Timer is out of scope and should be cancelled.
    loop->AdvanceClock(absl::Milliseconds(100));
    ASSERT_FALSE(executed);
}

TEST(TestEventLoop, ShutdownClearsPendingTasks) {
    auto loop = TestEventLoop::Create();
    bool executed = false;
    loop->Post([&]() { executed = true; }).IgnoreError();
    loop->Post([&]() { executed = true; }, absl::Milliseconds(100)).IgnoreError();

    loop->ShutdownAndWait().IgnoreError();
    loop->RunAll();
    loop->AdvanceClock(absl::Milliseconds(100));

    ASSERT_FALSE(executed);
}

// This test verifies that a repeating task can be executed multiple times
// without crashing. It directly targets the use-after-move bug in the
// faulty implementation.
TEST(TestEventLoop, RecurringTaskDoesNotCrashOnSubsequentExecutions) {
    // ARRANGE: Create an event loop and a counter.
    auto loop = TestEventLoop::Create();
    std::atomic<int> execution_count = 0;

    // Schedule a task to run every 10ms, starting immediately.
    auto timer = loop->ScheduleRepeating(
            [&execution_count]() {
                execution_count++;
                return true;
            },
            absl::ZeroDuration(),   // Initial delay of 0 means it's due immediately.
            absl::Milliseconds(10)  // Repeat every 10ms.
    );

    // ACT & ASSERT (First Execution)
    // Advance the clock by 1ms. This should cause the task scheduled
    // at t=0 to run.
    loop->AdvanceClock(absl::Milliseconds(1));

    // With a correct implementation, the count is 1.
    // The faulty implementation would crash inside this AdvanceClock call.
    ASSERT_EQ(execution_count, 1);

    // ACT & ASSERT (Second Execution)
    // Advance the clock by another 10ms. This should cause the re-scheduled
    // task at t=10ms to run.
    loop->AdvanceClock(absl::Milliseconds(10));
    ASSERT_EQ(execution_count, 2);

    // ACT & ASSERT (Third Execution)
    // Advancing again proves the task continues to be rescheduled correctly.
    loop->AdvanceClock(absl::Milliseconds(10));
    ASSERT_EQ(execution_count, 3);
}

TEST(TestEventLoop, RunOne) {
    auto loop = TestEventLoop::Create();
    int count = 0;
    loop->Post([&]() { count++; }).IgnoreError();
    loop->Post([&]() { count++; }).IgnoreError();
    ASSERT_TRUE(loop->RunOne());
    ASSERT_EQ(1, count);
    ASSERT_TRUE(loop->RunOne());
    ASSERT_EQ(2, count);
}

TEST(TestEventLoop, RunMany) {
    auto loop = TestEventLoop::Create();
    int count = 0;
    loop->Post([&]() { count++; }).IgnoreError();
    loop->Post([&]() { count++; }).IgnoreError();
    loop->Post([&]() { count++; }).IgnoreError();
    ASSERT_TRUE(loop->RunMany(3));
    ASSERT_EQ(3, count);
}

TEST(TestEventLoop, RunManyTimeout) {
    auto loop = TestEventLoop::Create();
    int count = 0;
    loop->Post([&]() { count++; }).IgnoreError();
    ASSERT_EQ(loop->RunMany(2), 1);
    ASSERT_EQ(1, count);
}

TEST(TestEventLoop, RescheduleRepeatingTimer) {
    auto loop = TestEventLoop::Create();
    int counter = 0;

    auto handle = loop->ScheduleRepeating(
            [&]() {
                counter++;
                return true;
            },
            absl::Milliseconds(100), absl::Milliseconds(100));

    // Let it fire once.
    loop->AdvanceClock(absl::Milliseconds(120));
    ASSERT_EQ(counter, 1);

    // Reschedule to fire sooner and more frequently.
    handle->Schedule(absl::Milliseconds(20), absl::Milliseconds(20));

    // Check that it fires again quickly.
    loop->AdvanceClock(absl::Milliseconds(30));
    ASSERT_EQ(counter, 2);

    // Check that it fires again quickly.
    loop->AdvanceClock(absl::Milliseconds(15));
    ASSERT_EQ(counter, 3);
}

TEST(TestEventLoop, PauseAndResumeEmitsStateChanges) {
    auto loop = TestEventLoop::Create();
    ASSERT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);

    std::vector<LooperStatusEvent::State> states;
    auto subscription = android::base::eventing::MakeScopedCallback(
            *loop, [&](const LooperStatusEvent& event) { states.push_back(event.state); });

    loop->Pause();
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kPaused);
    EXPECT_EQ(absl::StrCat(LooperStatusEvent{.state = loop->GetState()}), "PAUSED");

    loop->Resume();
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);

    ASSERT_EQ(states.size(), 2u);
    EXPECT_EQ(states[0], LooperStatusEvent::State::kPaused);
    EXPECT_EQ(states[1], LooperStatusEvent::State::kRunning);
}

TEST(TestEventLoop, PostWithDelayRunsOnceTheClockAdvances) {
    auto loop = TestEventLoop::Create();
    std::atomic<bool> ran = false;
    ASSERT_THAT(loop->Post([&ran] { ran = true; }, absl::Seconds(1)), absl_testing::IsOk());

    loop->AdvanceClock(absl::Seconds(2));
    loop->RunAll();

    EXPECT_TRUE(ran) << "Post(fn, 1s) never ran, even after the clock advanced by 2s";
}
