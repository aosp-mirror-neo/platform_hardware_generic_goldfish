/* Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>

#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/base/cpu_time.h"
#include "android/base/system.h"
#include "goldfish/async/event_loop.h"

namespace goldfish::metrics {

class CpuUsage final {
  public:
    CpuUsage(::goldfish::async::EventLoop* main_event_loop,
             const std::vector<::goldfish::async::EventLoop*>& vcpu_event_loops)
            : main_loop_(main_event_loop)
            , vcpu_loops_(vcpu_event_loops.begin(), vcpu_event_loops.end()) {}
    ~CpuUsage() = default;
    CpuUsage(const CpuUsage&) = delete;
    CpuUsage& operator=(const CpuUsage&) = delete;
    CpuUsage(CpuUsage&&) = delete;
    CpuUsage& operator=(CpuUsage&&) = delete;

    void ScheduleUpdateNow() {
        main_loop_.ScheduleNow();
        for (auto& vcpu : vcpu_loops_) {
            vcpu.ScheduleNow();
        }
    }

    using CpuTimeReader = std::function<void(android::base::CpuTime cputime)>;

    void UseMainCpuUsage(CpuTimeReader f) const { f(main_loop_.GetLastDiff()); }

    void ForEachVCpuUsage(CpuTimeReader f) const {
        for (const auto& vcpu_loop : vcpu_loops_) {
            f(vcpu_loop.GetLastDiff());
        }
    }

  private:
    class LoopMeasurement {
      public:
        LoopMeasurement(goldfish::async::EventLoop* loop)
                : loop_(*loop), task_(loop_.CreateTimer([this] {
                    absl::MutexLock lock(mutex_);
                    auto old = last_measurement_;
                    last_measurement_ = android::base::System::Get()->GetCpuTime();
                    last_diff_ = last_measurement_ - old;
                    return true;
                })) {}

        android::base::CpuTime GetLastDiff() const {
            absl::MutexLock lock(mutex_);
            return last_diff_;
        }

        void ScheduleNow() { task_->Schedule(/*delay=*/absl::ZeroDuration()); }

      private:
        ::goldfish::async::EventLoop& loop_;
        ::goldfish::async::EventLoop::ScopedTimer task_;

        mutable absl::Mutex mutex_;
        android::base::CpuTime last_measurement_ ABSL_GUARDED_BY(mutex_) = {};
        android::base::CpuTime last_diff_ ABSL_GUARDED_BY(mutex_) = {};
    };

    LoopMeasurement main_loop_;
    std::vector<LoopMeasurement> vcpu_loops_;

    ::goldfish::async::EventLoop::ScopedTimer timer_;
};

}  // namespace goldfish::metrics
