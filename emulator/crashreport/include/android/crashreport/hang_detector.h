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
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"

#include "goldfish/async/event_loop.h"

namespace android::crashreport {

// Use this interface if your hangdetector needs to
// keep track of state.
class StatefulHangdetector {
  public:
    virtual ~StatefulHangdetector() = default;
    virtual bool Check() = 0;
};

/**
 * HangDetector - a class that monitors a set of Loopers and checks if any of
 * those is hanging. It calls a user-supplied callback in that case.
 *
 * HangDetector uses EventLoop::createTimer() to get a timer object for each event
 * loop it watches. Separate thread wakes every couple of seconds to check if
 * it needs to schedule a new task on a looper, or, if a task was scheduled for
 * a while and didn't finish in time, to call the |hangCallback|.
 *
 * Note: Be careful with the timing.hang_loop_iteration_timeout. Setting it too
 * aggressively can prevent the hang detector from functioning properly.
 */
class HangDetector {
  public:
    struct Timing {
        // Timeout between worker thread's loop iterations.
        const absl::Duration hang_loop_iteration_timeout;
        // Timeout between hang checks.
        const absl::Duration hang_check_timeout;
    };

    static constexpr Timing DefaultTiming() {
        return {.hang_loop_iteration_timeout = absl::Seconds(5),
                .hang_check_timeout = absl::Seconds(15)};
    }

    using HangCallback = std::function<void(std::string_view message)>;
    using ActivePredicate = std::function<bool()>;
    // Returns absl::OkStatus() if healthy, or an error status if hanging.
    using HangPredicate = std::function<absl::Status()>;

    HangDetector() = default;
    virtual ~HangDetector() = default;
    HangDetector(const HangDetector&) = delete;
    HangDetector& operator=(const HangDetector&) = delete;
    HangDetector(HangDetector&&) = delete;
    HangDetector& operator=(HangDetector&&) = delete;

    virtual void AddWatchedLooper(std::string loop_name, ::goldfish::async::EventLoop& event_loop,
                                  absl::Duration task_timeout, ActivePredicate is_active) = 0;

    virtual void RemoveWatchedLooper(::goldfish::async::EventLoop& event_loop) = 0;

    virtual void AddPredicate(std::string name, HangPredicate check) = 0;

    virtual void RemovePredicate(std::string_view name) = 0;

    virtual void Stop() = 0;

    static std::unique_ptr<HangDetector> Create(
            HangCallback hang_callback, Timing timing,
            absl::Clock& clock = absl::Clock::GetRealClock());
};

}  // namespace android::crashreport
