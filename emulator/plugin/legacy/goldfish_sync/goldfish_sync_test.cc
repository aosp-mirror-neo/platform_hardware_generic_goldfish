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

#include "goldfish_sync.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"

#include "android/status/status_matcher_macros.h"
#include "goldfish/archive/deque_archive.h"
#include "goldfish/legacy/common/fake_guest_memory.h"
#include "goldfish/legacy/goldfish_gpu/fake_goldfish_gpu.h"

struct QEMUFile {
    goldfish::archive::DequeArchive archive;
};

extern "C" size_t qemu_get_buffer(  // NOLINT(readability-identifier-naming)
        QEMUFile* f, uint8_t* buf, size_t size) {
    if (f == nullptr || buf == nullptr || size == 0) {
        return 0;
    }
    const size_t n = std::min(size, f->archive.storage.size());
    if (n > 0) {
        (void)f->archive.Read(buf, n);
    }
    return n;
}

extern "C" void qemu_put_buffer(  // NOLINT(readability-identifier-naming)
        QEMUFile* f, const uint8_t* buf, size_t size) {
    if (f != nullptr && buf != nullptr && size > 0) {
        f->archive.Write(buf, size);
    }
}

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

class TestGoldfishGpu : public FakeGoldfishGpu {
  public:
    TestGoldfishGpu() = default;
    explicit TestGoldfishGpu(bool keep_stale_callback)
            : keep_stale_callback_(keep_stale_callback) {}

    void SetTimelineIncCallback(TimelineIncCallback callback) override {
        if (callback == nullptr && keep_stale_callback_) {
            return;
        }
        timeline_inc_cb = std::move(callback);
    }

    void TriggerHostWait(uint64_t glsync_handle, uint64_t thread_handle,
                         uint64_t guest_timeline_handle) override {
        last_glsync = glsync_handle;
        last_thread = thread_handle;
        last_timeline = guest_timeline_handle;
        trigger_wait_calls++;
    }

