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

#include "goldfish/legacy/goldfish_gpu/transport/asg_blob_table.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "android/status/status_matcher_macros.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_streams.h"
#include "goldfish/legacy/goldfish_gpu/testing/mock_gfxstream.h"

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::goldfish::legacy::testing::MockAsgConsumer;
using ::goldfish::legacy::testing::MockRenderer;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr uint64_t kBlobSize = kAsgRingRegionSize + 65536;

AsgBlobTable MakeTable() {
    return AsgBlobTable([](uint64_t gpa) { return reinterpret_cast<void*>(gpa + 0x1000); });
}

TEST(AsgBlobTableTest, CreateStartPingCloseDestroy) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    EXPECT_EQ(table.NextId(), 1U);

    ASSERT_OK_AND_ASSIGN(const AsgBlob blob, table.Create(kBlobSize));
    EXPECT_EQ(blob.context_handle, 1U);
    EXPECT_EQ(blob.blob_id, 1U);
    EXPECT_EQ(blob.size, kBlobSize);
    ASSERT_NE(blob.external_addr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(blob.external_addr) % kAsgStorageAlignment, 0U);
    EXPECT_EQ(table.NextId(), 2U);
    EXPECT_THAT(table.SortedIds(), IsEmpty()) << "no consumer yet";

    EXPECT_THAT(table.Start(nullptr, blob), StatusIs(absl::StatusCode::kFailedPrecondition));
    ASSERT_OK(table.Start(&renderer, blob));
    ASSERT_OK(table.Start(&renderer, blob)) << "idempotent";
    ASSERT_EQ(renderer.asg_consumers.size(), 1U);
    const MockAsgConsumer* consumer = renderer.asg_consumers[0].get();
    EXPECT_EQ(consumer->info.buffer, reinterpret_cast<char*>(blob.external_addr));
    EXPECT_EQ(consumer->info.buffer_size, 65536U);
    EXPECT_EQ(consumer->info.ring_storage, reinterpret_cast<char*>(blob.external_addr + 65536));
    EXPECT_EQ(consumer->info.callbacks.getPtr(0x10), reinterpret_cast<char*>(0x1010));
    EXPECT_THAT(table.SortedIds(), ElementsAre(1));

    ASSERT_OK(table.Ping(blob));
    AsgBlobTable::View view = table.Snapshot();
    ASSERT_EQ(view.registered.size(), 1U);
    EXPECT_TRUE(view.registered[0].second.wakeup_requested);
    EXPECT_EQ(view.registered[0].second.consumer, consumer);
    EXPECT_EQ(view.blobs.size(), 1U);
    EXPECT_EQ(view.blobs[0].first, blob.blob_id);
    EXPECT_EQ(view.blobs[0].second.entry, view.registered[0].second.entry);

    std::shared_ptr<AsgConsumerEntry> entry = table.Close(blob, /*retire=*/false);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(table.Snapshot().registered.empty());
    EXPECT_TRUE(table.Snapshot().blobs.empty());
    AsgBlobTable::DestroyConsumers(&renderer, {entry});
    EXPECT_EQ(renderer.DestroyCount(const_cast<MockAsgConsumer*>(consumer)), 1);
    EXPECT_EQ(table.Close(blob, false), nullptr) << "already closed";
    EXPECT_THAT(table.Ping(blob), StatusIs(absl::StatusCode::kNotFound));
    EXPECT_THAT(table.Start(&renderer, blob), StatusIs(absl::StatusCode::kNotFound));
}

// goldfish_address_space rounds the region up to the host page size; the
// tail padding must not leak into buffer_size, which the guest uses as a
// power-of-two ring mask.
TEST(AsgBlobTableTest, PageAlignmentPaddingDoesNotChangeThePowerOfTwoBufferSize) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob blob, table.Create(kBlobSize + 4096));
    ASSERT_OK(table.Start(&renderer, blob));
    const MockAsgConsumer* consumer = renderer.asg_consumers[0].get();
    EXPECT_EQ(consumer->info.buffer, reinterpret_cast<char*>(blob.external_addr));
    EXPECT_EQ(consumer->info.buffer_size, 65536U);
    EXPECT_EQ(consumer->info.ring_storage, reinterpret_cast<char*>(blob.external_addr + 65536));
}

