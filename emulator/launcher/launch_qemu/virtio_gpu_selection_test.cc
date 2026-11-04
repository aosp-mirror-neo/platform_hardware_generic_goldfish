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

#include <gtest/gtest.h>

#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"

#include "android/cmdline_definitions.h"
#include "android/status/status_matcher_macros.h"
#include "goldfish/snapshot/snapshot_store.h"

namespace android::goldfish {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::goldfish::snapshot::SnapshotGpuDevice;
using ::testing::HasSubstr;

TEST(VirtioGpuSelectionTest, DefaultsToGfxstream) {
    AndroidOptions opts{};
    EXPECT_THAT(SelectedVirtioGpuDevice(opts), IsOkAndHolds(VirtioGpuDevice::kGfxstream));
    EXPECT_EQ(kDefaultVirtioGpuDevice, VirtioGpuDevice::kGfxstream);
}

TEST(VirtioGpuSelectionTest, OptionSelectsTheDevice) {
    AndroidOptions opts{};
    char gfxstream[] = "gfxstream";
    opts.virtio_gpu_device = gfxstream;
    EXPECT_THAT(SelectedVirtioGpuDevice(opts), IsOkAndHolds(VirtioGpuDevice::kGfxstream));
    char mixed[] = "GfxStream";
    opts.virtio_gpu_device = mixed;
    EXPECT_THAT(SelectedVirtioGpuDevice(opts), IsOkAndHolds(VirtioGpuDevice::kGfxstream));
    char rutabaga[] = "rutabaga";
    opts.virtio_gpu_device = rutabaga;
    EXPECT_THAT(SelectedVirtioGpuDevice(opts), IsOkAndHolds(VirtioGpuDevice::kRutabaga));
}

TEST(VirtioGpuSelectionTest, UnknownValueIsRejected) {
    AndroidOptions opts{};
    char virgl[] = "virgl";
    opts.virtio_gpu_device = virgl;
    EXPECT_THAT(SelectedVirtioGpuDevice(opts),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("-virtio-gpu-device 'virgl': expected rutabaga or gfxstream")));
}

TEST(VirtioGpuSelectionTest, NamesAndSnapshotField) {
    EXPECT_EQ(VirtioGpuDeviceName(VirtioGpuDevice::kRutabaga), "virtio-gpu-rutabaga");
    EXPECT_EQ(VirtioGpuDeviceName(VirtioGpuDevice::kGfxstream), "virtio-gpu-gfxstream");
    EXPECT_EQ(ToSnapshotGpuDevice(VirtioGpuDevice::kRutabaga), SnapshotGpuDevice::kRutabaga);
    EXPECT_EQ(ToSnapshotGpuDevice(VirtioGpuDevice::kGfxstream), SnapshotGpuDevice::kGfxstream);
}

}  // namespace
}  // namespace android::goldfish