    // The GPU's own VMState sections drive its snapshot; the device must never call these.
    absl::Status Quiesce(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status PreSave(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::StatusOr<std::vector<uint8_t>> Save(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return std::vector<uint8_t>{};
    }
    absl::Status PostSave(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status PreLoad(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status Load(std::string_view /*snapshot_name*/, const uint8_t* /*meta_data*/,
                      size_t /*meta_size*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status PostLoad(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }

    TimelineIncCallback timeline_inc_cb;
    uint64_t last_glsync = 0;
    uint64_t last_thread = 0;
    uint64_t last_timeline = 0;
    int trigger_wait_calls = 0;
    int gpu_snapshot_calls = 0;

  private:
    const bool keep_stale_callback_ = false;
};

TEST(GoldfishSyncTest, GuestTriggerWaitForwardsToGpu) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    bool irq_state = false;
    TestGoldfishGpu gpu;

    GoldfishSyncDevice dev(std::move(mem), [&irq_state](bool high) { irq_state = high; }, gpu);

    constexpr uint64_t kGuestCmdGpa = 0x1000;
    dev.Write(kSyncRegBatchGuestCommandAddr, static_cast<uint32_t>(kGuestCmdGpa));
    dev.Write(kSyncRegBatchGuestCommandAddrHigh, static_cast<uint32_t>(kGuestCmdGpa >> 32));
    EXPECT_EQ(dev.Read(kSyncRegBatchGuestCommandAddr), static_cast<uint32_t>(kGuestCmdGpa));
    EXPECT_EQ(dev.Read(kSyncRegBatchGuestCommandAddrHigh),
              static_cast<uint32_t>(kGuestCmdGpa >> 32));

    GoldfishSyncGuestCommand gcmd{};
    gcmd.host_command = kSyncCmdTriggerHostWait;
    gcmd.glsync_handle = 0xAABB;
    gcmd.thread_handle = 0xCCDD;
    gcmd.guest_timeline_handle = 0xEEFF;
    mem_ptr->Put(kGuestCmdGpa, gcmd);

    dev.Write(kSyncRegBatchGuestCommand, 0);
    EXPECT_EQ(gpu.trigger_wait_calls, 1);
    EXPECT_EQ(gpu.last_glsync, 0xAABBU);
    EXPECT_EQ(gpu.last_thread, 0xCCDDU);
    EXPECT_EQ(gpu.last_timeline, 0xEEFFU);
}

TEST(GoldfishSyncTest, TimelineIncCoalescesAndDrainsToReady) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    bool irq_state = false;
    TestGoldfishGpu gpu;

    GoldfishSyncDevice dev(std::move(mem), [&irq_state](bool high) { irq_state = high; }, gpu);

    constexpr uint64_t kBatchCmdGpa = 0x2000;
    dev.Write(kSyncRegBatchCommandAddr, static_cast<uint32_t>(kBatchCmdGpa));
    dev.Write(kSyncRegBatchCommandAddrHigh, static_cast<uint32_t>(kBatchCmdGpa >> 32));
    EXPECT_EQ(dev.Read(kSyncRegBatchCommandAddr), static_cast<uint32_t>(kBatchCmdGpa));
    EXPECT_EQ(dev.Read(kSyncRegBatchCommandAddrHigh), static_cast<uint32_t>(kBatchCmdGpa >> 32));

    ASSERT_TRUE(gpu.timeline_inc_cb != nullptr);
    gpu.timeline_inc_cb(0x42, 1);
    gpu.timeline_inc_cb(0x42, 2);
    EXPECT_TRUE(irq_state);

    uint32_t cmd = dev.Read(kSyncRegBatchCommand);
    EXPECT_EQ(cmd, kSyncCmdSyncTimelineInc);
    auto out = mem_ptr->Get<GoldfishSyncCommand>(kBatchCmdGpa);
    EXPECT_EQ(out.cmd, kSyncCmdSyncTimelineInc);
    EXPECT_EQ(out.handle, 0x42U);
    EXPECT_EQ(out.time_arg, 3U);

    // Next read drains to READY and lowers IRQ.
    cmd = dev.Read(kSyncRegBatchCommand);
    EXPECT_EQ(cmd, kSyncCmdReady);
    EXPECT_FALSE(irq_state);
}

TEST(GoldfishSyncTest, StaleCallbackAfterDestructionIsSafeNoOp) {
    TestGoldfishGpu gpu(/*keep_stale_callback=*/true);
    std::atomic<int> irq_calls{0};
    {
        auto mem = std::make_unique<FakeGuestMemory>(0x10000);
        GoldfishSyncDevice dev(std::move(mem), [&irq_calls](bool /*high*/) { irq_calls++; }, gpu);
        ASSERT_TRUE(gpu.timeline_inc_cb != nullptr);
    }
    // Invoking the captured callback after GoldfishSyncDevice is destroyed must be a safe no-op.
    gpu.timeline_inc_cb(0x99, 5);
    EXPECT_EQ(irq_calls.load(), 0);
}

TEST(GoldfishSyncTest, ConcurrentTimelineIncAndGuestDrain) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    std::atomic<bool> irq_state{false};
    TestGoldfishGpu gpu;

    GoldfishSyncDevice dev(std::move(mem), [&irq_state](bool high) { irq_state.store(high); }, gpu);

    constexpr uint64_t kBatchCmdGpa = 0x3000;
    dev.Write(kSyncRegBatchCommandAddr, static_cast<uint32_t>(kBatchCmdGpa));
    dev.Write(kSyncRegBatchCommandAddrHigh, static_cast<uint32_t>(kBatchCmdGpa >> 32));

    constexpr uint32_t kNumThreads = 4;
    constexpr uint32_t kIncsPerThread = 250;
    std::vector<std::thread> producers;
    for (uint32_t t = 0; t < kNumThreads; ++t) {
        producers.emplace_back([&gpu]() {
            for (uint32_t i = 0; i < kIncsPerThread; ++i) {
                gpu.timeline_inc_cb(0x777, 1);
            }
        });
    }

    uint32_t total_incremented = 0;
    for (auto& th : producers) {
        th.join();
    }

    while (dev.Read(kSyncRegBatchCommand) == kSyncCmdSyncTimelineInc) {
        auto out = mem_ptr->Get<GoldfishSyncCommand>(kBatchCmdGpa);
        EXPECT_EQ(out.handle, 0x777U);
        total_incremented += out.time_arg;
    }
    EXPECT_EQ(total_incremented, kNumThreads * kIncsPerThread);
    EXPECT_FALSE(irq_state.load());
}

TEST(GoldfishSyncTest, BatchCommandAddr32BitProgrammingWorksWithoutHighRegisterWrite) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    bool irq_state = false;
    TestGoldfishGpu gpu;

    GoldfishSyncDevice dev(std::move(mem), [&irq_state](bool high) { irq_state = high; }, gpu);

    // Program only the low 32-bit kSyncRegBatchCommandAddr (as a 32-bit guest kernel does).
    constexpr uint32_t kBatchCmdGpa32 = 0x4000;
    dev.Write(kSyncRegBatchCommandAddr, kBatchCmdGpa32);

    ASSERT_TRUE(gpu.timeline_inc_cb != nullptr);
    gpu.timeline_inc_cb(0x55, 7);
    EXPECT_TRUE(irq_state);

    EXPECT_EQ(dev.Read(kSyncRegBatchCommand), kSyncCmdSyncTimelineInc);
    const auto out = mem_ptr->Get<GoldfishSyncCommand>(kBatchCmdGpa32);
    EXPECT_EQ(out.cmd, kSyncCmdSyncTimelineInc);
    EXPECT_EQ(out.handle, 0x55U);
    EXPECT_EQ(out.time_arg, 7U);

    // Also verify guest command dispatch works with 32-bit address programming.
    constexpr uint32_t kGuestCmdGpa32 = 0x5000;
    dev.Write(kSyncRegBatchGuestCommandAddr, kGuestCmdGpa32);
    GoldfishSyncGuestCommand gcmd{};
    gcmd.host_command = kSyncCmdTriggerHostWait;
    gcmd.glsync_handle = 0x1234;
    gcmd.thread_handle = 0x5678;
    gcmd.guest_timeline_handle = 0x9ABC;
    mem_ptr->Put(kGuestCmdGpa32, gcmd);
    dev.Write(kSyncRegBatchGuestCommand, 0);
    EXPECT_EQ(gpu.trigger_wait_calls, 1);
    EXPECT_EQ(gpu.last_glsync, 0x1234U);
    EXPECT_EQ(gpu.last_thread, 0x5678U);
    EXPECT_EQ(gpu.last_timeline, 0x9ABCU);
}

TEST(GoldfishSyncTest, SnapshotSaveAndRestorePreservesRegistersPendingQueueAndIrq) {
    auto mem1 = std::make_unique<FakeGuestMemory>(0x10000);
    bool irq1 = false;
    TestGoldfishGpu gpu1;
    GoldfishSyncDevice dev1(std::move(mem1), [&irq1](bool high) { irq1 = high; }, gpu1);

    constexpr uint64_t kBatchCmdGpa = 0x100005000ULL;
    constexpr uint64_t kBatchGuestCmdGpa = 0x200006000ULL;
    dev1.Write(kSyncRegBatchCommandAddr, static_cast<uint32_t>(kBatchCmdGpa));
    dev1.Write(kSyncRegBatchCommandAddrHigh, static_cast<uint32_t>(kBatchCmdGpa >> 32));
    dev1.Write(kSyncRegBatchGuestCommandAddr, static_cast<uint32_t>(kBatchGuestCmdGpa));
    dev1.Write(kSyncRegBatchGuestCommandAddrHigh, static_cast<uint32_t>(kBatchGuestCmdGpa >> 32));

    gpu1.timeline_inc_cb(0xAA, 4);
    gpu1.timeline_inc_cb(0xBB, 9);
    EXPECT_TRUE(irq1);

    archive::DequeArchive saved;
    dev1.SaveState(saved);
    ASSERT_FALSE(saved.Empty());
    EXPECT_EQ(gpu1.gpu_snapshot_calls, 0);

    auto mem2 = std::make_unique<FakeGuestMemory>(0x20000);
    FakeGuestMemory* mem2_ptr = mem2.get();
    bool irq2 = false;
    TestGoldfishGpu gpu2;
    GoldfishSyncDevice dev2(std::move(mem2), [&irq2](bool high) { irq2 = high; }, gpu2);

    ASSERT_OK(dev2.LoadState(saved));
    EXPECT_TRUE(saved.Empty());
    EXPECT_EQ(gpu2.gpu_snapshot_calls, 0);
    EXPECT_TRUE(irq2);
    EXPECT_EQ(dev2.Read(kSyncRegBatchCommandAddr), static_cast<uint32_t>(kBatchCmdGpa));
    EXPECT_EQ(dev2.Read(kSyncRegBatchCommandAddrHigh), static_cast<uint32_t>(kBatchCmdGpa >> 32));
    EXPECT_EQ(dev2.Read(kSyncRegBatchGuestCommandAddr), static_cast<uint32_t>(kBatchGuestCmdGpa));
    EXPECT_EQ(dev2.Read(kSyncRegBatchGuestCommandAddrHigh),
              static_cast<uint32_t>(kBatchGuestCmdGpa >> 32));

    // Reprogram low address inside mem2 bounds to drain restored pending commands.
    constexpr uint32_t kDrainGpa = 0x5000U;
    dev2.Write(kSyncRegBatchCommandAddrHigh, 0U);
    dev2.Write(kSyncRegBatchCommandAddr, kDrainGpa);

    EXPECT_EQ(dev2.Read(kSyncRegBatchCommand), kSyncCmdSyncTimelineInc);
    auto c1 = mem2_ptr->Get<GoldfishSyncCommand>(kDrainGpa);
    EXPECT_EQ(c1.handle, 0xAAU);
    EXPECT_EQ(c1.time_arg, 4U);

    EXPECT_EQ(dev2.Read(kSyncRegBatchCommand), kSyncCmdSyncTimelineInc);
    auto c2 = mem2_ptr->Get<GoldfishSyncCommand>(kDrainGpa);
    EXPECT_EQ(c2.handle, 0xBBU);
    EXPECT_EQ(c2.time_arg, 9U);

    EXPECT_EQ(dev2.Read(kSyncRegBatchCommand), kSyncCmdReady);
    EXPECT_FALSE(irq2);
}

// Builds a device with a few pending commands and returns its serialized state.
archive::DequeArchive SaveStateWithPendingCommands(TestGoldfishGpu& gpu) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    bool irq = false;
    GoldfishSyncDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);
    dev.Write(kSyncRegBatchCommandAddr, 0x1000U);
    dev.Write(kSyncRegBatchGuestCommandAddr, 0x2000U);
    gpu.timeline_inc_cb(0x11, 1);
    gpu.timeline_inc_cb(0x22, 2);
    gpu.timeline_inc_cb(0x33, 3);
    archive::DequeArchive out;
    dev.SaveState(out);
    return out;
}

