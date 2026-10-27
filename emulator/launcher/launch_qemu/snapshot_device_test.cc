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

#include "snapshot_device.h"

#include <gtest/gtest.h>

#include <fstream>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"

#include "android/base/testing/test_temp_dir.h"
#include "android/cmdline_definitions.h"
#include "android/status/status_matcher_macros.h"
#include "fake_emulator.h"
#include "goldfish/file/file.h"
#include "goldfish/fsimage/qcow2.h"

namespace android::goldfish::test {

using ::absl_testing::StatusIs;
using ::android::base::TestTempDir;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Return;
using ::testing::ReturnRef;

class SnapshotDeviceTest : public ::testing::Test {
  protected:
    void WriteSnapshotVersion(int version, const char* name = "default_boot") {
        const fs::path snapshot_dir = temp_dir_.Path() / "snapshots" / name;
        ASSERT_OK(base::file::mkdir_recursive(snapshot_dir, 0755));
        std::ofstream ofs(snapshot_dir / "version.txt");
        ofs << version;
    }

    // The AVD content directory; tests may point it at a sub-directory.
    fs::path content() const { return content_.empty() ? temp_dir_.Path() : content_; }
    fs::path vmstate_image() const { return content() / "snapshots.qcow2"; }

    // The -blockdev/-vmstate-node pair every session with snapshots enabled
    // emits, followed by `tail`.
    std::vector<std::string> WithVmState(std::vector<std::string> tail) const {
        std::vector<std::string> params = {
            "-blockdev",
            absl::StrCat("driver=qcow2,node-name=snapshots,file.driver=file,file.filename=",
                         Device::EscapeQemuOptionValue(vmstate_image().string()),
                         ",overlap-check=none,cache.direct=off,cache.no-flush=on"),
            "-vmstate-node", "snapshots"};
        params.insert(params.end(), tail.begin(), tail.end());
        return params;
    }

    static bool IsVmStateImage(const fs::path& p) {
        return ::goldfish::fsimage::IsValidQcow2Image(p, /*allow_empty_backing_path=*/true);
    }

    TestTempDir temp_dir_{"snapshot_device_test"};
    HardwareConfig hw_{};
    fs::path content_;
};

TEST_F(SnapshotDeviceTest, ColdBootWhenSnapshotVersionMissing) {
    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, WarmBootWhenSnapshotVersionMatches) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kLoading);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_THAT(
            dev.getQemuParameters(emu.config()),
            ElementsAreArray(WithVmState({"-loadvm", "default_boot", "-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, NoSnapshotDisablesLoadAndSaveViaCmdline) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));

    AndroidOptions opts{};
    opts.no_snapshot = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedCmdline);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kDisabledCmdline);
    EXPECT_FALSE(dev.is_enabled());
    EXPECT_FALSE(dev.snapshots_enabled());
    EXPECT_FALSE(SnapshotDevice::are_snapshots_enabled(emu.config()));
    EXPECT_THAT(dev.getQemuParameters(emu.config()), IsEmpty());
}

TEST_F(SnapshotDeviceTest, NoSnapshotLoadSkipsLoadViaCmdline) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));

    AndroidOptions opts{};
    opts.no_snapshot_load = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedCmdline);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_TRUE(dev.snapshots_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, NoSnapstorageSkipsLoadViaCmdline) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));

    AndroidOptions opts{};
    opts.no_snapstorage = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedCmdline);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, NoSnapshotSaveDisablesSaveViaCmdline) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));

    AndroidOptions opts{};
    opts.no_snapshot_save = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kLoading);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kDisabledCmdline);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_TRUE(dev.snapshots_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-loadvm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, ForceColdBootSkipsLoadAndSaveViaAvd) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));
    hw_.fastboot_forceColdBoot = true;

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedAvd);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kDisabledAvd);
    EXPECT_FALSE(dev.is_enabled());
    // fastboot.forceColdBoot only governs the Quickboot snapshot.
    EXPECT_TRUE(dev.snapshots_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()), ElementsAreArray(WithVmState({})));
}

