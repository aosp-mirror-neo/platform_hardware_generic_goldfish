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
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

#include "android/status/status_macros.h"
#include "device.h"
#include "goldfish/metrics/snapshot_metrics.h"
#include "goldfish/snapshot/snapshot_store.h"
#include "render-utils/snapshot_operations.h"
#include "virtio_gpu_selection.h"

namespace android::goldfish {

// The snapshot version this emulator writes and accepts; see
// goldfish/snapshot/snapshot_store.h for the layout and history. The launcher
// composes it here (it is the only goldfish binary that depends on gfxstream
// headers) and passes it to the QEMU plugin through the avdstart device.
using ::goldfish::snapshot::kSnapshotVersionBase;

// Name of the Quickboot snapshot, loaded at boot and saved at exit when no
// -snapshot <name> is given. Only the launcher knows this name: QEMU and the
// emulator plugin treat every snapshot alike.
inline constexpr std::string_view kDefaultSnapshotName = "default_boot";

inline constexpr int kGfxstreamSnapshotVersion =
        static_cast<int>(gfxstream::kFramebufferSnapshotVersion);
static_assert(kGfxstreamSnapshotVersion > 0 && kGfxstreamSnapshotVersion < (1 << 15),
              "gfxstream framebuffer snapshot version does not fit in 15 bits.");

static_assert(::goldfish::snapshot::MakeSnapshotVersion(
                      kGfxstreamSnapshotVersion,
                      ::goldfish::snapshot::SnapshotGpuDevice::kGfxstream) > 0,
              "Snapshot version must be a positive int32.");

// The snapshot version this launch writes and accepts for the given
// virtio-gpu device: the device is part of the version (see
// snapshot_store.h), so a snapshot saved with the other device is cold-booted
// instead of failing mid-load.
constexpr int SnapshotVersionFor(VirtioGpuDevice device) {
    return ::goldfish::snapshot::MakeSnapshotVersion(kGfxstreamSnapshotVersion,
                                                     ToSnapshotGpuDevice(device));
}

// SnapshotVersionFor() the device selected by -virtio-gpu-device; an invalid
// option value is reported as INVALID_ARGUMENT.
inline absl::StatusOr<int> CurrentSnapshotVersion(const EmulatorConfig& emulator) {
    ASSIGN_OR_RETURN(const VirtioGpuDevice device, SelectedVirtioGpuDevice(emulator.opts()));
    return SnapshotVersionFor(device);
}

// Handles QEMU snapshot command line parameters: -loadvm/-savevm for the
// Quickboot snapshot, and the block node that holds every snapshot's VM
// state.
//
// QEMU stores the VM state of an internal snapshot (guest RAM, device state)
// in one qcow2 image and only a disk-state snapshot in every other one. Left
// to itself it picks the first writable image it finds, which for us was
// encryptionkey.img.qcow2 or, with an sdcard attached, sdcard*.qcow2
// (b/576262656). This device attaches <avd>/snapshots.qcow2 as a block node
// with no guest device and tells QEMU with -vmstate-node to put the VM state
// there, so the location is stable and disk images stay disk images.
class SnapshotDevice : public Device {
  public:
    using LoadStatus = ::goldfish::metrics::QuickbootLoadStatus;
    using SaveStatus = ::goldfish::metrics::QuickbootSaveStatus;

    // QEMU block node name of the VM-state image; also the value passed to
    // -vmstate-node.
    static constexpr std::string_view kVmStateNodeName = "snapshots";

    explicit SnapshotDevice() : Device("snapshot") {}

    absl::Status initialize(const EmulatorConfig& emulator) override;
    std::vector<std::string> getQemuParameters(const EmulatorConfig& emulator) const override;

    LoadStatus load_status() const { return load_status_; }
    SaveStatus save_status() const { return save_status_; }
    // Whether the launch-time Quickboot snapshot is loaded at boot or saved at exit.
    bool is_enabled() const {
        return load_status_ == LoadStatus::kLoading || save_status_ == SaveStatus::kSaving;
    }
    // Whether any snapshot may be saved or loaded in this session, at launch
    // or at runtime (gRPC, console). Only -no-snapshot turns this off;
    // -no-snapshot-load / -no-snapshot-save only affect the Quickboot snapshot.
    bool snapshots_enabled() const { return snapshots_enabled_; }
    // The version written to / required in version.txt (valid after initialize()).
    int snapshot_version() const { return snapshot_version_; }
    // <avd>/snapshots.qcow2, the image attached as the VM-state node. Empty
    // when snapshots are disabled (-no-snapshot) and no such image exists yet.
    const std::filesystem::path& vmstate_image() const { return vmstate_image_; }
    // Whether vmstate_image() is attached read-only because it is not
    // writable. Snapshots can then be loaded but not saved.
    bool vmstate_read_only() const { return vmstate_read_only_; }

    static std::string_view get_snapshot_name(const EmulatorConfig& emulator);
    static bool are_snapshots_enabled(const EmulatorConfig& emulator);

  private:
    static bool should_load_snapshot(const EmulatorConfig& emulator, std::string& reason,
                                     LoadStatus* status = nullptr);
    static bool should_save_snapshot(const EmulatorConfig& emulator, std::string& reason,
                                     SaveStatus* status = nullptr);
    // Sets vmstate_image_ to <avd>/snapshots.qcow2. Creates the image if
    // missing, replacing anything there that is not a qcow2 image, and returns
    // true when an existing file was replaced, in which case every snapshot of
    // this AVD has lost its VM state. With -no-snapshot it only attaches an
    // existing qcow2 image, so that listing and deleting snapshots still see
    // the VM state, and never creates or replaces one.
    absl::StatusOr<bool> prepare_vmstate_image(const ::goldfish::snapshot::SnapshotStore& store);
    // Why snapshot `name` is not loadable by `store` (version file missing,
    // or which fields of the version differ), for logs and errors.
    static std::string describe_incompatibility(const ::goldfish::snapshot::SnapshotStore& store,
                                                std::string_view name);

    std::string snapshot_name_;
    std::filesystem::path vmstate_image_;
    bool strict_snapshot_load_ = false;
    bool vmstate_read_only_ = false;
    bool snapshots_enabled_ = true;
    int snapshot_version_ = SnapshotVersionFor(kDefaultVirtioGpuDevice);
    LoadStatus load_status_ = LoadStatus::kSkippedOldSnapshot;
    SaveStatus save_status_ = SaveStatus::kDisabledCmdline;
};

}  // namespace android::goldfish
