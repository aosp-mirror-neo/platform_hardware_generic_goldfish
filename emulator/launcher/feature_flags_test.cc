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

#include "android/goldfish/feature_flags.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "google/protobuf/text_format.h"

#include "android/base/bazel_info.h"
#include "android/base/testing/test_system.h"
#include "android/status/status_matcher_macros.h"
#include "emulator_feature_patterns.pb.h"
#include "emulator_features.pb.h"
#include "goldfish/file/file.h"

namespace android::goldfish {
namespace {

namespace fs = std::filesystem;
using S = android_studio::EmulatorFeatureFlagState;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

void WriteTextFile(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

TEST(FeatureFlagsTest, ParseFeatureFileSkipsLegacyAndHandlesSpecialNamesViaCreate) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path guest_ini = dir / "guest.ini";

    WriteTextFile(host_ini,
                  "# Test advanced features file\n"
                  "mac80211hwsimUserspaceManaged=off\n"
                  "mac80211hwsimUserspaceManaged=on\n"
                  "Minigbm = ON\n"
                  "ForceLavapipe = OFF\n"
                  "GuestAngle = default\n"
                  "Uwb = foo\n"
                  "QtRawKeyboardInput = on\n"
                  "VsockSnapshotLoadFixed_b231345789 = on\n"
                  "HostComposition = on\n"
                  "GLPipeChecksum = on\n"
                  "GrallocSync = on\n"
                  "VulkanNativeSwapchain = on\n"
                  "HVF = on\n"
                  "WindowsHypervisorPlatform = on\n"
                  "RefCountPipe\n");
    WriteTextFile(guest_ini,
                  "mac80211hwsimUserspaceManaged = on\n"
                  "Minigbm = on\n"
                  "GuestAngle = on\n"
                  "Uwb = on\n"
                  "VsockSnapshotLoadFixed_b231345789 = on\n"
                  "someUnknownGuestFeature = off\n"
                  "I did something = wrong =h ere\n");

    const std::vector<std::string> cli_flags = {
        "VulkanNativeSwapchain,-VulkanNativeSwapchain,HVF,WindowsHypervisorPlatform",
    };
    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = host_ini,
                                                       .guest_ini_path = guest_ini,
                                                       .cli_feature_flags = cli_flags,
                                                   }));

    // Duplicate key takes last value ('on').
    EXPECT_TRUE(flags.IsEnabled(S::MAC80211HWSIM_USERSPACE_MANAGED));
    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    // 'default' and 'foo' in host INI are not 'on'.
    EXPECT_FALSE(flags.IsEnabled(S::GUEST_ANGLE));
    EXPECT_FALSE(flags.IsEnabled(S::UWB));

    // Special INI key names map to their enum values.
    EXPECT_TRUE(flags.IsEnabled(S::DEVICE_KEYBOARD_QT_RAW_INPUT));
    EXPECT_TRUE(flags.IsEnabled(S::VSOCK_SNAPSHOT_LOAD_FIXED_B231345789));

    // Legacy flags are recognized without error, ignored in INI and CLI overrides,
    // and never reported as enabled or overridden.
    for (auto legacy : {S::HOST_COMPOSITION_V1, S::GL_PIPE_CHECKSUM, S::GRALLOC_SYNC,
                        S::VULKAN_NATIVE_SWAPCHAIN, S::HVF, S::WINDOWS_HYPERVISOR_PLATFORM}) {
        EXPECT_FALSE(flags.IsEnabled(legacy));
        EXPECT_FALSE(flags.IsOverridden(legacy));
    }
}

TEST(FeatureFlagsTest, HostAndGuestBaselineEvaluation) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host_advancedFeatures.ini";
    const fs::path guest_ini = dir / "guest_advancedFeatures.ini";

    WriteTextFile(host_ini,
                  "NetsimWebUi = on\n"
                  "ForceLavapipe = off\n"
                  "Minigbm = on\n"
                  "GuestAngle = on\n"
                  "Uwb = off\n");
    WriteTextFile(guest_ini,
                  "Minigbm = on\n"
                  "GuestAngle = off\n"
                  "Uwb = on\n");

    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = host_ini,
                                                       .guest_ini_path = guest_ini,
                                                   }));

    // Host-only features depend solely on host INI.
    EXPECT_TRUE(flags.IsEnabled(S::NETSIMWEBUI));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));

    // Guest features require BOTH host INI and guest INI to be 'on'.
    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
    EXPECT_TRUE(flags.IsEnabledByGuest(S::MINIGBM));

    EXPECT_FALSE(flags.IsEnabled(S::GUEST_ANGLE));
    EXPECT_FALSE(flags.IsEnabledByGuest(S::GUEST_ANGLE));

    EXPECT_FALSE(flags.IsEnabled(S::UWB));
    EXPECT_TRUE(flags.IsEnabledByGuest(S::UWB));

    EXPECT_THAT(flags.GetEnabled(), ElementsAre(S::MINIGBM, S::NETSIMWEBUI));
    EXPECT_THAT(flags.GetEnabledNonOverride(), ElementsAre(S::MINIGBM, S::NETSIMWEBUI));
    EXPECT_THAT(flags.GetEnabledOverride(), IsEmpty());
    EXPECT_THAT(flags.GetDisabledOverride(), IsEmpty());
}

