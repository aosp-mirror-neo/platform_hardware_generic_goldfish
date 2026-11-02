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

#include "gpu_device.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"

#include "android/base/system.h"
#include "android/base/testing/test_system.h"
#include "android/base/testing/test_temp_dir.h"
#include "android/cmdline_definitions.h"
#include "android/status/status_matcher_macros.h"
#include "fake_emulator.h"
#include "goldfish/file/file.h"
#include "goldfish/gvk/i_meta_loader.h"
#include "snapshot_device.h"
#include "virtio_gpu_selection.h"

namespace android::goldfish::test {

using ::absl::ScopedMockLog;
using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::Pair;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::StartsWith;
using ::testing::UnorderedElementsAre;

namespace {

struct FakePhysicalDevice {
    VkPhysicalDeviceProperties properties{};
    uint64_t device_local_memory_bytes = 0;
    bool has_graphics_queue = true;
};

FakePhysicalDevice MakeFakePhysicalDevice(std::string_view name, uint32_t vendor_id,
                                          uint32_t device_id, VkPhysicalDeviceType device_type,
                                          uint32_t api_version, uint32_t driver_version,
                                          uint64_t memory_bytes, bool has_graphics_queue) {
    FakePhysicalDevice dev;
    dev.properties.apiVersion = api_version;
    dev.properties.driverVersion = driver_version;
    dev.properties.vendorID = vendor_id;
    dev.properties.deviceID = device_id;
    dev.properties.deviceType = device_type;
    std::strncpy(dev.properties.deviceName, name.data(),
                 std::min(name.size(), sizeof(dev.properties.deviceName) - 1));
    dev.device_local_memory_bytes = memory_bytes;
    dev.has_graphics_queue = has_graphics_queue;
    return dev;
}

class FakeVulkanLoader : public ::goldfish::gvk::IMetaLoader {
  public:
    explicit FakeVulkanLoader(std::vector<FakePhysicalDevice> host_devices,
                              std::vector<FakePhysicalDevice> software_devices = {})
            : host_devices_(std::move(host_devices)),
              software_devices_(std::move(software_devices)) {}

    uint32_t enumerateInstanceVersion() const override { return VK_MAKE_API_VERSION(0, 1, 3, 0); }

    std::vector<VkLayerProperties> enumerateInstanceLayerProperties() const override { return {}; }

    std::vector<VkExtensionProperties> enumerateInstanceExtensionProperties(
            const char* /*layer_name*/) const override {
        return {};
    }

    bool instance_destroyed() const {
        return instances_created_ > 0 && instances_created_ == instances_destroyed_;
    }
    const std::string& observed_vk_driver_files() const { return observed_vk_driver_files_; }
    const std::string& observed_vk_icd_filenames() const { return observed_vk_icd_filenames_; }
    const std::string& observed_vk_add_driver_files() const {
        return observed_vk_add_driver_files_;
    }
    const std::string& observed_vk_loader_drivers_disable() const {
        return observed_vk_loader_drivers_disable_;
    }

  private:
    VkInstance createInstance(const VkInstanceCreateInfo& /*create_info*/) const override {
        observed_vk_driver_files_ = base::System::GetEnvironmentVariable("VK_DRIVER_FILES");
        observed_vk_icd_filenames_ = base::System::GetEnvironmentVariable("VK_ICD_FILENAMES");
        observed_vk_add_driver_files_ = base::System::GetEnvironmentVariable("VK_ADD_DRIVER_FILES");
        observed_vk_loader_drivers_disable_ =
                base::System::GetEnvironmentVariable("VK_LOADER_DRIVERS_DISABLE");
        ++instances_created_;
        return reinterpret_cast<VkInstance>(const_cast<FakeVulkanLoader*>(this));
    }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr() const override { return &GetInstanceProcAddr; }

    const std::vector<FakePhysicalDevice>* GetActiveDevices() const {
        if (!observed_vk_icd_filenames_.empty() || !observed_vk_add_driver_files_.empty()) {
            return nullptr;
        }
        if (!observed_vk_driver_files_.empty()) {
            if (!observed_vk_loader_drivers_disable_.empty()) {
                return nullptr;
            }
            const fs::path icd_path(observed_vk_driver_files_);
            if (!android::base::file::is_file(icd_path)) {
                return nullptr;
            }
            const std::string filename = icd_path.filename().string();
            if (filename == "lvp_icd.json" || filename == "vk_swiftshader_icd.json") {
                return &software_devices_;
            }
#if defined(__APPLE__)
            if (filename == "libkosmickrisp_icd.json" || filename == "MoltenVK_icd.json") {
                return &host_devices_;
            }
#endif
            return nullptr;
        }
#if !defined(__APPLE__)
        if (observed_vk_loader_drivers_disable_ == "*lvp*,*swiftshader*") {
            return &host_devices_;
        }
#endif
        return nullptr;
    }

