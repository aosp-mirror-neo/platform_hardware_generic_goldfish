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

#include "goldfish/metrics/studio_file_metrics_writer.h"

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#include "android/base/system.h"
#include "android/process/process.h"
#include "google_logs_publishing.pb.h"
#include "re2/re2.h"

namespace goldfish::metrics {

using namespace std::chrono_literals;

namespace {
// Note "emulator-metrics2-" to operate independently of qemu2 files.
constexpr std::string_view kFileNameFormat = "emulator-metrics2-%s-%d-%d.open";
constexpr re2::LazyRE2 kOpenFileRegex = {.pattern_ = R"(emulator-metrics2-(.*)-(\d+)-\d+.open)"};

constexpr std::string_view kFinalExtension = "trk";
constexpr std::string_view kLockSuffix = ".lock";

constexpr absl::Duration kLockDuration = absl::Seconds(60);

fs::path LockPath(fs::path file_path) {
    file_path += kLockSuffix;
    return file_path;
}

void RefreshLockFile(const fs::path& file_path) {
    if (file_path.empty()) {
        return;
    }
    if (auto s = android::base::file::touch(LockPath(file_path)); !s.ok()) {
        LOG(ERROR) << "Failed to create metrics lock file: " << s;
    }
}

void ClearLockFile(const fs::path& file_path) {
    if (file_path.empty()) {
        return;
    }
    if (auto s = android::base::file::rm(LockPath(file_path)); !s.ok()) {
        LOG(ERROR) << "Failed to remove metrics lock file: " << s;
    }
}

bool IsLockFileLive(const fs::path& file_path) {
    auto lock_path = LockPath(file_path);
    if (!android::base::file::exists(lock_path)) {
        return false;
    }
    auto last_write_time = android::base::file::last_write_time(lock_path);
    if (!last_write_time.ok()) {
        return false;
    }
    return (*last_write_time + kLockDuration) > absl::Now();
}

}  // namespace

StudioFileMetricsWriter::StudioFileMetricsWriter(const fs::path& spool_dir,
                                                 const std::string& session_id,
                                                 goldfish::async::EventLoop& main_loop)
        : spool_dir_(spool_dir)
        , session_id_(session_id)
        , pid_(android::base::System::GetCurrentProcessPid())
        , max_file_duration_timer_(main_loop.ScheduleRepeating(
                  [this] {
                      absl::MutexLock lock(mutex_);
                      if (open_file_path_.empty()) {
                          return true;
                      }
                      // Check if we should close the current file every 10s.
                      if (absl::Now() > current_file_latest_close_time_) {
                          FinalizeCurrentFile();
                      } else {
                          RefreshLockFile(open_file_path_);
                      }
                      return true;
                  },
                  absl::Seconds(10), absl::Seconds(10))) {}

StudioFileMetricsWriter::~StudioFileMetricsWriter() {
    max_file_duration_timer_.reset();
    absl::MutexLock lock(mutex_);
    FinalizeCurrentFile();
}

void StudioFileMetricsWriter::Write(MetricsEvent event) {
    absl::MutexLock lock(mutex_);
    if (open_file_path_.empty()) {
        OpenNextFile();
    }

    wireless_android_play_playlog::LogEvent log_event;
    log_event.set_event_time_ms(event.time_ms);
    if (!event.as_event.SerializeToString(log_event.mutable_source_extension())) {
        LOG(ERROR) << "Failed to serialize metrics event (time_ms=" << event.time_ms << "). "
                   << "Individual telemetry metrics for this event will be dropped. "
                   << "Protobuf initialization errors: "
                   << event.as_event.InitializationErrorString();
        return;
    }

    if (!current_file_.is_open()) {
        LOG(ERROR) << "Failed to open metrics file, report will be lost: "
                   << log_event.ShortDebugString();
        return;
    }

    {
        google::protobuf::io::OstreamOutputStream raw_output(&current_file_);
        google::protobuf::io::CodedOutputStream coded_output(&raw_output);
        // Write delimited message: [size (varint)][serialized message]
        coded_output.WriteVarint32(log_event.ByteSizeLong());
        log_event.SerializeWithCachedSizes(&coded_output);
        if (coded_output.HadError()) {
            LOG(ERROR) << "Error occurred while serializing metrics report to file: "
                       << log_event.ShortDebugString() << " - " << open_file_path_;
        }
    }
    current_file_.flush();
    if (!current_file_) {
        LOG(ERROR) << "Error occurred while flushing metrics file: " << open_file_path_;
    }

    current_file_record_count_++;
    if (current_file_record_count_ >= kMaxRecordsPerFile) {
        FinalizeCurrentFile();
    }
}

void StudioFileMetricsWriter::OpenNextFile() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
    DCHECK(!current_file_.is_open());
    open_file_path_ =
            spool_dir_ / absl::StrFormat(kFileNameFormat, session_id_, pid_, file_counter_++);
    RefreshLockFile(open_file_path_);
    current_file_.clear();
    current_file_.open(open_file_path_, std::ios::binary | std::ios::app);
    if (!current_file_) {
        LOG(ERROR) << "Error occurred while opening metrics file: " << open_file_path_;
    }
    if (!current_file_.is_open()) {
        LOG(ERROR) << "Failed to open metrics file: " << open_file_path_;
        // Keep state consistent.
        open_file_path_.clear();
    } else {
        current_file_record_count_ = 0;
        current_file_latest_close_time_ = absl::Now() + kMaxFileDuration;
    }
}

