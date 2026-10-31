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

#include <atomic>
#include <cstdint>
#include <ios>

#include "absl/log/log.h"

#include "render-utils/gralloc_enums.h"

namespace goldfish::legacy {
namespace {

std::atomic<HostCallbacks*> g_target{nullptr};

HostCallbacks* Target() { return g_target.load(std::memory_order_acquire); }

bool TryMapUserMemory(uint64_t gpa, void* hva, uint64_t size) {
    HostCallbacks* target = Target();
    if (target == nullptr) {
        LOG(ERROR) << "goldfish_gpu: map_user_memory(gpa=0x" << std::hex << gpa << ", size=0x"
                   << size << ", hva=" << hva << ") called with no GoldfishGpu installed";
        return false;
    }
    return target->MapUserMemory(gpa, hva, size);
}

bool TryUnmapUserMemory(uint64_t gpa, uint64_t size) {
    HostCallbacks* target = Target();
    if (target == nullptr) {
        LOG(ERROR) << "goldfish_gpu: unmap_user_memory(gpa=0x" << std::hex << gpa << ", size=0x"
                   << size << ") called with no GoldfishGpu installed";
        return false;
    }
    return target->UnmapUserMemory(gpa, size);
}

void* DmaGetHostAddr(uint64_t gpa) {
    HostCallbacks* target = Target();
    return target != nullptr ? target->ResolveDmaHostAddr(gpa) : nullptr;
}

const AddressSpaceHwFuncs kHwFuncs = {
    .allocSharedHostRegion = [](uint64_t, uint64_t*) { return -1; },
    .freeSharedHostRegion = [](uint64_t) { return 0; },
    .allocSharedHostRegionLocked = [](uint64_t, uint64_t*) { return -1; },
    .freeSharedHostRegionLocked = [](uint64_t) { return 0; },
    .getPhysAddrStart = []() -> uint64_t { return 0; },
    .getPhysAddrStartLocked = []() -> uint64_t { return 0; },
    .getGuestPageSize = []() -> uint32_t { return 4096; },
    .allocSharedHostRegionFixedLocked = [](uint64_t, uint64_t) { return -1; },
};

address_space_device_control_ops MakeAddressSpaceOps() {
    address_space_device_control_ops ops = {};
    ops.gen_handle = []() -> uint32_t { return 0; };
    ops.destroy_handle = [](uint32_t) {};
    ops.tell_ping_info = [](uint32_t, uint64_t) {};
    ops.ping = [](uint32_t) {};
    ops.add_memory_mapping = [](uint64_t gpa, void* ptr, uint64_t size) -> int {
        return TryMapUserMemory(gpa, ptr, size) ? 1 : 0;
    };
    ops.remove_memory_mapping = [](uint64_t gpa, void* /*ptr*/, uint64_t size) -> int {
        return TryUnmapUserMemory(gpa, size) ? 1 : 0;
    };
    ops.get_host_ptr = &DmaGetHostAddr;
    ops.handle_to_context = [](uint32_t) -> void* { return nullptr; };
    ops.clear = []() {};
    ops.hostmem_register = [](const struct MemEntry*) -> uint64_t { return 0; };
    ops.hostmem_unregister = [](uint64_t) {};
    ops.ping_at_hva = [](uint32_t, void*) {};
    ops.register_deallocation_callback = [](void*, uint64_t,
                                            address_space_device_deallocation_callback_t) {};
    ops.run_deallocation_callbacks = [](uint64_t) {};
    ops.control_get_hw_funcs = []() -> const AddressSpaceHwFuncs* { return &kHwFuncs; };
    ops.create_instance = [](const struct AddressSpaceCreateInfo&) {};
    return ops;
}

}  // namespace

void RenderLibBridge::Install(HostCallbacks* target) {
    g_target.store(target, std::memory_order_release);
}

HostCallbacks* RenderLibBridge::Current() { return Target(); }

gfxstream_dma_ops RenderLibBridge::DmaOps() {
    return gfxstream_dma_ops{
        .get_host_addr = &DmaGetHostAddr,
        .unlock = [](uint64_t) {},
    };
}

gfxstream_vm_ops RenderLibBridge::VmOps() {
    gfxstream_vm_ops vm_ops = {};
    vm_ops.map_user_memory = [](uint64_t gpa, void* hva, uint64_t size) {
        TryMapUserMemory(gpa, hva, size);
    };
    vm_ops.unmap_user_memory = [](uint64_t gpa, uint64_t size) { TryUnmapUserMemory(gpa, size); };
    vm_ops.unmap_user_memory_async = vm_ops.unmap_user_memory;
    vm_ops.lookup_user_memory = &DmaGetHostAddr;
    vm_ops.register_vulkan_instance = [](uint64_t, const char*) {};
    vm_ops.unregister_vulkan_instance = [](uint64_t) {};
    vm_ops.set_skip_snapshot_save = [](bool) {};
    vm_ops.set_skip_snapshot_save_reason = [](uint32_t) {};
    vm_ops.set_snapshot_uses_vulkan = []() {};
    vm_ops.add_crash_reporter_log = [](const char*) {};
    return vm_ops;
}

address_space_device_control_ops* RenderLibBridge::AddressSpaceOps() {
    // gfxstream keeps the pointer, so the table must outlive every RenderLib.
    static address_space_device_control_ops s_ops = MakeAddressSpaceOps();
    return &s_ops;
}

uint64_t RenderLibBridge::SyncCreateTimeline() {
    HostCallbacks* target = Target();
    return target != nullptr ? target->CreateSyncTimeline() : 0;
}

int RenderLibBridge::SyncCreateFence(uint64_t /*timeline*/, uint32_t /*pt*/) { return 0; }

void RenderLibBridge::SyncTimelineInc(uint64_t timeline, uint32_t count) {
    if (HostCallbacks* target = Target()) {
        target->SyncTimelineInc(timeline, count);
    }
}

void RenderLibBridge::SyncDestroyTimeline(uint64_t /*timeline*/) {}

void RenderLibBridge::SyncRegisterTriggerWait(gfxstream_sync_trigger_wait_t trigger_fn) {
    if (HostCallbacks* target = Target()) {
        target->RegisterSyncTriggerWait(trigger_fn);
    }
}

bool RenderLibBridge::SyncDeviceExists() {
    HostCallbacks* target = Target();
    return target != nullptr && target->SyncDeviceExists();
}

void RenderLibBridge::Configure(gfxstream::RenderLib* render_lib, uint32_t guest_api_level,
                                SelectedRenderer renderer) {
    render_lib->setGuestAndroidApiLevel(static_cast<int>(guest_api_level));
    render_lib->setGrallocImplementation(GOLDFISH_GRALLOC);
    render_lib->setDmaOps(DmaOps());
    render_lib->setSyncDevice(&SyncCreateTimeline, &SyncCreateFence, &SyncTimelineInc,
                              &SyncDestroyTimeline, &SyncRegisterTriggerWait, &SyncDeviceExists);
    render_lib->setAddressSpaceDeviceControlOps(AddressSpaceOps());
    render_lib->setVmOps(VmOps());
    render_lib->setRenderer(renderer);
}

}  // namespace goldfish::legacy