    static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                                      const VkAllocationCallbacks* /*allocator*/) {
        ++reinterpret_cast<FakeVulkanLoader*>(instance)->instances_destroyed_;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL
    EnumeratePhysicalDevices(VkInstance instance, uint32_t* p_physical_device_count,
                             VkPhysicalDevice* p_physical_devices) {
        auto* self = reinterpret_cast<FakeVulkanLoader*>(instance);
        const auto* active_devices = self->GetActiveDevices();
        const size_t available_count = active_devices ? active_devices->size() : 0;
        if (!p_physical_devices) {
            *p_physical_device_count = static_cast<uint32_t>(available_count);
            return VK_SUCCESS;
        }
        const uint32_t count =
                std::min(*p_physical_device_count, static_cast<uint32_t>(available_count));
        for (uint32_t i = 0; i < count; ++i) {
            p_physical_devices[i] = reinterpret_cast<VkPhysicalDevice>(
                    const_cast<FakePhysicalDevice*>(&(*active_devices)[i]));
        }
        *p_physical_device_count = count;
        return count < available_count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(
            VkPhysicalDevice physical_device, VkPhysicalDeviceProperties* p_properties) {
        *p_properties = reinterpret_cast<const FakePhysicalDevice*>(physical_device)->properties;
    }

    static VKAPI_ATTR void VKAPI_CALL
    GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical_device,
                                      VkPhysicalDeviceMemoryProperties* p_memory_properties) {
        const auto* dev = reinterpret_cast<const FakePhysicalDevice*>(physical_device);
        *p_memory_properties = {};
        if (dev->device_local_memory_bytes > 0) {
            p_memory_properties->memoryHeapCount = 1;
            p_memory_properties->memoryHeaps[0].size = dev->device_local_memory_bytes;
            p_memory_properties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        }
    }

    static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(
            VkPhysicalDevice physical_device, uint32_t* p_queue_family_property_count,
            VkQueueFamilyProperties* p_queue_family_properties) {
        const auto* dev = reinterpret_cast<const FakePhysicalDevice*>(physical_device);
        if (!p_queue_family_properties) {
            *p_queue_family_property_count = 1;
            return;
        }
        if (*p_queue_family_property_count > 0) {
            p_queue_family_properties[0] = {};
            p_queue_family_properties[0].queueCount = 1;
            p_queue_family_properties[0].queueFlags =
                    dev->has_graphics_queue ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT;
            *p_queue_family_property_count = 1;
        }
    }

    static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
            VkPhysicalDevice /*physical_device*/, const char* /*p_layer_name*/,
            uint32_t* p_property_count, VkExtensionProperties* /*p_properties*/) {
        *p_property_count = 0;
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice /*physical_device*/,
                                                       const VkDeviceCreateInfo* /*p_create_info*/,
                                                       const VkAllocationCallbacks* /*p_allocator*/,
                                                       VkDevice* /*p_device*/) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice /*device*/,
                                                                      const char* /*p_name*/) {
        return nullptr;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(
            VkDevice /*device*/, const VkRenderPassCreateInfo* /*p_create_info*/,
            const VkAllocationCallbacks* /*p_allocator*/, VkRenderPass* /*p_render_pass*/) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance /*instance*/,
                                                                        const char* name) {
        const std::string_view fn_name(name);
        if (fn_name == "vkDestroyInstance") {
            return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
        }
        if (fn_name == "vkEnumeratePhysicalDevices") {
            return reinterpret_cast<PFN_vkVoidFunction>(&EnumeratePhysicalDevices);
        }
        if (fn_name == "vkGetPhysicalDeviceProperties") {
            return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceProperties);
        }
        if (fn_name == "vkGetPhysicalDeviceMemoryProperties") {
            return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceMemoryProperties);
        }
        if (fn_name == "vkGetPhysicalDeviceQueueFamilyProperties") {
            return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceQueueFamilyProperties);
        }
        if (fn_name == "vkEnumerateDeviceExtensionProperties") {
            return reinterpret_cast<PFN_vkVoidFunction>(&EnumerateDeviceExtensionProperties);
        }
        if (fn_name == "vkCreateDevice") {
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
        }
        if (fn_name == "vkGetDeviceProcAddr") {
            return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
        }
        if (fn_name == "vkCreateRenderPass") {
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateRenderPass);
        }
        return nullptr;
    }

    std::vector<FakePhysicalDevice> host_devices_;
    std::vector<FakePhysicalDevice> software_devices_;
    mutable size_t instances_created_ = 0;
    mutable size_t instances_destroyed_ = 0;
    mutable std::string observed_vk_driver_files_;
    mutable std::string observed_vk_icd_filenames_;
    mutable std::string observed_vk_add_driver_files_;
    mutable std::string observed_vk_loader_drivers_disable_;
};

void SetupFakeVulkanLibDir(const base::TestTempDir& temp_dir, FakeEmulator& emu) {
    const fs::path vulkan_dir = temp_dir.path / "vulkan";
    ASSERT_OK(android::base::file::mkdir_recursive(vulkan_dir, 0755));
    ASSERT_OK(android::base::file::touch(vulkan_dir / "libkosmickrisp_icd.json"));
    ASSERT_OK(android::base::file::touch(vulkan_dir / "MoltenVK_icd.json"));
    ASSERT_OK(android::base::file::touch(vulkan_dir / "lvp_icd.json"));
    ASSERT_OK(android::base::file::touch(vulkan_dir / "vk_swiftshader_icd.json"));
    emu.emulator_paths().lib64_directory = temp_dir.path;
}

}  // namespace