TEST(AsgBlobTableTest, RestoreRejectsHandleZeroAndTinyBlobs) {
    AsgBlobTable table = MakeTable();
    EXPECT_THAT(table.Restore(0, kBlobSize).status(),
                StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(table.Restore(5, kAsgRingRegionSize).status(),
                StatusIs(absl::StatusCode::kInvalidArgument));
    ASSERT_OK_AND_ASSIGN(const AsgBlob blob, table.Restore(5, kBlobSize));
    EXPECT_EQ(blob.context_handle, 5U);
    EXPECT_EQ(table.NextId(), 6U) << "Restore raises the handle counter past the handle";
    table.RaiseNextId(3);
    EXPECT_EQ(table.NextId(), 6U);
    table.RaiseNextId(9);
    EXPECT_EQ(table.NextId(), 9U);
}

TEST(AsgBlobTableTest, StaleBlobIdDoesNotCloseTheReplacement) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob old_blob, table.Create(kBlobSize));
    ASSERT_OK(table.Start(&renderer, old_blob));
    // Same-session reload: the address space device restores the same handle
    // before the stale old blob is destroyed.
    ASSERT_OK_AND_ASSIGN(const AsgBlob new_blob, table.Restore(old_blob.context_handle, kBlobSize));
    EXPECT_NE(new_blob.blob_id, old_blob.blob_id);
    ASSERT_OK(table.Start(&renderer, new_blob));
    ASSERT_EQ(renderer.asg_consumers.size(), 2U);

    // The old blob only closes its own (orphaned) blob-id entry.
    std::shared_ptr<AsgConsumerEntry> old_entry = table.Close(old_blob, false);
    ASSERT_NE(old_entry, nullptr);
    EXPECT_EQ(old_entry->blob_id, old_blob.blob_id);
    AsgBlobTable::View view = table.Snapshot();
    ASSERT_EQ(view.registered.size(), 1U);
    EXPECT_EQ(view.registered[0].second.blob_id, new_blob.blob_id);
    ASSERT_OK(table.Ping(new_blob));

    // A blob_id of 0 (legacy caller) matches by handle alone.
    AsgBlob by_handle = new_blob;
    by_handle.blob_id = 0;
    EXPECT_NE(table.Close(by_handle, false), nullptr);
    EXPECT_TRUE(table.Snapshot().blobs.empty());
}

TEST(AsgBlobTableTest, RetireAndDrain) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob blob, table.Create(kBlobSize));
    ASSERT_OK(table.Start(&renderer, blob));
    void* consumer = renderer.asg_consumers[0].get();

    EXPECT_EQ(table.Close(blob, /*retire=*/true), nullptr);
    EXPECT_EQ(renderer.DestroyCount(consumer), 0) << "retire must not touch the renderer";
    EXPECT_EQ(table.Snapshot().retired_count, 1U);
    table.DrainRetired(&renderer);
    EXPECT_EQ(renderer.DestroyCount(consumer), 1);
    EXPECT_EQ(table.Snapshot().retired_count, 0U);
}

TEST(AsgBlobTableTest, SnapshotHelpersDriveEveryConsumer) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob a, table.Create(kBlobSize));
    ASSERT_OK_AND_ASSIGN(const AsgBlob b, table.Create(kBlobSize));
    ASSERT_OK_AND_ASSIGN(const AsgBlob idle, table.Create(kBlobSize));
    ASSERT_OK(table.Start(&renderer, b));
    ASSERT_OK(table.Start(&renderer, a));
    EXPECT_THAT(table.SortedIds(), ElementsAre(a.context_handle, b.context_handle));
    (void)idle;

    table.PauseAll(renderer);
    for (const auto& c : renderer.asg_consumers) {
        EXPECT_TRUE(c->pre_saved);
    }
    for (const auto& [handle, e] : table.Snapshot().registered) {
        if (e.consumer != nullptr) {
            EXPECT_TRUE(e.pause_pre_snapshot_requested);
            EXPECT_FALSE(e.resume_post_snapshot_requested);
        }
    }
    std::vector<uint8_t> sink_bytes;
    BufferStream sink(&sink_bytes);
    table.WaitAllPaused(renderer, &sink);
    EXPECT_EQ(sink_bytes.size(), 8U) << "one be32 marker per consumer";

    std::vector<uint8_t> saved;
    BufferStream out(&saved);
    table.SaveEntry(&renderer, a.context_handle, &out);
    table.SaveEntry(&renderer, idle.context_handle, &out);  // no consumer: nothing written
    table.SaveEntry(&renderer, 999, &out);                  // unknown: nothing written
    EXPECT_EQ(saved.size(), 4U);

    table.ResumeAll(renderer);
    for (const auto& c : renderer.asg_consumers) {
        EXPECT_TRUE(c->post_saved);
    }
    table.ReloadRingConfigs(renderer);
    for (const auto& c : renderer.asg_consumers) {
        EXPECT_TRUE(c->ring_config_reloaded);
    }
}

