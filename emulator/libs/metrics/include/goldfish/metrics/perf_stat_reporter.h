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

#include <chrono>
#include <fstream>

#include "android/goldfish/hardware_config.h"
#include "goldfish/file/file.h"
#include "goldfish/metrics/cpu_usage.h"
#include "goldfish/metrics/text_metrics_writer.h"
#include "studio_stats.pb.h"

namespace goldfish::metrics {

void FillMemUsage(android_studio::EmulatorMemoryUsage* mem_usage_proto,
                  const android::goldfish::HardwareConfig* hw = nullptr);

class PerfStatReporter {
  public:
    PerfStatReporter(goldfish::async::EventLoop* main_loop,
                     const std::vector<goldfish::async::EventLoop*>& vcpu_loops,
                     const android::goldfish::HardwareConfig& hw, fs::path dump_file_path)
            : main_loop_(*main_loop), cpu_usage_(main_loop, vcpu_loops), hw_(hw) {
        if (!dump_file_path.empty()) {
            dump_file_ = std::make_unique<::goldfish::metrics::StreamOwnerTextMetricsWriter>(
                    std::make_unique<std::ofstream>(dump_file_path));
        }

        using namespace std::chrono_literals;
        updater_ = main_loop_.ScheduleRepeating(
                [this] {
                    if (dump_file_) {
                        MetricsEvent event;
                        event.time_ms = android::base::System::Get()->GetUnixTimeUs() / 1000;
                        FillEvent(event.as_event);
                        dump_file_->Write(std::move(event));
                    }

                    cpu_usage_.ScheduleUpdateNow();
                    return true;
                },
                /*initial_delay=*/absl::Seconds(5), /*interval=*/absl::Seconds(5));

        cpu_usage_.ScheduleUpdateNow();
    }

    void FillEvent(android_studio::AndroidStudioEvent& event) {
        FillPerfStats(hw_, cpu_usage_, event.mutable_emulator_performance_stats());
    }

  private:
    static void FillPerfStats(const android::goldfish::HardwareConfig& hw,
                              const CpuUsage& cpu_usage,
                              android_studio::EmulatorPerformanceStats* stats_out);

    goldfish::async::EventLoop& main_loop_;
    CpuUsage cpu_usage_;
    const android::goldfish::HardwareConfig& hw_;
    std::unique_ptr<goldfish::metrics::TextMetricsWriter> dump_file_;
    ::goldfish::async::EventLoop::ScopedTimer updater_;
};

}  // namespace goldfish::metrics