TEST(GpuDeviceTest, DefaultSoftwareGpuEnv) {
    base::TestSystem sys("");
    FakeEmulator emu;
    emu.emulator_paths().launcher_directory = "/default/launcher";
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.getVulkanIcd(), "lavapipe");
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(Pair("ANDROID_EMU_RENDERER", "lavapipe"),
                                     Pair("ANDROID_EMU_VK_ICD", "lavapipe"),
                                     Pair("ANDROID_EGL_ON_EGL", "1"),
                                     Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                                     Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/default/launcher")));
}

// Exactly one virtio-gpu device goes on the command line; the properties are
// the same for both, only the device name differs (-virtio-gpu-device).
TEST(GpuDeviceTest, DefaultsToVirtioGpuGfxstreamAndWarnsWithRutabagaFallback) {
    base::TestSystem sys("");
    FakeEmulator emu;
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         AllOf(HasSubstr("virtio-gpu-gfxstream is now used by default"),
                               HasSubstr("https://issuetracker.google.com/issues/new?component="),
                               HasSubstr("-virtio-gpu-device rutabaga"),
                               HasSubstr("deprecated virtio-gpu-rutabaga"))));
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         HasSubstr("virtio-gpu-rutabaga is deprecated")))
            .Times(0);
    log.StartCapturingLogs();
    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    log.StopCapturingLogs();
    EXPECT_EQ(dev.virtio_gpu_device(), VirtioGpuDevice::kGfxstream);
    const std::vector<std::string> params = dev.getQemuParameters(emu.config());
    ASSERT_THAT(params, ElementsAre(Eq("-device"), HasSubstr("gfxstream-vulkan=on")));
    EXPECT_THAT(params[1],
                AllOf(StartsWith("virtio-gpu-gfxstream,"), Not(HasSubstr("virtio-gpu-rutabaga"))));
}

TEST(GpuDeviceTest, VirtioGpuDeviceOptionSelectsRutabagaOrGfxstream) {
    base::TestSystem sys("");
    AndroidOptions rutabaga_opts{};
    rutabaga_opts.virtio_gpu_device = const_cast<char*>("rutabaga");
    FakeEmulator rutabaga_emu(rutabaga_opts);
    HardwareConfig hw;
    EXPECT_CALL(rutabaga_emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    SnapshotDevice snapshot_dev;
    GpuDevice rutabaga_dev("gpu0", snapshot_dev);
    {
        ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
        EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                             HasSubstr("virtio-gpu-gfxstream is now used by default")))
                .Times(0);
        EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                             AllOf(HasSubstr("virtio-gpu-rutabaga is deprecated"),
                                   HasSubstr("switch to virtio-gpu-gfxstream"),
                                   HasSubstr("https://issuetracker.google.com/issues/new?"
                                             "component="))));
        log.StartCapturingLogs();
        EXPECT_OK(rutabaga_dev.initialize(rutabaga_emu.config()));
    }
    EXPECT_EQ(rutabaga_dev.virtio_gpu_device(), VirtioGpuDevice::kRutabaga);
    const std::vector<std::string> rutabaga_params =
            rutabaga_dev.getQemuParameters(rutabaga_emu.config());
    ASSERT_THAT(rutabaga_params, ElementsAre(Eq("-device"), HasSubstr("gfxstream-vulkan=on")));
    EXPECT_THAT(rutabaga_params[1],
                AllOf(StartsWith("virtio-gpu-rutabaga,"), Not(HasSubstr("virtio-gpu-gfxstream"))));

    // Same properties as the gfxstream device, just a different device name.
    // An explicit -virtio-gpu-device gfxstream is an opt-in: no warning at all.
    AndroidOptions gfxstream_opts{};
    gfxstream_opts.virtio_gpu_device = const_cast<char*>("gfxstream");
    FakeEmulator gfxstream_emu(gfxstream_opts);
    EXPECT_CALL(gfxstream_emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
    GpuDevice gfxstream_dev("gpu0", snapshot_dev);
    {
        ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
        EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _, HasSubstr("virtio-gpu"))).Times(0);
        log.StartCapturingLogs();
        EXPECT_OK(gfxstream_dev.initialize(gfxstream_emu.config()));
    }
    EXPECT_EQ(gfxstream_dev.virtio_gpu_device(), VirtioGpuDevice::kGfxstream);
    const std::vector<std::string> gfxstream_params =
            gfxstream_dev.getQemuParameters(gfxstream_emu.config());
    ASSERT_EQ(gfxstream_params.size(), 2u);
    EXPECT_EQ(rutabaga_params[1].substr(std::strlen("virtio-gpu-rutabaga")),
              gfxstream_params[1].substr(std::strlen("virtio-gpu-gfxstream")));
}

