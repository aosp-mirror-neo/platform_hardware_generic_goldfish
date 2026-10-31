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

#include "goldfish/legacy/goldfish_gpu/transport/opengles_pipe_table.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/status/status_matcher_macros.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_streams.h"
#include "goldfish/legacy/goldfish_gpu/testing/mock_gfxstream.h"

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::goldfish::legacy::testing::MockRenderChannel;
using ::goldfish::legacy::testing::MockRenderer;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr uint8_t kPayload[] = {1, 2, 3, 4};

TEST(OpenglesPipeTableTest, CreateWriteReadClose) {
    MockRenderer renderer;
    OpenglesPipeTable table;
    EXPECT_EQ(table.size(), 0U);
    EXPECT_EQ(table.NextId(), 1U);

    absl::Notification got;
    std::vector<uint8_t> received;
    ASSERT_OK_AND_ASSIGN(const uint32_t id, table.Create(
                                                    &renderer,
                                                    [&](const uint8_t* data, size_t size) {
                                                        received.assign(data, data + size);
                                                        got.Notify();
                                                    },
                                                    /*on_writable=*/nullptr));
    EXPECT_EQ(id, 1U);
    EXPECT_EQ(table.NextId(), 2U);
    EXPECT_EQ(table.size(), 1U);
    ASSERT_EQ(renderer.channels.size(), 1U);

    const OpenglesPipeTable::View view = table.Snapshot();
    ASSERT_EQ(view.pipes.size(), 1U);
    EXPECT_TRUE(view.pipes[0].reader_started);
    EXPECT_FALSE(view.pipes[0].paused_for_snapshot);
    EXPECT_FALSE(view.pipes[0].closing);

    ASSERT_OK(table.Write(id, kPayload, sizeof(kPayload)));
    ASSERT_TRUE(got.WaitForNotificationWithTimeout(absl::Seconds(5)));
    EXPECT_THAT(received, ElementsAre(1, 2, 3, 4));

    EXPECT_THAT(table.Write(99, kPayload, 4), StatusIs(absl::StatusCode::kNotFound));
    table.Close(id);
    EXPECT_EQ(table.size(), 0U);
    table.Close(id);  // idempotent
}

TEST(OpenglesPipeTableTest, WriteOnFullChannelReturnsUnavailableThenWritable) {
    MockRenderer renderer;
    OpenglesPipeTable table;

    absl::Notification reader_entered;
    absl::Notification release_reader;
    absl::Notification writable;
    absl::Notification third_read;
    std::atomic<int> reads{0};
    ASSERT_OK_AND_ASSIGN(const uint32_t id, table.Create(
                                                    &renderer,
                                                    [&](const uint8_t*, size_t) {
                                                        const int n = reads.fetch_add(1);
                                                        if (n == 0) {
                                                            // Hold the reader so the channel can
                                                            // fill up.
                                                            reader_entered.Notify();
                                                            release_reader.WaitForNotification();
                                                        } else if (n == 2) {
                                                            third_read.Notify();
                                                        }
                                                    },
                                                    [&]() {
                                                        if (!writable.HasBeenNotified()) {
                                                            writable.Notify();
                                                        }
                                                    }));
    ASSERT_EQ(renderer.channels.size(), 1U);
    renderer.channels[0]->SetCapacity(1);

    // First write is drained by the reader, which then blocks in the callback.
    ASSERT_OK(table.Write(id, kPayload, sizeof(kPayload)));
    ASSERT_TRUE(reader_entered.WaitForNotificationWithTimeout(absl::Seconds(5)));
    EXPECT_TRUE(table.Snapshot().pipes[0].reading_in_progress);
    // Second write fills the (capacity 1) channel.
    ASSERT_OK(table.Write(id, kPayload, sizeof(kPayload)));
    // Third write must not block: UNAVAILABLE, naming the pipe and the size.
    const absl::Status full = table.Write(id, kPayload, sizeof(kPayload));
    EXPECT_THAT(full, StatusIs(absl::StatusCode::kUnavailable, HasSubstr("4-byte")));
    EXPECT_THAT(full.message(), HasSubstr(std::to_string(id)));
    EXPECT_FALSE(writable.HasBeenNotified());

    // Draining the channel fires the writable callback; the retry succeeds.
    release_reader.Notify();
    ASSERT_TRUE(writable.WaitForNotificationWithTimeout(absl::Seconds(5)));
    ASSERT_OK(table.Write(id, kPayload, sizeof(kPayload)));
    ASSERT_TRUE(third_read.WaitForNotificationWithTimeout(absl::Seconds(5)));
    table.Close(id);
    EXPECT_EQ(reads.load(), 3);
}

