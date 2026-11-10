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
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace gfxstream {
class RenderLib;
}  // namespace gfxstream

namespace goldfish::legacy {

struct AsgBlob {
    uint8_t* external_addr = nullptr;
    uint64_t size = 0;
    uint32_t context_handle = 0;
    uint32_t blob_id = 0;
};

using TimelineIncCallback = std::function<void(uint64_t timeline, uint32_t count)>;
using OpenglesReadCallback = std::function<void(const uint8_t* data, size_t size)>;
// Invoked (from a renderer thread) once a `WriteOpenglesPipe()` that returned
// UNAVAILABLE may be retried.
using OpenglesWritableCallback = std::function<void()>;
using MapUserMemoryCallback = std::function<bool(uint64_t gpa, void* hva, uint64_t size)>;
using UnmapUserMemoryCallback = std::function<bool(uint64_t gpa, uint64_t size)>;
using ResolveDmaHostAddrCallback = std::function<void*(uint64_t gpa)>;
using GoldfishPostCallback =
        std::function<void(uint32_t width, uint32_t height, const uint8_t* bgra_pixels)>;

// Controls how state-invariant verification in GoldfishGpu reacts to a
// violation at snapshot and lifecycle boundaries (mirrors GfxstreamVerifyMode
// in virtio_gpu_gfxstream). Can also be overridden via the
// ANDROID_EMU_GOLDFISH_GPU_VERIFY environment variable ("off", "log",
// "enforce", "strict").
enum class GoldfishGpuVerifyMode {
    kOff = 0,      // Skip all invariant checks.
    kLog = 1,      // Log each violation as ERROR and continue (default).
    kEnforce = 2,  // Log each violation and fail the operation.
    kStrict = 3,   // Log each violation as FATAL (abort immediately).
};

struct GoldfishGpuConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    // Android API level of the guest system image (`RenderLib::setGuestAndroidApiLevel`).
    // Required: `Start()` rejects 0.
    uint32_t guest_api_level = 0;
    // gfxstream feature overrides: a ';'- (or ',') separated list of
    // "<Feature>:enabled|disabled" entries, e.g.
    // "Vulkan:disabled;GlAsyncSwap:enabled" (the same entries as the -feature
    // command line option). Parsed by BuildFeatureSet() in renderer_config.h;
    // empty keeps gfxstream's defaults.
    std::string renderer_features;
    // Where the renderer state of each snapshot is kept
    // (<snapshot_directory>/<snapshot name>/renderer/); empty disables on-disk
    // renderer snapshots.
    std::filesystem::path snapshot_directory;
    GoldfishPostCallback on_post;
    GoldfishGpuVerifyMode verify_mode = GoldfishGpuVerifyMode::kLog;
};

class IGoldfishGpu {
  public:
    virtual ~IGoldfishGpu() = default;

    // Lifecycle & configuration.
    virtual absl::Status Start(GoldfishGpuConfig config) = 0;
    virtual void Stop() = 0;
    // Like Stop(), but for process exit: quiesces the render threads and
    // channels without destroying the gfxstream Renderer/RenderLib, whose
    // teardown (FrameBuffer::finalize) is not safe once QEMU is exiting.
    virtual void StopForProcessExit() = 0;
    virtual void FlushReadPixelPipeline() = 0;
    // The verify mode is normally part of GoldfishGpuConfig; this changes it
    // at any time, also after Start() (tests switch to kEnforce/kStrict for a
    // single scenario without restarting the renderer).
    virtual void SetVerifyMode(GoldfishGpuVerifyMode mode) = 0;
    virtual GoldfishGpuVerifyMode GetVerifyMode() const = 0;
    virtual absl::Status VerifySteadyState() = 0;

