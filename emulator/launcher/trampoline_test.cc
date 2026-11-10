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

#include "trampoline.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>

#include "absl/status/status.h"

#include "android/base/bazel_info.h"
#include "android/base/testing/test_system.h"
#include "android/goldfish/mock_avd.h"

using android::base::TestSystem;
using android::goldfish::FeatureFlags;
using android::goldfish::MockAvd;
using android::goldfish::ShouldTrampolineToQemu2;
using ::testing::Return;

namespace {

absl::StatusOr<FeatureFlags> CreateTestFeatureFlags(TestSystem& sys, bool has_required_features) {
    fs::path temp_file = sys.GetTempRoot()->Path() / "advancedFeatures.ini";
    std::ofstream out(temp_file);
    if (has_required_features) {
        out << "mac80211hwsimUserspaceManaged=on\n";
        out << "DynamicPartition=on\n";
    } else {
        out << "# Empty features\n";
    }
    out.close();
    return FeatureFlags::Create({.guest_ini_path = temp_file});
}

}  // namespace

TEST(TrampolineTest, shouldTrampolineIfApiLevelIsLessThan37) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/true);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(35));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags));
}

TEST(TrampolineTest, shouldNotTrampolineIfApiLevelIs37OrGreaterAndHasRequiredFeatures) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/true);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

    EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd), flags));

    auto avd_high = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd_high, ApiLevel()).WillRepeatedly(Return(38));

    EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd_high), flags));
}

TEST(TrampolineTest, shouldTrampolineIfRequiredFeatureIsMissing) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/false);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags));

    auto avd_high = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd_high, ApiLevel()).WillRepeatedly(Return(38));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd_high), flags));
}

TEST(TrampolineTest, shouldTrampolineIfFeatureFlagsFailedToParse) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = FeatureFlags::Create({
        .guest_ini_path = sys.GetTempRoot()->Path() / "nonexistent_advancedFeatures.ini",
    });
    ASSERT_FALSE(flags.ok());

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags));
}

// A required feature that is listed but not "on" (explicitly off, or an unrecognized value)
// must not count as supported. Previously only the key's presence was checked.
TEST(TrampolineTest, shouldTrampolineIfRequiredFeatureIsNotOn) {
    for (const char* value : {"off", "OFF", "foo", "1", "yes"}) {
        TestSystem sys("bin", "myhome");
        sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

        fs::path feature_file = sys.GetTempRoot()->Path() / "advancedFeatures.ini";
        std::ofstream(feature_file) << "mac80211hwsimUserspaceManaged = " << value << "\n"
                                    << "DynamicPartition = on\n";
        auto flags = FeatureFlags::Create({.guest_ini_path = feature_file});

        auto avd = std::make_unique<MockAvd>();
        EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

        EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags)) << "value=" << value;
    }

    for (const char* value : {"off", "OFF", "foo", "1", "yes"}) {
        TestSystem sys("bin", "myhome");
        sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

        fs::path feature_file = sys.GetTempRoot()->Path() / "advancedFeatures.ini";
        std::ofstream(feature_file) << "mac80211hwsimUserspaceManaged = on\n"
                                    << "DynamicPartition = " << value << "\n";
        auto flags = FeatureFlags::Create({.guest_ini_path = feature_file});

        auto avd = std::make_unique<MockAvd>();
        EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

        EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags)) << "value=" << value;
    }
}

TEST(TrampolineTest, shouldNotTrampolineIfRequiredFeatureIsOnInAnyCase) {
    for (const char* value : {"on", "ON", "On"}) {
        TestSystem sys("bin", "myhome");
        sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

        fs::path feature_file = sys.GetTempRoot()->Path() / "advancedFeatures.ini";
        std::ofstream(feature_file) << "Mac80211hwsimUserspaceManaged = " << value << "\n"
                                    << "DynamicPartition = " << value << "\n";
        auto flags = FeatureFlags::Create({.guest_ini_path = feature_file});

        auto avd = std::make_unique<MockAvd>();
        EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(0));
        EXPECT_CALL(*avd, GetLastRunQemuVersion()).WillRepeatedly(Return(std::optional<int>(1)));
        EXPECT_CALL(*avd, GetDeviceType())
                .WillRepeatedly(Return(android::goldfish::DeviceType::kPhone));
        EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

        EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd), flags)) << "value=" << value;
    }
}

