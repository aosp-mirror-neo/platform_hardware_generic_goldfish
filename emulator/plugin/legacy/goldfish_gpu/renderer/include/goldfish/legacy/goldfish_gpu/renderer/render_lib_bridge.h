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

#include "render-utils/RenderLib.h"
#include "render-utils/address_space_operations.h"
#include "render-utils/dma_device.h"
#include "render-utils/renderer_enums.h"
#include "render-utils/sync_device.h"
#include "render-utils/vm_operations.h"

// gfxstream's RenderLib is configured with plain C function pointers
// (dma/sync/vm/address-space ops) that carry no context argument, so every
// callback from a render thread has to reach the GoldfishGpu through a
// process-wide pointer. This bridge is the only place that pointer lives: the
// trampolines forward to the installed HostCallbacks, and a test can install a
// fake target and call the function pointers directly.

namespace goldfish::legacy {

class HostCallbacks {
  public:
    virtual ~HostCallbacks() = default;

    // BAR 1 host-visible memory. Return false (and log why) when the mapping
    // cannot be made so gfxstream reports it instead of leaving the guest
    // with an unmapped allocation.
    virtual bool MapUserMemory(uint64_t gpa, void* hva, uint64_t size) = 0;
    virtual bool UnmapUserMemory(uint64_t gpa, uint64_t size) = 0;
    // Guest physical address -> host pointer (null when unknown).
    virtual void* ResolveDmaHostAddr(uint64_t gpa) = 0;

    // goldfish_sync timelines.
    virtual uint64_t CreateSyncTimeline() = 0;
    virtual void SyncTimelineInc(uint64_t timeline, uint32_t count) = 0;
    virtual void RegisterSyncTriggerWait(gfxstream_sync_trigger_wait_t trigger_fn) = 0;
    virtual bool SyncDeviceExists() = 0;
};

class RenderLibBridge {
  public:
    // Sets the target of every trampoline. With no target the memory
    // operations fail (and log), the DMA lookup returns null and the sync
    // operations are no-ops. Thread-safe; takes effect for subsequent calls.
    static void Install(HostCallbacks* target);
    static HostCallbacks* Current();

    // Applies the goldfish settings to `render_lib`: guest API level, goldfish
    // gralloc, the trampolines below, and the selected backend.
    static void Configure(gfxstream::RenderLib* render_lib, uint32_t guest_api_level,
                          SelectedRenderer renderer);

    // The op tables Configure() installs (also for tests).
    static gfxstream_dma_ops DmaOps();
    static gfxstream_vm_ops VmOps();
    static address_space_device_control_ops* AddressSpaceOps();

    // The sync-device entry points Configure() installs.
    static uint64_t SyncCreateTimeline();
    static int SyncCreateFence(uint64_t timeline, uint32_t pt);
    static void SyncTimelineInc(uint64_t timeline, uint32_t count);
    static void SyncDestroyTimeline(uint64_t timeline);
    static void SyncRegisterTriggerWait(gfxstream_sync_trigger_wait_t trigger_fn);
    static bool SyncDeviceExists();
};

}  // namespace goldfish::legacy