TEST(GoldfishSyncTest, LoadStateRejectsEveryTruncatedPrefix) {
    TestGoldfishGpu gpu1;
    const archive::DequeArchive saved = SaveStateWithPendingCommands(gpu1);
    ASSERT_FALSE(saved.Empty());

    TestGoldfishGpu gpu2;
    int irq_calls = 0;
    GoldfishSyncDevice dev(
            std::make_unique<FakeGuestMemory>(0x10000),
            [&irq_calls](bool /*high*/) { irq_calls++; }, gpu2);
    for (size_t n = 0; n < saved.storage.size(); ++n) {
        SCOPED_TRACE(n);
        archive::DequeArchive prefix;
        prefix.storage.assign(saved.storage.begin(),
                              saved.storage.begin() + static_cast<ptrdiff_t>(n));
        EXPECT_THAT(dev.LoadState(prefix), StatusIs(absl::StatusCode::kDataLoss));
    }
    // A failed load leaves the device untouched.
    EXPECT_EQ(irq_calls, 0);
    EXPECT_EQ(dev.Read(kSyncRegBatchCommandAddr), 0U);
    EXPECT_EQ(dev.Read(kSyncRegBatchCommand), kSyncCmdReady);

    // The full stream still loads.
    archive::DequeArchive full = saved;
    ASSERT_OK(dev.LoadState(full));
    EXPECT_TRUE(full.Empty());
    EXPECT_EQ(irq_calls, 1);
}

