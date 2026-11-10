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


// C ABI of the GoldfishGpu lifecycle and snapshot hooks used by the QEMU
// shims (goldfish_gpu_low_level.c and goldfish_pipe_low_level.c). Every
// function forwards to the process-wide IGoldfishGpu and turns an absl::Status
// into a logged bool.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu_c_api.h"

extern "C" bool GoldfishGpuCoreStart(uint32_t width, uint32_t height, uint32_t guest_api_level,
                                     const char* renderer_features, const char* snapshot_directory,
                                     GoldfishGpuFrameCallback on_frame, void* opaque, char* err_buf,
                                     size_t err_buf_size) {
    goldfish::legacy::GoldfishGpuConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.guest_api_level = guest_api_level;
    if (renderer_features != nullptr) {
        cfg.renderer_features = renderer_features;
    }
    if (snapshot_directory != nullptr) {
        cfg.snapshot_directory = snapshot_directory;
    }
    if (on_frame != nullptr) {
        cfg.on_post = [on_frame, opaque](uint32_t w, uint32_t h, const uint8_t* bgra) {
            on_frame(opaque, w, h, bgra);
        };
    }

    absl::Status st = goldfish::legacy::GetGoldfishGpu().Start(std::move(cfg));
    if (!st.ok()) {
        if (err_buf != nullptr && err_buf_size > 0) {
            const std::string msg(st.message());
            const size_t copy_len = std::min(msg.size(), err_buf_size - 1);
            std::memcpy(err_buf, msg.data(), copy_len);
            err_buf[copy_len] = '\0';
        }
        return false;
    }
    return true;
}

extern "C" void GoldfishGpuCoreStop(void) {
    goldfish::legacy::GetGoldfishGpu().Stop();
}

extern "C" void GoldfishGpuCoreStopForProcessExit(void) {
    goldfish::legacy::GetGoldfishGpu().StopForProcessExit();
}

extern "C" void GoldfishGpuCoreFlush(void) {
    goldfish::legacy::GetGoldfishGpu().FlushReadPixelPipeline();
}

extern "C" bool GoldfishGpuCoreQuiesce(const char* snapshot_name) {
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = goldfish::legacy::GetGoldfishGpu().Quiesce(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_gpu: Quiesce failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishGpuCorePreSave(const char* snapshot_name) {
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = goldfish::legacy::GetGoldfishGpu().PreSave(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_gpu: PreSave failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishGpuCoreSave(const char* snapshot_name, uint8_t** out_data,
                                    size_t* out_size) {
    if (out_data == nullptr || out_size == nullptr) {
        return false;
    }
    *out_data = nullptr;
    *out_size = 0;
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::StatusOr<std::vector<uint8_t>> bytes = goldfish::legacy::GetGoldfishGpu().Save(name);
    if (!bytes.ok()) {
        LOG(ERROR) << "goldfish_gpu: Save failed: " << bytes.status();
        return false;
    }
    if (!bytes->empty()) {
        auto* buf = new uint8_t[bytes->size()];
        std::memcpy(buf, bytes->data(), bytes->size());
        *out_data = buf;
        *out_size = bytes->size();
    }
    return true;
}

extern "C" void GoldfishGpuCoreFreeBuffer(uint8_t* data) {
    delete[] data;
}

extern "C" bool GoldfishGpuCorePostSave(const char* snapshot_name) {
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = goldfish::legacy::GetGoldfishGpu().PostSave(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_gpu: PostSave failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishGpuCorePreLoad(const char* snapshot_name) {
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = goldfish::legacy::GetGoldfishGpu().PreLoad(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_gpu: PreLoad failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishGpuCoreLoad(const char* snapshot_name, const uint8_t* data, size_t size) {
    if (data == nullptr && size > 0) {
        LOG(ERROR) << "goldfish_gpu: Load called with a null blob of " << size << " bytes";
        return false;
    }
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = goldfish::legacy::GetGoldfishGpu().Load(name, data, size);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_gpu: Load failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishGpuCorePostLoad(const char* snapshot_name) {
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = goldfish::legacy::GetGoldfishGpu().PostLoad(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_gpu: PostLoad failed: " << st;
        return false;
    }
    return true;
}

extern "C" void GoldfishGpuCoreOnSnapshotFinished(void) {
    goldfish::legacy::GetGoldfishGpu().OnSnapshotOperationFinished();
}

extern "C" size_t GoldfishGpuCoreMaxSnapshotSize(void) {
    return static_cast<size_t>(GOLDFISH_GPU_SNAPSHOT_MAX_BYTES);
}