TEST(FeatureFlagsTest, MissingIniFileReturnsNotFoundError) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host_advancedFeatures.ini";

    WriteTextFile(host_ini,
                  "NetsimWebUi = on\n"
                  "Minigbm = on\n");

    auto missing_guest = FeatureFlags::Create({
        .host_ini_path = host_ini,
        .guest_ini_path = dir / "missing_guest.ini",
    });
    EXPECT_FALSE(missing_guest.ok());
    EXPECT_EQ(missing_guest.status().code(), absl::StatusCode::kNotFound);

    auto missing_host = FeatureFlags::Create({
        .host_ini_path = dir / "missing_host.ini",
    });
    EXPECT_FALSE(missing_host.ok());
    EXPECT_EQ(missing_host.status().code(), absl::StatusCode::kNotFound);

    // Omitting guest_ini_path (empty path) succeeds and disables guest features.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = host_ini,
                                                   }));
    EXPECT_TRUE(flags.IsEnabled(S::NETSIMWEBUI));
    EXPECT_FALSE(flags.IsEnabled(S::MINIGBM));
    EXPECT_FALSE(flags.IsEnabledByGuest(S::MINIGBM));
}

TEST(FeatureFlagsTest, OverridePrecedenceOrder) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path guest_ini = dir / "guest.ini";
    const fs::path user_ini = dir / "user.ini";

    WriteTextFile(host_ini,
                  "NetsimWebUi = on\n"
                  "NetsimCliUi = off\n"
                  "ForceLavapipe = off\n"
                  "Minigbm = on\n"
                  "GuestAngle = on\n");
    WriteTextFile(guest_ini,
                  "Minigbm = on\n"
                  "GuestAngle = on\n");

    // User INI overrides NetsimWebUi -> off, NetsimCliUi -> on, ForceLavapipe -> on.
    WriteTextFile(user_ini,
                  "NetsimWebUi = off\n"
                  "NetsimCliUi = on\n"
                  "ForceLavapipe = on\n");

    // AVD config overrides NetsimCliUi -> off, ForceLavapipe -> off.
    const std::string_view avd_flags = "-NetsimCliUi,-ForceLavapipe";

    // Env var overrides ForceLavapipe -> on, Minigbm -> off.
    const std::string_view env_flags = "ForceLavapipe,-Minigbm";

    // CLI overrides Minigbm -> on, GuestAngle -> off.
    const std::vector<std::string> cli_flags = {"Minigbm,-GuestAngle"};

    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = host_ini,
                                                       .guest_ini_path = guest_ini,
                                                       .user_ini_path = user_ini,
                                                       .avd_feature_flags = avd_flags,
                                                       .env_feature_flags = env_flags,
                                                       .cli_feature_flags = cli_flags,
                                                   }));

    EXPECT_FALSE(flags.IsEnabled(S::NETSIMWEBUI));
    EXPECT_TRUE(flags.IsOverridden(S::NETSIMWEBUI));

    EXPECT_FALSE(flags.IsEnabled(S::NETSIMCLIUI));
    EXPECT_TRUE(flags.IsOverridden(S::NETSIMCLIUI));

    EXPECT_TRUE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    EXPECT_TRUE(flags.IsOverridden(S::FORCE_LAVAPIPE));

    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
    EXPECT_TRUE(flags.IsOverridden(S::MINIGBM));

    EXPECT_FALSE(flags.IsEnabled(S::GUEST_ANGLE));
    EXPECT_TRUE(flags.IsOverridden(S::GUEST_ANGLE));

    EXPECT_THAT(flags.GetEnabledNonOverride(),
                ElementsAre(S::MINIGBM, S::NETSIMWEBUI, S::GUEST_ANGLE));
    EXPECT_THAT(flags.GetEnabledOverride(), ElementsAre(S::MINIGBM, S::FORCE_LAVAPIPE));
    EXPECT_THAT(flags.GetDisabledOverride(),
                ElementsAre(S::NETSIMWEBUI, S::NETSIMCLIUI, S::GUEST_ANGLE));
    EXPECT_THAT(flags.GetEnabled(), ElementsAre(S::MINIGBM, S::FORCE_LAVAPIPE));
}