TEST(GpuDeviceTest, InvalidVirtioGpuDeviceOptionIsRejected) {
    base::TestSystem sys("");
    AndroidOptions opts{};
    opts.virtio_gpu_device = const_cast<char*>("virgl");
    FakeEmulator emu(opts);
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_THAT(
            dev.initialize(emu.config()),
            ::absl_testing::StatusIs(
                    absl::StatusCode::kInvalidArgument,
                    HasSubstr(
                            "Invalid -virtio-gpu-device 'virgl': expected rutabaga or gfxstream")));
}

TEST(GpuDeviceTest, EscapesCommasInSnapshotDirectoryAndRendererFeatures) {
    base::TestSystem sys("");
    const fs::path avd_content = sys.GetTempRoot()->Path() / "avd,with,comma";
    ASSERT_OK(base::file::mkdir_recursive(avd_content, 0755));

    char renderer_features[] = "FeatureA:enabled,FeatureB:disabled";
    AndroidOptions opts{.renderer_features = renderer_features};
    FakeEmulator emu(opts);
    HardwareConfig hw;
    hw.hw_lcd_width = 1080;
    hw.hw_lcd_height = 2400;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));

    SnapshotDevice snapshot_dev;
    EXPECT_OK(snapshot_dev.initialize(emu.config()));
    ASSERT_TRUE(snapshot_dev.is_enabled());

    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));

    const fs::path expected_snapshot_dir = avd_content / "snapshots" / "";
    EXPECT_THAT(
            dev.getQemuParameters(emu.config()),
            ElementsAre(Eq("-device"),
                        AllOf(HasSubstr("renderer_features=FeatureA:enabled,,FeatureB:disabled;"),
                              HasSubstr(absl::StrCat("snapshot_directory=",
                                                     Device::EscapeQemuOptionValue(
                                                             expected_snapshot_dir.string()))))));
}

TEST(GpuDeviceTest, SnapshotDirectoryPresentWhenOnlyQuickbootIsOff) {
    base::TestSystem sys("");
    const fs::path avd_content = sys.GetTempRoot()->Path() / "avd";
    ASSERT_OK(base::file::mkdir_recursive(avd_content, 0755));

    // Runtime snapshots (gRPC, console) stay available with -no-snapshot-load
    // -no-snapshot-save, so the GPU still needs its snapshot directory.
    AndroidOptions opts{};
    opts.no_snapshot_load = true;
    opts.no_snapshot_save = true;
    FakeEmulator emu(opts);
    HardwareConfig hw;
    hw.hw_lcd_width = 1080;
    hw.hw_lcd_height = 2400;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));

    SnapshotDevice snapshot_dev;
    EXPECT_OK(snapshot_dev.initialize(emu.config()));
    ASSERT_FALSE(snapshot_dev.is_enabled());
    ASSERT_TRUE(snapshot_dev.snapshots_enabled());

    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));

    const fs::path expected_snapshot_dir = avd_content / "snapshots" / "";
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAre(Eq("-device"),
                            AllOf(HasSubstr("VulkanSnapshots:enabled"),
                                  HasSubstr(absl::StrCat("snapshot_directory=",
                                                         Device::EscapeQemuOptionValue(
                                                                 expected_snapshot_dir.string()))))));
}

TEST(GpuDeviceTest, NoSnapshotDisablesSnapshotDirectory) {
    base::TestSystem sys("");
    const fs::path avd_content = sys.GetTempRoot()->Path() / "avd";
    ASSERT_OK(base::file::mkdir_recursive(avd_content, 0755));

    AndroidOptions opts{};
    opts.no_snapshot = true;
    FakeEmulator emu(opts);
    HardwareConfig hw;
    hw.hw_lcd_width = 1080;
    hw.hw_lcd_height = 2400;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
    EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));

    SnapshotDevice snapshot_dev;
    EXPECT_OK(snapshot_dev.initialize(emu.config()));
    ASSERT_FALSE(snapshot_dev.snapshots_enabled());

    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_THAT(dev.getQemuParameters(emu.config()),
                ElementsAre(Eq("-device"), AllOf(Not(HasSubstr("VulkanSnapshots:enabled")),
                                                 Not(HasSubstr("snapshot_directory=")))));
}

TEST(GpuDeviceTest, HostGpuModeAndLauncherDirEnv) {
    base::TestSystem sys("");

    char gpu_mode[] = "host";
    AndroidOptions opts{.gpu = gpu_mode};
    FakeEmulator emu(opts);
    emu.emulator_paths().launcher_directory = "/path/to/launcher";
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));

#if defined(__APPLE__)
    EXPECT_EQ(dev.getVulkanIcd(), "kosmickrisp");
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(Pair("ANDROID_EMU_RENDERER", "host"),
                                     Pair("ANDROID_EMU_VK_ICD", "kosmickrisp"),
                                     Pair("ANDROID_EGL_ON_EGL", "1"),
                                     Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                                     Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher")));

    sys.EnvSet("ANDROID_EMU_VK_SELECT_GPU", "1");
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(Pair("ANDROID_EMU_RENDERER", "host"),
                                     Pair("ANDROID_EMU_VK_ICD", "kosmickrisp"),
                                     Pair("ANDROID_EGL_ON_EGL", "1"),
                                     Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                                     Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher"),
                                     Pair("ANDROID_EMU_VK_SELECT_GPU", "1")));
