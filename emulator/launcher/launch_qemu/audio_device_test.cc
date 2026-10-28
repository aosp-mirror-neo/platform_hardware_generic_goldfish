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

#include "audio_device.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>

#include "absl/status/status_matchers.h"

#include "android/base/testing/test_system.h"
#include "android/cmdline_definitions.h"
#include "android/goldfish/avd.h"
#include "android/status/status_matcher_macros.h"
#include "fake_emulator.h"
#include "goldfish/file/file.h"

namespace android::goldfish::test {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Return;

TEST(AudioDeviceTest, NoAudioFlagUsesNoneBackendWithMixingEngineOff) {
    std::string audio_opt = "wav";
    AndroidOptions opts{.noaudio = 1, .audio = audio_opt.data()};
    FakeEmulator emu(std::move(opts));
    EXPECT_CALL(emu.mock_avd(), Arch()).WillRepeatedly(Return(Avd::CpuArchitecture::kX86));

    AudioDevice dev("05.0");
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(AudioDevice::getAudioBackend(emu.config().opts()), "");
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAre(Eq("-audiodev"),
                            Eq("none,id=mainaudiodev,out.mixing-engine=off,in.mixing-engine=off"),
                            Eq("-device"), Eq("virtio-sound-pci,audiodev=mainaudiodev,addr=05.0")));
}

TEST(AudioDeviceTest, ExplicitAudioOptionUsesFixed48kHzS16SettingsOnX86AndArm) {
    std::string audio_opt = "wav";
    AndroidOptions opts{.audio = audio_opt.data()};
    FakeEmulator emu(std::move(opts));

    AudioDevice dev("06.0");
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(AudioDevice::getAudioBackend(emu.config().opts()), "wav");

    EXPECT_CALL(emu.mock_avd(), Arch()).WillOnce(Return(Avd::CpuArchitecture::kX86));
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAre(Eq("-audiodev"),
                            Eq("wav,id=mainaudiodev,"
                               "out.mixing-engine=on,out.fixed-settings=on,"
                               "out.frequency=48000,out.format=s16,out.channels=2,"
                               "in.mixing-engine=on,in.fixed-settings=on,"
                               "in.frequency=48000,in.format=s16,in.channels=1"),
                            Eq("-device"), Eq("virtio-sound-pci,audiodev=mainaudiodev,addr=06.0")));

    EXPECT_CALL(emu.mock_avd(), Arch()).WillOnce(Return(Avd::CpuArchitecture::kArm));
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAre(Eq("-audiodev"),
                            Eq("wav,id=mainaudiodev,"
                               "out.mixing-engine=on,out.fixed-settings=on,"
                               "out.frequency=48000,out.format=s16,out.channels=2,"
                               "in.mixing-engine=on,in.fixed-settings=on,"
                               "in.frequency=48000,in.format=s16,in.channels=1"),
                            Eq("-device"), Eq("virtio-sound-pci,audiodev=mainaudiodev,addr=06.0")));
}

TEST(AudioDeviceTest, UnsupportedArchitectureReturnsEmptyParameters) {
    std::string audio_opt = "wav";
    AndroidOptions opts{.audio = audio_opt.data()};
    FakeEmulator emu(std::move(opts));

    AudioDevice dev("06.0");
    EXPECT_CALL(emu.mock_avd(), Arch()).WillOnce(Return(Avd::CpuArchitecture::kRiscV));
    EXPECT_THAT(dev.getQemuParameters(emu.config()), IsEmpty());

    EXPECT_CALL(emu.mock_avd(), Arch()).WillOnce(Return(Avd::CpuArchitecture::kUnknown));
    EXPECT_THAT(dev.getQemuParameters(emu.config()), IsEmpty());
}

TEST(AudioDeviceTest, DetectHostAudioBackendPlatformBehavior) {
#if defined(_WIN32)
    EXPECT_EQ(AudioDevice::detectHostAudioBackend(), "wasapi");
#elif defined(__APPLE__)
    EXPECT_EQ(AudioDevice::detectHostAudioBackend(), "coreaudio");
#elif defined(__linux__)
    base::TestSystem sys("");
    sys.EnvSet("XDG_RUNTIME_DIR", "");
    EXPECT_EQ(AudioDevice::detectHostAudioBackend(), "");

    const std::filesystem::path unicode_runtime = std::filesystem::path(testing::TempDir()) /
                                                  "pulse-\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    const std::filesystem::path pulse_dir = unicode_runtime / "pulse";
    ASSERT_OK(base::file::mkdir_recursive(pulse_dir, 0700U));
    const std::filesystem::path pid_file = pulse_dir / "pid";
    ASSERT_OK(base::file::touch(pid_file));

    sys.EnvSet("XDG_RUNTIME_DIR", unicode_runtime.string());
    EXPECT_EQ(AudioDevice::detectHostAudioBackend(), "pa");

    EXPECT_OK(base::file::rm_recursive(unicode_runtime));
#endif
}

}  // namespace
}  // namespace android::goldfish::test