TEST_F(SnapshotDeviceTest, ColdBootWhenSnapshotVersionMismatches) {
    WriteSnapshotVersion(1);

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, ColdBootWhenGfxstreamSnapshotVersionMismatches) {
    WriteSnapshotVersion(((kGfxstreamSnapshotVersion - 1) << 16) | kSnapshotVersionBase);

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_TRUE(dev.is_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, InitializeCreatesDirectoryButDoesNotWriteVersionUpFront) {
    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    const fs::path snapshot_dir = temp_dir_.Path() / "snapshots" / "default_boot";
    EXPECT_TRUE(base::file::is_dir(snapshot_dir));
    // version.txt is only written when a save completes; a crash before the exit-time save must
    // not leave a version file claiming compatibility next to stale or missing qcow2 state.
    EXPECT_FALSE(base::file::exists(snapshot_dir / "version.txt"));
}

TEST_F(SnapshotDeviceTest, InitializeLeavesStaleVersionAloneUntilNextSave) {
    WriteSnapshotVersion(1);

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    std::ifstream ifs(temp_dir_.Path() / "snapshots" / "default_boot" / "version.txt");
    int version = 0;
    ASSERT_TRUE(ifs >> version);
    EXPECT_EQ(version, 1) << "a cold boot must not rewrite version.txt before the save";
}

TEST_F(SnapshotDeviceTest, InvalidSnapshotNameIsRejectedUpFront) {
    for (const char* name : {"a/b", "my..snap", "."}) {
        SCOPED_TRACE(name);
        AndroidOptions opts{};
        opts.snapshot = const_cast<char*>(name);
        FakeEmulator emu(opts);
        EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
        EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

        SnapshotDevice dev;
        EXPECT_THAT(dev.initialize(emu.config()),
                    StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("Invalid snapshot name")));
    }
}

TEST_F(SnapshotDeviceTest, DirectoryErrorWhenSnapshotDirCreationFails) {
    ASSERT_OK(base::file::touch(temp_dir_.Path() / "snapshots"));

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kDirectoryError);
    EXPECT_FALSE(dev.is_enabled());
    EXPECT_THAT(dev.getQemuParameters(emu.config()), ElementsAreArray(WithVmState({})));
}

// VM-state image (<avd>/snapshots.qcow2, b/576262656).

TEST_F(SnapshotDeviceTest, CreatesVmStateImageAndNamesTheNode) {
    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.vmstate_image(), vmstate_image());
    EXPECT_TRUE(IsVmStateImage(vmstate_image()));
    EXPECT_EQ(::goldfish::fsimage::ReadQcow2BackingPath(vmstate_image()).value_or("x"), fs::path());
}

TEST_F(SnapshotDeviceTest, KeepsExistingVmStateImage) {
    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice first;
    ASSERT_OK(first.initialize(emu.config()));
    const auto mtime = base::file::last_write_time(vmstate_image());
    ASSERT_OK(mtime);

    SnapshotDevice second;
    EXPECT_OK(second.initialize(emu.config()));
    EXPECT_EQ(base::file::last_write_time(vmstate_image()).value(), *mtime)
            << "an existing snapshots.qcow2 must not be recreated";
}

TEST_F(SnapshotDeviceTest, ReplacesVmStateFileThatIsNotQcow2) {
    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));
    std::ofstream(vmstate_image()) << "not a qcow2";

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_TRUE(IsVmStateImage(vmstate_image()));
}