#else
    EXPECT_EQ(dev.getVulkanIcd(), "");
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(
                        Pair("ANDROID_EMU_RENDERER", "host"), Pair("ANDROID_EMU_VK_ICD", ""),
                        Pair("ANDROID_EGL_ON_EGL", "1"), Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                        Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher")));

    sys.EnvSet("ANDROID_EMU_VK_SELECT_GPU", "1");
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(
                        Pair("ANDROID_EMU_RENDERER", "host"), Pair("ANDROID_EMU_VK_ICD", ""),
                        Pair("ANDROID_EGL_ON_EGL", "1"), Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                        Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher"),
                        Pair("ANDROID_EMU_VK_SELECT_GPU", "1")));
#endif
}

#if defined(__APPLE__)
TEST(GpuDeviceTest, MacOsMoltenVkSelection) {
    base::TestSystem sys("");
    sys.EnvSet("ANDROID_EMU_VK_SELECT_ICD", "moltenvk");

    char gpu_mode[] = "host";
    AndroidOptions opts{.gpu = gpu_mode};
    FakeEmulator emu(opts);
    emu.emulator_paths().launcher_directory = "/path/to/launcher";
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_EQ(dev.getVulkanIcd(), "moltenvk");
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(Pair("ANDROID_EMU_RENDERER", "host"),
                                     Pair("ANDROID_EMU_VK_ICD", "moltenvk"),
                                     Pair("ANDROID_EGL_ON_EGL", "1"),
                                     Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                                     Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher")));
}
#endif

#ifdef __linux__
TEST(GpuDeviceTest, LinuxPreloadLibgccEnv) {
    base::TestSystem sys("");
    sys.EnvSet("ANDROID_EMU_PRELOAD_LIBGCC", "1");

    FakeEmulator emu;
    emu.emulator_paths().launcher_directory = "/path/to/launcher";
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_THAT(dev.getQemuEnv(emu.config()),
                UnorderedElementsAre(Pair("ANDROID_EMU_RENDERER", "lavapipe"),
                                     Pair("ANDROID_EMU_VK_ICD", "lavapipe"),
                                     Pair("ANDROID_EGL_ON_EGL", "1"),
                                     Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                                     Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher"),
                                     Pair("LD_PRELOAD", "/lib/x86_64-linux-gnu/libgcc_s.so.1")));

    sys.EnvSet("LD_PRELOAD", "/custom/preload.so");
    EXPECT_THAT(
            dev.getQemuEnv(emu.config()),
            UnorderedElementsAre(
                    Pair("ANDROID_EMU_RENDERER", "lavapipe"),
                    Pair("ANDROID_EMU_VK_ICD", "lavapipe"), Pair("ANDROID_EGL_ON_EGL", "1"),
                    Pair("ANGLE_DEFAULT_PLATFORM", "vulkan"),
                    Pair("ANDROID_EMULATOR_LAUNCHER_DIR", "/path/to/launcher"),
                    Pair("LD_PRELOAD", "/lib/x86_64-linux-gnu/libgcc_s.so.1:/custom/preload.so")));
}
#endif

TEST(GpuDeviceTest, ProbesAndSelectsBestGpuInHostMode) {
    base::TestSystem sys("");
    base::TestTempDir temp_dir("gpu_device_test");

    auto loader = std::make_shared<FakeVulkanLoader>(std::vector<FakePhysicalDevice>{
        MakeFakePhysicalDevice("Intel(R) UHD Graphics", 0x8086, 0x3e92,
                               VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                               VK_MAKE_API_VERSION(0, 1, 2, 0), 100, 4294967296ULL, true),
        MakeFakePhysicalDevice("NVIDIA GeForce RTX 4080", 0x10de, 0x2704,
                               VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                               VK_MAKE_API_VERSION(0, 1, 3, 250), 535, 17179869184ULL, true),
    });

    char gpu_mode[] = "host";
    AndroidOptions opts{.gpu = gpu_mode};
    FakeEmulator emu(opts);
    emu.emulator_paths().launcher_directory = "/path/to/launcher";
    SetupFakeVulkanLibDir(temp_dir, emu);
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    GpuDevice::setVulkanLoader_TestOnly(loader);
    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    GpuDevice::setVulkanLoader_TestOnly(nullptr);
    EXPECT_TRUE(loader->instance_destroyed());

    ASSERT_EQ(dev.getHostGpus().size(), 2u);
    EXPECT_STREQ(dev.getHostGpus()[0].properties.deviceName, "Intel(R) UHD Graphics");
    EXPECT_EQ(dev.getHostGpus()[0].device_local_memory_bytes, 4294967296ULL);
    EXPECT_STREQ(dev.getHostGpus()[1].properties.deviceName, "NVIDIA GeForce RTX 4080");
    EXPECT_EQ(dev.getHostGpus()[1].device_local_memory_bytes, 17179869184ULL);

    ASSERT_TRUE(dev.getSelectedGpu().has_value());
    EXPECT_STREQ(dev.getSelectedGpu()->properties.deviceName, "NVIDIA GeForce RTX 4080");
    EXPECT_EQ(dev.getSelectedGpu()->device_local_memory_bytes, 17179869184ULL);
}

TEST(GpuDeviceTest, SelectGpuRespectsEnvVarOverrideByIndexAndName) {
    base::TestSystem sys("");
    base::TestTempDir temp_dir("gpu_device_test");
    auto loader = std::make_shared<FakeVulkanLoader>(std::vector<FakePhysicalDevice>{
        MakeFakePhysicalDevice("Intel(R) UHD Graphics", 0x8086, 0x3e92,
                               VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                               VK_MAKE_API_VERSION(0, 1, 2, 0), 100, 4294967296ULL, true),
        MakeFakePhysicalDevice("NVIDIA GeForce RTX 4080", 0x10de, 0x2704,
                               VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                               VK_MAKE_API_VERSION(0, 1, 3, 250), 535, 17179869184ULL, true),
    });

    char gpu_mode[] = "host";
    AndroidOptions opts{.gpu = gpu_mode};
    FakeEmulator emu(opts);
    SetupFakeVulkanLibDir(temp_dir, emu);
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    GpuDevice::setVulkanLoader_TestOnly(loader);
    SnapshotDevice snapshot_dev;

    sys.EnvSet("ANDROID_EMU_VK_SELECT_GPU", "0");
    {
        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(emu.config()));
        ASSERT_TRUE(dev.getSelectedGpu().has_value());
        EXPECT_STREQ(dev.getSelectedGpu()->properties.deviceName, "Intel(R) UHD Graphics");
    }

    sys.EnvSet("ANDROID_EMU_VK_SELECT_GPU", "uhd graphics");
    {
        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(emu.config()));
        ASSERT_TRUE(dev.getSelectedGpu().has_value());
        EXPECT_STREQ(dev.getSelectedGpu()->properties.deviceName, "Intel(R) UHD Graphics");
    }
    GpuDevice::setVulkanLoader_TestOnly(nullptr);
}

