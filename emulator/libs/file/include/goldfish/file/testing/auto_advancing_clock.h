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

#include <functional>
#include <utility>

#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"

namespace android::base::file::testing {

// Test `absl::Clock` backed by `absl::SimulatedClock` whose `Sleep()` advances
// simulated time inline (instead of blocking for another thread) and invokes an
// optional per-sleep callback for single-threaded retry tests.
class AutoAdvancingClock : public absl::SimulatedClock {
  public:
    explicit AutoAdvancingClock(absl::Time start = absl::UnixEpoch(),
                                std::function<void(absl::Duration)> on_sleep = {})
            : absl::SimulatedClock(start), on_sleep_(std::move(on_sleep)) {}

    void Sleep(absl::Duration d) override {
        if (d > absl::ZeroDuration()) {
            AdvanceTime(d);
            total_slept_ += d;
        }
        ++sleep_calls_;
        if (on_sleep_) {
            on_sleep_(d);
        }
    }

    void SleepUntil(absl::Time wakeup_time) override {
        const absl::Duration d = wakeup_time - TimeNow();
        Sleep(d > absl::ZeroDuration() ? d : absl::ZeroDuration());
    }

    absl::Duration TotalSlept() const { return total_slept_; }
    int SleepCalls() const { return sleep_calls_; }

  private:
    std::function<void(absl::Duration)> on_sleep_;
    absl::Duration total_slept_ = absl::ZeroDuration();
    int sleep_calls_ = 0;
};

}  // namespace android::base::file::testing