TEST(GoldfishSyncTest, LoadStateRejectsHugePendingCountNamingTheLimit) {
    archive::DequeArchive bad;
    bad << uint32_t{0x4753594EU} << uint32_t{1U} << uint64_t{0x1000U} << uint64_t{0x2000U} << true
        << uint32_t{0xffffffffU};

    TestGoldfishGpu gpu2;
    GoldfishSyncDevice dev(std::make_unique<FakeGuestMemory>(0x10000), [](bool /*high*/) {}, gpu2);
    EXPECT_THAT(dev.LoadState(bad),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("pending_count 4294967295 exceeds 1000000")));
}

TEST(GoldfishSyncTest, LoadStateRejectsPendingCountLargerThanStream) {
    // 1000 commands claimed, but 0 are present: rejected without mutating the device.
    archive::DequeArchive bad;
    bad << uint32_t{0x4753594EU} << uint32_t{1U} << uint64_t{0x1000U} << uint64_t{0x2000U} << true
        << uint32_t{1000U};

    TestGoldfishGpu gpu2;
    GoldfishSyncDevice dev(std::make_unique<FakeGuestMemory>(0x10000), [](bool /*high*/) {}, gpu2);
    EXPECT_THAT(dev.LoadState(bad),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("goldfish_sync pending command 0 of 1000")));
}