TEST(GpuDeviceTest, ProbesHostGpusAndSelectsSoftwareGpuInDefaultSoftwareMode) {
    base::TestSystem sys("");
    base::TestTempDir temp_dir("gpu_device_test");
    auto loader = std::make_shared<FakeVulkanLoader>(
            std::vector<FakePhysicalDevice>{
                MakeFakePhysicalDevice("NVIDIA GeForce RTX 4080", 0x10de, 0x2704,
                                       VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                       VK_MAKE_API_VERSION(0, 1, 3, 250), 535, 17179869184ULL,
                                       true),
            },
            std::vector<FakePhysicalDevice>{
                MakeFakePhysicalDevice("llvmpipe (LLVM 21.1.8, 256 bits)", 0x10005, 0x0,
                                       VK_PHYSICAL_DEVICE_TYPE_CPU,
                                       VK_MAKE_API_VERSION(0, 1, 4, 305), 1, 0ULL, true),
            });

    FakeEmulator emu;
    emu.emulator_paths().launcher_directory = "/path/to/launcher";
    SetupFakeVulkanLibDir(temp_dir, emu);
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    GpuDevice::setVulkanLoader_TestOnly(loader);
    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    GpuDevice::setVulkanLoader_TestOnly(nullptr);
    EXPECT_TRUE(loader->instance_destroyed());

    EXPECT_EQ(dev.getVulkanIcd(), "lavapipe");
    ASSERT_EQ(dev.getHostGpus().size(), 1u);
    EXPECT_STREQ(dev.getHostGpus()[0].properties.deviceName, "NVIDIA GeForce RTX 4080");

    ASSERT_TRUE(dev.getSelectedGpu().has_value());
    EXPECT_STREQ(dev.getSelectedGpu()->properties.deviceName, "llvmpipe (LLVM 21.1.8, 256 bits)");
    EXPECT_EQ(dev.getSelectedGpu()->properties.deviceType, VK_PHYSICAL_DEVICE_TYPE_CPU);
    EXPECT_EQ(dev.getSelectedGpu()->properties.vendorID, 0x10005u);
}