TEST(AsgBlobTableTest, RestoreConsumerFromStream) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    std::vector<uint8_t> bytes;
    BufferStream out(&bytes);
    out.putBe32(0xA5600042);

    BufferStream in1(bytes.data(), bytes.size());
    EXPECT_THAT(table.LoadEntry(&renderer, 7, &in1), StatusIs(absl::StatusCode::kDataLoss))
            << "handle was never restored";

    ASSERT_OK(table.Restore(7, kBlobSize));
    BufferStream in2(bytes.data(), bytes.size());
    ASSERT_OK(table.LoadEntry(&renderer, 7, &in2));
    ASSERT_EQ(renderer.asg_consumers.size(), 1U);
    EXPECT_EQ(renderer.asg_consumers[0]->saved_marker, 0xA5600042U);
    EXPECT_TRUE(renderer.asg_consumers[0]->post_load_registered);

    BufferStream in3(bytes.data(), bytes.size());
    EXPECT_THAT(table.LoadEntry(&renderer, 7, &in3), StatusIs(absl::StatusCode::kDataLoss))
            << "repeated handle";
}

TEST(AsgBlobTableTest, PreLoadSequenceDestroysConsumersButKeepsEntries) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob live, table.Create(kBlobSize));
    ASSERT_OK_AND_ASSIGN(const AsgBlob gone, table.Create(kBlobSize));
    ASSERT_OK(table.Start(&renderer, live));
    ASSERT_OK(table.Start(&renderer, gone));
    void* live_consumer = renderer.asg_consumers[0].get();
    void* gone_consumer = renderer.asg_consumers[1].get();
    ASSERT_OK(table.Ping(live));
    EXPECT_EQ(table.Close(gone, /*retire=*/true), nullptr);

    table.MarkAllExiting();
    for (const auto& [handle, e] : table.Snapshot().registered) {
        EXPECT_TRUE(e.exiting);
    }
    table.StopAll(&renderer);
    EXPECT_EQ(renderer.DestroyCount(live_consumer), 1);
    EXPECT_EQ(renderer.DestroyCount(gone_consumer), 1);
    AsgBlobTable::View view = table.Snapshot();
    ASSERT_EQ(view.registered.size(), 1U);
    EXPECT_EQ(view.registered[0].first, live.context_handle);
    EXPECT_EQ(view.registered[0].second.consumer, nullptr);
    EXPECT_FALSE(view.registered[0].second.exiting);
    EXPECT_FALSE(view.registered[0].second.wakeup_requested);
    EXPECT_EQ(view.retired_count, 0U);
    EXPECT_EQ(table.NextId(), 3U);
}

TEST(AsgBlobTableTest, ResetClearsHandlesButNotBlobIds) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob a, table.Create(kBlobSize));
    ASSERT_OK_AND_ASSIGN(const AsgBlob b, table.Create(kBlobSize));
    ASSERT_OK(table.Start(&renderer, a));
    EXPECT_EQ(table.Close(b, true), nullptr);
    std::vector<std::shared_ptr<AsgConsumerEntry>> all = table.Reset();
    EXPECT_EQ(all.size(), 2U);
    EXPECT_EQ(table.NextId(), 1U);
    EXPECT_TRUE(table.Snapshot().registered.empty());
    EXPECT_EQ(table.Snapshot().retired_count, 0U);
    AsgBlobTable::DestroyConsumers(&renderer, all);
    EXPECT_TRUE(renderer.asg_consumers.empty());
    ASSERT_OK_AND_ASSIGN(const AsgBlob c, table.Create(kBlobSize));
    EXPECT_EQ(c.context_handle, 1U);
    EXPECT_EQ(c.blob_id, 3U) << "blob ids stay process-unique";
}