TEST(TrampolineTest, shouldNotTrampolineIfEnvVarIsSet) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "1");

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ApiLevel()).Times(0);

    EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd), FeatureFlags{}));
}

TEST(TrampolineTest, shouldTrampolineIfForcedVersionIs2) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(2));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), FeatureFlags{}));
}

TEST(TrampolineTest, shouldNotTrampolineIfForcedVersionIs10OrGreater) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(10));

    EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd), FeatureFlags{}));
}

TEST(TrampolineTest, shouldTrampolineIfLastRunVersionIs2) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/true);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(0));
    EXPECT_CALL(*avd, GetDeviceType())
            .WillRepeatedly(Return(android::goldfish::DeviceType::kPhone));
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));
    EXPECT_CALL(*avd, GetLastRunQemuVersion()).WillRepeatedly(Return(std::optional<int>(2)));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags));
}

TEST(TrampolineTest, shouldTrampolineIfDeviceIsNotPhone) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/true);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(0));
    EXPECT_CALL(*avd, GetLastRunQemuVersion()).WillRepeatedly(Return(std::optional<int>(1)));
    EXPECT_CALL(*avd, GetDeviceType()).WillRepeatedly(Return(android::goldfish::DeviceType::kTv));

    EXPECT_TRUE(ShouldTrampolineToQemu2(std::move(avd), flags));
}

TEST(TrampolineTest, shouldNotTrampolineIfDeviceIsPhoneAndApi37OrGreater) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/true);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(0));
    EXPECT_CALL(*avd, GetLastRunQemuVersion()).WillRepeatedly(Return(std::optional<int>(1)));
    EXPECT_CALL(*avd, GetDeviceType())
            .WillRepeatedly(Return(android::goldfish::DeviceType::kPhone));
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

    EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd), flags));
}

TEST(TrampolineTest, shouldNotTrampolineIfDeviceIsUnknownAndApi37OrGreater) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    auto flags = CreateTestFeatureFlags(sys, /*has_required_features=*/true);

    auto avd = std::make_unique<MockAvd>();
    EXPECT_CALL(*avd, ForcedTrampolineVersion()).WillRepeatedly(Return(0));
    EXPECT_CALL(*avd, GetLastRunQemuVersion()).WillRepeatedly(Return(std::optional<int>(1)));
    EXPECT_CALL(*avd, GetDeviceType())
            .WillRepeatedly(Return(android::goldfish::DeviceType::kUnknown));
    EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(Return(37));

    EXPECT_FALSE(ShouldTrampolineToQemu2(std::move(avd), flags));
}

TEST(TrampolineTest, shouldTrampolineIfAvdFailedToLoad) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "");

    EXPECT_TRUE(ShouldTrampolineToQemu2(absl::InvalidArgumentError("Sysimg file error"),
                                        FeatureFlags{}));
}

TEST(TrampolineTest, shouldNotTrampolineIfAvdFailedToLoadAndEnvVarIsSet) {
    TestSystem sys("bin", "myhome");
    sys.EnvSet("AEMU_NO_TRAMPOLINE", "1");

    EXPECT_FALSE(ShouldTrampolineToQemu2(absl::InvalidArgumentError("Sysimg file error"),
                                         FeatureFlags{}));
}

#ifdef _WIN32
#define EXE ".exe"
#define EMULATOR "emulator.exe"
#else
#define EXE ""
#define EMULATOR "emulator"
#endif

fs::path FindFakeLauncherMain() {
    return android::base::Bazel::RunfilesPath(
            absl::StrCat("goldfish+/emulator/launcher/fake_launcher_main", EXE));
}

