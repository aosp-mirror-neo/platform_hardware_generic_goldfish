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

#include "virtio_gpu_selection.h"

#include <string>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

namespace android::goldfish {

absl::StatusOr<VirtioGpuDevice> SelectedVirtioGpuDevice(const AndroidOptions& opts) {
    if (!opts.virtio_gpu_device) {
        return kDefaultVirtioGpuDevice;
    }
    const std::string value = absl::AsciiStrToLower(opts.virtio_gpu_device);
    if (value == "rutabaga") {
        return VirtioGpuDevice::kRutabaga;
    }
    if (value == "gfxstream") {
        return VirtioGpuDevice::kGfxstream;
    }
    return absl::InvalidArgumentError(absl::StrCat("Invalid -virtio-gpu-device '",
                                                   opts.virtio_gpu_device,
                                                   "': expected rutabaga or gfxstream"));
}

std::string_view VirtioGpuDeviceName(VirtioGpuDevice device) {
    switch (device) {
    case VirtioGpuDevice::kRutabaga:
        return "virtio-gpu-rutabaga";
    case VirtioGpuDevice::kGfxstream:
        return "virtio-gpu-gfxstream";
    }
    return "virtio-gpu-gfxstream";
}

}  // namespace android::goldfish
