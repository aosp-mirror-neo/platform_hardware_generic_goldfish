// Copyright (C) 2026 The Android Open Source Project
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

#include "snapshot_device.h"

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "android/status/status_macros.h"
#include "goldfish/file/file.h"
#include "goldfish/fsimage/qcow2.h"
#include "goldfish/snapshot/snapshot_store.h"

namespace android::goldfish {

using ::goldfish::snapshot::FileSnapshotStore;
using ::goldfish::snapshot::SnapshotStore;

absl::Status SnapshotDevice::initialize(const EmulatorConfig& emulator) {
    snapshot_name_ = get_snapshot_name(emulator);
    // A malformed -snapshot name is a user error: report it as such instead of
    // letting it surface later as a confusing "version missing" cold boot.
    if (auto s = ::goldfish::snapshot::ValidateSnapshotName(snapshot_name_); !s.ok()) {
        return s;
    }
    strict_snapshot_load_ = emulator.opts().strict_snapshot_load;
    snapshots_enabled_ = are_snapshots_enabled(emulator);

    ASSIGN_OR_RETURN(snapshot_version_, CurrentSnapshotVersion(emulator));
    FileSnapshotStore store(emulator.avd().GetContentPath(), snapshot_version_);
    ASSIGN_OR_RETURN(const bool vmstate_recreated, prepare_vmstate_image(store));

    std::string skip_load_reason;
    if (should_load_snapshot(emulator, skip_load_reason, &load_status_)) {
        if (vmstate_recreated) {
            // version.txt still claims compatibility, but the VM state it
            // described is gone with the image that held it; -loadvm would
            // make QEMU fail to start instead of cold booting.
            if (strict_snapshot_load_) {
                return absl::FailedPreconditionError(
                        "Unable to load snapshot: the VM-state image was recreated");
            }
            LOG(WARNING) << "VM-state image was recreated, performing cold boot.";
            load_status_ = LoadStatus::kSkippedOldSnapshot;
        } else if (store.IsCompatible(snapshot_name_)) {
            LOG(INFO) << "Loading '" << snapshot_name_ << "' ...";
        } else {
            const std::string reason = describe_incompatibility(store, snapshot_name_);
            if (strict_snapshot_load_) {
                return absl::FailedPreconditionError(
                        absl::StrCat("Unable to load snapshot: ", reason));
            }
            LOG(WARNING) << "Snapshot '" << snapshot_name_ << "' is not loadable (" << reason
                         << "), performing cold boot.";
            load_status_ = LoadStatus::kSkippedOldSnapshot;
        }
    } else {
        if (strict_snapshot_load_) {
            return absl::FailedPreconditionError(
                    absl::StrCat("Unable to load snapshot: ", skip_load_reason));
        }
        LOG(WARNING) << "Snapshot load is disabled: " << skip_load_reason
                     << ", performing cold boot.";
    }

    std::string skip_save_reason;
    const bool save = should_save_snapshot(emulator, skip_save_reason, &save_status_);
    if (save && vmstate_read_only_) {
        LOG(WARNING) << "Snapshot save is disabled: " << vmstate_image_ << " is read-only.";
        save_status_ = SaveStatus::kDisabledCmdline;
    } else if (save) {
        // Only create the directory here. version.txt is written once a save
        // actually completes (see QemuVmOperations), so that a crash before the
        // exit-time save cannot leave a version file next to stale or missing
        // qcow2 state.
        if (auto s = store.Prepare(snapshot_name_); !s.ok()) {
            LOG(WARNING) << "Failed to create snapshot directory: " << s
                         << ", and snapshot save is disabled.";
            save_status_ = SaveStatus::kDirectoryError;
        }
    } else {
        LOG(WARNING) << "Snapshot save is disabled: " << skip_save_reason << ".";
    }

    return absl::OkStatus();
}

std::string SnapshotDevice::describe_incompatibility(const SnapshotStore& store,
                                                     std::string_view name) {
    const absl::StatusOr<int> found = store.ReadVersion(name);
    if (!found.ok()) {
        return absl::StrCat("snapshot version missing: ", found.status().message());
    }
    std::string reason = absl::StrCat(
            "snapshot version ", *found, " != ", store.Version(), ": ",
            ::goldfish::snapshot::DescribeSnapshotVersionMismatch(*found, store.Version()));
    // When only the virtio-gpu device differs, the snapshot is still loadable
    // by selecting the device it was saved with: say so, since this is what
    // every pre-existing snapshot hits after the default device changes.
    using ::goldfish::snapshot::SnapshotGpuDevice;
    using ::goldfish::snapshot::SnapshotVersionGpuDevice;
    const SnapshotGpuDevice saved_with = SnapshotVersionGpuDevice(*found);
    const bool known_device = saved_with == SnapshotGpuDevice::kRutabaga ||
                              saved_with == SnapshotGpuDevice::kGfxstream;
    if (known_device && saved_with != SnapshotVersionGpuDevice(store.Version()) &&
        ::goldfish::snapshot::SnapshotVersionBase(*found) ==
                ::goldfish::snapshot::SnapshotVersionBase(store.Version()) &&
        ::goldfish::snapshot::SnapshotVersionGfxstream(*found) ==
                ::goldfish::snapshot::SnapshotVersionGfxstream(store.Version())) {
        absl::StrAppend(&reason, " (pass '-virtio-gpu-device ",
                        saved_with == SnapshotGpuDevice::kGfxstream ? "gfxstream" : "rutabaga",
                        "' to load it)");
    }
    return reason;
}

absl::StatusOr<bool> SnapshotDevice::prepare_vmstate_image(const SnapshotStore& store) {
    const std::filesystem::path image = store.VmStateImage();
    const bool exists = base::file::exists(image);
    // snapshots.qcow2 has no backing file by design, so allow an empty backing
    // path; the default would reject (and delete) a perfectly good image on
    // every boot.
    if (exists && ::goldfish::fsimage::IsValidQcow2Image(image, /*allow_empty_backing_path=*/true)) {
        vmstate_image_ = image;
        // An image we cannot write to (e.g. a symlink to a shared, read-only
        // copy) is attached read-only: snapshots can still be loaded from it,
        // but nothing can be saved to it.
        vmstate_read_only_ = !base::file::can_write(image);
        if (vmstate_read_only_) {
            LOG(INFO) << image << " is not writable, attaching it read-only.";
        }
        return false;
    }
    if (!snapshots_enabled_) {
        // With -no-snapshot nothing may be saved or loaded, but ListSnapshots
        // and DeleteSnapshot stay available, which is why an existing image is
        // attached above: without the node QEMU would resolve the VM state to
        // the first writable disk image and a delete would leave the VM state
        // behind in snapshots.qcow2. Never create or replace the image in such
        // a session, though.
        return false;
    }
    if (exists) {
        // Whatever is there cannot hold snapshots; the snapshots that referred
        // to it are lost either way, so start over rather than fail to boot.
        LOG(WARNING) << image
                     << " is not a qcow2 image, recreating it; existing snapshots "
                        "of this AVD will cold boot.";
        RETURN_IF_ERROR(base::file::rm(image));
    }
    // The VM state is written past the end of the virtual disk, so the image
    // needs no virtual size at all; it only has to be a valid qcow2.
    RETURN_IF_ERROR(::goldfish::fsimage::CreateQcow2Overlay(image, /*raw_size=*/0));
    vmstate_image_ = image;
    return exists;
}

std::vector<std::string> SnapshotDevice::getQemuParameters(
        const EmulatorConfig& /*emulator*/) const {
    std::vector<std::string> params;
    if (!vmstate_image_.empty()) {
        // No -device: the node is owned by the monitor and only used for
        // snapshots. Cache settings match Qcow2Drive; the snapshot is as
        // durable as the disk images it belongs with.
        params.push_back("-blockdev");
        params.push_back(absl::StrCat(
                "driver=qcow2,node-name=", kVmStateNodeName,
                ",file.driver=file,file.filename=", EscapeQemuOptionValue(vmstate_image_.string()),
                ",overlap-check=none,cache.direct=off,cache.no-flush=on",
                vmstate_read_only_ ? ",read-only=on" : ""));
        params.push_back("-vmstate-node");
        params.push_back(std::string(kVmStateNodeName));
    }
    if (load_status_ == LoadStatus::kLoading) {
        params.push_back("-loadvm");
        params.push_back(snapshot_name_);
        if (strict_snapshot_load_) {
            params.push_back("-strict-loadvm");
        }
    }
    if (save_status_ == SaveStatus::kSaving) {
        params.push_back("-savevm");
        params.push_back(snapshot_name_);
    }
    return params;
}

bool SnapshotDevice::should_load_snapshot(const EmulatorConfig& emulator, std::string& reason,
                                          LoadStatus* status) {
    const auto& o = emulator.opts();
    const auto& a = emulator.avd();
    if (a.Hw().fastboot_forceColdBoot) {
        reason = "fastboot.forceColdBoot is true";
        if (status) {
            *status = LoadStatus::kSkippedAvd;
        }
        return false;
    }
    if (o.no_snapshot) {
        reason = "-no-snapshot is set";
        if (status) {
            *status = LoadStatus::kSkippedCmdline;
        }
        return false;
    }
    if (o.no_snapshot_load) {
        reason = "-no-snapshot-load is set";
        if (status) {
            *status = LoadStatus::kSkippedCmdline;
        }
        return false;
    }
    // Hidden alias of -no-snapshot-load, still used by Android Studio for Cold Boot.
    if (o.no_snapstorage) {
        reason = "-no-snapstorage is set";
        if (status) {
            *status = LoadStatus::kSkippedCmdline;
        }
        return false;
    }
    if (status) {
        *status = LoadStatus::kLoading;
    }
    return true;
}

bool SnapshotDevice::should_save_snapshot(const EmulatorConfig& emulator, std::string& reason,
                                          SaveStatus* status) {
    const auto& o = emulator.opts();
    const auto& a = emulator.avd();
    if (a.Hw().fastboot_forceColdBoot) {
        reason = "fastboot.forceColdBoot is true";
        if (status) {
            *status = SaveStatus::kDisabledAvd;
        }
        return false;
    }
    if (o.no_snapshot) {
        reason = "-no-snapshot is set";
        if (status) {
            *status = SaveStatus::kDisabledCmdline;
        }
        return false;
    }
    if (o.no_snapshot_save) {
        reason = "-no-snapshot-save is set";
        if (status) {
            *status = SaveStatus::kDisabledCmdline;
        }
        return false;
    }
    if (status) {
        *status = SaveStatus::kSaving;
    }
    return true;
}

std::string_view SnapshotDevice::get_snapshot_name(const EmulatorConfig& emulator) {
    const auto& o = emulator.opts();
    return (o.snapshot && o.snapshot[0] != '\0') ? o.snapshot : kDefaultSnapshotName;
}

bool SnapshotDevice::are_snapshots_enabled(const EmulatorConfig& emulator) {
    return !emulator.opts().no_snapshot;
}

}  // namespace android::goldfish