TEST(GpuDeviceTest, RestoresVulkanEnvVarsAfterProbing) {
    base::TestSystem sys("");
    base::TestTempDir temp_dir("gpu_device_test");

    auto loader = std::make_shared<FakeVulkanLoader>(std::vector<FakePhysicalDevice>{
        MakeFakePhysicalDevice("Apple M3 Max", 0x106b, 0x0001,
                               VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                               VK_MAKE_API_VERSION(0, 1, 3, 0), 1, 34359738368ULL, true),
    });

    char gpu_mode[] = "host";
    AndroidOptions opts{.gpu = gpu_mode};
    FakeEmulator emu(opts);
    emu.emulator_paths().launcher_directory = "/path/to/launcher";
    SetupFakeVulkanLibDir(temp_dir, emu);
    HardwareConfig hw;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

    base::System::SetEnvironmentVariable("VK_DRIVER_FILES", "/preexisting/driver_icd.json");
    base::System::SetEnvironmentVariable("VK_ICD_FILENAMES", "/preexisting/icd_filenames.json");
    base::System::SetEnvironmentVariable("VK_ADD_DRIVER_FILES", "/preexisting/add_driver.json");
    base::System::SetEnvironmentVariable("VK_LOADER_DRIVERS_DISABLE", "*preexisting*");

    GpuDevice::setVulkanLoader_TestOnly(loader);
    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    GpuDevice::setVulkanLoader_TestOnly(nullptr);

#if defined(__APPLE__)
    EXPECT_NE(loader->observed_vk_driver_files(), "/preexisting/driver_icd.json");
    EXPECT_FALSE(loader->observed_vk_driver_files().empty());
    EXPECT_EQ(loader->observed_vk_loader_drivers_disable(), "");
#else
    EXPECT_EQ(loader->observed_vk_driver_files(), "");
    EXPECT_EQ(loader->observed_vk_loader_drivers_disable(), "*lvp*,*swiftshader*");
#endif
    EXPECT_EQ(loader->observed_vk_icd_filenames(), "");
    EXPECT_EQ(loader->observed_vk_add_driver_files(), "");

    EXPECT_EQ(base::System::GetEnvironmentVariable("VK_DRIVER_FILES"),
              "/preexisting/driver_icd.json");
    EXPECT_EQ(base::System::GetEnvironmentVariable("VK_ICD_FILENAMES"),
              "/preexisting/icd_filenames.json");
    EXPECT_EQ(base::System::GetEnvironmentVariable("VK_ADD_DRIVER_FILES"),
              "/preexisting/add_driver.json");
    EXPECT_EQ(base::System::GetEnvironmentVariable("VK_LOADER_DRIVERS_DISABLE"), "*preexisting*");

    base::System::SetEnvironmentVariable("VK_DRIVER_FILES", "");
    base::System::SetEnvironmentVariable("VK_ICD_FILENAMES", "");
    base::System::SetEnvironmentVariable("VK_ADD_DRIVER_FILES", "");
    base::System::SetEnvironmentVariable("VK_LOADER_DRIVERS_DISABLE", "");
}

TEST(GpuDeviceTest, ForceGpuModeFeatureFlags) {
    using S = android_studio::EmulatorFeatureFlagState;
    base::TestSystem sys("");
    HardwareConfig hw;
    SnapshotDevice snapshot_dev;

    // ForceGpuHost overrides default auto (lavapipe) mode to host mode.
    {
        FakeEmulator emu;
        emu.feature_flags().SetEnabledOverride(S::FORCE_GPU_HOST, true);
        EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(emu.config()));
        EXPECT_THAT(dev.getQemuEnv(emu.config()),
                    testing::Contains(Pair("ANDROID_EMU_RENDERER", "host")));
    }

    // ForceLavapipe / ForceSwiftshader / ForceGpuSoftware override -gpu host to lavapipe.
    for (auto flag :
         {S::FORCE_GPU_SOFTWARE, S::FORCE_LAVAPIPE, S::FORCE_SWIFTSHADER, S::FORCE_ANGLE}) {
        char gpu_mode[] = "host";
        AndroidOptions opts{.gpu = gpu_mode};
        FakeEmulator emu(opts);
        emu.feature_flags().SetEnabledOverride(flag, true);
        EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));

        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(emu.config()));
        EXPECT_EQ(dev.getVulkanIcd(), "lavapipe");
        EXPECT_THAT(dev.getQemuEnv(emu.config()),
                    testing::Contains(Pair("ANDROID_EMU_RENDERER", "lavapipe")));
    }
}

