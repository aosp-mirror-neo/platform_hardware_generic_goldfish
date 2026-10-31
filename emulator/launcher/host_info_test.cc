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

#include "host_info.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include "android/base/testing/test_system.h"
#include "android/base/testing/test_temp_dir.h"
#include "android/goldfish/mock_avd.h"
#include "studio_stats.pb.h"

using android::base::TestSystem;
using android::goldfish::MockAvd;
using ::testing::Return;
using ::testing::ReturnRef;

namespace android::goldfish {

using android::base::TestSystem;

TEST(HostInfoTest, RunningInCiDefaultFalse) {
    TestSystem test_sys("/tmp", "/home", "");
    EXPECT_FALSE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithCi1) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("CI", "1");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithCiTrue) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("CI", "true");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithContinuousIntegration) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("CONTINUOUS_INTEGRATION", "1");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithGithubActions) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("GITHUB_ACTIONS", "true");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithGitlabCi) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("GITLAB_CI", "1");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithJenkins) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("JENKINS_URL", "http://jenkins.example.com");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiFalseWithBuildIdAlone) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("BUILD_ID", "12345");
    EXPECT_FALSE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithTfBuild) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("TF_BUILD", "1");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, RunningInCiTrueWithCircleCi) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("CIRCLECI", "true");
    EXPECT_TRUE(IsRunningInCi());
}

TEST(HostInfoTest, AndroidCliDefinedDefaultFalse) {
    TestSystem test_sys("/tmp", "/home", "");
    EXPECT_FALSE(IsAndroidCliDefined());
}

TEST(HostInfoTest, AndroidCliDefinedFalseWhenNotOne) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("ANDROID_CLI", "0");
    EXPECT_FALSE(IsAndroidCliDefined());
    test_sys.EnvSet("ANDROID_CLI", "true");
    EXPECT_FALSE(IsAndroidCliDefined());
}

TEST(HostInfoTest, AndroidCliDefinedTrueWhenOne) {
    TestSystem test_sys("/tmp", "/home", "");
    test_sys.EnvSet("ANDROID_CLI", "1");
    EXPECT_TRUE(IsAndroidCliDefined());
}

TEST(HostInfoTest, RunningInContainerWithEnvVar) {
    TestSystem test_sys("/tmp", "/home", "");
#if defined(__linux__)
    test_sys.EnvSet("container", "podman");
    EXPECT_TRUE(IsRunningInContainer());
#endif
}

TEST(HostInfoTest, RunningInContainerWithK8s) {
    TestSystem test_sys("/tmp", "/home", "");
#if defined(__linux__)
    test_sys.EnvSet("KUBERNETES_SERVICE_HOST", "10.0.0.1");
    EXPECT_TRUE(IsRunningInContainer());
#endif
}

class HostInfoVulkanTest : public ::testing::Test {
  protected:
    static std::filesystem::path StudioPrefsDir() {
        static const android::base::TestTempDir kTempDir("host_info_studio_prefs");
        return kTempDir.Path() / "studio_prefs";
    }

    void SetUp() override {
        std::filesystem::remove_all(StudioPrefsDir());
        test_sys_.EnvSet("ANDROID_STUDIO_PREFERENCES", StudioPrefsDir().string());

        EXPECT_CALL(avd_, ApiLevel()).WillRepeatedly(Return(37));
        EXPECT_CALL(avd_, Arch()).WillRepeatedly(Return(Avd::CpuArchitecture::kArm));
        EXPECT_CALL(avd_, BuildId())
                .WillRepeatedly(Return("google/generic_system_google/generic:17/CE2A.260420.008/"
                                       "15505217:userdebug/dev-keys"));
        EXPECT_CALL(avd_, BuildTimestamp()).WillRepeatedly(Return(1779905333));
        EXPECT_CALL(avd_, DisplayName()).WillRepeatedly(Return("PIXEL_8"));
        EXPECT_CALL(avd_, GetSystemImagePaths()).WillRepeatedly(ReturnRef(sys_image_paths_));
        EXPECT_CALL(avd_, Hw()).WillRepeatedly(ReturnRef(hw_config_));
    }

    void TearDown() override { std::filesystem::remove_all(StudioPrefsDir()); }

    TestSystem test_sys_{"/tmp", "/home", ""};
    MockAvd avd_;
    SystemImagePaths sys_image_paths_;
    HardwareConfig hw_config_;
    FeatureFlags feature_flags_;
};