    // Snapshot operations.
    virtual absl::Status Quiesce(std::string_view snapshot_name = "") = 0;
    virtual absl::Status PreSave(std::string_view snapshot_name) = 0;
    // Returns the meta blob that `PreSave()` cached. Pure read: FAILED_PRECONDITION
    // unless `PreSave()` completed for this snapshot (empty when not started).
    virtual absl::StatusOr<std::vector<uint8_t>> Save(std::string_view snapshot_name) = 0;
    virtual absl::Status PostSave(std::string_view snapshot_name) = 0;
    virtual absl::Status PreLoad(std::string_view snapshot_name) = 0;
    virtual absl::Status Load(std::string_view snapshot_name, const uint8_t* meta_data,
                              size_t meta_size) = 0;
    virtual absl::Status PostLoad(std::string_view snapshot_name) = 0;
    // Called when QEMU reports that a snapshot save or load finished (completed
    // or failed). Undoes a `Quiesce()`/`PreSave()`/`PreLoad()` whose matching
    // `PostSave()`/`PostLoad()` never ran because an earlier VMState section
    // failed, so the renderer does not stay paused after the VM resumes.
    virtual void OnSnapshotOperationFinished() = 0;

    // Goldfish Address Space / ASG operations. The ASG and the opengles pipe
    // surfaces share one lifecycle vocabulary: Create (fresh id) | Restore
    // (device re-announces a saved id on snapshot load) -> ... -> Close.
    virtual void SetMemoryMappingCallbacks(MapUserMemoryCallback map_cb,
                                           UnmapUserMemoryCallback unmap_cb,
                                           ResolveDmaHostAddrCallback resolve_dma_cb) = 0;
    virtual absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t size) = 0;
    // Snapshot load: registers storage for a saved `context_handle` before
    // `Load()` recreates its gfxstream consumer.
    virtual absl::StatusOr<AsgBlob> RestoreAsgBlob(uint32_t context_handle, uint64_t size) = 0;
    virtual absl::Status StartAsg(const AsgBlob& blob) = 0;
    virtual absl::Status PingAsg(const AsgBlob& blob) = 0;
    // Never blocks (runs with the BQL held from an RCU-deferred teardown).
    virtual void CloseAsg(const AsgBlob& blob) = 0;

    // Goldfish Sync operations.
    virtual void SetTimelineIncCallback(TimelineIncCallback callback) = 0;
    virtual void TriggerHostWait(uint64_t glsync_handle, uint64_t thread_handle,
                                 uint64_t guest_timeline_handle) = 0;

    // Goldfish Pipe GPU service operations.
    virtual uint64_t CreateProcessContext() = 0;
    // Asynchronous: the gfxstream cleanup waits for the process' render
    // threads, which need the guest (and the BQL) to make progress, so it runs
    // on a worker thread and this returns immediately.
    virtual void DestroyProcessContext(uint64_t puid) = 0;
    virtual void ColorBufferRef(uint32_t handle) = 0;
    virtual void ColorBufferLastRef(uint32_t handle) = 0;
    virtual absl::StatusOr<uint32_t> CreateOpenglesPipe(OpenglesReadCallback on_host_data,
                                                        OpenglesWritableCallback on_writable) = 0;
    // Snapshot load: re-attaches the pipe's callbacks to a saved `id`. The
    // pipe device loads before `Load()` recreates the render channel, so the
    // callbacks are staged until then; `Load()` fails if any stay unmatched.
    virtual absl::Status RestoreOpenglesPipe(uint32_t id, OpenglesReadCallback on_host_data,
                                             OpenglesWritableCallback on_writable) = 0;
    // Never blocks. Returns UNAVAILABLE when the render channel cannot accept
    // `size` bytes right now; the caller must retry after `on_writable` fires.
    virtual absl::Status WriteOpenglesPipe(uint32_t id, const uint8_t* data, size_t size) = 0;
    virtual void CloseOpenglesPipe(uint32_t id) = 0;
};

IGoldfishGpu& GetGoldfishGpu();

using RenderLibFactory = std::function<std::unique_ptr<gfxstream::RenderLib>()>;
void SetRenderLibFactoryForTesting(RenderLibFactory factory);
// Blocks until every DestroyProcessContext() cleanup queued so far has run.
void WaitForProcessCleanupsForTesting();

}  // namespace goldfish::legacy