TEST(GpuDeviceTest, RendererFeaturesRespectFeatureFlags) {
    using S = android_studio::EmulatorFeatureFlagState;
    base::TestSystem sys("");
    const fs::path avd_content = sys.GetTempRoot()->Path() / "avd";
    ASSERT_OK(base::file::mkdir_recursive(avd_content, 0755));

    HardwareConfig hw;
    hw.hw_lcd_width = 1080;
    hw.hw_lcd_height = 2400;

    // Default flags: GuestVulkanOnly, VulkanNativeSwapchain, VulkanVirtualQueue,
    // VulkanSnapshots, VulkanUseDedicatedAhbMemoryType enabled;
    // VulkanBatchedDescriptorSetUpdate disabled.
    {
        FakeEmulator emu;
        EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
        EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));

        SnapshotDevice snapshot_dev;
        EXPECT_OK(snapshot_dev.initialize(emu.config()));
        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(emu.config()));

        EXPECT_THAT(
                dev.getQemuParameters(emu.config()),
                ElementsAre(
                        Eq("-device"),
                        AllOf(HasSubstr("GuestVulkanOnly:enabled"),
                              HasSubstr("VulkanNativeSwapchain:enabled"),
                              HasSubstr("VulkanVirtualQueue:enabled"),
                              HasSubstr("VulkanSnapshots:enabled"),
                              HasSubstr("VulkanBatchedDescriptorSetUpdate:disabled"),
                              HasSubstr("VulkanUseDedicatedAhbMemoryType:enabled"),
                              testing::Not(HasSubstr("x-gfxstream-gles=on")))));
    }

    // Disabling GuestAngle, VulkanVirtualQueue, VulkanSnapshots, VulkanUseDedicatedAhbMemoryType
    // and enabling VulkanBatchedDescriptorSetUpdate + optional Vulkan debug/memory flags.
    {
        FakeEmulator emu;
        emu.feature_flags().SetEnabledOverride(S::GUEST_ANGLE, false);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_VIRTUAL_QUEUE, false);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_SNAPSHOTS, false);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_USE_DEDICATED_AHB_MEMORY_TYPE, false);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_BATCHED_DESCRIPTOR_SET_UPDATE, true);
        emu.feature_flags().SetEnabledOverride(S::BYPASS_VULKAN_DEVICE_FEATURE_OVERRIDES, true);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_ALLOCATE_DEVICE_MEMORY_ONLY, true);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_ALLOCATE_HOST_MEMORY, true);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_DEBUG_UTILS, true);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_COMMAND_BUFFER_CHECKPOINTS, true);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_ROBUSTNESS, true);
        emu.feature_flags().SetEnabledOverride(S::VULKAN_PROTECTED_MEMORY_EMULATION, true);
        EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
        EXPECT_CALL(emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));

        SnapshotDevice snapshot_dev;
        EXPECT_OK(snapshot_dev.initialize(emu.config()));
        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(emu.config()));

        EXPECT_THAT(
                dev.getQemuParameters(emu.config()),
                ElementsAre(
                        Eq("-device"),
                        AllOf(testing::Not(HasSubstr("GuestVulkanOnly:enabled")),
                              testing::Not(HasSubstr("VulkanNativeSwapchain:enabled")),
                              testing::Not(HasSubstr("VulkanVirtualQueue:enabled")),
                              testing::Not(HasSubstr("VulkanSnapshots:enabled")),
                              testing::Not(HasSubstr("VulkanUseDedicatedAhbMemoryType:enabled")),
                              HasSubstr("x-gfxstream-gles=on"),
                              HasSubstr("renderer_features=VulkanBatchedDescriptorSetUpdate:enabled;"),
                              HasSubstr("BypassVulkanDeviceFeatureOverrides:enabled"),
                              HasSubstr("VulkanAllocateDeviceMemoryOnly:enabled"),
                              HasSubstr("VulkanAllocateHostMemory:enabled"),
                              HasSubstr("VulkanDebugUtils:enabled"),
                              HasSubstr("VulkanCommandBufferCheckpoints:enabled"),
                              HasSubstr("VulkanRobustness:enabled"),
                              HasSubstr("VulkanProtectedMemoryEmulation:enabled"))));
    }

    // With skiagl, VulkanVirtualQueue is disabled by default unless explicitly overridden on;
    // with graphite, VulkanVirtualQueue is enabled with default feature flags.
    {
        char skiagl[] = "skiagl";
        FakeEmulator skiagl_off_emu(AndroidOptions{.systemui_renderer = skiagl});
        EXPECT_CALL(skiagl_off_emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
        EXPECT_CALL(skiagl_off_emu.mock_avd(), GetContentPath())
                .WillRepeatedly(Return(avd_content));

        SnapshotDevice snapshot_dev;
        EXPECT_OK(snapshot_dev.initialize(skiagl_off_emu.config()));
        GpuDevice dev("gpu0", snapshot_dev);
        EXPECT_OK(dev.initialize(skiagl_off_emu.config()));
        EXPECT_THAT(dev.getQemuParameters(skiagl_off_emu.config()),
                    ElementsAre(Eq("-device"), Not(HasSubstr("VulkanVirtualQueue:enabled"))));

        FakeEmulator skiagl_on_emu(AndroidOptions{.systemui_renderer = skiagl});
        skiagl_on_emu.feature_flags().SetEnabledOverride(S::VULKAN_VIRTUAL_QUEUE, true);
        EXPECT_CALL(skiagl_on_emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
        EXPECT_CALL(skiagl_on_emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));
        EXPECT_THAT(dev.getQemuParameters(skiagl_on_emu.config()),
                    ElementsAre(Eq("-device"), HasSubstr("VulkanVirtualQueue:enabled")));

        char graphite[] = "graphite";
        FakeEmulator graphite_emu(AndroidOptions{.systemui_renderer = graphite});
        EXPECT_CALL(graphite_emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
        EXPECT_CALL(graphite_emu.mock_avd(), GetContentPath()).WillRepeatedly(Return(avd_content));
        EXPECT_THAT(dev.getQemuParameters(graphite_emu.config()),
                    ElementsAre(Eq("-device"), HasSubstr("VulkanVirtualQueue:enabled")));
    }
}

TEST(GpuDeviceTest, DisabledBelowApi37) {
    base::TestSystem sys("");
    FakeEmulator emu;
    HardwareConfig hw;
    hw.hw_lcd_width = 1080;
    hw.hw_lcd_height = 2400;
    EXPECT_CALL(emu.mock_avd(), Hw()).WillRepeatedly(ReturnRef(hw));
    EXPECT_CALL(emu.mock_avd(), ApiLevel()).WillRepeatedly(Return(36));

    SnapshotDevice snapshot_dev;
    GpuDevice dev("gpu0", snapshot_dev);
    EXPECT_OK(dev.initialize(emu.config()));
    EXPECT_TRUE(dev.getQemuParameters(emu.config()).empty());
}

}  // namespace android::goldfish::test
