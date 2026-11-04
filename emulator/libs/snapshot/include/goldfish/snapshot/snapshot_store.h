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

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace goldfish::snapshot {

// Snapshot version stored in <avd>/snapshots/<name>/version.txt and checked
// before a snapshot is loaded (by the launcher before passing -loadvm, and by
// VmOperations before a runtime load). Layout of the 32-bit value:
//
//   [30..16]  gfxstream framebuffer snapshot version
//             (gfxstream::kFramebufferSnapshotVersion, 15 bits; bit 31 is
//             left clear so the value is a positive int32)
//   [15..14]  SnapshotGpuDevice: which virtio-gpu device wrote the VM state
//             (0 = virtio-gpu-rutabaga, 1 = virtio-gpu-gfxstream). The two
//             devices use differently named VMState sections, so a snapshot
//             saved with one cannot be loaded by the other; encoding the
//             device here turns that into an up-front cold boot.
//   [13.. 0]  kSnapshotVersionBase, the emulator's own format version
//
// Folding the gfxstream version in here means that whenever gfxstream changes
// its framebuffer stream format, snapshots written with the previous format
// are rejected up front and the emulator falls back to a cold boot instead of
// failing inside FrameBuffer::onLoad() after guest RAM and device state have
// already been restored.
//
// The launcher composes the full version with MakeSnapshotVersion() (it is the
// only goldfish binary that may depend on gfxstream headers) and hands it to
// the QEMU plugin through the avdstart device, so that both sides agree.
//
// Bump kSnapshotVersionBase for incompatible changes on the goldfish side
// (e.g. VMState or hardware-properties.ini layout changes).
// History:
//   1 -> 2: hardware-properties.ini retired cachePartition/useQcow2 fields
//           (b/573413085) and folded gfxstream::kFramebufferSnapshotVersion
//           into bits [30..16].
//   2 -> 3: the renderer (gfxstream/rutabaga) state moved from
//           <avd>/renderersave/default_boot/ to <avd>/snapshots/<name>/renderer/
//           (b/574891578); older snapshots cannot find their renderer state.
//   3 -> 4: the VM state (guest RAM + device state) moved from the first
//           writable disk image QEMU happened to find (encryptionkey.img.qcow2,
//           or sdcard*.qcow2 when an sdcard is attached) to the dedicated
//           <avd>/snapshots.qcow2 (b/576262656); older snapshots are not
//           looked for in the old place.
//   4 -> 5: avd_universe::gps::Location added an accuracy field (8-byte
//           double), altering the avd_info VMState binary layout.
//   (5, b/576450291): bits [15..14] became the SnapshotGpuDevice field. Not a
//           base bump: snapshots written before the field existed read as
//           device 0 = rutabaga, so switching the default to
//           virtio-gpu-gfxstream (device 1) cold-boots them cleanly while
//           -virtio-gpu-device rutabaga continues to match device 0.
inline constexpr int kSnapshotVersionBase = 5;
static_assert(kSnapshotVersionBase > 0 && kSnapshotVersionBase < (1 << 14),
              "Base snapshot version does not fit in 14 bits.");

// The virtio-gpu device that owns the renderer state of a snapshot.
enum class SnapshotGpuDevice : int {
    kRutabaga = 0,   // QEMU's virtio-gpu-rutabaga (VMState "virtio-gpu-rutabaga")
    kGfxstream = 1,  // goldfish plugin's virtio-gpu-gfxstream (VMState "virtio-gpu-gfxstream")
};

inline constexpr int kSnapshotGpuDeviceShift = 14;
inline constexpr int kSnapshotGpuDeviceMask = 0x3 << kSnapshotGpuDeviceShift;

// The full snapshot version for the given gfxstream framebuffer snapshot
// version (must fit in 15 bits) and virtio-gpu device.
constexpr int MakeSnapshotVersion(int gfxstream_snapshot_version,
                                  SnapshotGpuDevice gpu_device = SnapshotGpuDevice::kRutabaga) {
    return (gfxstream_snapshot_version << 16) |
           (static_cast<int>(gpu_device) << kSnapshotGpuDeviceShift) | kSnapshotVersionBase;
}

constexpr SnapshotGpuDevice SnapshotVersionGpuDevice(int version) {
    return static_cast<SnapshotGpuDevice>((version & kSnapshotGpuDeviceMask) >>
                                          kSnapshotGpuDeviceShift);
}
constexpr int SnapshotVersionBase(int version) {
    return version & ((1 << kSnapshotGpuDeviceShift) - 1);
}
constexpr int SnapshotVersionGfxstream(int version) {
    return version >> 16;
}

// The QEMU device name ("virtio-gpu-rutabaga" / "virtio-gpu-gfxstream") for a
// SnapshotGpuDevice, or "unknown" for a value no emulator writes.
std::string_view SnapshotGpuDeviceName(SnapshotGpuDevice device);

// A human readable reason why a snapshot with version `found` cannot be loaded
// by an emulator expecting `expected`, naming the field(s) that differ (e.g.
// "saved with virtio-gpu-gfxstream but this emulator uses virtio-gpu-rutabaga").
// Returns an empty string when the versions are equal.
std::string DescribeSnapshotVersionMismatch(int found, int expected);

// Whether `name` can be used as a snapshot name. A snapshot name is both the
// qcow2 internal snapshot tag and a directory name under <avd>/snapshots, so
// it must be non-empty, must not be ".", and must not contain "..", path
// separators ('/', '\\'), or characters reserved in filenames on Windows
// (':', '*', '?', '"', '<', '>', '|').
bool IsValidSnapshotName(std::string_view name);

// OK if IsValidSnapshotName(name), else INVALID_ARGUMENT with a message that
// spells out the rules, for callers that report the name to a user.
absl::Status ValidateSnapshotName(std::string_view name);

// Owns the layout and lifecycle of the per-snapshot metadata that lives
// outside the qcow2 image, under <avd>/snapshots/<name>/. Snapshot names are
// opaque here: only the launcher knows about the Quickboot snapshot
// ("default_boot", the -snapshot default); QEMU and the plugin treat every
// snapshot alike.
//
//
//   <avd>/snapshots/<name>/version.txt   Version() at save time
//   <avd>/snapshots/<name>/renderer/     gfxstream/rutabaga renderer state,
//                                        written and read by the virtio-gpu
//                                        device (virtio-gpu-rutabaga or
//                                        virtio-gpu-gfxstream, which is given
//                                        snapshot_directory=Root())
//
// The VM state itself is the qcow2 internal snapshot tagged <name>; this
// class never touches it. All methods take the snapshot name, never a path,
// and reject invalid names (see IsValidSnapshotName). Implementations must be
// safe to call from any thread; callers serialize operations on one name.
class SnapshotStore {
  public:
    virtual ~SnapshotStore() = default;

    // The snapshot version this emulator writes and accepts.
    virtual int Version() const = 0;

    // <avd>/snapshots
    virtual std::filesystem::path Root() const = 0;
    // <avd>/snapshots/<name>
    virtual std::filesystem::path Dir(std::string_view name) const = 0;
    // <avd>/snapshots.qcow2: the qcow2 image that holds the VM state of every
    // snapshot of this AVD (one internal qcow2 snapshot per name). It is
    // attached to QEMU as a block node without a guest device and named with
    // -vmstate-node, so the VM state never lands in a disk image.
    virtual std::filesystem::path VmStateImage() const = 0;

    // Whether <avd>/snapshots/<name>/ exists.
    virtual bool Exists(std::string_view name) const = 0;

    // The version recorded in version.txt. NOT_FOUND if the snapshot has no
    // version file, DATA_LOSS if it cannot be parsed.
    virtual absl::StatusOr<int> ReadVersion(std::string_view name) const = 0;

    // Whether version.txt exists and equals Version().
    virtual bool IsCompatible(std::string_view name) const = 0;

    // Creates <avd>/snapshots/<name>/ (and parents) if missing.
    virtual absl::Status Prepare(std::string_view name) = 0;

    // Creates the directory if needed and records Version() in version.txt,
    // overwriting any previous value.
    virtual absl::Status WriteVersion(std::string_view name) = 0;

    // Removes <avd>/snapshots/<name>/ and everything under it. Succeeds if the
    // directory does not exist.
    virtual absl::Status Remove(std::string_view name) = 0;
};

// SnapshotStore backed by the file system of an AVD content directory.
class FileSnapshotStore : public SnapshotStore {
  public:
    // `snapshot_version` is the value written by WriteVersion() and required
    // by IsCompatible(); see MakeSnapshotVersion().
    FileSnapshotStore(std::filesystem::path avd_content_dir, int snapshot_version);

    int Version() const override { return version_; }
    std::filesystem::path Root() const override;
    std::filesystem::path Dir(std::string_view name) const override;
    std::filesystem::path VmStateImage() const override;
    bool Exists(std::string_view name) const override;
    absl::StatusOr<int> ReadVersion(std::string_view name) const override;
    bool IsCompatible(std::string_view name) const override;
    absl::Status Prepare(std::string_view name) override;
    absl::Status WriteVersion(std::string_view name) override;
    absl::Status Remove(std::string_view name) override;

  private:
    std::filesystem::path VersionFile(std::string_view name) const;

    const std::filesystem::path avd_content_dir_;
    const std::filesystem::path snapshots_dir_;
    const int version_;
};

}  // namespace goldfish::snapshot
