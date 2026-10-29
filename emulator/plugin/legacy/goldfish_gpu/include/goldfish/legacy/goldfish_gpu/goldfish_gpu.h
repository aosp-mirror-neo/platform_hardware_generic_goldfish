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

#include <cstddef>
#include <cstdint>
#include <functional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace goldfish::legacy {

struct AsgBlob {
    uint8_t* external_addr = nullptr;
    uint64_t size = 0;
    uint32_t context_handle = 0;
    uint32_t blob_id = 0;
};

using TimelineIncCallback = std::function<void(uint64_t timeline, uint32_t count)>;
using OpenglesReadCallback = std::function<void(const uint8_t* data, size_t size)>;
using MapUserMemoryCallback = std::function<bool(uint64_t gpa, void* hva, uint64_t size)>;
using UnmapUserMemoryCallback = std::function<bool(uint64_t gpa, uint64_t size)>;
using ResolveDmaHostAddrCallback = std::function<void*(uint64_t gpa)>;

class IGoldfishGpu {
  public:
    virtual ~IGoldfishGpu() = default;

    // Goldfish Address Space / ASG operations.
    virtual void SetMemoryMappingCallbacks(MapUserMemoryCallback map_cb,
                                           UnmapUserMemoryCallback unmap_cb,
                                           ResolveDmaHostAddrCallback resolve_dma_cb) = 0;
    virtual absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t size) = 0;
    virtual absl::Status StartAsg(const AsgBlob& blob) = 0;
    virtual absl::Status PingAsg(const AsgBlob& blob) = 0;
    virtual void DestroyAsg(const AsgBlob& blob) = 0;

    // Goldfish Sync operations.
    virtual void SetTimelineIncCallback(TimelineIncCallback callback) = 0;
    virtual void TriggerHostWait(uint64_t glsync_handle, uint64_t thread_handle,
                                 uint64_t guest_timeline_handle) = 0;
    virtual void ReceiveHostCmdResult(uint32_t cmd, uint64_t handle, uint32_t time_arg,
                                      uint64_t hostcmd_handle) = 0;

    // Goldfish Pipe GPU service operations.
    virtual uint64_t CreateProcessContext() = 0;
    virtual void DestroyProcessContext(uint64_t puid) = 0;
    virtual void ColorBufferLastRef(uint32_t handle) = 0;
    virtual absl::StatusOr<uint32_t> OpenOpenglesPipe(OpenglesReadCallback on_host_data) = 0;
    virtual absl::Status WriteOpenglesPipe(uint32_t id, const uint8_t* data, size_t size) = 0;
    virtual void CloseOpenglesPipe(uint32_t id) = 0;
};

IGoldfishGpu& GetGoldfishGpu();

}  // namespace goldfish::legacy