TEST(TrampolineDeathTest, trampolineToQemu2Success) {
    fs::path mock_src = FindFakeLauncherMain();
    ASSERT_FALSE(mock_src.empty()) << "Could not find fake_launcher_main in runfiles!";

    TestSystem sys("bin", "myhome");
    fs::path temp_dir = sys.GetTempRoot()->Path();
    fs::path launcher_dir = temp_dir / "nested" / "bin";
    fs::path dest = temp_dir / "emulator" / EMULATOR;

    fs::create_directories(dest.parent_path());
    fs::copy_file(mock_src, dest, fs::copy_options::overwrite_existing);
    fs::permissions(dest, fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);

    EXPECT_EXIT(android::goldfish::TrampolineToQemu2(launcher_dir, {},
                                                     [] {
                                                         std::fprintf(stderr,
                                                                      "BEFORE_EXEC_CALLED\n");
                                                         std::fflush(stderr);
                                                     })
                        .IgnoreError(),
                ::testing::ExitedWithCode(42), "BEFORE_EXEC_CALLED");
}

TEST(TrampolineDeathTest, trampolineToQemu2ArgsForwarded) {
    fs::path mock_src = FindFakeLauncherMain();
    ASSERT_FALSE(mock_src.empty()) << "Could not find fake_launcher_main in runfiles!";

    TestSystem sys("bin", "myhome");
    fs::path temp_dir = sys.GetTempRoot()->Path();
    fs::path launcher_dir = temp_dir / "nested" / "bin";
    fs::path dest = temp_dir / "emulator" / EMULATOR;

    fs::create_directories(dest.parent_path());
    fs::copy_file(mock_src, dest, fs::copy_options::overwrite_existing);
    fs::permissions(dest, fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);

    EXPECT_EXIT(android::goldfish::TrampolineToQemu2(launcher_dir, {"foo", "bar"}).IgnoreError(),
                ::testing::ExitedWithCode(43), "");
}

TEST(TrampolineTest, trampolineToQemu2BinaryNotExists) {
    TestSystem sys("bin", "myhome");
    fs::path temp_dir = sys.GetTempRoot()->Path();
    fs::path launcher_dir = temp_dir / "nested" / "bin";

    bool before_exec_called = false;
    auto status = android::goldfish::TrampolineToQemu2(launcher_dir, {},
                                                       [&] { before_exec_called = true; });
    EXPECT_FALSE(before_exec_called);
    EXPECT_TRUE(absl::IsNotFound(status)) << status;
    EXPECT_THAT(status.message(), ::testing::HasSubstr("Legacy emulator binary not found at"));
    EXPECT_THAT(status.message(), ::testing::HasSubstr("sdkmanager --install emulator"));
}

#ifndef _WIN32
TEST(TrampolineTest, trampolineToQemu2BinaryNotExecutable) {
    fs::path mock_src = FindFakeLauncherMain();
    ASSERT_FALSE(mock_src.empty()) << "Could not find fake_launcher_main in runfiles!";

    TestSystem sys("bin", "myhome");
    fs::path temp_dir = sys.GetTempRoot()->Path();
    fs::path launcher_dir = temp_dir / "nested" / "bin";
    fs::path dest = temp_dir / "emulator" / EMULATOR;

    fs::create_directories(dest.parent_path());
    fs::copy_file(mock_src, dest, fs::copy_options::overwrite_existing);
    fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write);

    bool before_exec_called = false;
    auto status = android::goldfish::TrampolineToQemu2(launcher_dir, {},
                                                       [&] { before_exec_called = true; });
    EXPECT_FALSE(before_exec_called);
    EXPECT_TRUE(absl::IsPermissionDenied(status)) << status;
    EXPECT_THAT(status.message(), ::testing::HasSubstr("Cannot execute legacy emulator binary at"));
    EXPECT_THAT(status.message(), ::testing::HasSubstr("sdkmanager --install emulator"));
}
#endif
