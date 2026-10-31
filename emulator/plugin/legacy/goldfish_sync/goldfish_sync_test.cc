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

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "goldfish/legacy/common/fake_guest_memory.h"
#include "goldfish/legacy/goldfish_gpu/fake_goldfish_gpu.h"

namespace goldfish::legacy {
namespace {

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

    TimelineIncCallback timeline_inc_cb;
    uint64_t last_glsync = 0;
    uint64_t last_thread = 0;
    uint64_t last_timeline = 0;
    int trigger_wait_calls = 0;

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

}  // namespace
}  // namespace goldfish::legacy
