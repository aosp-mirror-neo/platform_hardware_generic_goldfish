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

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

namespace goldfish::legacy {

class FakeGoldfishGpu : public IGoldfishGpu {
  public:
    void SetMemoryMappingCallbacks(MapUserMemoryCallback /*map_cb*/,
                                   UnmapUserMemoryCallback /*unmap_cb*/,
                                   ResolveDmaHostAddrCallback /*resolve_dma_cb*/) override {}
    absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t /*size*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::Status StartAsg(const AsgBlob& /*blob*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::Status PingAsg(const AsgBlob& /*blob*/) override {
        return absl::UnimplementedError("unused");
    }
    void DestroyAsg(const AsgBlob& /*blob*/) override {}
    void SetTimelineIncCallback(TimelineIncCallback /*callback*/) override {}
    void TriggerHostWait(uint64_t /*glsync_handle*/, uint64_t /*thread_handle*/,
                         uint64_t /*guest_timeline_handle*/) override {}
    void ReceiveHostCmdResult(uint32_t /*cmd*/, uint64_t /*handle*/, uint32_t /*time_arg*/,
                              uint64_t /*hostcmd_handle*/) override {}
    uint64_t CreateProcessContext() override { return 0; }
    void DestroyProcessContext(uint64_t /*puid*/) override {}
    void ColorBufferLastRef(uint32_t /*handle*/) override {}
    absl::StatusOr<uint32_t> OpenOpenglesPipe(OpenglesReadCallback /*on_host_data*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::Status WriteOpenglesPipe(uint32_t /*id*/, const uint8_t* /*data*/,
                                   size_t /*size*/) override {
        return absl::UnimplementedError("unused");
    }
    void CloseOpenglesPipe(uint32_t /*id*/) override {}
};

}  // namespace goldfish::legacy