TEST_F(HostInfoVulkanTest, setVulkanIcdHostDefault) {
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "", {},
                          std::nullopt);

    EXPECT_EQ(event.emulator_details().vulkan_icd(),
              android_studio::EmulatorDetails::HOST_DEFAULT_VK);
}

TEST_F(HostInfoVulkanTest, setVulkanIcdSwiftshader) {
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "swiftshader", {},
                          std::nullopt);

    EXPECT_EQ(event.emulator_details().vulkan_icd(),
              android_studio::EmulatorDetails::SWIFTSHADER_VK);
}

TEST_F(HostInfoVulkanTest, setVulkanIcdLavapipe) {
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "lavapipe", {},
                          std::nullopt);

    EXPECT_EQ(event.emulator_details().vulkan_icd(), android_studio::EmulatorDetails::LAVAPIPE_VK);
}

TEST_F(HostInfoVulkanTest, setVulkanIcdMoltenvk) {
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "moltenvk", {},
                          std::nullopt);

    EXPECT_EQ(event.emulator_details().vulkan_icd(), android_studio::EmulatorDetails::MOLTEN_VK);
}

TEST_F(HostInfoVulkanTest, setVulkanIcdKosmickrisp) {
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "kosmickrisp", {},
                          std::nullopt);

    EXPECT_EQ(event.emulator_details().vulkan_icd(),
              android_studio::EmulatorDetails::KOSMICKRISP_VK);
}

TEST_F(HostInfoVulkanTest, setVulkanIcdUnknown) {
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "unknown_icd", {},
                          std::nullopt);

    EXPECT_EQ(event.emulator_details().vulkan_icd(), android_studio::EmulatorDetails::UNKNOWN_VK);
}

TEST_F(HostInfoVulkanTest, setDeviceNameMappings) {
    using android_studio::EmulatorAvdInfo;
    const std::pair<std::string, EmulatorAvdInfo::EmulatorDeviceName> kTestCases[] = {
        {"pixel_10a", EmulatorAvdInfo::PIXEL_10A},
        {"pixel_11_pro_fold", EmulatorAvdInfo::PIXEL_11_PRO_FOLD},
        {"foldable_4_3", EmulatorAvdInfo::FOLDABLE_4_3},
        {"foldable_1_1", EmulatorAvdInfo::FOLDABLE_1_1},
        {"foldable_4_3_测试_🚀_é", EmulatorAvdInfo::UNKNOWN_EMULATOR_DEVICE_NAME},
        {"ai_glasses_device", EmulatorAvdInfo::AI_GLASSES_DEVICE},
        {"ai_glasses_displayless", EmulatorAvdInfo::AI_GLASSES_AUDIO_DEVICE},
        {"non_existent_device", EmulatorAvdInfo::UNKNOWN_EMULATOR_DEVICE_NAME},
    };

    for (const auto& [hw_device_name, expected_device_name] : kTestCases) {
        hw_config_.hw_device_name = hw_device_name;
        android_studio::AndroidStudioEvent event;
        FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "", {},
                              std::nullopt);

        EXPECT_EQ(event.emulator_details().avd_info().device_name(), expected_device_name)
                << "Failed for hw_device_name: " << hw_device_name;
    }
}

TEST_F(HostInfoVulkanTest, setsProductChannelFromStudioConfig) {
    // Default with no Studio config is UNKNOWN_LIFE_CYCLE_CHANNEL.
    {
        android_studio::AndroidStudioEvent event;
        FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "", {},
                              std::nullopt);
        EXPECT_EQ(event.product_details().channel(),
                  android_studio::ProductDetails::UNKNOWN_LIFE_CYCLE_CHANNEL);
    }

    const std::filesystem::path prefs_dir = StudioPrefsDir();
    std::filesystem::create_directories(prefs_dir / "options");

    {
        std::ofstream os(prefs_dir / "options" / "updates.xml");
        os << R"xml(<application><component name="UpdatesConfigurable">
            <option name="UPDATE_CHANNEL_TYPE" value="eap" />
        </component></application>)xml";
    }
    {
        android_studio::AndroidStudioEvent event;
        FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "", {},
                              std::nullopt);
        EXPECT_EQ(event.product_details().channel(), android_studio::ProductDetails::CANARY);
    }

    {
        std::ofstream os(prefs_dir / "options" / "updates.xml");
        os << R"xml(<application><component name="UpdatesConfigurable">
            <option name="UPDATE_CHANNEL_TYPE" value="release" />
        </component></application>)xml";
    }
    {
        android_studio::AndroidStudioEvent event;
        FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "", {},
                              std::nullopt);
        EXPECT_EQ(event.product_details().channel(), android_studio::ProductDetails::STABLE);
    }
}

