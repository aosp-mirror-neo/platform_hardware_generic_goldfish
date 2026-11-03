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
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

namespace goldfish::legacy {

class FakeGoldfishGpu : public IGoldfishGpu {
  public:
    absl::Status Start(GoldfishGpuConfig /*config*/) override { return absl::OkStatus(); }
    void Stop() override {}
    void StopForProcessExit() override { Stop(); }
    void FlushReadPixelPipeline() override {}
    void SetVerifyMode(GoldfishGpuVerifyMode /*mode*/) override {}
    GoldfishGpuVerifyMode GetVerifyMode() const override { return GoldfishGpuVerifyMode::kLog; }
    absl::Status VerifySteadyState() override { return absl::OkStatus(); }

    absl::Status Quiesce(std::string_view /*snapshot_name*/) override { return absl::OkStatus(); }
    absl::Status PreSave(std::string_view /*snapshot_name*/) override { return absl::OkStatus(); }
    absl::StatusOr<std::vector<uint8_t>> Save(std::string_view /*snapshot_name*/) override {
        return std::vector<uint8_t>{};
    }
    absl::Status PostSave(std::string_view /*snapshot_name*/) override { return absl::OkStatus(); }
    absl::Status PreLoad(std::string_view /*snapshot_name*/) override { return absl::OkStatus(); }
    absl::Status Load(std::string_view /*snapshot_name*/, const uint8_t* /*meta_data*/,
                      size_t /*meta_size*/) override {
        return absl::OkStatus();
    }
    absl::Status PostLoad(std::string_view /*snapshot_name*/) override { return absl::OkStatus(); }
    void OnSnapshotOperationFinished() override {}

    void SetMemoryMappingCallbacks(MapUserMemoryCallback /*map_cb*/,
                                   UnmapUserMemoryCallback /*unmap_cb*/,
                                   ResolveDmaHostAddrCallback /*resolve_dma_cb*/) override {}
    absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t /*size*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::StatusOr<AsgBlob> RestoreAsgBlob(uint32_t /*context_handle*/, uint64_t size) override {
        return CreateAsgBlob(size);
    }
    absl::Status StartAsg(const AsgBlob& /*blob*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::Status PingAsg(const AsgBlob& /*blob*/) override {
        return absl::UnimplementedError("unused");
    }
    void CloseAsg(const AsgBlob& /*blob*/) override {}
    void SetTimelineIncCallback(TimelineIncCallback /*callback*/) override {}
    void TriggerHostWait(uint64_t /*glsync_handle*/, uint64_t /*thread_handle*/,
                         uint64_t /*guest_timeline_handle*/) override {}
    uint64_t CreateProcessContext() override { return 0; }
    void DestroyProcessContext(uint64_t /*puid*/) override {}
    void ColorBufferRef(uint32_t /*handle*/) override {}
    void ColorBufferLastRef(uint32_t /*handle*/) override {}
    absl::StatusOr<uint32_t> CreateOpenglesPipe(OpenglesReadCallback /*on_host_data*/,
                                                OpenglesWritableCallback /*on_writable*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::Status RestoreOpenglesPipe(uint32_t /*id*/, OpenglesReadCallback /*on_host_data*/,
                                     OpenglesWritableCallback /*on_writable*/) override {
        return absl::UnimplementedError("unused");
    }
    absl::Status WriteOpenglesPipe(uint32_t /*id*/, const uint8_t* /*data*/,
                                   size_t /*size*/) override {
        return absl::UnimplementedError("unused");
    }
    void CloseOpenglesPipe(uint32_t /*id*/) override {}
};

}  // namespace goldfish::legacy
