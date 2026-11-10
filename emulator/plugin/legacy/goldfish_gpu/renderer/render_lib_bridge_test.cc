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

#include "goldfish/legacy/goldfish_gpu/renderer/render_lib_bridge.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "goldfish/legacy/goldfish_gpu/testing/mock_gfxstream.h"

namespace goldfish::legacy {
namespace {

using ::goldfish::legacy::testing::MockRenderLib;
using ::goldfish::legacy::testing::MockRenderer;

class FakeHost final : public HostCallbacks {
  public:
    bool MapUserMemory(uint64_t gpa, void* hva, uint64_t size) override {
        maps.push_back({gpa, hva, size});
        return map_result;
    }
    bool UnmapUserMemory(uint64_t gpa, uint64_t size) override {
        unmaps.push_back({gpa, nullptr, size});
        return map_result;
    }
    void* ResolveDmaHostAddr(uint64_t gpa) override { return reinterpret_cast<void*>(gpa + 1); }
    uint64_t CreateSyncTimeline() override { return ++timelines; }
    void SyncTimelineInc(uint64_t timeline, uint32_t count) override {
        last_inc_timeline = timeline;
        last_inc_count = count;
    }
    void RegisterSyncTriggerWait(gfxstream_sync_trigger_wait_t fn) override { trigger_wait = fn; }
    bool SyncDeviceExists() override { return device_exists; }

    struct Call {
        uint64_t gpa;
        void* hva;
        uint64_t size;
    };
    std::vector<Call> maps;
    std::vector<Call> unmaps;
    bool map_result = true;
    uint64_t timelines = 0;
    uint64_t last_inc_timeline = 0;
    uint32_t last_inc_count = 0;
    gfxstream_sync_trigger_wait_t trigger_wait = nullptr;
    bool device_exists = false;
};

class RenderLibBridgeTest : public ::testing::Test {
  protected:
    void TearDown() override { RenderLibBridge::Install(nullptr); }
    FakeHost host_;
    uint8_t hva_ = 0;
};

TEST_F(RenderLibBridgeTest, WithoutTargetEverythingFailsSafely) {
    RenderLibBridge::Install(nullptr);
    EXPECT_EQ(RenderLibBridge::Current(), nullptr);
    EXPECT_EQ(RenderLibBridge::AddressSpaceOps()->add_memory_mapping(0x1000, &hva_, 4096), 0);
    EXPECT_EQ(RenderLibBridge::AddressSpaceOps()->remove_memory_mapping(0x1000, &hva_, 4096), 0);
    EXPECT_EQ(RenderLibBridge::DmaOps().get_host_addr(0x1000), nullptr);
    EXPECT_EQ(RenderLibBridge::VmOps().lookup_user_memory(0x1000), nullptr);
    RenderLibBridge::VmOps().map_user_memory(0x1000, &hva_, 4096);  // must not crash
    EXPECT_EQ(RenderLibBridge::SyncCreateTimeline(), 0U);
    EXPECT_FALSE(RenderLibBridge::SyncDeviceExists());
    RenderLibBridge::SyncTimelineInc(1, 1);
    RenderLibBridge::SyncRegisterTriggerWait(nullptr);
}

TEST_F(RenderLibBridgeTest, MemoryOpsForwardAndReportFailure) {
    RenderLibBridge::Install(&host_);
    EXPECT_EQ(RenderLibBridge::Current(), &host_);
    auto* ops = RenderLibBridge::AddressSpaceOps();
    EXPECT_EQ(ops->add_memory_mapping(0x1000, &hva_, 4096), 1);
    EXPECT_EQ(ops->remove_memory_mapping(0x1000, &hva_, 4096), 1);
    ASSERT_EQ(host_.maps.size(), 1U);
    EXPECT_EQ(host_.maps[0].gpa, 0x1000U);
    EXPECT_EQ(host_.maps[0].hva, &hva_);
    EXPECT_EQ(host_.maps[0].size, 4096U);
    ASSERT_EQ(host_.unmaps.size(), 1U);

    host_.map_result = false;
    EXPECT_EQ(ops->add_memory_mapping(0x2000, &hva_, 4096), 0);
    EXPECT_EQ(ops->remove_memory_mapping(0x2000, &hva_, 4096), 0);

    gfxstream_vm_ops vm = RenderLibBridge::VmOps();
    vm.map_user_memory(0x3000, &hva_, 8);
    vm.unmap_user_memory(0x3000, 8);
    vm.unmap_user_memory_async(0x3000, 8);
    EXPECT_EQ(host_.maps.size(), 3U);
    EXPECT_EQ(host_.unmaps.size(), 4U);

    EXPECT_EQ(RenderLibBridge::DmaOps().get_host_addr(0x10), reinterpret_cast<void*>(0x11));
    EXPECT_EQ(ops->get_host_ptr(0x20), reinterpret_cast<void*>(0x21));
    EXPECT_EQ(vm.lookup_user_memory(0x30), reinterpret_cast<void*>(0x31));
    EXPECT_EQ(ops->control_get_hw_funcs()->getGuestPageSize(), 4096U);
}

TEST_F(RenderLibBridgeTest, ConfigureInstallsEverythingOnTheRenderLib) {
    RenderLibBridge::Install(&host_);
    MockRenderLib lib(std::make_shared<MockRenderer>());
    RenderLibBridge::Configure(&lib, 30, SELECTED_RENDERER_ANGLE_INDIRECT);
    EXPECT_EQ(lib.api_level, 30);
    EXPECT_EQ(lib.gralloc, GOLDFISH_GRALLOC);
    EXPECT_EQ(lib.selected_renderer, SELECTED_RENDERER_ANGLE_INDIRECT);
    EXPECT_EQ(lib.asg_ops, RenderLibBridge::AddressSpaceOps());
    EXPECT_NE(lib.dma_ops.get_host_addr, nullptr);
    EXPECT_NE(lib.vm_ops.map_user_memory, nullptr);
    // setSyncDevice() registers the trigger-wait entry point immediately.
    EXPECT_EQ(host_.trigger_wait, &MockRenderLib::OnTriggerWait);

    ASSERT_NE(lib.sync_create_timeline, nullptr);
    EXPECT_EQ(lib.sync_create_timeline(), 1U);
    EXPECT_EQ(lib.sync_create_timeline(), 2U);
    lib.sync_timeline_inc(2, 7);
    EXPECT_EQ(host_.last_inc_timeline, 2U);
    EXPECT_EQ(host_.last_inc_count, 7U);
    EXPECT_FALSE(lib.sync_device_exists());
    host_.device_exists = true;
    EXPECT_TRUE(lib.sync_device_exists());
    EXPECT_EQ(RenderLibBridge::SyncCreateFence(1, 1), 0);
}

}  // namespace
}  // namespace goldfish::legacy