TEST_F(HostInfoVulkanTest, populatesHostGpusAndSelectedGpu) {
    VulkanGpuDeviceInfo integrated_gpu{};
    integrated_gpu.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 2, 189);
    integrated_gpu.properties.driverVersion = 0x00400001;
    integrated_gpu.properties.vendorID = 0x8086;
    integrated_gpu.properties.deviceID = 0x9a49;
    integrated_gpu.properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    std::strncpy(integrated_gpu.properties.deviceName, "Intel(R) Xe Graphics",
                 sizeof(integrated_gpu.properties.deviceName) - 1);
    integrated_gpu.device_local_memory_bytes = 2147483648ULL;

    VulkanGpuDeviceInfo discrete_gpu{};
    discrete_gpu.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 277);
    discrete_gpu.properties.driverVersion = 0x8a920000;
    discrete_gpu.properties.vendorID = 0x10de;
    discrete_gpu.properties.deviceID = 0x2684;
    discrete_gpu.properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    std::strncpy(discrete_gpu.properties.deviceName, "NVIDIA GeForce RTX 4090",
                 sizeof(discrete_gpu.properties.deviceName) - 1);
    discrete_gpu.device_local_memory_bytes = 25769803776ULL;

    const std::vector<VulkanGpuDeviceInfo> host_gpus = {integrated_gpu, discrete_gpu};

    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "", host_gpus,
                          discrete_gpu);

    const auto& details = event.emulator_details();
    EXPECT_EQ(details.vulkan_icd(), android_studio::EmulatorDetails::HOST_DEFAULT_VK);
    ASSERT_EQ(details.host_gpu_size(), 2);

    EXPECT_EQ(details.host_gpu(0).make(), "0x8086");
    EXPECT_EQ(details.host_gpu(0).device_id(), "0x9a49");
    EXPECT_EQ(details.host_gpu(0).model(), "Intel(R) Xe Graphics");
    EXPECT_EQ(details.host_gpu(0).renderer(), "Intel(R) Xe Graphics");
    EXPECT_EQ(details.host_gpu(0).revision_id(), "0x400001");
    EXPECT_EQ(details.host_gpu(0).version(), "1.2.189");

    EXPECT_EQ(details.host_gpu(1).make(), "0x10de");
    EXPECT_EQ(details.host_gpu(1).device_id(), "0x2684");
    EXPECT_EQ(details.host_gpu(1).model(), "NVIDIA GeForce RTX 4090");
    EXPECT_EQ(details.host_gpu(1).renderer(), "NVIDIA GeForce RTX 4090");
    EXPECT_EQ(details.host_gpu(1).revision_id(), "0x8a920000");
    EXPECT_EQ(details.host_gpu(1).version(), "1.3.277");

    ASSERT_TRUE(details.has_active_vulkan_host_gpu());
    const auto& active = details.active_vulkan_host_gpu();
    EXPECT_EQ(active.vendor_id(), 0x10de);
    EXPECT_EQ(active.device_id(), 0x2684);
    EXPECT_EQ(active.device_type(),
              android_studio::EmulatorGpuVkInfo::PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    EXPECT_EQ(active.api_version(), static_cast<int32_t>(VK_MAKE_API_VERSION(0, 1, 3, 277)));
    EXPECT_EQ(active.driver_version(), static_cast<int32_t>(0x8a920000));
    EXPECT_EQ(active.gpu_memory(), 25769803776LL);
    EXPECT_EQ(active.device_name(), "NVIDIA GeForce RTX 4090");
}

