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

#include "goldfish/metrics/metrics_reporter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "goldfish/metrics/testing/fake_metrics_writer.h"

namespace goldfish::metrics {

using ::goldfish::metrics::testing::FakeMetricsWriter;

TEST(MetricsReporterTest, ReportPing) {
    MetricsReporter reporter;
    auto writer = std::make_unique<FakeMetricsWriter>();
    auto* writer_ptr = writer.get();
    reporter.SetWriter(std::move(writer));

    reporter.Report([](android_studio::AndroidStudioEvent& /*event*/) {
        // No modification needed, SetBaseFields will set kind and session_id.
    });

    ASSERT_TRUE(writer_ptr->WaitForEvents(1, absl::Seconds(5)));

    absl::MutexLock lock(writer_ptr->mutex);
    ASSERT_EQ(writer_ptr->events.size(), 1);
    EXPECT_EQ(writer_ptr->events[0].kind(), android_studio::AndroidStudioEvent::EMULATOR_PING);
    EXPECT_EQ(writer_ptr->events[0].studio_session_id(), reporter.session_id());
}

TEST(MetricsReporterTest, ReportMultiple) {
    MetricsReporter reporter;
    auto writer = std::make_unique<FakeMetricsWriter>();
    auto* writer_ptr = writer.get();
    reporter.SetWriter(std::move(writer));

    for (int i = 0; i < 5; ++i) {
        reporter.Report([i](android_studio::AndroidStudioEvent& event) {
            event.set_kind(static_cast<android_studio::AndroidStudioEvent::EventKind>(i + 1));
        });
    }

    ASSERT_TRUE(writer_ptr->WaitForEvents(5, absl::Seconds(5)));

    absl::MutexLock lock(writer_ptr->mutex);
    ASSERT_EQ(writer_ptr->events.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(writer_ptr->events[i].kind(), i + 1);
    }
}

TEST(MetricsReporterTest, ReportWithCustomCallback) {
    MetricsReporter reporter;
    auto writer = std::make_unique<FakeMetricsWriter>();
    auto* writer_ptr = writer.get();
    reporter.SetWriter(std::move(writer));

    reporter.Report([](android_studio::AndroidStudioEvent& event) {
        event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_PING);
        auto* details = event.mutable_emulator_details();
        details->set_core_version("custom-version");
    });

    ASSERT_TRUE(writer_ptr->WaitForEvents(1, absl::Seconds(5)));

    absl::MutexLock lock(writer_ptr->mutex);
    ASSERT_EQ(writer_ptr->events.size(), 1);
    EXPECT_EQ(writer_ptr->events[0].emulator_details().core_version(), "custom-version");
}

TEST(MetricsReporterTest, NoWriterNoCrash) {
    MetricsReporter reporter;
    // No writer set.
    reporter.Report([](android_studio::AndroidStudioEvent& /*event*/) {});
    // Should not crash.
}

TEST(MetricsReporterTest, SetWriter) {
    MetricsReporter reporter;

    auto writer1 = std::make_unique<FakeMetricsWriter>();
    auto* writer1_ptr = writer1.get();
    reporter.SetWriter(std::move(writer1));

    reporter.Report([](android_studio::AndroidStudioEvent& event) {
        event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_PING);
    });
    ASSERT_TRUE(writer1_ptr->WaitForEvents(1, absl::Seconds(5)));
    {
        absl::MutexLock lock(writer1_ptr->mutex);
        EXPECT_EQ(writer1_ptr->events.size(), 1);
    }

    auto writer2 = std::make_unique<FakeMetricsWriter>();
    auto* writer2_ptr = writer2.get();
    // writer1 is destroyed here.
    reporter.SetWriter(std::move(writer2));

    reporter.Report([](android_studio::AndroidStudioEvent& event) {
        event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_PING);
    });
    ASSERT_TRUE(writer2_ptr->WaitForEvents(1, absl::Seconds(5)));

    {
        absl::MutexLock lock(writer2_ptr->mutex);
        EXPECT_EQ(writer2_ptr->events.size(), 1);
    }
}

namespace {

class LifecycleTrackingWriter final : public MetricsWriter {
  public:
    LifecycleTrackingWriter(absl::Notification* first_write_entered,
                            absl::Notification* allow_writes,
                            std::vector<int>* delivered_boot_times, std::atomic<bool>* destroyed)
            : first_write_entered_(first_write_entered)
            , allow_writes_(allow_writes)
            , delivered_boot_times_(delivered_boot_times)
            , destroyed_(destroyed) {}

    ~LifecycleTrackingWriter() override { destroyed_->store(true, std::memory_order_release); }