TEST(OpenglesPipeTableTest, SpaceFreedBetweenFullWriteAndRearmIsNotMissed) {
    // The lost-wakeup window: tryWrite() sees a full channel, then the render
    // thread frees space *before* the caller re-arms CanWrite. The re-arm
    // consumes the CanWrite event immediately, so if TryWrite() only reported
    // UNAVAILABLE the caller would wait for a callback that already fired.
    // TryWrite() must instead retry once after re-arming and succeed.
    MockRenderer renderer;
    OpenglesPipeTable table;
    absl::Notification reader_entered;
    absl::Notification release_reader;
    ASSERT_OK_AND_ASSIGN(const uint32_t id,
                         table.Create(
                                 &renderer,
                                 [&](const uint8_t*, size_t) {
                                     if (!reader_entered.HasBeenNotified()) {
                                         reader_entered.Notify();
                                         release_reader.WaitForNotification();
                                     }
                                 },
                                 /*on_writable=*/nullptr));
    MockRenderChannel& channel = *renderer.channels[0];
    channel.SetCapacity(1);
    ASSERT_OK(table.Write(id, kPayload, sizeof(kPayload)));  // drained by the (parked) reader
    ASSERT_TRUE(reader_entered.WaitForNotificationWithTimeout(absl::Seconds(5)));
    ASSERT_OK(table.Write(id, kPayload, sizeof(kPayload)));  // fills the channel
    ASSERT_EQ(channel.QueuedForTest(), 1U);

    // Free the slot inside the window between "full" and the re-arm.
    int hook_runs = 0;
    channel.SetOnceOnFullWrite([&] {
        ++hook_runs;
        EXPECT_TRUE(channel.PopOneForTest());
    });
    EXPECT_OK(table.Write(id, kPayload, sizeof(kPayload)));
    EXPECT_EQ(hook_runs, 1);
    EXPECT_EQ(channel.QueuedForTest(), 1U) << "the retried write was queued";

    // Without the window the plain full case still reports UNAVAILABLE.
    EXPECT_THAT(table.Write(id, kPayload, sizeof(kPayload)),
                StatusIs(absl::StatusCode::kUnavailable));
    release_reader.Notify();
    table.Close(id);
}

TEST(OpenglesPipeTableTest, ConcurrentWritersAreAllDelivered) {
    MockRenderer renderer;
    OpenglesPipeTable table;
    constexpr int kNumWriters = 4;
    constexpr int kWritesPerThread = 25;
    constexpr int kExpectedBytes = kNumWriters * kWritesPerThread * 4;
    std::atomic<int> total{0};
    absl::Notification all_read;
    ASSERT_OK_AND_ASSIGN(const uint32_t id,
                         table.Create(
                                 &renderer,
                                 [&](const uint8_t*, size_t size) {
                                     if (total.fetch_add(static_cast<int>(size)) +
                                                 static_cast<int>(size) >=
                                         kExpectedBytes) {
                                         if (!all_read.HasBeenNotified()) {
                                             all_read.Notify();
                                         }
                                     }
                                 },
                                 nullptr));
    std::atomic<bool> start{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < kNumWriters; ++t) {
        writers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kWritesPerThread; ++i) {
                EXPECT_OK(table.Write(id, kPayload, sizeof(kPayload)));
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& w : writers) {
        w.join();
    }
    EXPECT_TRUE(all_read.WaitForNotificationWithTimeout(absl::Seconds(5)));
    table.Close(id);
}

