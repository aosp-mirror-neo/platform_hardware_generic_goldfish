// Copyright 2015 The Android Open Source Project
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

#include "android/crashreport/crash_system.h"

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "android/base/system.h"
#include "android/crashreport/crash_uploader.h"
#include "android/process/process.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/settings.h"
#include "goldfish/file/file.h"
#include "goldfish/tools/aemu_version.h"
#include "util/misc/uuid.h"

#ifdef _WIN32
#include <io.h>
#endif

using android::base::System;
using crashpad::CrashReportDatabase;

namespace android::crashreport {

#ifndef CRASHURL
#error "You must define CRASHURL to be the URL you want to upload crash reports to."
#endif
constexpr char kCrashUrl[] = CRASHURL;

class CrashSystemImpl : public CrashSystem {
  public:
    CrashSystemImpl() : client_(new crashpad::CrashpadClient()) {}

    bool active() const override { return initialized_; }

    absl::Status initialize() override {
        if (initialized_) {
            return absl::FailedPreconditionError("Crash reporting is already initialized.");
        }

        if (!System::Get()->GetEnableCrashReporting()) {
            LOG(INFO) << "Crash reporting is disabled, not reporting crashes.";
            return absl::OkStatus();
        }

        auto handler_path = handlerExe();
        if (handler_path.empty()) {
            return absl::NotFoundError(
                    "Crash handler not found. Set AEMU_CRASHPAD_HANDLER to the crashpad_handler "
                    "executable.");
        }
        auto database_path = ensureDatabaseDirectory(databaseDirectory());
        if (!database_path.ok()) {
            return database_path.status();
        }
        database_path_ = *std::move(database_path);
        auto metrics_path = ::base::FilePath();
        auto annotations = std::map<std::string, std::string>{
            {"prod", "AndroidEmulator"},
            {"ver", std::string(goldfish::version::GetEmulatorFullVersion())}};

        VLOG(1) << "Starting crashpad-handler: " << handler_path;
        auto file_path = ::base::FilePath(database_path_.native());
        std::vector<std::string> args = {"--no-rate-limit"};
#ifdef __APPLE__
        args.emplace_back(absl::StrCat("--monitor-pid=", System::Get()->GetCurrentProcessPid()));
#endif
        // Disable indirectly referenced memory capture on Windows to prevent minidump bloat.
        // This avoids scanning thread stacks for pointers and capturing arbitrary memory pages,
        // significantly reducing disk I/O and upload payload sizes.
        crashpad::CrashpadInfo* crashpad_info = crashpad::CrashpadInfo::GetCrashpadInfo();
        if (crashpad_info) {
            crashpad_info->set_gather_indirectly_referenced_memory(crashpad::TriState::kDisabled,
                                                                   0);
        }
        const bool active = client_->StartHandler(::base::FilePath(handler_path.native()),
                                                  file_path, metrics_path, kCrashUrl, annotations,
                                                  std::move(args), true, false);

        VLOG(1) << "Status of handler: " << (active ? "active" : "inactive");
        database_ = CrashReportDatabase::Initialize(file_path);
        initialized_ = active && database_;

        if (database_ && database_->GetSettings()) {
            database_->GetSettings()->SetUploadsEnabled(false);
        }

        VLOG(1) << "Using database: " << database_path_;
        // StartHandler() and CrashReportDatabase::Initialize() do not say why they failed.
        if (!active) {
            return absl::UnknownError(absl::StrCat(
                    "crashpad_handler '", handler_path.string(),
                    "' did not start for crash report database '", database_path_.string(),
                    "'. Verify that the executable exists, has execute permissions, and its "
                    "dependencies can be resolved."));
        }
        if (!database_) {
            return absl::UnknownError(absl::StrCat(
                    "Cannot open crash report database '", database_path_.string(),
                    "'. Check directory permissions and remove any stale database lock files."));
        }
        return absl::OkStatus();
    }

    void uploadEntries(const Consent consent) override {
        if (!database_ || !initialized_) {
            LOG(INFO) << "Crash database unavailable, or not initialized, we will not report any "
                         "crashes.";
            return;
        }

        const std::string message = database_path_.string();
        LOG(INFO) << "Storing crashdata in: " << message << ", detection is "
                  << (initialized_ ? "enabled" : "disabled")
                  << " for process: " << base::Process::Me()->pid();

        const bool are_uploads_enabled = (consent == Consent::ALWAYS);
        if (database_ && are_uploads_enabled) {
            LOG(INFO) << "Crash reports will be automatically uploaded to: " << kCrashUrl;
            database_->GetSettings()->SetUploadsEnabled(are_uploads_enabled);
        }

        // Get consent for any report that was not yet uploaded.
        std::vector<crashpad::UUID> to_remove;
        std::vector<CrashReportDatabase::Report> reports;
        std::vector<CrashReportDatabase::Report> pending_reports;
        database_->GetCompletedReports(&reports);
        database_->GetPendingReports(&pending_reports);
        reports.insert(reports.end(), pending_reports.begin(), pending_reports.end());

        auto status =
                consent == Consent::ALWAYS ? ReportAction::UPLOAD_REMOVE : ReportAction::REMOVE;
        for (const auto& report : reports) {
            if (!report.uploaded) {
                switch (status) {
                case ReportAction::UPLOAD_REMOVE: {
                    std::thread upload([this, report]() { ProcessReport(report); });
                    upload.detach();
                    break;
                }
                case ReportAction::REMOVE:
                    LOG(INFO) << "No consent for crashreport " << report.uuid.ToString()
                              << ", deleting.";
                    to_remove.push_back(report.uuid);
                    break;
                case ReportAction::UNDECIDED_KEEP:
                    LOG(INFO) << "Failed to get consent, keeping " << report.id << " for now.";
                    break;
                }
            } else {
                to_remove.push_back(report.uuid);
            }
        }

        for (const auto& report : to_remove) {
            database_->DeleteReport(report);
        }
    }

