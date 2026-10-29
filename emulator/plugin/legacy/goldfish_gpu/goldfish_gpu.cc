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

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"

namespace goldfish::legacy {
namespace {

class GoldfishGpu final : public IGoldfishGpu {
  public:
    GoldfishGpu() = default;

    void SetMemoryMappingCallbacks(MapUserMemoryCallback map_cb, UnmapUserMemoryCallback unmap_cb,
                                   ResolveDmaHostAddrCallback resolve_dma_cb)
            ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        map_user_memory_cb_ = std::move(map_cb);
        unmap_user_memory_cb_ = std::move(unmap_cb);
        resolve_dma_cb_ = std::move(resolve_dma_cb);
    }

    absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t size) override {
        LOG(QFATAL) << "GoldfishGpu::CreateAsgBlob unimplemented (size=" << size << ")";
        return absl::UnimplementedError("GoldfishGpu::CreateAsgBlob unimplemented");
    }

    absl::Status StartAsg(const AsgBlob& blob) override {
        LOG(QFATAL) << "GoldfishGpu::StartAsg unimplemented (context_handle=" << blob.context_handle
                    << ", blob_id=" << blob.blob_id << ")";
        return absl::UnimplementedError("GoldfishGpu::StartAsg unimplemented");
    }

    absl::Status PingAsg(const AsgBlob& blob) override {
        LOG(QFATAL) << "GoldfishGpu::PingAsg unimplemented (context_handle=" << blob.context_handle
                    << ", blob_id=" << blob.blob_id << ")";
        return absl::UnimplementedError("GoldfishGpu::PingAsg unimplemented");
    }

    void DestroyAsg(const AsgBlob& blob) override {
        LOG(QFATAL) << "GoldfishGpu::DestroyAsg unimplemented (context_handle="
                    << blob.context_handle << ", blob_id=" << blob.blob_id << ")";
    }

    void SetTimelineIncCallback(TimelineIncCallback callback) ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        timeline_inc_cb_ = std::move(callback);
    }

    void TriggerHostWait(uint64_t glsync_handle, uint64_t thread_handle,
                         uint64_t guest_timeline_handle) override {
        LOG(QFATAL) << "GoldfishGpu::TriggerHostWait unimplemented (glsync=" << glsync_handle
                    << ", thread=" << thread_handle << ", timeline=" << guest_timeline_handle
                    << ")";
    }

    void ReceiveHostCmdResult(uint32_t cmd, uint64_t handle, uint32_t time_arg,
                              uint64_t hostcmd_handle) override {
        LOG(QFATAL) << "GoldfishGpu::ReceiveHostCmdResult unimplemented (cmd=" << cmd
                    << ", handle=" << handle << ", time_arg=" << time_arg
                    << ", hostcmd=" << hostcmd_handle << ")";
    }

    uint64_t CreateProcessContext() override {
        LOG(QFATAL) << "GoldfishGpu::CreateProcessContext unimplemented";
        return 0;
    }

    void DestroyProcessContext(uint64_t puid) override {
        LOG(QFATAL) << "GoldfishGpu::DestroyProcessContext unimplemented (puid=" << puid << ")";
    }

    void ColorBufferLastRef(uint32_t handle) override {
        LOG(QFATAL) << "GoldfishGpu::ColorBufferLastRef unimplemented (handle=" << handle << ")";
    }

    absl::StatusOr<uint32_t> OpenOpenglesPipe(OpenglesReadCallback /*on_host_data*/) override {
        LOG(QFATAL) << "GoldfishGpu::OpenOpenglesPipe unimplemented";
        return absl::UnimplementedError("GoldfishGpu::OpenOpenglesPipe unimplemented");
    }

    absl::Status WriteOpenglesPipe(uint32_t id, const uint8_t* /*data*/, size_t size) override {
        LOG(QFATAL) << "GoldfishGpu::WriteOpenglesPipe unimplemented (id=" << id
                    << ", size=" << size << ")";
        return absl::UnimplementedError("GoldfishGpu::WriteOpenglesPipe unimplemented");
    }

    void CloseOpenglesPipe(uint32_t id) override {
        LOG(QFATAL) << "GoldfishGpu::CloseOpenglesPipe unimplemented (id=" << id << ")";
    }

  private:
    MapUserMemoryCallback map_user_memory_cb_ ABSL_GUARDED_BY(mu_);
    UnmapUserMemoryCallback unmap_user_memory_cb_ ABSL_GUARDED_BY(mu_);
    ResolveDmaHostAddrCallback resolve_dma_cb_ ABSL_GUARDED_BY(mu_);
    TimelineIncCallback timeline_inc_cb_ ABSL_GUARDED_BY(mu_);
    absl::Mutex mu_;
};

}  // namespace

IGoldfishGpu& GetGoldfishGpu() {
    static absl::NoDestructor<GoldfishGpu> instance;
    return *instance;
}

}  // namespace goldfish::legacy
