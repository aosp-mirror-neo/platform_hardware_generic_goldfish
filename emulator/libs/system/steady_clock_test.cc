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
#include "goldfish/system/steady_clock.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "goldfish/system/testing/fake_clocks.h"

namespace goldfish::system {
namespace {

TEST(SteadyClockTest, RealClockIsPositiveAndNonDecreasing) {
    const SteadyClock& clock = SteadyClock::GetRealClock();
    const absl::Duration t0 = clock.Now();
    EXPECT_GT(t0, absl::ZeroDuration());

    absl::SleepFor(absl::Milliseconds(2));
    const absl::Duration t1 = clock.Now();
    EXPECT_GE(t1, t0);
}

TEST(FakeClocksTest, FakeSteadyClockSetAndAdvance) {
    testing::FakeSteadyClock clock;
    EXPECT_EQ(clock.Now(), absl::ZeroDuration());

    clock.Set(absl::Seconds(5));
    EXPECT_EQ(clock.Now(), absl::Seconds(5));

    clock.Advance(absl::Milliseconds(250));
    EXPECT_EQ(clock.Now(), absl::Milliseconds(5250));
}

TEST(FakeClocksTest, FakeVirtualClockSetAndAdvance) {
    testing::FakeVirtualClock clock(absl::Nanoseconds(100));
    EXPECT_EQ(clock.Now(), absl::Nanoseconds(100));

    clock.Advance(absl::Nanoseconds(50));
    EXPECT_EQ(clock.Now(), absl::Nanoseconds(150));

    clock.Set(absl::Seconds(1));
    EXPECT_EQ(clock.Now(), absl::Seconds(1));
}

TEST(FakeClocksTest, ConcurrentAdvanceAndReadIsThreadSafe) {
    testing::FakeSteadyClock steady_clock;
    testing::FakeVirtualClock virtual_clock;
    std::atomic<bool> start{false};

    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 1000; ++i) {
            steady_clock.Advance(absl::Nanoseconds(1));
            virtual_clock.Advance(absl::Nanoseconds(1));
        }
    });
    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        absl::Duration prev_steady = absl::ZeroDuration();
        absl::Duration prev_virtual = absl::ZeroDuration();
        for (int i = 0; i < 1000; ++i) {
            const absl::Duration cur_steady = steady_clock.Now();
            const absl::Duration cur_virtual = virtual_clock.Now();
            EXPECT_GE(cur_steady, prev_steady);
            EXPECT_GE(cur_virtual, prev_virtual);
            prev_steady = cur_steady;
            prev_virtual = cur_virtual;
        }
    });

    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    EXPECT_EQ(steady_clock.Now(), absl::Nanoseconds(1000));
    EXPECT_EQ(virtual_clock.Now(), absl::Nanoseconds(1000));
}

}  // namespace
}  // namespace goldfish::system
