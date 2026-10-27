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

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/file/file.h"
#include "goldfish/metrics/metrics_writer.h"

namespace goldfish::metrics {

class StudioFileMetricsWriter : public MetricsWriter {
  public:
    StudioFileMetricsWriter(const fs::path& spool_dir, const std::string& session_id,
                            goldfish::async::EventLoop& main_loop);
    ~StudioFileMetricsWriter() override;

    void Write(MetricsEvent event) override;

    // Finalizes any abandoned .open files in `spool_dir` and returns the unique session IDs
    // that had at least one abandoned file.
    static std::vector<std::string> FinalizeAbandonedSessionFiles(const fs::path& spool_dir);

    // Finalizes all .open files in `spool_dir` written by `(session_id, pid)` after
    // that child process has exited, without touching other processes' .open files.
    static void FinalizeProcessFiles(const fs::path& spool_dir, std::string_view session_id,
                                     uint32_t pid);

  private:
    void OpenNextFile();
    void FinalizeCurrentFile();

    const fs::path spool_dir_;
    const std::string session_id_;
    const int pid_;
    int file_counter_{0};

    std::shared_ptr<goldfish::async::EventLoop::Timer> max_file_duration_timer_;

    absl::Mutex mutex_;
    fs::path open_file_path_ ABSL_GUARDED_BY(mutex_);
    std::ofstream current_file_ ABSL_GUARDED_BY(mutex_);
    int current_file_record_count_ ABSL_GUARDED_BY(mutex_) = 0;
    absl::Time current_file_latest_close_time_ ABSL_GUARDED_BY(mutex_);

    const int kMaxRecordsPerFile = 1000;
    const absl::Duration kMaxFileDuration = absl::Seconds(10 * 60);
};

}  // namespace goldfish::metrics
