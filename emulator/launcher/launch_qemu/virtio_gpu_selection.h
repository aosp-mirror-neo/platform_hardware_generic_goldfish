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

#include <string_view>

#include "absl/status/statusor.h"

#include "android/cmdline_definitions.h"
#include "goldfish/snapshot/snapshot_store.h"

namespace android::goldfish {

// Which virtio-gpu device the launcher instantiates for API >= 37 guests.
// Both drive gfxstream; they differ in where the device lives and how it
// reaches the renderer (b/576450291):
//   kRutabaga:  QEMU's in-tree virtio-gpu-rutabaga, through librutabaga_ffi.
//               Deprecated fallback; selecting it logs a warning.
//   kGfxstream: the goldfish plugin's virtio-gpu-gfxstream, calling
//               libgfxstream_backend's stream_renderer_* API directly.
// Exactly one of them is put on the command line, chosen with the
// "-virtio-gpu-device gfxstream|rutabaga" option (AndroidOptions::virtio_gpu_device).
enum class VirtioGpuDevice {
    kRutabaga,
    kGfxstream,
};

inline constexpr VirtioGpuDevice kDefaultVirtioGpuDevice = VirtioGpuDevice::kGfxstream;

// The device selected by `opts` (-virtio-gpu-device; unset means the
// default). INVALID_ARGUMENT, naming the option and the accepted values, for
// anything else.
absl::StatusOr<VirtioGpuDevice> SelectedVirtioGpuDevice(const AndroidOptions& opts);

// The QEMU -device name: "virtio-gpu-rutabaga" / "virtio-gpu-gfxstream".
std::string_view VirtioGpuDeviceName(VirtioGpuDevice device);

// The snapshot-version field value for the device (see snapshot_store.h).
constexpr ::goldfish::snapshot::SnapshotGpuDevice ToSnapshotGpuDevice(VirtioGpuDevice device) {
    return device == VirtioGpuDevice::kGfxstream
                   ? ::goldfish::snapshot::SnapshotGpuDevice::kGfxstream
                   : ::goldfish::snapshot::SnapshotGpuDevice::kRutabaga;
}

}  // namespace android::goldfish