TEST(FeatureFlagsTest, UserIniDefaultResetsToBaseline) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path user_ini = dir / "user.ini";

    WriteTextFile(host_ini, "NetsimWebUi = on\n");
    WriteTextFile(user_ini, "NetsimWebUi = default\n");

    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = host_ini,
                                                       .user_ini_path = user_ini,
                                                   }));

    EXPECT_TRUE(flags.IsEnabled(S::NETSIMWEBUI));
    EXPECT_FALSE(flags.IsOverridden(S::NETSIMWEBUI));
}

TEST(FeatureFlagsTest, SetIfNotOverridden) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path guest_ini = dir / "guest.ini";

    WriteTextFile(host_ini,
                  "ForceSwiftshader = off\n"
                  "ForceLavapipe = off\n"
                  "Minigbm = off\n"
                  "GuestAngle = off\n");
    WriteTextFile(guest_ini,
                  "Minigbm = on\n"
                  "GuestAngle = off\n");

    ASSERT_OK_AND_ASSIGN(FeatureFlags flags, FeatureFlags::Create({
                                                 .host_ini_path = host_ini,
                                                 .guest_ini_path = guest_ini,
                                                 .avd_feature_flags = "-ForceLavapipe",
                                             }));

    // Feature not overridden -> SetIfNotOverridden updates it without marking overridden.
    flags.SetIfNotOverridden(S::FORCE_SWIFTSHADER, true);
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_SWIFTSHADER));
    EXPECT_FALSE(flags.IsOverridden(S::FORCE_SWIFTSHADER));

    // Feature overridden by user/AVD -> SetIfNotOverridden must not change it.
    flags.SetIfNotOverridden(S::FORCE_LAVAPIPE, true);
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));

    // Guest feature enabled by guest -> SetIfNotOverridden can enable it.
    flags.SetIfNotOverridden(S::MINIGBM, true);
    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
    EXPECT_FALSE(flags.IsOverridden(S::MINIGBM));

    // Guest feature disabled by guest -> SetIfNotOverridden must not enable it.
    flags.SetIfNotOverridden(S::GUEST_ANGLE, true);
    EXPECT_FALSE(flags.IsEnabled(S::GUEST_ANGLE));
}

TEST(FeatureFlagsTest, CreateForTestingSetsBaselineWithoutMarkingOverridden) {
    auto flags = FeatureFlags::CreateForTesting({S::MINIGBM, S::VULKAN_SNAPSHOTS});

    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
    // CreateForTesting must not mutate the guest image capability state.
    EXPECT_FALSE(flags.IsEnabledByGuest(S::MINIGBM));
    EXPECT_FALSE(flags.IsOverridden(S::MINIGBM));
    EXPECT_TRUE(flags.IsEnabled(S::VULKAN_SNAPSHOTS));
    EXPECT_FALSE(flags.IsOverridden(S::VULKAN_SNAPSHOTS));
    EXPECT_THAT(flags.GetEnabledOverride(), IsEmpty());
    EXPECT_THAT(flags.GetEnabledNonOverride(), ElementsAre(S::VULKAN_SNAPSHOTS, S::MINIGBM));

    // Overrides take precedence over the initial baseline while preserving baseline reporting.
    flags.SetEnabledOverride(S::MINIGBM, false);
    EXPECT_FALSE(flags.IsEnabled(S::MINIGBM));
    EXPECT_TRUE(flags.IsOverridden(S::MINIGBM));
    EXPECT_THAT(flags.GetEnabledNonOverride(), ElementsAre(S::VULKAN_SNAPSHOTS, S::MINIGBM));
    EXPECT_THAT(flags.GetDisabledOverride(), ElementsAre(S::MINIGBM));
}

TEST(FeatureFlagsTest, AllEnumFeaturesRoundTripBetweenNameAndFeatureLookup) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path guest_ini = dir / "guest.ini";

    for (int i = S::EmulatorFeatureFlag_MIN; i <= S::EmulatorFeatureFlag_MAX; ++i) {
        if (!S::EmulatorFeatureFlag_IsValid(i)) {
            continue;
        }
        const auto feature = static_cast<EmulatorFeatureFlag>(i);
        if (feature == S::EMULATOR_FEATURE_FLAG_UNSPECIFIED) {
            continue;
        }

        const std::string& enum_name = S::EmulatorFeatureFlag_Name(feature);
        std::string stripped_lower;
        for (char c : enum_name) {
            if (c != '_') {
                stripped_lower.push_back(
                        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }

        for (std::string_view candidate :
             {std::string_view(enum_name), std::string_view(stripped_lower)}) {
            const std::string ini_line = std::string(candidate) + " = on\n";
            WriteTextFile(host_ini, ini_line);
            WriteTextFile(guest_ini, ini_line);

            ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                               .host_ini_path = host_ini,
                                                               .guest_ini_path = guest_ini,
                                                               .avd_feature_flags = candidate,
                                                           }));
            if (!flags.GetEnabled().empty()) {
                EXPECT_THAT(flags.GetEnabled(), ElementsAre(feature)) << "candidate=" << candidate;
                EXPECT_THAT(flags.GetEnabledOverride(), ElementsAre(feature))
                        << "candidate=" << candidate;
            }
        }
    }
}

