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

#include "goldfish/metrics/snapshot_metrics.h"

#include <openssl/sha.h>

#include "absl/strings/escaping.h"

#include "goldfish/metrics/perf_stat_reporter.h"

namespace goldfish::metrics {

namespace {

constexpr std::string_view kSnapshotNameSalt = "5a2b3c4d5e6f7a8b123456789abcdef0";

/// Converts `duration` to whole milliseconds for snapshot telemetry fields.
uint64_t ToDurationMs(absl::Duration duration) {
    return static_cast<uint64_t>(absl::ToInt64Milliseconds(duration));
}

bool IsQuickbootLoadSucceeded(
        android_studio::EmulatorQuickbootLoad::EmulatorQuickbootLoadState state) {
    return state == android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_SUCCEEDED ||
           state == android_studio::EmulatorQuickbootLoad::
                            EMULATOR_QUICKBOOT_LOAD_SUCCEEDED_WITH_VULKAN;
}

bool IsQuickbootSaveSucceeded(
        android_studio::EmulatorQuickbootSave::EmulatorQuickbootSaveState state) {
    return state == android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_SUCCEEDED ||
           state == android_studio::EmulatorQuickbootSave::
                            EMULATOR_QUICKBOOT_SAVE_SUCCEEDED_WITH_VULKAN;
}

void ReportQuickbootLoadSkipped(
        MetricsReporter& reporter,
        android_studio::EmulatorQuickbootLoad::EmulatorQuickbootLoadState state,
        android_studio::EmulatorSnapshotFailureReason failure_reason =
                android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_UNSPECIFIED) {
    SnapshotOperationStats stats;
    stats.for_save = false;
    stats.is_quickboot = true;
    stats.include_memory_usage = false;
    stats.quickboot_load_state = state;
    stats.failure_reason = failure_reason;
    ReportSnapshotMetrics(reporter, stats);
}

void ReportQuickbootSaveSkipped(
        MetricsReporter& reporter,
        android_studio::EmulatorQuickbootSave::EmulatorQuickbootSaveState state,
        android_studio::EmulatorSnapshotFailureReason failure_reason =
                android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_UNSPECIFIED) {
    SnapshotOperationStats stats;
    stats.for_save = true;
    stats.is_quickboot = true;
    stats.include_memory_usage = false;
    stats.quickboot_save_state = state;
    stats.failure_reason = failure_reason;
    ReportSnapshotMetrics(reporter, stats);
}

/// Populates and reports metrics for a generic (non-quickboot) snapshot save or
/// load operation.
void ReportGenericSnapshot(MetricsReporter& reporter, std::string_view name, bool for_save,
                           bool success, absl::Duration duration) {
    SnapshotOperationStats stats;
    stats.for_save = for_save;
    stats.is_quickboot = false;
    stats.name = std::string(name);
    stats.duration_ms = ToDurationMs(duration);
    if (!success) {
        stats.ReportFailure();
    }
    ReportSnapshotMetrics(reporter, stats);
}

}  // namespace

std::string AnonymizeSnapshotName(std::string_view name) {
    if (name.empty()) {
        return "";
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, kSnapshotNameSalt.data(), kSnapshotNameSalt.size());
    SHA256_Update(&ctx, name.data(), name.size());
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
    return absl::BytesToHexString(
            std::string_view(reinterpret_cast<const char*>(digest), sizeof(digest)));
}

void FillEmulatorSnapshot(android_studio::EmulatorSnapshot* snapshot,
                          const SnapshotOperationStats& stats) {
    if (!snapshot) {
        return;
    }

    if (stats.for_save) {
        snapshot->set_save_state(stats.save_state);
        if (stats.save_state != android_studio::EMULATOR_SNAPSHOT_SAVE_SUCCEEDED_NORMAL) {
            snapshot->set_save_failure_reason(stats.failure_reason);
            return;
        }
        snapshot->set_save_duration_ms(stats.duration_ms);
        if (stats.ram_duration_ms > 0) {
            snapshot->set_ram_save_duration_ms(stats.ram_duration_ms);
        }
        if (stats.textures_duration_ms > 0) {
            snapshot->set_textures_save_duration_ms(stats.textures_duration_ms);
        }
    } else {
        snapshot->set_load_state(stats.load_state);
        if (stats.load_state != android_studio::EMULATOR_SNAPSHOT_LOAD_SUCCEEDED_NORMAL) {
            snapshot->set_load_failure_reason(stats.failure_reason);
            return;
        }
        snapshot->set_load_duration_ms(stats.duration_ms);
        if (stats.ram_duration_ms > 0) {
            snapshot->set_ram_load_duration_ms(stats.ram_duration_ms);
        }
        if (stats.textures_duration_ms > 0) {
            snapshot->set_textures_load_duration_ms(stats.textures_duration_ms);
        }
    }

    snapshot->set_name(AnonymizeSnapshotName(stats.name));
    if (stats.ram_size > 0) {
        snapshot->set_ram_size_bytes(stats.ram_size);
    }
    if (stats.textures_size > 0) {
        snapshot->set_textures_size_bytes(stats.textures_size);
    }
    if (stats.ram_size > 0 || stats.textures_size > 0) {
        snapshot->set_size_bytes(stats.ram_size + stats.textures_size);
    }
}

void FillSnapshotMetrics(android_studio::AndroidStudioEvent& event,
                         const SnapshotOperationStats& stats) {
    bool succeeded = false;
    if (stats.is_quickboot) {
        if (stats.for_save) {
            auto* qb_save = event.mutable_emulator_details()->mutable_quickboot_save();
            qb_save->set_state(stats.quickboot_save_state);
            if (IsQuickbootSaveSucceeded(stats.quickboot_save_state)) {
                succeeded = true;
                qb_save->set_duration_ms(stats.duration_ms);
                qb_save->set_sesion_uptime_ms(stats.session_uptime_ms);  // NOTYPO
                FillEmulatorSnapshot(event.mutable_emulator_details()->add_snapshot_saves(), stats);
            } else {
                qb_save->mutable_snapshot()->set_save_failure_reason(stats.failure_reason);
            }
        } else {
            auto* qb_load = event.mutable_emulator_details()->mutable_quickboot_load();
            qb_load->set_state(stats.quickboot_load_state);
            if (IsQuickbootLoadSucceeded(stats.quickboot_load_state)) {
                succeeded = true;
                qb_load->set_duration_ms(stats.duration_ms);
                FillEmulatorSnapshot(event.mutable_emulator_details()->add_snapshot_loads(), stats);
            } else {
                qb_load->mutable_snapshot()->set_load_failure_reason(stats.failure_reason);
            }
        }
    } else {
        android_studio::EmulatorSnapshot* snapshot = nullptr;
        if (stats.for_save) {
            succeeded =
                    (stats.save_state == android_studio::EMULATOR_SNAPSHOT_SAVE_SUCCEEDED_NORMAL);
            snapshot = event.mutable_emulator_details()->add_snapshot_saves();
        } else {
            succeeded =
                    (stats.load_state == android_studio::EMULATOR_SNAPSHOT_LOAD_SUCCEEDED_NORMAL);
            snapshot = event.mutable_emulator_details()->add_snapshot_loads();
        }
        FillEmulatorSnapshot(snapshot, stats);
    }

    if (succeeded && stats.include_memory_usage) {
        event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
        FillMemUsage(event.mutable_emulator_performance_stats()->add_memory_usage());
    }
}

void ReportSnapshotMetrics(MetricsReporter& reporter, const SnapshotOperationStats& stats) {
    reporter.Report([stats](android_studio::AndroidStudioEvent& event) {
        FillSnapshotMetrics(event, stats);
    });
}

void ReportQuickbootSkippedMetrics(MetricsReporter& reporter, QuickbootLoadStatus load_status,
                                   QuickbootSaveStatus save_status) {
    switch (load_status) {
        using enum QuickbootLoadStatus;
    case kLoading:
        break;
    case kSkippedCmdline:
        ReportQuickbootLoadSkipped(
                reporter,
                android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_COLD_CMDLINE);
        break;
    case kSkippedAvd:
        ReportQuickbootLoadSkipped(
                reporter, android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_COLD_AVD);
        break;
    case kSkippedOldSnapshot:
        ReportQuickbootLoadSkipped(
                reporter,
                android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_COLD_OLD_SNAPSHOT);
        break;
    }

    switch (save_status) {
        using enum QuickbootSaveStatus;
    case kSaving:
        break;
    case kDirectoryError:
        ReportQuickbootSaveSkipped(
                reporter, android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_FAILED,
                android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);
        break;
    case kDisabledCmdline:
        ReportQuickbootSaveSkipped(
                reporter,
                android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_DISABLED_CMDLINE);
        break;
    case kDisabledAvd:
        ReportQuickbootSaveSkipped(
                reporter,
                android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_DISABLED_AVD);
        break;
    }
}

void ReportGenericSnapshotSave(MetricsReporter& reporter, std::string_view name, bool success,
                               absl::Duration duration) {
    ReportGenericSnapshot(reporter, name, /*for_save=*/true, success, duration);
}

void ReportGenericSnapshotLoad(MetricsReporter& reporter, std::string_view name, bool success,
                               absl::Duration duration) {
    ReportGenericSnapshot(reporter, name, /*for_save=*/false, success, duration);
}

void ReportQuickbootLoad(MetricsReporter& reporter, std::string_view name, bool success,
                         QuickbootMetricsState& state,
                         android_studio::EmulatorSnapshotFailureReason failure_reason) {
    const absl::Time now = absl::Now();
    SnapshotOperationStats stats;
    stats.for_save = false;
    stats.is_quickboot = true;
    stats.name = std::string(name);
    stats.duration_ms = ToDurationMs(now - state.op_start_time);
    if (success) {
        state.session_start_time = now;
    } else {
        stats.ReportFailure(failure_reason);
    }
    ReportSnapshotMetrics(reporter, stats);
}

void ReportQuickbootSave(MetricsReporter& reporter, std::string_view name, bool success,
                         const QuickbootMetricsState& state,
                         android_studio::EmulatorSnapshotFailureReason failure_reason) {
    const absl::Time now = absl::Now();
    SnapshotOperationStats stats;
    stats.for_save = true;
    stats.is_quickboot = true;
    stats.name = std::string(name);
    stats.duration_ms = ToDurationMs(now - state.op_start_time);
    stats.session_uptime_ms = ToDurationMs(now - state.session_start_time);
    if (!success) {
        stats.ReportFailure(failure_reason);
    }
    ReportSnapshotMetrics(reporter, stats);
}

}  // namespace goldfish::metrics