TEST(OpenglesPipeTableTest, PauseWaitsForInFlightReadAndBlocksFurtherReads) {
    MockRenderer renderer;
    OpenglesPipeTable table;
    absl::Notification in_read;
    absl::Notification release_read;
    std::atomic<int> reads{0};
    ASSERT_OK_AND_ASSIGN(const uint32_t id, table.Create(
                                                    &renderer,
                                                    [&](const uint8_t*, size_t) {
                                                        if (reads.fetch_add(1) == 0) {
                                                            in_read.Notify();
                                                            release_read.WaitForNotification();
                                                        }
                                                    },
                                                    nullptr));
    ASSERT_OK(table.Write(id, kPayload, 4));
    ASSERT_TRUE(in_read.WaitForNotificationWithTimeout(absl::Seconds(5)));

    // PauseAll() must not return while the reader is inside on_read.
    absl::Notification paused;
    std::thread pauser([&] {
        table.PauseAll();
        paused.Notify();
    });
    EXPECT_FALSE(paused.WaitForNotificationWithTimeout(absl::Milliseconds(200)));
    release_read.Notify();
    ASSERT_TRUE(paused.WaitForNotificationWithTimeout(absl::Seconds(5)));
    pauser.join();
    EXPECT_TRUE(table.Snapshot().pipes[0].paused_for_snapshot);
    EXPECT_FALSE(table.Snapshot().pipes[0].reading_in_progress);

    // Data queued while paused stays in the channel until ResumeAll().
    ASSERT_OK(table.Write(id, kPayload, 4));
    absl::SleepFor(absl::Milliseconds(100));
    EXPECT_EQ(reads.load(), 1);
    table.ResumeAll();
    for (int i = 0; i < 100 && reads.load() < 2; ++i) {
        absl::SleepFor(absl::Milliseconds(20));
    }
    EXPECT_EQ(reads.load(), 2);
    table.Close(id);
}

TEST(OpenglesPipeTableTest, LoadEntryCreatesPausedChannelAndResumeStartsItsReader) {
    MockRenderer renderer;
    OpenglesPipeTable table;
    std::vector<uint8_t> bytes;
    BufferStream out(&bytes);
    out.putBe32(0xCAFE1234);
    BufferStream in(bytes.data(), bytes.size());

    ASSERT_OK(table.LoadEntry(&renderer, 7, &in));
    EXPECT_EQ(table.NextId(), 8U);
    EXPECT_THAT(table.SortedIds(), ElementsAre(7));
    OpenglesPipeTable::View view = table.Snapshot();
    ASSERT_EQ(view.pipes.size(), 1U);
    EXPECT_TRUE(view.pipes[0].paused_for_snapshot);
    EXPECT_FALSE(view.pipes[0].reader_started);
    EXPECT_EQ(renderer.channels[0]->SavedTag(), 0xCAFE1234U);

    BufferStream in2(bytes.data(), bytes.size());
    EXPECT_THAT(table.LoadEntry(&renderer, 7, &in2), StatusIs(absl::StatusCode::kDataLoss));

    // Restore() on a channel that already exists attaches the callbacks
    // directly: nothing is staged, so there is no pending restore to match.
    absl::Notification got;
    ASSERT_OK(table.Restore(7, [&](const uint8_t*, size_t) { got.Notify(); }, nullptr));
    ASSERT_OK(table.VerifyNoPendingRestores());
    table.ResumeAll();
    view = table.Snapshot();
    EXPECT_TRUE(view.pipes[0].reader_started);
    EXPECT_FALSE(view.pipes[0].paused_for_snapshot);
    ASSERT_OK(table.Write(7, kPayload, 4));
    ASSERT_TRUE(got.WaitForNotificationWithTimeout(absl::Seconds(5)));

    std::vector<uint8_t> saved;
    BufferStream save_out(&saved);
    table.SaveEntry(&renderer, 7, &save_out);
    EXPECT_EQ(saved.size(), 4U);
    table.Reset();
    EXPECT_EQ(table.size(), 0U);
    EXPECT_EQ(table.NextId(), 1U);
}