TEST(FeatureFlagsTest, AllHostIniFeaturesAreMappedToEnum) {
    const fs::path ini_path = android::base::Bazel::RunfilesPath(
            "goldfish+/emulator/launcher/data/advancedFeatures.ini");
    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = ini_path,
                                                   }));
    EXPECT_FALSE(flags.GetEnabled().empty());
}

TEST(FeatureFlagsTest, UnknownFeatureInHostIniReturnsErrorWhileGuestIniSucceeds) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path bad_host_ini = dir / "bad_host.ini";
    const fs::path good_host_ini = dir / "good_host.ini";
    const fs::path guest_ini = dir / "guest.ini";

    WriteTextFile(bad_host_ini,
                  "NetsimWebUi = on\n"
                  "UnrecognizedHostFeature = on\n");
    WriteTextFile(good_host_ini,
                  "NetsimWebUi = on\n"
                  "Minigbm = on\n");
    WriteTextFile(guest_ini,
                  "Minigbm = on\n"
                  "UnrecognizedGuestFeature = on\n");

    auto bad_host = FeatureFlags::Create({
        .host_ini_path = bad_host_ini,
    });
    EXPECT_FALSE(bad_host.ok());
    EXPECT_EQ(bad_host.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(bad_host.status().message(), ::testing::HasSubstr(bad_host_ini.string()));
    EXPECT_THAT(bad_host.status().message(), ::testing::HasSubstr("UnrecognizedHostFeature"));

    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, FeatureFlags::Create({
                                                       .host_ini_path = good_host_ini,
                                                       .guest_ini_path = guest_ini,
                                                   }));
    EXPECT_TRUE(flags.IsEnabled(S::NETSIMWEBUI));
    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
}

TEST(FeatureFlagsTest, Api37OrGreaterPatchesMinigbmAndGuestAngleInGuestFeatures) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path guest_ini = dir / "guest.ini";

    WriteTextFile(host_ini,
                  "Minigbm = on\n"
                  "GuestAngle = on\n"
                  "DynamicPartition = on\n");
    // System image INI omits Minigbm and GuestAngle.
    WriteTextFile(guest_ini, "DynamicPartition = on\n");

    // Below API 37, omitted guest features remain disabled.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags api36_flags, FeatureFlags::Create({
                                                             .host_ini_path = host_ini,
                                                             .guest_ini_path = guest_ini,
                                                             .guest_api_level = 36,
                                                         }));
    EXPECT_FALSE(api36_flags.IsEnabledByGuest(S::MINIGBM));
    EXPECT_FALSE(api36_flags.IsEnabled(S::MINIGBM));
    EXPECT_FALSE(api36_flags.IsEnabledByGuest(S::GUEST_ANGLE));
    EXPECT_FALSE(api36_flags.IsEnabled(S::GUEST_ANGLE));

    // At API 37+, Minigbm and GuestAngle are patched to On in guest features.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags api37_flags, FeatureFlags::Create({
                                                             .host_ini_path = host_ini,
                                                             .guest_ini_path = guest_ini,
                                                             .guest_api_level = 37,
                                                         }));
    EXPECT_TRUE(api37_flags.IsEnabledByGuest(S::MINIGBM));
    EXPECT_TRUE(api37_flags.IsEnabled(S::MINIGBM));
    EXPECT_TRUE(api37_flags.IsEnabledByGuest(S::GUEST_ANGLE));
    EXPECT_TRUE(api37_flags.IsEnabled(S::GUEST_ANGLE));

    // Explicit user/CLI overrides can still disable them on API 37+.
    const std::vector<std::string> cli_flags = {"-GuestAngle,+ForceLavapipe"};
    ASSERT_OK_AND_ASSIGN(const FeatureFlags overridden_flags, FeatureFlags::Create({
                                                                  .host_ini_path = host_ini,
                                                                  .guest_ini_path = guest_ini,
                                                                  .guest_api_level = 37,
                                                                  .cli_feature_flags = cli_flags,
                                                              }));
    EXPECT_TRUE(overridden_flags.IsEnabledByGuest(S::GUEST_ANGLE));
    EXPECT_FALSE(overridden_flags.IsEnabled(S::GUEST_ANGLE));
    EXPECT_TRUE(overridden_flags.IsOverridden(S::GUEST_ANGLE));
    EXPECT_TRUE(overridden_flags.IsEnabled(S::FORCE_LAVAPIPE));
    EXPECT_TRUE(overridden_flags.IsOverridden(S::FORCE_LAVAPIPE));

    // Explicit 'off' in the guest INI on API 37+ is respected (not overwritten by try_emplace).
    const fs::path explicit_off_guest_ini = dir / "explicit_off_guest.ini";
    WriteTextFile(explicit_off_guest_ini,
                  "DynamicPartition = on\n"
                  "GuestAngle = off\n");
    ASSERT_OK_AND_ASSIGN(const FeatureFlags explicit_off_flags,
                         FeatureFlags::Create({
                             .host_ini_path = host_ini,
                             .guest_ini_path = explicit_off_guest_ini,
                             .guest_api_level = 37,
                         }));
    EXPECT_TRUE(explicit_off_flags.IsEnabledByGuest(S::MINIGBM));
    EXPECT_TRUE(explicit_off_flags.IsEnabled(S::MINIGBM));
    EXPECT_FALSE(explicit_off_flags.IsEnabledByGuest(S::GUEST_ANGLE));
    EXPECT_FALSE(explicit_off_flags.IsEnabled(S::GUEST_ANGLE));
}

