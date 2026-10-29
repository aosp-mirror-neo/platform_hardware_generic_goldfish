// Copyright 2026 The Android Open Source Project
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

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/studio_stats_wrapper.h"

namespace goldfish::metrics {

enum class QuickbootLoadStatus {
    kLoading,
    kSkippedCmdline,
    kSkippedAvd,
    kSkippedOldSnapshot,
};

enum class QuickbootSaveStatus {
    kSaving,
    kDisabledCmdline,
    kDisabledAvd,
    kDirectoryError,
};

struct QuickbootMetricsState {
    absl::Time session_start_time = absl::Now();
    absl::Time op_start_time;
};

struct SnapshotOperationStats {
    bool for_save = true;
    bool is_quickboot = false;
    bool include_memory_usage = true;
    std::string name;
    uint64_t duration_ms = 0;
    uint64_t session_uptime_ms = 0;

    int64_t ram_size = 0;
    int64_t textures_size = 0;
    uint64_t ram_duration_ms = 0;
    uint64_t textures_duration_ms = 0;

    android_studio::EmulatorSnapshotSaveState save_state{
        android_studio::EMULATOR_SNAPSHOT_SAVE_SUCCEEDED_NORMAL};
    android_studio::EmulatorSnapshotLoadState load_state{
        android_studio::EMULATOR_SNAPSHOT_LOAD_SUCCEEDED_NORMAL};
    android_studio::EmulatorQuickbootLoad::EmulatorQuickbootLoadState quickboot_load_state{
        android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_SUCCEEDED};
    android_studio::EmulatorQuickbootSave::EmulatorQuickbootSaveState quickboot_save_state{
        android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_SUCCEEDED};
    android_studio::EmulatorSnapshotFailureReason failure_reason{
        android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_UNSPECIFIED};

    /// Marks this snapshot operation as failed with the given `reason`.
    void ReportFailure(android_studio::EmulatorSnapshotFailureReason reason =
                               android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR) {
        if (for_save) {
            save_state = android_studio::EMULATOR_SNAPSHOT_SAVE_FAILED;
            quickboot_save_state =
                    android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_FAILED;
        } else {
            load_state = android_studio::EMULATOR_SNAPSHOT_LOAD_FAILED;
            quickboot_load_state =
                    android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_FAILED;
        }
        failure_reason = reason;
    }
};

std::string AnonymizeSnapshotName(std::string_view name);

void FillEmulatorSnapshot(android_studio::EmulatorSnapshot* snapshot,
                          const SnapshotOperationStats& stats);

void FillSnapshotMetrics(android_studio::AndroidStudioEvent& event,
                         const SnapshotOperationStats& stats);

void ReportSnapshotMetrics(MetricsReporter& reporter, const SnapshotOperationStats& stats);

void ReportQuickbootSkippedMetrics(MetricsReporter& reporter, QuickbootLoadStatus load_status,
                                   QuickbootSaveStatus save_status);

void ReportGenericSnapshotSave(MetricsReporter& reporter, std::string_view name, bool success,
                               absl::Duration duration);

void ReportGenericSnapshotLoad(MetricsReporter& reporter, std::string_view name, bool success,
                               absl::Duration duration);

void ReportQuickbootLoad(MetricsReporter& reporter, std::string_view name, bool success,
                         QuickbootMetricsState& state,
                         android_studio::EmulatorSnapshotFailureReason failure_reason =
                                 android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);

void ReportQuickbootSave(MetricsReporter& reporter, std::string_view name, bool success,
                         const QuickbootMetricsState& state,
                         android_studio::EmulatorSnapshotFailureReason failure_reason =
                                 android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);

}  // namespace goldfish::metrics