TEST(OpenglesPipeTableTest, RestoreBeforeLoadEntryAttachesCallbackAndDetectsUnmatchedRestores) {
    MockRenderer renderer;
    OpenglesPipeTable table;
    EXPECT_THAT(table.Restore(0, nullptr, nullptr), StatusIs(absl::StatusCode::kInvalidArgument));

    // Load, device half (goldfish_pipe, MIG_PRI_DEFAULT): the channels do not
    // exist yet, so the callbacks are staged.
    absl::Notification got;
    ASSERT_OK(table.Restore(7, [&](const uint8_t*, size_t) { got.Notify(); }, nullptr));
    ASSERT_OK(table.Restore(9, nullptr, nullptr));
    EXPECT_EQ(table.size(), 0U);
    EXPECT_THAT(table.VerifyNoPendingRestores(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("pipe channel 7")));

    // Closing a rolled-back pipe before LoadEntry drops its staged restore.
    table.Close(9);

    // Load, GPU half (MIG_PRI_LOW): LoadEntry creates the channel and attaches
    // the staged callbacks.
    std::vector<uint8_t> bytes;
    BufferStream out(&bytes);
    out.putBe32(0xCAFE1234);
    BufferStream in(bytes.data(), bytes.size());
    ASSERT_OK(table.LoadEntry(&renderer, 7, &in));
    ASSERT_OK(table.VerifyNoPendingRestores());

    table.ResumeAll();
    ASSERT_OK(table.Write(7, kPayload, 4));
    ASSERT_TRUE(got.WaitForNotificationWithTimeout(absl::Seconds(5)));
    table.Reset();
}

TEST(OpenglesPipeTableTest, StoppedChannelMakesWriteFailAndStopAllJoinsReaders) {
    MockRenderer renderer;
    OpenglesPipeTable table;
    ASSERT_OK_AND_ASSIGN(const uint32_t a, table.Create(&renderer, nullptr, nullptr));
    ASSERT_OK_AND_ASSIGN(const uint32_t b, table.Create(&renderer, nullptr, nullptr));
    EXPECT_THAT(table.SortedIds(), ElementsAre(a, b));
    renderer.channels[0]->stop();
    EXPECT_THAT(table.Write(a, kPayload, 4),
                StatusIs(absl::StatusCode::kInternal, HasSubstr("stopped")));
    table.StopAll();
    EXPECT_EQ(table.size(), 0U);
    EXPECT_EQ(table.NextId(), 3U) << "StopAll keeps the id counter";
    table.RaiseNextId(10);
    EXPECT_EQ(table.NextId(), 10U);
    table.RaiseNextId(5);
    EXPECT_EQ(table.NextId(), 10U);
}

TEST(OpenglesPipeTableTest, CloseFromInsideOnReadDoesNotDeadlock) {
    MockRenderer renderer;
    auto table = std::make_unique<OpenglesPipeTable>();
    absl::Notification closed;
    uint32_t id = 0;
    ASSERT_OK_AND_ASSIGN(id, table->Create(
                                     &renderer,
                                     [&](const uint8_t*, size_t) {
                                         // The guest closing the pipe while data is
                                         // being delivered runs Close() on the reader
                                         // thread itself: it must neither self-join
                                         // nor throw, and the channel must be gone
                                         // from the table when it returns.
                                         table->Close(id);
                                         EXPECT_EQ(table->size(), 0U);
                                         closed.Notify();
                                     },
                                     nullptr));
    ASSERT_OK(table->Write(id, kPayload, 4));
    ASSERT_TRUE(closed.WaitForNotificationWithTimeout(absl::Seconds(5)));
    EXPECT_THAT(table->Write(id, kPayload, 4), StatusIs(absl::StatusCode::kNotFound));
    absl::Notification destroyed;
    std::thread t([&] {
        table.reset();
        destroyed.Notify();
    });
    EXPECT_TRUE(destroyed.WaitForNotificationWithTimeout(absl::Seconds(5)));
    t.join();
    // The reader was detached by Close(); it keeps the PipeChannel (and thus
    // the mock channel) alive until ReaderLoop() returns. Wait for that so the
    // thread does not outlive `renderer`.
    const absl::Time deadline = absl::Now() + absl::Seconds(5);
    while (renderer.channels[0].use_count() > 1 && absl::Now() < deadline) {
        absl::SleepFor(absl::Milliseconds(1));
    }
    EXPECT_EQ(renderer.channels[0].use_count(), 1) << "detached reader still running";
}

}  // namespace
}  // namespace goldfish::legacy