TEST(FeatureFlagsTest, ServerPatternHostGpuAndDisjunctionMatching) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path patterns_file = dir / "patterns.protobuf";

    WriteTextFile(patterns_file, R"pb(
        # 1. Matches AMD CPU on 64-bit OS (no GPU constraint).
        pattern {
            hwconfig {
                hostinfo {
                    cpu_manufacturer: "AMD"
                    os_bit_count: 64
                }
            }
            featureaction {
                feature: ForceSwiftshader
                enable: true
            }
        }
        # 2. Does not match (requires no virt_support OR 32-bit OS).
        pattern {
            hwconfig {
                hostinfo {
                    virt_support: false
                }
            }
            hwconfig {
                hostinfo {
                    os_bit_count: 32
                }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
        }
        # 3. Requires BOTH Intel and NVIDIA GPUs on an AMD CPU.
        pattern {
            hwconfig {
                hostinfo {
                    cpu_manufacturer: "AMD"
                }
                hostgpuinfo {
                    make: "Intel"
                }
                hostgpuinfo {
                    make: "NVIDIA"
                }
            }
            featureaction {
                feature: ForceANGLE
                enable: true
            }
        }
        # 4. Requires BOTH AMD and NVIDIA GPUs (does not match Intel + NVIDIA).
        pattern {
            hwconfig {
                hostinfo {
                    cpu_manufacturer: "AMD"
                }
                hostgpuinfo {
                    make: "AMD"
                }
                hostgpuinfo {
                    make: "NVIDIA"
                }
            }
            featureaction {
                feature: ForceGpuSoftware
                enable: true
            }
        }
        # 5. Disjunction: matches Intel OR AMD CPU.
        pattern {
            hwconfig {
                hostinfo {
                    cpu_manufacturer: "Intel"
                }
            }
            hwconfig {
                hostinfo {
                    cpu_manufacturer: "AMD"
                }
            }
            featureaction {
                feature: ForceGpuHost
                enable: true
            }
        }
        # 6. Empty pattern (no hwconfig) never matches.
        pattern {
            featureaction {
                feature: ForceLavapipeForSoftwareRendering
                enable: true
            }
        })pb");

    ASSERT_OK_AND_ASSIGN(FeatureFlags flags, FeatureFlags::Create({
                                                 .default_patterns_path = patterns_file,
                                             }));

    // Before ApplyHostFeaturePatterns: hostinfo-constrained patterns (including virt_support: false)
    // must not match.
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_SWIFTSHADER));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_ANGLE));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_GPU_SOFTWARE));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_GPU_HOST));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE_FOR_SOFTWARE_RENDERING));

    const HostInfo host_info{
        .cpu_manufacturer = "AMD",
        .os_platform = "Windows",
        .cpu_model_name = 10,
        .virt_support = true,
        .running_in_vm = false,
    };
    flags.ApplyHostFeaturePatterns(host_info);

    // Before GPU probing: CPU-only patterns (1 and 5) match; GPU pattern (3) does not yet match.
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_SWIFTSHADER));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_ANGLE));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_GPU_SOFTWARE));
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_GPU_HOST));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE_FOR_SOFTWARE_RENDERING));

    // After probing NVIDIA + Intel GPUs: pattern 3 matches, pattern 4 still does not.
    const std::vector<HostGpuInfo> gpus = {
        HostGpuInfo{.make = "NVIDIA"},
        HostGpuInfo{.make = "Intel"},
    };
    flags.ApplyGpuFeaturePatterns(gpus);
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_ANGLE));
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_GPU_SOFTWARE));
}