TEST(AsgBlobTableTest, OnUnavailableReadPriorityIsExitPauseResumeWakeup) {
    AsgConsumerEntry entry;
    {
        absl::MutexLock lock(&entry.mu);
        entry.wakeup_requested = true;
        entry.resume_post_snapshot_requested = true;
        entry.pause_pre_snapshot_requested = true;
    }
    EXPECT_EQ(entry.OnUnavailableRead(), gfxstream::AsgOnUnavailableReadStatus::kPauseForSnapshot);
    EXPECT_EQ(entry.OnUnavailableRead(),
              gfxstream::AsgOnUnavailableReadStatus::kResumeAfterSnapshot);
    EXPECT_EQ(entry.OnUnavailableRead(), gfxstream::AsgOnUnavailableReadStatus::kContinue);
    {
        absl::MutexLock lock(&entry.mu);
        EXPECT_FALSE(entry.wakeup_requested);
        entry.wakeup_requested = true;
        entry.exiting = true;
    }
    EXPECT_EQ(entry.OnUnavailableRead(), gfxstream::AsgOnUnavailableReadStatus::kExit);
    EXPECT_EQ(entry.OnUnavailableRead(), gfxstream::AsgOnUnavailableReadStatus::kExit)
            << "exiting is sticky";
}

TEST(AsgBlobTableTest, OnUnavailableReadBlocksUntilWork) {
    auto entry = std::make_shared<AsgConsumerEntry>();
    absl::Notification returned;
    std::thread render_thread([&] {
        EXPECT_EQ(entry->OnUnavailableRead(), gfxstream::AsgOnUnavailableReadStatus::kContinue);
        returned.Notify();
    });
    EXPECT_FALSE(returned.WaitForNotificationWithTimeout(absl::Milliseconds(100)));
    {
        absl::MutexLock lock(&entry->mu);
        entry->wakeup_requested = true;
    }
    EXPECT_TRUE(returned.WaitForNotificationWithTimeout(absl::Seconds(5)));
    render_thread.join();
}

// An RCU-deferred Close() (BQL held) racing a PreLoad that dropped the BQL
// must destroy the consumer exactly once. Interleaving forced: the pre-load
// path is inside addressSpaceGraphicsConsumerDestroy() (held open by
// `destroy_delay`) when Close() hands the same entry to a second destroyer.
// Fails without TakeConsumerForDestroy(): both see the live pointer.
TEST(AsgBlobTableTest, ConcurrentCloseAndPreLoadDestroyConsumerOnce) {
    MockRenderer renderer;
    AsgBlobTable table = MakeTable();
    ASSERT_OK_AND_ASSIGN(const AsgBlob blob, table.Create(kBlobSize));
    ASSERT_OK(table.Start(&renderer, blob));
    void* const consumer = renderer.asg_consumers[0].get();

    absl::Notification destroy_started;
    {
        absl::MutexLock lock(&renderer.destroy_mu);
        renderer.on_destroy_started = &destroy_started;
    }
    renderer.destroy_delay = absl::Milliseconds(200);

    std::thread pre_loader([&] {
        table.MarkAllExiting();
        table.StopAll(&renderer);
    });
    ASSERT_TRUE(destroy_started.WaitForNotificationWithTimeout(absl::Seconds(5)));
    // The pre-loader is blocked inside the (first) destroy of `consumer`.
    std::shared_ptr<AsgConsumerEntry> entry = table.Close(blob, /*retire=*/false);
    ASSERT_NE(entry, nullptr);
    AsgBlobTable::DestroyConsumers(&renderer, {entry});
    pre_loader.join();
    {
        absl::MutexLock lock(&renderer.destroy_mu);
        renderer.on_destroy_started = nullptr;
    }
    EXPECT_EQ(renderer.DestroyCount(consumer), 1);
    EXPECT_TRUE(renderer.asg_consumers.empty());
}

}  // namespace
}  // namespace goldfish::legacy