TEST_F(SnapshotDeviceTest, ColdBootWhenVmStateImageWasRecreated) {
    // version.txt says the snapshot is compatible, but the image that held its
    // VM state is being replaced: -loadvm would make QEMU fail to start.
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));
    std::ofstream(vmstate_image()) << "not a qcow2";

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_TRUE(IsVmStateImage(vmstate_image()));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, ColdBootWhenSnapshotWasSavedWithTheOtherVirtioGpuDevice) {
    // No -virtio-gpu-device: virtio-gpu-gfxstream (default), so a snapshot saved
    // under the previous default (virtio-gpu-rutabaga) cold-boots cleanly.
    WriteSnapshotVersion(::goldfish::snapshot::MakeSnapshotVersion(
            kGfxstreamSnapshotVersion, ::goldfish::snapshot::SnapshotGpuDevice::kRutabaga));

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    {
        // The cold-boot warning names the option that would load the snapshot.
        absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
        EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, ::testing::_,
                             AllOf(HasSubstr("performing cold boot"),
                                   HasSubstr("saved with virtio-gpu-rutabaga"),
                                   HasSubstr("pass '-virtio-gpu-device rutabaga' to load it"))));
        log.StartCapturingLogs();
        EXPECT_OK(dev.initialize(emu.config()));
    }
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedOldSnapshot);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kSaving);
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, StrictLoadNamesTheVirtioGpuDeviceMismatch) {
    WriteSnapshotVersion(::goldfish::snapshot::MakeSnapshotVersion(
            kGfxstreamSnapshotVersion, ::goldfish::snapshot::SnapshotGpuDevice::kRutabaga));

    AndroidOptions opts{};
    opts.strict_snapshot_load = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_THAT(dev.initialize(emu.config()),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("saved with virtio-gpu-rutabaga but this emulator "
                                         "uses virtio-gpu-gfxstream"),
                               HasSubstr("pass '-virtio-gpu-device rutabaga' to load it"))));
}

TEST_F(SnapshotDeviceTest, StrictLoadHintsGfxstreamWhenSnapshotWasSavedWithIt) {
    WriteSnapshotVersion(SnapshotVersionFor(VirtioGpuDevice::kGfxstream));

    AndroidOptions opts{};
    opts.strict_snapshot_load = true;
    opts.virtio_gpu_device = const_cast<char*>("rutabaga");
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_THAT(dev.initialize(emu.config()),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("saved with virtio-gpu-gfxstream but this emulator "
                                         "uses virtio-gpu-rutabaga"),
                               HasSubstr("pass '-virtio-gpu-device gfxstream' to load it"))));
}

TEST_F(SnapshotDeviceTest, NoVirtioGpuDeviceHintWhenMoreThanTheDeviceDiffers) {
    // Selecting the other device would not make this snapshot loadable.
    WriteSnapshotVersion(::goldfish::snapshot::MakeSnapshotVersion(
            kGfxstreamSnapshotVersion - 1, ::goldfish::snapshot::SnapshotGpuDevice::kRutabaga));

    AndroidOptions opts{};
    opts.strict_snapshot_load = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_THAT(dev.initialize(emu.config()),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("saved with virtio-gpu-rutabaga"),
                               Not(HasSubstr("-virtio-gpu-device")))));
}

TEST_F(SnapshotDeviceTest, WarmBootWhenRutabagaDeviceSelectedAndSnapshotMatches) {
    WriteSnapshotVersion(SnapshotVersionFor(VirtioGpuDevice::kRutabaga));

    AndroidOptions opts{};
    opts.virtio_gpu_device = const_cast<char*>("rutabaga");
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.snapshot_version(), SnapshotVersionFor(VirtioGpuDevice::kRutabaga));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kLoading);
    EXPECT_THAT(
            dev.getQemuParameters(emu.config()),
            ElementsAreArray(WithVmState({"-loadvm", "default_boot", "-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, WarmBootWhenGfxstreamDeviceSelectedAndSnapshotMatches) {
    WriteSnapshotVersion(SnapshotVersionFor(VirtioGpuDevice::kGfxstream));

    AndroidOptions opts{};
    opts.virtio_gpu_device = const_cast<char*>("gfxstream");
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.snapshot_version(), SnapshotVersionFor(VirtioGpuDevice::kGfxstream));
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kLoading);
    EXPECT_THAT(
            dev.getQemuParameters(emu.config()),
            ElementsAreArray(WithVmState({"-loadvm", "default_boot", "-savevm", "default_boot"})));
}

TEST_F(SnapshotDeviceTest, InvalidVirtioGpuDeviceOptionIsReported) {
    AndroidOptions opts{};
    opts.virtio_gpu_device = const_cast<char*>("virgl");
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_THAT(dev.initialize(emu.config()),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         ::testing::HasSubstr("-virtio-gpu-device 'virgl'")));
}

TEST_F(SnapshotDeviceTest, StrictLoadFailsWhenVmStateImageWasRecreated) {
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));
    std::ofstream(vmstate_image()) << "not a qcow2";

    AndroidOptions opts{};
    opts.strict_snapshot_load = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_THAT(dev.initialize(emu.config()),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         ::testing::HasSubstr("VM-state image was recreated")));
}