TEST(FeatureFlagsTest, VersionFilteringInFeaturePatterns) {
    base::TestSystem sys("bin", "myhome");
    const fs::path patterns_file = sys.GetTempRoot()->Path() / "version_patterns.protobuf";
    WriteTextFile(patterns_file, R"pb(
        pattern {
            hwconfig {}
            featureaction {
                feature: ForceSwiftshader
                enable: true
                min_version { major: 34 minor: 1 patch: 0 }
                max_version { major: 35 minor: 2 patch: 5 }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
                min_version { major: 35 minor: 0 patch: 0 }
            }
        })pb");

    auto eval_for_version = [&](std::string_view version) {
        return FeatureFlags::Create({
            .default_patterns_path = patterns_file,
            .emulator_version = version,
        });
    };

    // Version 34.0.0: below both min_versions.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags v34_0, eval_for_version("34.0.0"));
    EXPECT_THAT(v34_0.GetEnabled(), IsEmpty());

    // Version 34.2.0: matches ForceSwiftshader only.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags v34_2, eval_for_version("34.2.0"));
    EXPECT_THAT(v34_2.GetEnabled(), ElementsAre(S::FORCE_SWIFTSHADER));

    // Version 35.1.0: matches both.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags v35_1, eval_for_version("35.1.0"));
    EXPECT_THAT(v35_1.GetEnabled(), ElementsAre(S::FORCE_SWIFTSHADER, S::FORCE_LAVAPIPE));

    // Version 35.2.5-1234567 (release with build number on exact max_version): matches both.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags v35_2_5_build, eval_for_version("35.2.5-1234567"));
    EXPECT_THAT(v35_2_5_build.GetEnabled(), ElementsAre(S::FORCE_SWIFTSHADER, S::FORCE_LAVAPIPE));

    // Version 36.0.0: above ForceSwiftshader max_version, matches ForceLavapipe only.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags v36_0, eval_for_version("36.0.0"));
    EXPECT_THAT(v36_0.GetEnabled(), ElementsAre(S::FORCE_LAVAPIPE));

    // Invalid/empty version: min-version constrained actions apply, max-version constrained do not.
    ASSERT_OK_AND_ASSIGN(const FeatureFlags v_empty, eval_for_version(""));
    EXPECT_THAT(v_empty.GetEnabled(), ElementsAre(S::FORCE_LAVAPIPE));
}

TEST(FeatureFlagsTest, OriginalFeatureFlagsProtobufLoadsAndAppliesPatterns) {
    const fs::path ini_path = android::base::Bazel::RunfilesPath(
            "goldfish+/emulator/launcher/data/advancedFeatures.ini");
    const fs::path proto_path = android::base::Bazel::RunfilesPath(
            "goldfish+/emulator/launcher/data/emu-original-feature-flags.protobuf");

    ASSERT_OK_AND_ASSIGN(FeatureFlags flags,
                         FeatureFlags::Create({
                             .host_ini_path = ini_path,
                             .default_patterns_path = proto_path,
                             .emulator_version = "37.1.6",
                         }));
    flags.ApplyHostFeaturePatterns(HostInfo{
        .cpu_manufacturer = "Intel",
        .os_platform = "Linux",
    });
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_SWIFTSHADER));

    // In emu-original-feature-flags.protobuf, AMD Radeon HD 5450 (make "1002", device_id "68f9")
    // enables ForceSwiftshader on version >= 36.1.
    const std::vector<HostGpuInfo> amd_gpu = {
        HostGpuInfo{.make = "0x1002", .device_id = "0x68f9"},
    };
    flags.ApplyGpuFeaturePatterns(amd_gpu);
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_SWIFTSHADER));
}

