// Copyright 2025 The Android Open Source Project
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
#include "emulator/crashreport/tool/crash_report_manager.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

#include "absl/log/log.h"

#include "android/crashreport/crash_system.h"
#include "android/crashreport/crash_uploader.h"
#include "client/settings.h"

namespace android {
namespace crashreport {

bool CrashReportManager::Initialize() {
    if (mDb) return true;  // Already initialized

    auto database_path = android::crashreport::CrashSystem::ensureDatabaseDirectory(
            android::crashreport::CrashSystem::databaseDirectory());
    if (!database_path.ok()) {
        LOG(ERROR) << "Failed to prepare crash database directory: " << database_path.status();
        return false;
    }
    for (int i = 0; !mDb && i < 5; i++) {
        mDb = crashpad::CrashReportDatabase::Initialize(::base::FilePath(*database_path));
        if (!mDb) {
            LOG(ERROR) << "Failed to initialize crash database, retrying in 1 second...";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (!mDb) {
        LOG(ERROR) << "Failed to initialize crash database " << database_path->string()
                   << " after multiple retries.";
        return false;
    }
    return true;
}

std::vector<crashpad::CrashReportDatabase::Report> CrashReportManager::GetAllReports() {
    if (!mDb) {
        LOG(WARNING) << "Database not initialized!";
        return {};
    }
    std::vector<crashpad::CrashReportDatabase::Report> reports;
    std::vector<crashpad::CrashReportDatabase::Report> pendingReports;
    mDb->GetCompletedReports(&reports);
    mDb->GetPendingReports(&pendingReports);
    reports.insert(reports.end(), pendingReports.begin(), pendingReports.end());
    return reports;
}

bool CrashReportManager::DeleteReport(const crashpad::UUID& uuid) {
    if (!mDb) return false;
    return mDb->DeleteReport(uuid) == crashpad::CrashReportDatabase::kNoError;
}

bool CrashReportManager::RequestUpload(const crashpad::UUID& uuid) {
    if (!mDb) return false;
    // crashpad::CrashReportDatabase::RequestUpload always returns true
    return mDb->RequestUpload(uuid);
}

std::optional<std::string> CrashReportManager::GetLatestReportPath() {
    if (!mDb) return std::nullopt;
    auto reports = GetAllReports();
    if (reports.empty()) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << reports.back().file_path;
    // Reports are sorted by creation time, so the last one is the latest.
    return ss.str();
}

void CrashReportManager::ForEachReport(
        std::function<void(const crashpad::CrashReportDatabase::Report& report)> action) {
    if (!mDb) {
        LOG(WARNING) << "Database not initialized!";
        return;
    }
    auto reports = GetAllReports();
    for (const auto& report : reports) {
        action(report);
    }
}

bool CrashReportManager::UploadCrashReports() {
    if (!mDb) return false;

    auto reports = GetAllReports();
    if (reports.empty()) {
        std::cout << "No reports found to upload." << std::endl;
        return true;
    }

    bool original_enabled = false;
    mDb->GetSettings()->GetUploadsEnabled(&original_enabled);
    mDb->GetSettings()->SetUploadsEnabled(true);

    std::cout << "Checking reports for upload..." << std::endl;
    int requested_count = 0;
    std::vector<crashpad::UUID> attempted_uploads;
    for (const auto& report : reports) {
        if (!report.uploaded) {
            attempted_uploads.push_back(report.uuid);
            mDb->RequestUpload(report.uuid);
            std::cout << "Attempting to upload report " << report.uuid.ToString() << "..."
                      << std::endl;
            auto result = android::crashreport::ProcessPendingReport(mDb.get(), report);
            if (result == android::crashreport::UploadResult::kSuccess) {
                std::cout << "Successfully uploaded report " << report.uuid.ToString() << std::endl;
                requested_count++;
            } else {
                std::cerr << "Failed to upload report " << report.uuid.ToString()
                          << " (Error: " << static_cast<int>(result) << ")" << std::endl;
            }
        }
    }

    if (requested_count == 0) {
        std::cout << "No reports were uploaded." << std::endl;
    } else {
        std::cout << "Uploaded " << requested_count << " reports." << std::endl;
    }

    // Now print status of completed reports that we attempted
    for (const auto& uuid : attempted_uploads) {
        crashpad::CrashReportDatabase::Report report;
        if (mDb->LookUpCrashReport(uuid, &report) == crashpad::CrashReportDatabase::kNoError) {
            if (report.id.empty()) {
                std::cout << "Report " << report.uuid.ToString()
                          << " is marked as completed but not yet remotely available." << std::endl;
                std::cout << "Please preserve the minidump found here: " << report.file_path
                          << std::endl;
            } else {
                std::cout << "Report " << report.uuid.ToString()
                          << " is available remotely as: " << report.id
                          << " (provide this ID when sharing with Google)" << std::endl;
            }
        }
    }

    mDb->GetSettings()->SetUploadsEnabled(original_enabled);

    return true;
}

}  // namespace crashreport
}  // namespace android