TEST_F(HostInfoVulkanTest, populatesSoftwareActiveVulkanHostGpu) {
    VulkanGpuDeviceInfo host_gpu{};
    host_gpu.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 2, 0);
    host_gpu.properties.vendorID = 0x1002;
    host_gpu.properties.deviceID = 0x73bf;
    host_gpu.properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    std::strncpy(host_gpu.properties.deviceName, "AMD Radeon RX 6800",
                 sizeof(host_gpu.properties.deviceName) - 1);

    VulkanGpuDeviceInfo software_gpu{};
    software_gpu.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 305);
    software_gpu.properties.driverVersion = 0x00000001;
    software_gpu.properties.vendorID = 0x10005;
    software_gpu.properties.deviceID = 0x0000;
    software_gpu.properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    std::strncpy(software_gpu.properties.deviceName, "llvmpipe (LLVM 21.1.8, 256 bits)",
                 sizeof(software_gpu.properties.deviceName) - 1);

    const std::vector<VulkanGpuDeviceInfo> host_gpus = {host_gpu};
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "lavapipe",
                          host_gpus, software_gpu);

    const auto& details = event.emulator_details();
    EXPECT_EQ(details.vulkan_icd(), android_studio::EmulatorDetails::LAVAPIPE_VK);
    ASSERT_EQ(details.host_gpu_size(), 1);
    EXPECT_EQ(details.host_gpu(0).model(), "AMD Radeon RX 6800");

    ASSERT_TRUE(details.has_active_vulkan_host_gpu());
    const auto& active = details.active_vulkan_host_gpu();
    EXPECT_EQ(active.vendor_id(), 0x10005);
    EXPECT_EQ(active.device_id(), 0);
    EXPECT_EQ(active.device_type(), android_studio::EmulatorGpuVkInfo::PHYSICAL_DEVICE_TYPE_CPU);
    EXPECT_EQ(active.api_version(), static_cast<int32_t>(VK_MAKE_API_VERSION(0, 1, 4, 305)));
    EXPECT_EQ(active.driver_version(), 1);
    EXPECT_EQ(active.device_name(), "llvmpipe (LLVM 21.1.8, 256 bits)");
}

TEST_F(HostInfoVulkanTest, omitsActiveVulkanHostGpuWhenNoneSelected) {
    VulkanGpuDeviceInfo host_gpu{};
    host_gpu.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 2, 0);
    host_gpu.properties.vendorID = 0x1002;
    host_gpu.properties.deviceID = 0x73bf;
    std::strncpy(host_gpu.properties.deviceName, "AMD Radeon RX 6800",
                 sizeof(host_gpu.properties.deviceName) - 1);

    const std::vector<VulkanGpuDeviceInfo> host_gpus = {host_gpu};
    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, feature_flags_, 1234, 5678, true, false, "lavapipe",
                          host_gpus, std::nullopt);

    const auto& details = event.emulator_details();
    EXPECT_EQ(details.vulkan_icd(), android_studio::EmulatorDetails::LAVAPIPE_VK);
    ASSERT_EQ(details.host_gpu_size(), 1);
    EXPECT_EQ(details.host_gpu(0).model(), "AMD Radeon RX 6800");
    EXPECT_FALSE(details.has_active_vulkan_host_gpu());
}

TEST_F(HostInfoVulkanTest, populatesFeatureFlagState) {
    using android_studio::EmulatorFeatureFlagState;
    const auto temp_dir = test_sys_.GetTempRoot()->Path();
    const auto host_ini = temp_dir / "host_advancedFeatures.ini";
    const auto guest_ini = temp_dir / "guest_advancedFeatures.ini";

    {
        std::ofstream os(host_ini);
        os << "VulkanSnapshots = on\n";
        os << "ForceSwiftshader = off\n";
        os << "Minigbm = on\n";
        os << "Uwb = on\n";
    }
    {
        std::ofstream os(guest_ini);
        os << "Minigbm = on\n";
        os << "Uwb = on\n";
    }

    auto flags = FeatureFlags::Create({
        .host_ini_path = host_ini,
        .guest_ini_path = guest_ini,
        .avd_feature_flags = "ForceSwiftshader,-Uwb",
    });
    ASSERT_TRUE(flags.ok()) << flags.status();

    android_studio::AndroidStudioEvent event;
    FillEmulatorHostEvent(event, avd_, *flags, 1234, 5678, true, false, "", {}, std::nullopt);

    const auto& ff_state = event.emulator_details().feature_flag_state();
    EXPECT_THAT(ff_state.attempted_enabled_feature_flags(),
                ::testing::UnorderedElementsAre(EmulatorFeatureFlagState::VULKAN_SNAPSHOTS,
                                                EmulatorFeatureFlagState::MINIGBM,
                                                EmulatorFeatureFlagState::UWB));
    EXPECT_THAT(ff_state.user_overridden_enabled_features(),
                ::testing::UnorderedElementsAre(EmulatorFeatureFlagState::FORCE_SWIFTSHADER));
    EXPECT_THAT(ff_state.user_overridden_disabled_features(),
                ::testing::UnorderedElementsAre(EmulatorFeatureFlagState::UWB));
    EXPECT_THAT(ff_state.resulting_enabled_features(),
                ::testing::UnorderedElementsAre(EmulatorFeatureFlagState::VULKAN_SNAPSHOTS,
                                                EmulatorFeatureFlagState::MINIGBM,
                                                EmulatorFeatureFlagState::FORCE_SWIFTSHADER));
}

}  // namespace android::goldfish