TEST(FeatureFlagsTest, AllEmulatorFeaturesProtoValuesMapToStudioStatsEnum) {
    // 1. Verify the bundled emu-original-feature-flags.protobuf parses cleanly and never
    // references the dummy mapping-robustness test entries.
    const fs::path bundled_proto_path = android::base::Bazel::RunfilesPath(
            "goldfish+/emulator/launcher/data/emu-original-feature-flags.protobuf");
    ASSERT_OK_AND_ASSIGN(const std::string bundled_text,
                         android::base::file::read_whole_file(bundled_proto_path));
    emulator_features::EmulatorFeaturePatterns bundled_patterns;
    ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(bundled_text, &bundled_patterns));
    ASSERT_GT(bundled_patterns.pattern_size(), 0);
    for (const auto& pattern : bundled_patterns.pattern()) {
        for (const auto& action : pattern.featureaction()) {
            ASSERT_TRUE(action.has_feature());
            EXPECT_NE(action.feature(), emulator_features::AMDOpenGLDriver);
            EXPECT_NE(action.feature(), emulator_features::MesaDRI);
        }
    }

    // 2. Verify every enum value defined in emulator_features.proto maps cleanly to a valid
    // android_studio::EmulatorFeatureFlagState::EmulatorFeatureFlag in studio_stats.proto, except
    // the two speculative dummy entries (AMDOpenGLDriver and MesaDRI) documented in
    // emulator_features.proto for mapping-robustness testing.
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path guest_ini = dir / "guest.ini";
    const fs::path cached_path = dir / "all_features.protobuf";
    WriteTextFile(guest_ini, "");

    emulator_features::EmulatorFeaturePatterns all_patterns;
    auto* pattern = all_patterns.add_pattern();
    pattern->add_hwconfig();

    int enum_value_count = 0;
    for (int i = emulator_features::Feature_MIN; i <= emulator_features::Feature_MAX; ++i) {
        if (!emulator_features::Feature_IsValid(i)) {
            continue;
        }
        ++enum_value_count;
        auto* action = pattern->add_featureaction();
        action->set_feature(static_cast<emulator_features::Feature>(i));
        action->set_enable(true);
    }
    ASSERT_GE(enum_value_count, 68);

    {
        std::ofstream out(cached_path, std::ios_base::binary | std::ios_base::trunc);
        ASSERT_TRUE(out.is_open());
        ASSERT_TRUE(all_patterns.SerializeToOstream(&out));
    }

    std::vector<std::string> unknown_warnings;
    absl::ScopedMockLog mock_log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(mock_log, Log(absl::LogSeverity::kWarning, ::testing::_,
                              HasSubstr("Unknown feature action in server patterns:")))
            .WillRepeatedly([&](absl::LogSeverity, auto, auto message) {
                unknown_warnings.emplace_back(message);
            });
    mock_log.StartCapturingLogs();
    const auto res = FeatureFlags::Create({
        .guest_ini_path = guest_ini,
        .cached_patterns_path = cached_path,
        .guest_api_level = 37,
    });
    mock_log.StopCapturingLogs();
    ASSERT_OK_AND_ASSIGN(const FeatureFlags flags, res);

    EXPECT_THAT(unknown_warnings,
                ::testing::UnorderedElementsAre(
                        "Unknown feature action in server patterns: AMDOpenGLDriver",
                        "Unknown feature action in server patterns: MesaDRI"));

    // Active host and guest features are enabled; renamed/deprecated legacy features map without
    // warning and remain disabled.
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_ANGLE));
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_SWIFTSHADER));
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_GPU_SOFTWARE));
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_GPU_HOST));
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_LAVAPIPE_FOR_SOFTWARE_RENDERING));
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));
    EXPECT_TRUE(flags.IsEnabled(S::GUEST_ANGLE));
    EXPECT_FALSE(flags.IsEnabled(S::HOST_COMPOSITION_V1));
    EXPECT_FALSE(flags.IsEnabled(S::HAXM));
    EXPECT_FALSE(flags.IsEnabled(S::DEPRECATED_IGNORE_HOST_OPENGL_ERRORS));
}

TEST(FeatureFlagsTest, ServerPatternsRespectOverridesAndGuestSupport) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path host_ini = dir / "host.ini";
    const fs::path guest_ini = dir / "guest.ini";
    const fs::path default_patterns = dir / "emu-original-feature-flags.protobuf";
    const fs::path cached_patterns = dir / "emu-last-feature-flags.protobuf";

    WriteTextFile(host_ini,
                  "ForceSwiftshader = off\n"
                  "ForceLavapipe = off\n"
                  "ForceGpuHost = on\n"
                  "Minigbm = off\n"
                  "GuestAngle = off\n");
    WriteTextFile(guest_ini,
                  "Minigbm = on\n"
                  "GuestAngle = off\n");

    WriteTextFile(default_patterns, R"pb(
        pattern {
            hwconfig {
                hostinfo {
                    cpu_manufacturer: "Intel"
                    os_platform: "Linux"
                }
            }
            featureaction {
                feature: ForceSwiftshader
                enable: true
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
            featureaction {
                feature: ForceGpuHost
                enable: false
            }
            featureaction {
                feature: Minigbm
                enable: true
            }
            featureaction {
                feature: GuestAngle
                enable: true
            }
        }
        pattern {
            hwconfig {
                hostgpuinfo {
                    make: "Intel (0x8086)"
                    device_id: "0x67df"
                }
            }
            featureaction {
                feature: ForceANGLE
                enable: true
            }
        })pb");
    // An empty cached patterns file is rejected and falls back to default_patterns.
    WriteTextFile(cached_patterns, "");

    ASSERT_OK_AND_ASSIGN(
            FeatureFlags flags,
            FeatureFlags::Create({
                .host_ini_path = host_ini,
                .guest_ini_path = guest_ini,
                .cached_patterns_path = cached_patterns,
                .default_patterns_path = default_patterns,
                .emulator_version = "35.1.0",
                .avd_feature_flags = "-ForceLavapipe",
            }));

    flags.ApplyHostFeaturePatterns(HostInfo{
        .cpu_manufacturer = "Intel",
        .os_platform = "Linux",
    });

    // Enabled by server pattern (not overridden).
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_SWIFTSHADER));
    EXPECT_FALSE(flags.IsOverridden(S::FORCE_SWIFTSHADER));

    // User/AVD override (-ForceLavapipe) wins over server pattern (enable: true).
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    EXPECT_TRUE(flags.IsOverridden(S::FORCE_LAVAPIPE));

    // Disabled by server pattern (not overridden).
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_GPU_HOST));

    // Guest feature enabled in guest.ini can be enabled by server pattern.
    EXPECT_TRUE(flags.IsEnabled(S::MINIGBM));

    // Guest feature disabled in guest.ini must NOT be enabled by server pattern.
    EXPECT_FALSE(flags.IsEnabled(S::GUEST_ANGLE));

    // GPU-dependent pattern does not match until GPUs are probed.
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_ANGLE));

    // Unprefixed substring of a hex ID ("67" inside "0x67df") must not false-match.
    const std::vector<HostGpuInfo> mismatched_gpus = {
        HostGpuInfo{.make = "0x8086", .device_id = "67"},
    };
    flags.ApplyGpuFeaturePatterns(mismatched_gpus);
    EXPECT_FALSE(flags.IsEnabled(S::FORCE_ANGLE));

    const std::vector<HostGpuInfo> probed_gpus = {
        HostGpuInfo{
            .make = "0x8086",
            .device_id = "67df",
        },
    };
    flags.ApplyGpuFeaturePatterns(probed_gpus);
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_ANGLE));
    EXPECT_THAT(flags.GetEnabledNonOverride(),
                ElementsAre(S::FORCE_ANGLE, S::FORCE_SWIFTSHADER, S::MINIGBM));
}

