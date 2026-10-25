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

#include <gtest/gtest.h>

#include "goldfish/metrics/testing/fake_metrics_writer.h"

namespace goldfish::metrics {

TEST(SnapshotMetricsTest, AnonymizeSnapshotName) {
    std::string name1 = AnonymizeSnapshotName("default_boot");
    std::string name2 = AnonymizeSnapshotName("default_boot");
    std::string name3 = AnonymizeSnapshotName("other_snapshot");

    EXPECT_EQ(name1, "63dd9123882b99b095d6bd086ba5c6c5fdc543a02827a5356fe380a0a73a1075");
    EXPECT_EQ(name1, name2);
    EXPECT_NE(name1, name3);
    EXPECT_EQ(AnonymizeSnapshotName(""), "");
}

TEST(SnapshotMetricsTest, FillEmulatorSnapshotSave) {
    SnapshotOperationStats stats;
    stats.for_save = true;
    stats.name = "default_boot";
    stats.duration_ms = 1500;
    stats.ram_size = 1000;
    stats.textures_size = 2000;

    android_studio::EmulatorSnapshot snapshot;
    FillEmulatorSnapshot(&snapshot, stats);

    EXPECT_NE(snapshot.name(), "default_boot");
    EXPECT_EQ(snapshot.save_duration_ms(), 1500);
    EXPECT_TRUE(snapshot.has_size_bytes());
    EXPECT_EQ(snapshot.size_bytes(), 3000);
    EXPECT_EQ(snapshot.save_state(), android_studio::EMULATOR_SNAPSHOT_SAVE_SUCCEEDED_NORMAL);
}

TEST(SnapshotMetricsTest, FillEmulatorSnapshotSaveFailed) {
    SnapshotOperationStats stats;
    stats.for_save = true;
    stats.name = "default_boot";
    stats.ReportFailure();

    android_studio::AndroidStudioEvent event;
    FillSnapshotMetrics(event, stats);

    EXPECT_NE(event.kind(), android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
    EXPECT_FALSE(event.has_emulator_performance_stats());
    ASSERT_EQ(event.emulator_details().snapshot_saves_size(), 1);
    const auto& snap = event.emulator_details().snapshot_saves(0);
    EXPECT_EQ(snap.save_state(), android_studio::EMULATOR_SNAPSHOT_SAVE_FAILED);
    EXPECT_EQ(snap.save_failure_reason(),
              android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);
    EXPECT_FALSE(snap.has_name());
}

TEST(SnapshotMetricsTest, FillSnapshotMetricsQuickbootLoadSucceeded) {
    SnapshotOperationStats stats;
    stats.for_save = false;
    stats.is_quickboot = true;
    stats.name = "default_boot";
    stats.duration_ms = 1200;
    stats.quickboot_load_state =
            android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_SUCCEEDED;

    android_studio::AndroidStudioEvent event;
    FillSnapshotMetrics(event, stats);

    EXPECT_EQ(event.kind(), android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
    EXPECT_TRUE(event.has_emulator_details());
    EXPECT_TRUE(event.emulator_details().has_quickboot_load());
    EXPECT_EQ(event.emulator_details().quickboot_load().duration_ms(), 1200);
    ASSERT_EQ(event.emulator_details().snapshot_loads_size(), 1);
    EXPECT_EQ(event.emulator_details().snapshot_loads(0).load_duration_ms(), 1200);
    EXPECT_FALSE(event.emulator_details().snapshot_loads(0).has_size_bytes());
}

TEST(SnapshotMetricsTest, FillSnapshotMetricsQuickbootLoadSkipped) {
    SnapshotOperationStats stats;
    stats.for_save = false;
    stats.is_quickboot = true;
    stats.include_memory_usage = false;
    stats.quickboot_load_state =
            android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_COLD_CMDLINE;
    stats.failure_reason = android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_UNSPECIFIED;

    android_studio::AndroidStudioEvent event;
    FillSnapshotMetrics(event, stats);

    EXPECT_NE(event.kind(), android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
    EXPECT_FALSE(event.has_emulator_performance_stats());
    EXPECT_EQ(event.emulator_details().snapshot_loads_size(), 0);
    ASSERT_TRUE(event.emulator_details().has_quickboot_load());
    EXPECT_EQ(event.emulator_details().quickboot_load().state(),
              android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_COLD_CMDLINE);
    EXPECT_EQ(event.emulator_details().quickboot_load().snapshot().load_failure_reason(),
              android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_UNSPECIFIED);
}

TEST(SnapshotMetricsTest, FillSnapshotMetricsQuickbootSaveSucceeded) {
    SnapshotOperationStats stats;
    stats.for_save = true;
    stats.is_quickboot = true;
    stats.name = "default_boot";
    stats.duration_ms = 800;
    stats.session_uptime_ms = 45000;
    stats.quickboot_save_state =
            android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_SUCCEEDED;

    android_studio::AndroidStudioEvent event;
    FillSnapshotMetrics(event, stats);

    EXPECT_EQ(event.kind(), android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
    ASSERT_TRUE(event.emulator_details().has_quickboot_save());
    EXPECT_EQ(event.emulator_details().quickboot_save().state(),
              android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_SUCCEEDED);
    EXPECT_EQ(event.emulator_details().quickboot_save().duration_ms(), 800);
    EXPECT_EQ(event.emulator_details().quickboot_save().sesion_uptime_ms(), 45000);  // NOTYPO
    ASSERT_EQ(event.emulator_details().snapshot_saves_size(), 1);
    EXPECT_EQ(event.emulator_details().snapshot_saves(0).save_duration_ms(), 800);
}

TEST(SnapshotMetricsTest, FillSnapshotMetricsQuickbootSaveFailed) {
    SnapshotOperationStats stats;
    stats.for_save = true;
    stats.is_quickboot = true;
    stats.name = "default_boot";
    stats.ReportFailure();

    android_studio::AndroidStudioEvent event;
    FillSnapshotMetrics(event, stats);

    EXPECT_NE(event.kind(), android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
    EXPECT_FALSE(event.has_emulator_performance_stats());
    EXPECT_EQ(event.emulator_details().snapshot_saves_size(), 0);
    ASSERT_TRUE(event.emulator_details().has_quickboot_save());
    EXPECT_EQ(event.emulator_details().quickboot_save().state(),
              android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_FAILED);
    EXPECT_EQ(event.emulator_details().quickboot_save().snapshot().save_failure_reason(),
              android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);
}

TEST(SnapshotMetricsTest, ReportQuickbootSkippedMetricsDirectoryError) {
    MetricsReporter reporter;
    auto writer = std::make_unique<testing::FakeMetricsWriter>();
    auto* writer_ptr = writer.get();
    reporter.SetWriter(std::move(writer));

    ReportQuickbootSkippedMetrics(reporter, QuickbootLoadStatus::kLoading,
                                  QuickbootSaveStatus::kDirectoryError);

    ASSERT_TRUE(writer_ptr->WaitForEvents(1, absl::Seconds(5)));
    const absl::MutexLock lock(writer_ptr->mutex);
    ASSERT_EQ(writer_ptr->events.size(), 1u);
    const auto& event = writer_ptr->events[0];
    ASSERT_TRUE(event.emulator_details().has_quickboot_save());
    EXPECT_EQ(event.emulator_details().quickboot_save().state(),
              android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_FAILED);
    EXPECT_EQ(event.emulator_details().quickboot_save().snapshot().save_failure_reason(),
              android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);
}

TEST(SnapshotMetricsTest, ReportQuickbootSaveSucceededAndFailed) {
    MetricsReporter reporter;
    auto writer = std::make_unique<testing::FakeMetricsWriter>();
    auto* writer_ptr = writer.get();
    reporter.SetWriter(std::move(writer));

    QuickbootMetricsState state;
    state.session_start_time = absl::Now() - absl::Seconds(10);
    state.op_start_time = absl::Now() - absl::Seconds(1);

    ReportQuickbootSave(reporter, "default_boot", /*success=*/false, state);
    ReportQuickbootSave(reporter, "default_boot", /*success=*/true, state);

    ASSERT_TRUE(writer_ptr->WaitForEvents(2, absl::Seconds(5)));
    const absl::MutexLock lock(writer_ptr->mutex);
    ASSERT_EQ(writer_ptr->events.size(), 2u);
    EXPECT_EQ(writer_ptr->events[0].emulator_details().quickboot_save().state(),
              android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_FAILED);
    EXPECT_EQ(writer_ptr->events[0]
                      .emulator_details()
                      .quickboot_save()
                      .snapshot()
                      .save_failure_reason(),
              android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_INTERNAL_ERROR);
    EXPECT_EQ(writer_ptr->events[1].emulator_details().quickboot_save().state(),
              android_studio::EmulatorQuickbootSave::EMULATOR_QUICKBOOT_SAVE_SUCCEEDED);
    EXPECT_GE(writer_ptr->events[1].emulator_details().quickboot_save().duration_ms(), 1000u);
    EXPECT_GE(
            writer_ptr->events[1].emulator_details().quickboot_save().sesion_uptime_ms(),  // NOTYPO
            10000u);
}

TEST(SnapshotMetricsTest, ReportQuickbootLoadUpdatesSessionStartTimeOnSuccessOnly) {
    MetricsReporter reporter;
    auto writer = std::make_unique<testing::FakeMetricsWriter>();
    auto* writer_ptr = writer.get();
    reporter.SetWriter(std::move(writer));

    const absl::Time initial_session_start = absl::Now() - absl::Seconds(10);
    QuickbootMetricsState state;
    state.session_start_time = initial_session_start;
    state.op_start_time = absl::Now() - absl::Seconds(1);

    ReportQuickbootLoad(reporter, "default_boot", /*success=*/false, state,
                        android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_CONFIG_MISMATCH_AVD);
    EXPECT_EQ(state.session_start_time, initial_session_start);

    ReportQuickbootLoad(reporter, "default_boot", /*success=*/true, state);
    EXPECT_GT(state.session_start_time, initial_session_start);

    ASSERT_TRUE(writer_ptr->WaitForEvents(2, absl::Seconds(5)));
    const absl::MutexLock lock(writer_ptr->mutex);
    ASSERT_EQ(writer_ptr->events.size(), 2u);
    EXPECT_EQ(writer_ptr->events[0].emulator_details().quickboot_load().state(),
              android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_FAILED);
    EXPECT_EQ(writer_ptr->events[0]
                      .emulator_details()
                      .quickboot_load()
                      .snapshot()
                      .load_failure_reason(),
              android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_CONFIG_MISMATCH_AVD);
    EXPECT_EQ(writer_ptr->events[1].emulator_details().quickboot_load().state(),
              android_studio::EmulatorQuickbootLoad::EMULATOR_QUICKBOOT_LOAD_SUCCEEDED);
}

}  // namespace goldfish::metrics