namespace {

bool FinalizeFile(const fs::path& path) {
    fs::path new_path = path;
    new_path.replace_extension(kFinalExtension);
    if (auto s = android::base::file::mv_file(path, new_path, /*fallback_to_copy_rm=*/false);
        !s.ok()) {
        LOG(ERROR) << "Failed to finalize metrics file: " << path << " - " << s;
        return false;
    }
    VLOG(1) << "Successfully finalized metrics file: " << path;
    ClearLockFile(path);
    return true;
}

}  // namespace

void StudioFileMetricsWriter::FinalizeCurrentFile() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
    if (open_file_path_.empty()) {
        DCHECK(!current_file_.is_open());
        return;
    }
    DCHECK(current_file_.is_open());
    current_file_.clear();
    current_file_.close();
    if (!current_file_) {
        LOG(ERROR) << "Error occurred while closing metrics file: " << open_file_path_;
    }
    FinalizeFile(open_file_path_);
    open_file_path_.clear();
    current_file_record_count_ = 0;
    current_file_latest_close_time_ = absl::InfiniteFuture();
}

// static
std::vector<std::string> StudioFileMetricsWriter::FinalizeAbandonedSessionFiles(
        const fs::path& spool_dir) {
    std::vector<std::string> abandoned_sessions;
    absl::flat_hash_set<std::string> seen_sessions;
    for (const auto& path : android::base::file::scan_dir(spool_dir, /*fullPath=*/true)) {
        const std::string path_as_string = path.filename().string();
        std::string_view session_id;
        uint32_t pid;
        if (!RE2::FullMatch(path_as_string, *kOpenFileRegex, &session_id, &pid)) {
            continue;
        }
        if (auto proc = android::base::Process::FromPid(pid); proc && proc->IsAlive()) {
            // Only check the lock file if the process actually exists.
            if (IsLockFileLive(path)) {
                continue;
            }
        }
        if (FinalizeFile(path) && seen_sessions.emplace(session_id).second) {
            abandoned_sessions.emplace_back(session_id);
        }
    }
    return abandoned_sessions;
}

// static
void StudioFileMetricsWriter::FinalizeProcessFiles(const fs::path& spool_dir,
                                                   std::string_view session_id, uint32_t pid) {
    for (const auto& path : android::base::file::scan_dir(spool_dir, /*fullPath=*/true)) {
        const std::string filename = path.filename().string();
        std::string_view matched_session_id;
        uint32_t matched_pid = 0;
        if (!RE2::FullMatch(filename, *kOpenFileRegex, &matched_session_id, &matched_pid)) {
            continue;
        }
        if (matched_session_id == session_id && matched_pid == pid) {
            FinalizeFile(path);
        }
    }
}

}  // namespace goldfish::metrics