TEST(FeatureFlagsTest, ApplyGpuFeaturePatternsMatchesModelAndRendererSubstringsAndTrademarks) {
    base::TestSystem sys("bin", "myhome");
    const fs::path default_patterns =
            sys.GetTempRoot()->Path() / "emu-original-feature-flags.protobuf";

    WriteTextFile(default_patterns, R"pb(
        pattern {
            hwconfig {
                hostgpuinfo {
                    make: "Intel (0x8086)"
                    model: "Intel Iris Pro"
                }
            }
            featureaction {
                feature: ForceANGLE
                enable: true
            }
        }
        pattern {
            hwconfig {
                hostgpuinfo {
                    renderer: "AMD Radeon RX 6700 XT"
                }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
        })pb");

    auto make_flags = [&]() {
        return FeatureFlags::Create({
            .default_patterns_path = default_patterns,
            .emulator_version = "37.1.0",
        });
    };

    // Unrelated or empty GPU strings do not match.
    {
        ASSERT_OK_AND_ASSIGN(FeatureFlags flags, make_flags());
        const std::vector<HostGpuInfo> unrelated_gpus = {
            HostGpuInfo{
                .make = "0x8086",
                .model = "Intel(R) UHD Graphics 770",
                .renderer = "AMD Radeon RX 6600 (RADV NAVI23)",
            },
            HostGpuInfo{.make = "0x8086"},
        };
        flags.ApplyGpuFeaturePatterns(unrelated_gpus);
        EXPECT_FALSE(flags.IsEnabled(S::FORCE_ANGLE));
        EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    }

    // ASCII trademark markers "(R)" / "(TM)" and model suffixes match pattern "Intel Iris Pro".
    {
        ASSERT_OK_AND_ASSIGN(FeatureFlags flags, make_flags());
        const std::vector<HostGpuInfo> iris_gpus = {
            HostGpuInfo{
                .make = "0x8086",
                .model = "Intel(R) Iris(TM) Pro Graphics 580",
            },
        };
        flags.ApplyGpuFeaturePatterns(iris_gpus);
        EXPECT_TRUE(flags.IsEnabled(S::FORCE_ANGLE));
        EXPECT_FALSE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    }

    // UTF-8 trademark symbols ("®" / "™"), spaced "(R)" / "(TM)", case variations, and driver
    // suffixes match.
    {
        ASSERT_OK_AND_ASSIGN(FeatureFlags flags, make_flags());
        const std::vector<HostGpuInfo> mixed_gpus = {
            HostGpuInfo{
                .make = "0x8086",
                .model = "intel® (R) iris™ (TM) pro graphics",
                .renderer = "amd radeon rx 6700 xt (RADV NAVI22)",
            },
        };
        flags.ApplyGpuFeaturePatterns(mixed_gpus);
        EXPECT_TRUE(flags.IsEnabled(S::FORCE_ANGLE));
        EXPECT_TRUE(flags.IsEnabled(S::FORCE_LAVAPIPE));
    }
}

}  // namespace
}  // namespace android::goldfish