TEST(GoldfishSyncTest, LoadStateRejectsBadMagicAndUnsupportedVersion) {
    TestGoldfishGpu gpu;
    GoldfishSyncDevice dev(std::make_unique<FakeGuestMemory>(0x10000), [](bool /*high*/) {}, gpu);

    archive::DequeArchive bad_magic;
    bad_magic << uint32_t{0xDEADBEEFU} << uint32_t{1U} << uint64_t{0} << uint64_t{0} << false
              << uint32_t{0};
    EXPECT_THAT(dev.LoadState(bad_magic),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("magic 0xdeadbeef")));

    archive::DequeArchive bad_version;
    bad_version << uint32_t{0x4753594EU} << uint32_t{2U} << uint64_t{0} << uint64_t{0} << false
                << uint32_t{0};
    EXPECT_THAT(dev.LoadState(bad_version),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("version 2")));

    archive::DequeArchive empty;
    EXPECT_THAT(dev.LoadState(empty), StatusIs(absl::StatusCode::kDataLoss));
}

TEST(GoldfishSyncTest, CoreSaveAndLoadRoundTripsThroughQemuFile) {
    TestGoldfishGpu gpu1;
    auto mem1 = std::make_unique<FakeGuestMemory>(0x10000);
    bool irq1 = false;
    GoldfishSyncDevice dev1(std::move(mem1), [&irq1](bool high) { irq1 = high; }, gpu1);
    dev1.Write(kSyncRegBatchCommandAddr, 0x3000U);
    gpu1.timeline_inc_cb(0x44, 5);

    QEMUFile file;
    IMmioDevice* core1 = &dev1;
    ASSERT_TRUE(GoldfishSyncCoreSave(core1, &file));
    EXPECT_FALSE(file.archive.Empty());

    TestGoldfishGpu gpu2;
    auto mem2 = std::make_unique<FakeGuestMemory>(0x10000);
    bool irq2 = false;
    GoldfishSyncDevice dev2(std::move(mem2), [&irq2](bool high) { irq2 = high; }, gpu2);
    IMmioDevice* core2 = &dev2;
    ASSERT_TRUE(GoldfishSyncCoreLoad(core2, &file));
    EXPECT_TRUE(file.archive.Empty());
    EXPECT_TRUE(irq2);
    EXPECT_EQ(dev2.Read(kSyncRegBatchCommandAddr), 0x3000U);
}

}  // namespace
}  // namespace goldfish::legacy