  private:
    void ProcessReport(const CrashReportDatabase::Report& report) {
        using namespace std::chrono_literals;
        std::vector<std::chrono::seconds> backoff{0s, 2s, 4s, 8s, 16s, 32s, 64s, 128s, 256s};
        auto status = UploadResult::kSuccess;
        auto wait = backoff.begin();

        database_->GetSettings()->SetUploadsEnabled(true);
        database_->RequestUpload(report.uuid);
        CrashReportDatabase::Report updated_report;

        do {
            std::this_thread::sleep_for(*wait);
            database_->LookUpCrashReport(report.uuid, &updated_report);
            LOG(INFO) << "Attempting to send crashreport " << updated_report.uuid.ToString()
                      << " to " << kCrashUrl;
            if (!updated_report.uploaded) {
                status = ProcessPendingReport(database_.get(), report);
            } else {
                status = UploadResult::kSuccess;
            }
            ++wait;
        } while (status == UploadResult::kRetry && wait != backoff.end());

        if (status == UploadResult::kSuccess) {
            // We should have a report id, fetch it!
            if (database_->LookUpCrashReport(report.uuid, &updated_report) ==
                CrashReportDatabase::OperationStatus::kNoError) {
                LOG(INFO) << "Report " << updated_report.uuid.ToString()
                          << " is available remotely as: " << updated_report.id;
            }
        } else {
            LOG(WARNING) << "Failed to send report.";
        }
    }

    std::unique_ptr<crashpad::CrashpadClient> client_;
    std::unique_ptr<CrashReportDatabase> database_;
    fs::path database_path_;
    bool initialized_{false};
};

const constexpr std::string_view kCrashpadDatabase = "emu-dev-crash-" VERSION ".db";
constexpr char kDatabaseEnvVar[] = "ANDROID_EMU_CRASH_REPORTING_DATABASE";
// Owner-only, because minidumps contain process memory.
constexpr unsigned kDatabaseDirectoryMode = 0700;

fs::path CrashSystem::databaseDirectory() {
    if (auto database_directory = System::Get()->EnvGet(kDatabaseEnvVar);
        !database_directory.empty()) {
        return {database_directory};
    }
    return System::Get()->GetTempDir() / kCrashpadDatabase;
}

absl::StatusOr<fs::path> CrashSystem::ensureDatabaseDirectory(const fs::path& database_path) {
    namespace file = android::base::file;
    // mkdir_recursive() does not create the first component of a relative path.
    auto absolute_path = file::make_absolute(database_path);
    if (!absolute_path.ok()) {
        return absl::Status(
                absolute_path.status().code(),
                absl::StrCat("Cannot resolve crash report database path '", database_path.string(),
                             "': ", absolute_path.status().message()));
    }
    const absl::Status created = file::mkdir_recursive(*absolute_path, kDatabaseDirectoryMode);
    // Checked even if mkdir_recursive() failed, in case another emulator created it first.
    if (file::is_dir(*absolute_path)) {
        return *std::move(absolute_path);
    }
    if (!created.ok()) {
        return absl::Status(created.code(),
                            absl::StrCat("Cannot create crash report database directory '",
                                         absolute_path->string(), "': ", created.message(),
                                         ". Set ", kDatabaseEnvVar, " to a writable directory."));
    }
    return absl::FailedPreconditionError(
            absl::StrCat("Crash report database path '", absolute_path->string(),
                         "' exists but is not a directory. Remove it or set ", kDatabaseEnvVar,
                         " to a directory."));
}

fs::path CrashSystem::handlerExe() {
    auto from_env = System::GetEnvironmentVariable("AEMU_CRASHPAD_HANDLER");
    if (from_env.empty()) {
        LOG(ERROR) << "AEMU_CRASHPAD_HANDLER envvar is empty - unable to locate crashpad_handler";
    }

    return {from_env};
}

CrashSystem& CrashSystem::get() {
    static CrashSystemImpl s_crash_system;
    return s_crash_system;
}

}  // namespace android::crashreport