TEST_F(SnapshotDeviceTest, NoVmStateImageWhenSnapshotsAreDisabled) {
    AndroidOptions opts{};
    opts.no_snapshot = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_FALSE(base::file::exists(vmstate_image()));
    EXPECT_TRUE(dev.vmstate_image().empty());
    EXPECT_THAT(dev.getQemuParameters(emu.config()), IsEmpty());
}

TEST_F(SnapshotDeviceTest, NoSnapshotStillAttachesExistingVmStateImage) {
    // ListSnapshots/DeleteSnapshot stay available under -no-snapshot; without
    // the node QEMU would look for the VM state in the first writable disk
    // image and a delete would leave it behind in snapshots.qcow2.
    WriteSnapshotVersion(SnapshotVersionFor(kDefaultVirtioGpuDevice));
    {
        // A previous session with snapshots enabled created the image.
        FakeEmulator enabled;
        EXPECT_CALL(enabled.mock_avd(), GetContentPath())
                .WillRepeatedly(Return(temp_dir_.Path()));
        EXPECT_CALL(enabled.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));
        SnapshotDevice creator;
        ASSERT_OK(creator.initialize(enabled.config()));
    }
    ASSERT_TRUE(IsVmStateImage(vmstate_image()));
    const auto mtime = base::file::last_write_time(vmstate_image());
    ASSERT_OK(mtime);

    AndroidOptions opts{};
    opts.no_snapshot = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_FALSE(dev.snapshots_enabled());
    EXPECT_EQ(dev.load_status(), SnapshotDevice::LoadStatus::kSkippedCmdline);
    EXPECT_EQ(dev.save_status(), SnapshotDevice::SaveStatus::kDisabledCmdline);
    EXPECT_EQ(dev.vmstate_image(), vmstate_image());
    EXPECT_EQ(base::file::last_write_time(vmstate_image()).value(), *mtime);
    // The node is attached, but no -loadvm/-savevm.
    EXPECT_THAT(dev.getQemuParameters(emu.config()), ElementsAreArray(WithVmState({})));
}

TEST_F(SnapshotDeviceTest, NoSnapshotLeavesInvalidVmStateFileAlone) {
    std::ofstream(vmstate_image()) << "not a qcow2";

    AndroidOptions opts{};
    opts.no_snapshot = true;
    FakeEmulator emu(opts);
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(temp_dir_.Path()));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_FALSE(IsVmStateImage(vmstate_image())) << "must not be replaced under -no-snapshot";
    EXPECT_TRUE(dev.vmstate_image().empty());
    EXPECT_THAT(dev.getQemuParameters(emu.config()), IsEmpty());
}

TEST_F(SnapshotDeviceTest, EscapesCommaAndKeepsUtf8InContentPath) {
    // Non-ASCII covers Windows UTF-8 path handling in the qcow2 helpers; the
    // comma must come out as ",," in the -blockdev option string.
    content_ = temp_dir_.Path() / "avd_\u6d4b\u8bd5_\U0001F680_\u00e9,1";
    ASSERT_OK(base::file::mkdir_recursive(content_, 0755));

    FakeEmulator emu;
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(content_));
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw_));

    SnapshotDevice dev;
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_TRUE(IsVmStateImage(vmstate_image()));

    const auto params = dev.getQemuParameters(emu.config());
    EXPECT_THAT(params, ElementsAreArray(WithVmState({"-savevm", "default_boot"})));
    ASSERT_GE(params.size(), 2u);
    // Build the expected fragment with path operators so the separator is the
    // native one (/ on POSIX, \ on Windows), as path::string() produces it.
    const std::string escaped_file =
            absl::StrCat((fs::path(",,1") / "snapshots.qcow2").string(), ",overlap-check");
    EXPECT_THAT(params[1], testing::HasSubstr(escaped_file))
            << "comma in the AVD path must be escaped as ,,";
    // The non-ASCII part is covered by ElementsAreArray() above, which compares
    // against the same path::string() encoding the device used; a UTF-8 literal
    // would not match on a Windows host whose narrow strings are not UTF-8.
}

}  // namespace android::goldfish::test