    void Write(MetricsEvent event) override {
        if (!first_write_entered_->HasBeenNotified()) {
            first_write_entered_->Notify();
        }
        allow_writes_->WaitForNotificationWithTimeout(absl::Seconds(5));
        delivered_boot_times_->push_back(
                static_cast<int>(event.as_event.emulator_details().wall_time()));
    }

  private:
    absl::Notification* first_write_entered_;
    absl::Notification* allow_writes_;
    std::vector<int>* delivered_boot_times_;
    std::atomic<bool>* destroyed_;
};

}  // namespace

TEST(MetricsReporterTest, ShutdownDrainsPendingEventsAndDestroysWriterSynchronously) {
    MetricsReporter reporter;
    absl::Notification first_write_entered;
    absl::Notification allow_writes;
    std::vector<int> delivered_values;
    std::atomic<bool> writer_destroyed{false};

    reporter.SetWriter(std::make_unique<LifecycleTrackingWriter>(
            &first_write_entered, &allow_writes, &delivered_values, &writer_destroyed));

    reporter.Report([](android_studio::AndroidStudioEvent& event) {
        event.mutable_emulator_details()->set_wall_time(101);
    });
    ASSERT_TRUE(first_write_entered.WaitForNotificationWithTimeout(absl::Seconds(5)));

    // Queue two more events while the worker thread is mid-Write() on event 1.
    // This also verifies Report() does not deadlock on a slow writer->Write().
    absl::Notification queued_during_write;
    std::thread producer([&]() {
        reporter.Report([](android_studio::AndroidStudioEvent& event) {
            event.mutable_emulator_details()->set_wall_time(202);
        });
        reporter.Report([](android_studio::AndroidStudioEvent& event) {
            event.mutable_emulator_details()->set_wall_time(303);
        });
        queued_during_write.Notify();
    });
    EXPECT_TRUE(queued_during_write.WaitForNotificationWithTimeout(absl::Seconds(2)));
    producer.join();

    absl::Notification shutdown_finished;
    std::thread shutdown_thread([&]() {
        reporter.Shutdown();
        shutdown_finished.Notify();
    });

    // Shutdown must wait for in-flight and queued events to finish writing.
    EXPECT_FALSE(shutdown_finished.WaitForNotificationWithTimeout(absl::Milliseconds(50)));
    allow_writes.Notify();
    ASSERT_TRUE(shutdown_finished.WaitForNotificationWithTimeout(absl::Seconds(5)));
    shutdown_thread.join();

    // Immediately after Shutdown() returns (while `reporter` is still alive),
    // all queued events must have been written and the writer must be destroyed.
    EXPECT_TRUE(writer_destroyed.load(std::memory_order_acquire));
    ASSERT_EQ(delivered_values.size(), 3U);
    EXPECT_EQ(delivered_values[0], 101);
    EXPECT_EQ(delivered_values[1], 202);
    EXPECT_EQ(delivered_values[2], 303);

    // Post-Shutdown Report() drops events, SetWriter() destroys the new writer
    // immediately, and repeated Shutdown() calls are safe no-ops.
    std::atomic<bool> late_writer_destroyed{false};
    reporter.SetWriter(std::make_unique<LifecycleTrackingWriter>(
            &first_write_entered, &allow_writes, &delivered_values, &late_writer_destroyed));
    EXPECT_TRUE(late_writer_destroyed.load(std::memory_order_acquire));

    reporter.Report([](android_studio::AndroidStudioEvent& event) {
        event.mutable_emulator_details()->set_wall_time(404);
    });
    reporter.Shutdown();
    EXPECT_EQ(delivered_values.size(), 3U);
}

#if defined(GTEST_HAS_DEATH_TEST) && !defined(NDEBUG)
namespace {

class SelfShuttingDownWriter final : public MetricsWriter {
  public:
    explicit SelfShuttingDownWriter(MetricsReporter* reporter) : reporter_(reporter) {}

    void Write(MetricsEvent /*event*/) override { reporter_->Shutdown(); }

  private:
    MetricsReporter* reporter_;
};

}  // namespace

TEST(MetricsReporterDeathTest, ShutdownFromWorkerThreadDiesInDebugBuilds) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
            {
                MetricsReporter reporter;
                reporter.SetWriter(std::make_unique<SelfShuttingDownWriter>(&reporter));
                reporter.Report([](android_studio::AndroidStudioEvent& /*event*/) {});
                reporter.Shutdown();
            },
            "must not be called from its worker thread");
}
#endif

}  // namespace goldfish::metrics
