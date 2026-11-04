// Copyright 2024 The Android Open Source Project
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

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

#include "android/base/system.h"
#include "android/goldfish/avd.h"
#include "android/goldfish/constants.h"
#include "android/goldfish/hardware_config.h"
#include "android/status/status_macros.h"
#include "goldfish/file/file.h"
#include "goldfish/gvk/instance_dispatch.h"
#include "goldfish/gvk/system_meta_loader.h"
#include "goldfish/parsing/resizable_display_config.h"
#include "goldfish/snapshot/snapshot_store.h"
#include "virtio_gpu_selection.h"

namespace android::goldfish {
namespace {

using android::base::System;

// Where users report problems with the virtio-gpu device switch
// (b/576450291): the public Android Emulator issue tracker component.
constexpr std::string_view kVirtioGpuBugReportUrl =
        "https://issuetracker.google.com/issues/new?component=192727";

::goldfish::gvk::IMetaLoader::Ptr& VulkanLoaderOverride() {
    static absl::NoDestructor<::goldfish::gvk::IMetaLoader::Ptr> override_loader;
    return *override_loader;
}

struct ProbedGpuDevice {
    VulkanGpuDeviceInfo info{};
    bool has_graphics_queue_family = false;
};

struct GpuSelection {
    std::string chosen_gpu_mode;
    std::string emu_renderer;
    std::string vulkan_icd;
    std::vector<VulkanGpuDeviceInfo> host_gpus;
    std::optional<VulkanGpuDeviceInfo> selected_gpu;
};

class ScopedVulkanEnvGuard {
  public:
    ScopedVulkanEnvGuard() {
        for (size_t i = 0; i < kEnvVars.size(); ++i) {
            saved_os_values_[i] = System::GetEnvironmentVariable(kEnvVars[i]);
        }
    }

    ~ScopedVulkanEnvGuard() {
        for (size_t i = 0; i < kEnvVars.size(); ++i) {
            System::SetEnvironmentVariable(kEnvVars[i], saved_os_values_[i]);
        }
    }

    ScopedVulkanEnvGuard(const ScopedVulkanEnvGuard&) = delete;
    ScopedVulkanEnvGuard& operator=(const ScopedVulkanEnvGuard&) = delete;

  private:
    static constexpr std::array<std::string_view, 4> kEnvVars = {
        "VK_DRIVER_FILES",
        "VK_ICD_FILENAMES",
        "VK_ADD_DRIVER_FILES",
        "VK_LOADER_DRIVERS_DISABLE",
    };
    std::array<std::string, 4> saved_os_values_;
};

std::string_view getIcdJsonFilename(std::string_view vk_icd) {
    if (vk_icd == "kosmickrisp") {
        return "libkosmickrisp_icd.json";
    }
    if (vk_icd == "moltenvk") {
        return "MoltenVK_icd.json";
    }
    if (vk_icd == "lavapipe") {
        return "lvp_icd.json";
    }
    if (vk_icd == "swiftshader") {
        return "vk_swiftshader_icd.json";
    }
    return "";
}

std::vector<ProbedGpuDevice> probeVulkanGpuDevices(const fs::path& vulkan_libdir,
                                                   std::string_view vk_icd) {
    ScopedVulkanEnvGuard env_guard;

    System::SetEnvironmentVariable("VK_ICD_FILENAMES", "");
    System::SetEnvironmentVariable("VK_ADD_DRIVER_FILES", "");

    if (vk_icd.empty()) {
        System::SetEnvironmentVariable("VK_DRIVER_FILES", "");
        // Disable CPU software rasterizers (Lavapipe and SwiftShader) when
        // probing for physical host GPUs:
        // 1. We only want physical/virtual GPU devices in host_gpus, not CPU
        //    software fallback drivers.
        // 2. Loading Lavapipe (e.g. via a system /usr/share/vulkan/icd.d/lvp_icd.json
        //    manifest on Linux) pulls in and initializes libLLVM.so (~75 MB)
        //    during extension enumeration and instance creation, adding seconds
        //    of startup latency and risking netsimd startup inactivity timeouts
        //    on slow or cold-cache hosts.
        System::SetEnvironmentVariable("VK_LOADER_DRIVERS_DISABLE", "*lvp*,*swiftshader*");
    } else {
        System::SetEnvironmentVariable("VK_LOADER_DRIVERS_DISABLE", "");
        const std::string_view icd_filename = getIcdJsonFilename(vk_icd);
        if (icd_filename.empty()) {
            LOG(WARNING) << "Unknown Vulkan ICD for GPU probe: " << vk_icd;
            return {};
        }
        const fs::path icd_path = vulkan_libdir / icd_filename;
        if (!android::base::file::is_file(icd_path)) {
            LOG(WARNING) << "Vulkan ICD file not found: " << icd_path.string();
            return {};
        }
        const auto abs_icd_path = android::base::file::make_absolute(icd_path);
        if (!abs_icd_path.ok()) {
            LOG(WARNING) << "Failed to resolve Vulkan ICD path: " << abs_icd_path.status();
            return {};
        }
        System::SetEnvironmentVariable("VK_DRIVER_FILES", abs_icd_path->string());
    }

    const auto& override_loader = VulkanLoaderOverride();
    const auto loader =
            override_loader
                    ? override_loader
                    : ::goldfish::gvk::SystemMetaLoader::get(vulkan_libdir.string().c_str());
    if (!loader) {
        LOG(WARNING) << "Failed to get Vulkan system meta loader from " << vulkan_libdir.string();
        return {};
    }

    const uint32_t instance_version = loader->enumerateInstanceVersion();
    if (instance_version < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
        LOG(WARNING) << absl::StrFormat(
                "Vulkan instance version too low: %d.%d.%d (expected >= 1.1.0)",
                VK_API_VERSION_MAJOR(instance_version), VK_API_VERSION_MINOR(instance_version),
                VK_API_VERSION_PATCH(instance_version));
        return {};
    }

    const VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "gpu_device_probe",
        .applicationVersion = VK_MAKE_API_VERSION(0, 1, 1, 0),
        .pEngineName = "android_emulator",
        .engineVersion = VK_MAKE_API_VERSION(0, 1, 1, 0),
        .apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0),
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };

#if defined(__APPLE__)
    // Portability drivers on macOS (such as MoltenVK) require
    // VK_KHR_portability_enumeration to be enumerated. Skip querying instance
    // extension properties on other platforms because each call to
    // vkEnumerateInstanceExtensionProperties(nullptr, ...) when no VkInstance
    // exists causes the Vulkan loader to scan, dlopen, and dlclose every
    // system ICD before vkCreateInstance loads them again.
    // Note: VK_KHR_portability_enumeration is built into the Khronos Vulkan
    // Loader starting from Vulkan 1.3.216, so in the future
    // enumerateInstanceExtensionProperties(nullptr) can also be skipped on
    // macOS when the Vulkan loader version is guaranteed to be >= 1.3.216.
    std::vector<const char*> enabled_extensions;
    const std::vector<VkExtensionProperties> instance_exts =
            loader->enumerateInstanceExtensionProperties(nullptr);
    for (const auto& ext : instance_exts) {
        if (std::strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            enabled_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            break;
        }
    }
    create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
    create_info.ppEnabledExtensionNames = enabled_extensions.data();
#endif

    const auto instance_dispatch = ::goldfish::gvk::InstanceDispatch::create(loader, create_info);
    if (!instance_dispatch) {
        return {};
    }

    uint32_t dev_count = 0;
    VkResult result = instance_dispatch->enumeratePhysicalDevices(&dev_count, nullptr);
    if (result != VK_SUCCESS) {
        LOG(WARNING) << "`enumeratePhysicalDevices` failed with " << result;
        return {};
    }
    if (dev_count == 0) {
        return {};
    }

    std::vector<VkPhysicalDevice> devs(dev_count);
    result = instance_dispatch->enumeratePhysicalDevices(&dev_count, devs.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        LOG(WARNING) << "`enumeratePhysicalDevices` failed with " << result;
        return {};
    }
    devs.resize(dev_count);

    std::vector<ProbedGpuDevice> probed_devices;
    probed_devices.reserve(dev_count);
    for (const VkPhysicalDevice dev : devs) {
        ProbedGpuDevice probed;
        probed.info.properties = instance_dispatch->getPhysicalDeviceProperties(dev);

        const VkPhysicalDeviceMemoryProperties mem_props =
                instance_dispatch->getPhysicalDeviceMemoryProperties(dev);
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
            if ((mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                probed.info.device_local_memory_bytes += mem_props.memoryHeaps[i].size;
            }
        }

        const std::vector<VkQueueFamilyProperties> queue_props =
                instance_dispatch->getPhysicalDeviceQueueFamilyProperties(dev);
        for (const auto& queue_family : queue_props) {
            if (queue_family.queueCount > 0 &&
                (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                probed.has_graphics_queue_family = true;
                break;
            }
        }

        probed_devices.push_back(probed);
    }
    return probed_devices;
}

// Keep the GPU selection algorithm in sync with VkEmulation::getSelectedGpuIndex
// in gfxstream (vk_common_operations.cpp) so the reported active GPU matches
// the device selected by gfxstream at runtime.
std::optional<size_t> selectVulkanGpuDeviceIndex(std::span<const ProbedGpuDevice> devices) {
    if (devices.empty()) {
        return std::nullopt;
    }

    // ANDROID_EMU_VK_SELECT_GPU is ignored when there is only one physical device,
    // matching VkEmulation::getSelectedGpuIndex in vk_common_operations.cpp.
    if (devices.size() == 1) {
        return devices[0].has_graphics_queue_family ? std::optional<size_t>(0) : std::nullopt;
    }

    const std::string enforced_gpu = System::Get()->EnvGet("ANDROID_EMU_VK_SELECT_GPU");
    if (!enforced_gpu.empty()) {
        std::optional<size_t> enforced_index;
        if (size_t parsed_index = 0; absl::SimpleAtoi(enforced_gpu, &parsed_index)) {
            if (parsed_index < devices.size()) {
                enforced_index = parsed_index;
            }
        } else {
            const std::string lowered_target = absl::AsciiStrToLower(enforced_gpu);
            for (size_t i = 0; i < devices.size(); ++i) {
                const auto& props = devices[i].info.properties;
                const std::string_view raw_name(
                        props.deviceName, strnlen(props.deviceName, sizeof(props.deviceName)));
                if (absl::StrContains(absl::AsciiStrToLower(raw_name), lowered_target)) {
                    enforced_index = i;
                }
            }
        }

        if (enforced_index.has_value() && devices[*enforced_index].has_graphics_queue_family) {
            return enforced_index;
        }
        LOG(WARNING) << "Could not select GPU with ANDROID_EMU_VK_SELECT_GPU=" << enforced_gpu;
    }

    auto score_device = [](const ProbedGpuDevice& device) -> uint32_t {
        if (!device.has_graphics_queue_family) {
            return 0;
        }

        constexpr std::array<uint32_t, 5> kDeviceTypeScoreTable = {
            100,   // VK_PHYSICAL_DEVICE_TYPE_OTHER = 0
            1000,  // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1
            2000,  // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2
            500,   // VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3
            600,   // VK_PHYSICAL_DEVICE_TYPE_CPU = 4
        };

        uint32_t score = 0;
        const uint32_t device_type = static_cast<uint32_t>(device.info.properties.deviceType);
        if (device_type < kDeviceTypeScoreTable.size()) {
            score += kDeviceTypeScoreTable[device_type];
        }

        const uint32_t major = VK_API_VERSION_MAJOR(device.info.properties.apiVersion);
        const uint32_t minor = VK_API_VERSION_MINOR(device.info.properties.apiVersion);
        const uint32_t patch = VK_API_VERSION_PATCH(device.info.properties.apiVersion);
        score += major * 5000 + std::min(minor, 10u) * 500 + std::min(patch, 400u);
        return score;
    };

    uint32_t max_score = 0;
    std::optional<size_t> selected_index;
    for (size_t i = 0; i < devices.size(); ++i) {
        const uint32_t score = score_device(devices[i]);
        if (score > max_score) {
            max_score = score;
            selected_index = i;
        }
    }
    return selected_index;
}

bool isHostGpuDenyListed(std::span<const ProbedGpuDevice> host_gpus,
                         std::optional<size_t> selected_gpu_index) {
    // TODO(b/520445586) Check server side gpu list and driver capabilities using |host_gpus| and
    // |selected_gpu_index| to determine if host is denylisted for GPU. Using software rendering by
    // default for now.
    (void)host_gpus;
    (void)selected_gpu_index;
    return true;
}

GpuSelection selectGpu(const EmulatorConfig& emulator) {
    const auto& o = emulator.opts();
    const auto& a = emulator.avd();

#if defined(__APPLE__)
    // Use KosmicKrisp as the default Vulkan ICD on macOS, allow changing to MoltenVK with an
    // environment variable
    const std::string host_vulkan_icd =
            (System::Get()->EnvGet("ANDROID_EMU_VK_SELECT_ICD") == "moltenvk") ? "moltenvk"
                                                                               : "kosmickrisp";
#else
    // Use system defaults
    const std::string host_vulkan_icd = "";
#endif

    fs::path vulkan_libdir;
    if (!emulator.emulator_paths().lib64_directory.empty()) {
        const fs::path candidate = emulator.emulator_paths().lib64_directory / "vulkan";
        if (android::base::file::is_dir(candidate)) {
            vulkan_libdir = candidate;
        }
    }

    std::vector<ProbedGpuDevice> probed_gpus;
    if (!vulkan_libdir.empty()) {
        probed_gpus = probeVulkanGpuDevices(vulkan_libdir, host_vulkan_icd);
    }
    const std::optional<size_t> selected_gpu_index = selectVulkanGpuDeviceIndex(probed_gpus);

    std::vector<VulkanGpuDeviceInfo> host_gpus;
    host_gpus.reserve(probed_gpus.size());
    for (const auto& dev : probed_gpus) {
        host_gpus.push_back(dev.info);
    }

    // Check command line and AVD options, use host by default
    const std::string user_gpu_mode = o.gpu ? std::string(o.gpu) : "";
    const std::string avd_gpu_mode = a.Hw().hw_gpu_mode;
    std::string chosen_gpu_mode =
            (!user_gpu_mode.empty() ? user_gpu_mode
                                    : (!avd_gpu_mode.empty() ? avd_gpu_mode : "auto"));

    using S = android_studio::EmulatorFeatureFlagState;
    const auto& ff = emulator.feature_flags();
    if (ff.IsEnabled(S::FORCE_GPU_HOST)) {
        chosen_gpu_mode = "host";
    } else if (ff.IsEnabled(S::FORCE_GPU_SOFTWARE) || ff.IsEnabled(S::FORCE_LAVAPIPE) ||
               ff.IsEnabled(S::FORCE_SWIFTSHADER) || ff.IsEnabled(S::FORCE_ANGLE)) {
        chosen_gpu_mode = "lavapipe";
    }

    bool use_host_gpu =
            (chosen_gpu_mode == "host") ||
            (chosen_gpu_mode == "auto" && !isHostGpuDenyListed(probed_gpus, selected_gpu_index));
    if (use_host_gpu && a.ApiLevel() < kVirtioReadyApiLevel) {
        // The goldfish_pipe (API < 37) path cannot drive the host GPU yet; every
        // software mode on this emulator resolves to lavapipe (SwiftShader is
        // being removed, b/570473107), so fall back to it instead of silently
        // picking a different renderer than the one that was asked for.
        LOG(WARNING) << "Host GPU rendering is not available below API " << kVirtioReadyApiLevel
                     << " yet (this AVD is API " << a.ApiLevel() << "); requested GPU mode '"
                     << chosen_gpu_mode << "' falls back to lavapipe.";
        use_host_gpu = false;
    }

    std::string chosen_emu_renderer;
    std::string chosen_vulkan_icd;
    std::optional<VulkanGpuDeviceInfo> selected_gpu;
    if (use_host_gpu) {
        chosen_emu_renderer = "host";
        chosen_vulkan_icd = host_vulkan_icd;
        if (selected_gpu_index.has_value()) {
            selected_gpu = host_gpus[*selected_gpu_index];
        }
    } else {
        // Use lavapipe for software or invalid modes
        chosen_emu_renderer = "lavapipe";
        chosen_vulkan_icd = "lavapipe";
        if (!vulkan_libdir.empty()) {
            const std::vector<ProbedGpuDevice> software_gpus =
                    probeVulkanGpuDevices(vulkan_libdir, chosen_vulkan_icd);
            if (const std::optional<size_t> software_gpu_index =
                        selectVulkanGpuDeviceIndex(software_gpus);
                software_gpu_index.has_value()) {
                selected_gpu = software_gpus[*software_gpu_index].info;
            }
        }
    }

    return {
        .chosen_gpu_mode = chosen_gpu_mode,
        .emu_renderer = chosen_emu_renderer,
        .vulkan_icd = chosen_vulkan_icd,
        .host_gpus = std::move(host_gpus),
        .selected_gpu = std::move(selected_gpu),
    };
}

}  // namespace

// static
void GpuDevice::setVulkanLoader_TestOnly(::goldfish::gvk::IMetaLoader::Ptr loader) {
    VulkanLoaderOverride() = std::move(loader);
}

absl::Status GpuDevice::initialize(const EmulatorConfig& emulator) {
    // -virtio-gpu-device: validated here so that a typo is reported at startup.
    ASSIGN_OR_RETURN(mVirtioGpuDevice, SelectedVirtioGpuDevice(emulator.opts()));
    LOG(INFO) << "Using virtio-gpu device: " << VirtioGpuDeviceName(mVirtioGpuDevice);
    if (mVirtioGpuDevice == VirtioGpuDevice::kRutabaga) {
        LOG(WARNING) << "virtio-gpu-rutabaga is deprecated and will be removed in a future "
                        "release. Please switch to virtio-gpu-gfxstream (the default: drop "
                        "'-virtio-gpu-device rutabaga'); if it does not work for you, please "
                        "report the problem at "
                     << kVirtioGpuBugReportUrl << " so it can be fixed before the removal.";
    } else if (!emulator.opts().virtio_gpu_device &&
               emulator.avd().ApiLevel() >= kVirtioReadyApiLevel) {
        // Only when the user did not choose on a virtio-gpu AVD: an explicit
        // -virtio-gpu-device gfxstream (or a legacy goldfish_pipe AVD) needs no
        // nudge on every launch.
        LOG(WARNING) << "virtio-gpu-gfxstream is now used by default. If you hit a problem, "
                        "please report it at "
                     << kVirtioGpuBugReportUrl
                     << "; until it is fixed, '-virtio-gpu-device rutabaga' selects the "
                        "deprecated virtio-gpu-rutabaga device.";
    }

    auto selection = selectGpu(emulator);
    LOG(INFO) << "Selected GPU mode: " << selection.chosen_gpu_mode;
    LOG(INFO) << "Using Vulkan ICD: "
              << (selection.vulkan_icd.empty() ? "System Default" : selection.vulkan_icd);
    mEmuRenderer = std::move(selection.emu_renderer);
    mVulkanIcd = std::move(selection.vulkan_icd);
    mHostGpus = std::move(selection.host_gpus);
    mSelectedGpu = std::move(selection.selected_gpu);
    return absl::OkStatus();
}

absl::flat_hash_map<std::string, std::string> GpuDevice::getQemuEnv(
        const EmulatorConfig& emulator) const {
    // Note: Gfxstream also uses ANDROID_EMULATOR_LIBRARY_DIR to locate GL and
    // Vulkan libraries, which is set in PluginInit::getQemuEnv.
    absl::flat_hash_map<std::string, std::string> env = {
        {"ANDROID_EMU_RENDERER", mEmuRenderer},
        {"ANDROID_EMU_VK_ICD", mVulkanIcd},
        {"ANDROID_EGL_ON_EGL", "1"},
        {"ANGLE_DEFAULT_PLATFORM", "vulkan"},
        {"ANDROID_EMULATOR_LAUNCHER_DIR", emulator.emulator_paths().launcher_directory.string()},
    };

    if (std::string select_gpu = System::Get()->EnvGet("ANDROID_EMU_VK_SELECT_GPU");
        !select_gpu.empty()) {
        env["ANDROID_EMU_VK_SELECT_GPU"] = std::move(select_gpu);
    }

#ifdef __linux__
    // The bundled Mesa libGLESv2.so/libEGL.so (used when the renderer needs
    // GLES, e.g. API < 37 guests) pull in lib64/libgallium_dri.so, which has a
    // ~4.4 KiB initial-exec TLS segment. glibc only reserves 512 bytes of
    // static TLS for dlopen()ed libraries, so the dlopen fails with "cannot
    // allocate memory in static TLS block" and gfxstream falls back to the
    // host's libGLESv2.so.2 (absent on minimal machines: crash). Reserve more.
    constexpr std::string_view kStaticTlsTunable = "glibc.rtld.optional_static_tls=16384";
    if (std::string tunables = System::Get()->EnvGet("GLIBC_TUNABLES"); tunables.empty()) {
        env["GLIBC_TUNABLES"] = std::string(kStaticTlsTunable);
    } else if (!absl::StrContains(tunables, "glibc.rtld.optional_static_tls")) {
        env["GLIBC_TUNABLES"] = absl::StrCat(tunables, ":", kStaticTlsTunable);
    } else {
        env["GLIBC_TUNABLES"] = std::move(tunables);
    }

    // Bug: 417138854: work around the log spam "bad fde: FDE is really a CIE"
    // when using the host system's libvulkan_lvp.so.
    if (System::Get()->EnvGet("ANDROID_EMU_PRELOAD_LIBGCC") == "1") {
        std::string current_preload = System::Get()->EnvGet("LD_PRELOAD");
        std::string libgcc_path = "/lib/x86_64-linux-gnu/libgcc_s.so.1";
        if (current_preload.empty()) {
            env["LD_PRELOAD"] = std::move(libgcc_path);
        } else {
            env["LD_PRELOAD"] = absl::StrCat(libgcc_path, ":", current_preload);
        }
    }
#endif

    // TODO: Decide which of these extra graphics / Gfxstream / VirtualScene /
    // VideoBridge environment variables should be passed through or replaced
    // with explicit parameters:
    //
    // constexpr std::string_view kPassthroughGpuVars[] = {
    //     // Linux display server / WSI
    //     "DISPLAY",
    //     "WAYLAND_DISPLAY",
    //     "XAUTHORITY",
    //
    //     // Vulkan Loader & Layers
    //     "VK_ICD_FILENAMES",
    //     "VK_DRIVER_FILES",
    //     "VK_ADD_LAYER_PATH",
    //     "VK_LAYER_PATH",
    //     "VK_INSTANCE_LAYERS",
    //     "VK_LOADER_LAYERS_DISABLE",
    //     "VK_LOADER_DEBUG",
    //     "ANDROID_EMU_VK_LOADER_PATH",
    //     "ANDROID_EMU_VK_NO_CLEANUP",
    //     "ANDROID_EMU_VK_DISABLE_DEFERRED_COMMANDS",
    //
    //     // Vulkan Validation Layers & RenderDoc (gfxstream)
    //     "ANDROID_EMU_VVL_BEHAVIOR",
    //     "ANDROID_EMU_VVL_INCLUDE_FILTER",
    //     "ANDROID_EMU_VVL_EXCLUDE_FILTER",
    //     "GFXSTREAM_USE_TESTING_VALIDATION_LAYERS",
    //     "ANDROID_EMU_RENDERDOC",
    //     "ANDROID_EMU_RENDERDOC_LIBRARY_PATH",
    //     "ANDROID_EMU_RENDERDOC_CAPTURE_PATH_TEMPLATE",
    //
    //     // Gfxstream Logging / Debug / Windowing
    //     "GFXSTREAM_LOG_LEVEL",
    //     "GFXSTREAM_LOG_VERBOSE",
    //     "ANDROID_EMUGL_VERBOSE",
    //     "ANDROID_EMU_VK_LOG_CALLS",
    //     "ANDROID_EMU_VIRTIO_DEBUG",
    //     "ANDROID_EMUGL_RENDERTHREAD_STATS",
    //     "SHOW_FPS_STATS",
    //     "ANDROID_GFXSTREAM_EGL",
    //     "ANDROID_GFXSTREAM_CAPTURE_VK_SNAPSHOT",
    //     "ANDROID_EMUGL_EXPERIMENTAL_FAST_PATH",
    //     "ANDROID_EMU_HEADLESS",
    //     "ANDROID_EMUGL_EGL_VALIDATION",
    //     "ANDROID_EMU_USE_METAL_BACKED_VIEWS",
    //
    //     // VirtualScene (ver) & VideoBridge (videobridge)
    //     "VER_RENDERER_BACKEND",
    //     "ANDROID_EMU_ENABLE_VIDEO360",
    //     "ANDROID_EMU_ENABLE_STREETVIEW",
    //     "ANDROID_EMU_STREETVIEW_IMAGE_PATH",
    //     "ANDROID_EMU_MAPS_KEY",
    //     "ANDROID_EMU_SAVE_STREETVIEW_IMAGES",
    //     "ANDROID_EMU_VIDEO_ENCODER",
    //     "NVENC_LIB_PATH",
    //     "CUDA_LIB_PATH",
    //     "LIBVA_PATH",
    //     "LIBVA_DRIVER_NAME",
    // };
    // auto* sys = System::Get();
    // for (std::string_view var : kPassthroughGpuVars) {
    //     if (std::string val = sys->EnvGet(var); !val.empty()) {
    //         env.emplace(var, std::move(val));
    //     }
    // }

    return env;
}

std::vector<std::string> GpuDevice::getQemuParameters(const EmulatorConfig& emulator) const {
    using S = android_studio::EmulatorFeatureFlagState;
    const auto& hw = emulator.avd().Hw();
    const AndroidOptions& opts = emulator.opts();
    const auto& ff = emulator.feature_flags();
    const bool legacy_goldfish_gpu = emulator.avd().ApiLevel() < kVirtioReadyApiLevel;

    std::vector<std::string> renderer_features;
    if (opts.renderer_features && *opts.renderer_features != '\0') {
        renderer_features.emplace_back(opts.renderer_features);
    }

    bool needs_gles = true;
    if (!legacy_goldfish_gpu && !opts.no_guest_angle && ff.IsEnabled(S::GUEST_ANGLE)) {
        renderer_features.push_back("GuestVulkanOnly:enabled");

        // VulkanNativeSwapchain composition should only be enabled with GuestVulkanOnly
        if (!opts.no_vulkan_composition) {
            renderer_features.push_back("VulkanNativeSwapchain:enabled");
            needs_gles = false;
        }
    }

    // Use skiavkthreaded by default on API 37+, skiaglthreaded on legacy API < 37
    std::string systemui_renderer =
            opts.systemui_renderer ? opts.systemui_renderer
                                   : (legacy_goldfish_gpu ? "skiaglthreaded" : "skiavkthreaded");

    // Enable virtual queue for non-skiagl renderers, or when explicitly requested for skiagl.
    if (!legacy_goldfish_gpu && ff.IsEnabled(S::VULKAN_VIRTUAL_QUEUE) &&
        (!systemui_renderer.starts_with("skiagl") || ff.IsOverridden(S::VULKAN_VIRTUAL_QUEUE))) {
        renderer_features.push_back("VulkanVirtualQueue:enabled");
    }

    // Snapshots can also be taken at runtime (gRPC, console), not only at exit.
    // On legacy GL-only (API <= 36), Vulkan is disabled so VkDecoderGlobalState
    // is never initialized; enabling VulkanSnapshots would abort in
    // FrameBuffer::onSave/onLoad when calling VkDecoderGlobalState::get().
    if (!legacy_goldfish_gpu && mSnapshotDevice.snapshots_enabled() &&
        ff.IsEnabled(S::VULKAN_SNAPSHOTS)) {
        renderer_features.push_back("VulkanSnapshots:enabled");
    }

    renderer_features.push_back(ff.IsEnabled(S::VULKAN_BATCHED_DESCRIPTOR_SET_UPDATE)
                                        ? "VulkanBatchedDescriptorSetUpdate:enabled"
                                        : "VulkanBatchedDescriptorSetUpdate:disabled");

    if (legacy_goldfish_gpu) {
        // API <= 36 (goldfish_pipe) only supports GL for now.
        renderer_features.push_back("Vulkan:disabled");
        renderer_features.push_back("GlAsyncSwap:enabled");
        renderer_features.push_back("GlDirectMem:enabled");
        renderer_features.push_back("HasSharedSlotsHostMemoryAllocator:enabled");
        renderer_features.push_back("RefCountPipe:enabled");
        renderer_features.push_back("YuvCache:enabled");
        renderer_features.push_back("HostComposition:disabled");
    } else if (ff.IsEnabled(S::VULKAN_USE_DEDICATED_AHB_MEMORY_TYPE)) {
        // Use a separate memory type for Ahb allocations, to avoid marking them
        // as host visible and coherent on lavapipe, which is not very well supported
        // in virtio-gpu path right now.
        renderer_features.push_back("VulkanUseDedicatedAhbMemoryType:enabled");
    }

    constexpr std::pair<EmulatorFeatureFlag, std::string_view> kOptionalRendererFeatures[] = {
        {S::BYPASS_VULKAN_DEVICE_FEATURE_OVERRIDES, "BypassVulkanDeviceFeatureOverrides"},
        {S::VULKAN_ALLOCATE_DEVICE_MEMORY_ONLY, "VulkanAllocateDeviceMemoryOnly"},
        {S::VULKAN_ALLOCATE_HOST_MEMORY, "VulkanAllocateHostMemory"},
        {S::VULKAN_DEBUG_UTILS, "VulkanDebugUtils"},
        {S::VULKAN_COMMAND_BUFFER_CHECKPOINTS, "VulkanCommandBufferCheckpoints"},
        {S::VULKAN_ROBUSTNESS, "VulkanRobustness"},
        {S::VULKAN_PROTECTED_MEMORY_EMULATION, "VulkanProtectedMemoryEmulation"},
    };
    for (const auto& [flag, name] : kOptionalRendererFeatures) {
        if (ff.IsEnabled(flag)) {
            renderer_features.push_back(absl::StrCat(name, ":enabled"));
        }
    }

    // Limit available GPU memory and hostmem to 4GB
    renderer_features.push_back("VulkanMaxSafeHeapSize:4294967296");

    // Temporarily limit guest to Vulkan 1.3, unless 1.4 is explicitly enabled via an env variable.
    std::string env_vk_enable_1_4 = System::Get()->EnvGet("ANDROID_EMU_VK_ENABLE_1_4");
    if (env_vk_enable_1_4.empty() || env_vk_enable_1_4[0] == '0') {
        renderer_features.push_back("GuestVulkanMaxApiVersion:1.3.0");
    }

    if (opts.vulkan_validation) {
        renderer_features.push_back(absl::StrCat("VulkanValidation:", opts.vulkan_validation));
    }
    if (opts.vulkan_validation_include_filter) {
        renderer_features.push_back(absl::StrCat("VulkanValidationIncludeFilter:",
                                                 opts.vulkan_validation_include_filter));
    }
    if (opts.vulkan_validation_exclude_filter) {
        renderer_features.push_back(absl::StrCat("VulkanValidationExcludeFilter:",
                                                 opts.vulkan_validation_exclude_filter));
    }

    size_t width;
    size_t height;
    if (hw.hw_resizable_configs.empty()) {
        width = hw.hw_lcd_width;
        height = hw.hw_lcd_height;
    } else {
        const auto resizable_configs =
                ::goldfish::parsing::ParseResizableDisplayConfig(hw.hw_resizable_configs);

        if (!resizable_configs || resizable_configs->empty()) {
            LOG(ERROR) << "Failed to parse hw_resizable_configs; GPU will be disabled. "
                          "config='"
                       << hw.hw_resizable_configs << "'";
            return {};
        }

        const auto& default_config = resizable_configs->front();
        width = default_config.width;
        height = default_config.height;
    }

    if (!width || !height) {
        LOG(ERROR) << "Invalid display dimensions; GPU will be disabled. "
                      "width="
                   << width << " height=" << height;
        return {};
    }

    const std::string joined_features = absl::StrJoin(renderer_features, ";");

    // The GPU device keeps the renderer state of each snapshot under
    // <snapshot_directory>/<snapshot name>/renderer/, next to the version.txt
    // that SnapshotStore writes to <snapshot_directory>/<snapshot name>/.
    std::optional<fs::path> snapshot_directory;
    if (mSnapshotDevice.snapshots_enabled()) {
        snapshot_directory =
                ::goldfish::snapshot::FileSnapshotStore(emulator.avd().GetContentPath(),
                                                        SnapshotVersionFor(mVirtioGpuDevice))
                        .Root() /
                "";
    }

    if (legacy_goldfish_gpu) {
        std::vector<std::string> args;
        const auto global = [&args](std::string_view property, std::string_view value) {
            args.push_back("-global");
            args.push_back(absl::StrCat("driver=goldfish_pipe,property=", property,
                                        ",value=", EscapeQemuOptionValue(value)));
        };
        global("xres", absl::StrCat(width));
        global("yres", absl::StrCat(height));
        // The gfxstream RenderLib needs the guest's API level (setGuestAndroidApiLevel);
        // goldfish_pipe refuses to start graphics without it.
        global("guest_api_level", absl::StrCat(emulator.avd().ApiLevel()));
        global("renderer_features", joined_features);
        if (snapshot_directory) {
            global("snapshot_directory", snapshot_directory->string());
        }
        return args;
    }

    std::string gfxstream_backends = "gfxstream-vulkan=on";
    if (needs_gles) {
        gfxstream_backends.append(",x-gfxstream-gles=on");
    }

    // Exactly one of virtio-gpu-rutabaga (QEMU, via librutabaga_ffi) or
    // virtio-gpu-gfxstream (goldfish plugin, via libgfxstream_backend); both
    // take the same properties. See virtio_gpu_selection.h.
    std::vector<std::string> params = {std::string(VirtioGpuDeviceName(mVirtioGpuDevice))};
    if (hw.hw_sensor_hinge) {
        params.push_back("edid=off");
        params.push_back("max_outputs=2");
    }
    if (opts.no_window) {
        params.push_back("wsi=headless");
    }

    params.push_back(absl::StrCat("id=", id()));
    params.push_back("hostmem=4096M");
    params.push_back(gfxstream_backends);
    params.push_back("x-gfxstream-composer=on");
    params.push_back(absl::StrCat("renderer_features=", EscapeQemuOptionValue(joined_features)));
    if (snapshot_directory) {
        params.push_back(absl::StrCat("snapshot_directory=",
                                      EscapeQemuOptionValue(snapshot_directory->string())));
    }

    params.push_back(absl::StrCat("xres=", width));
    params.push_back(absl::StrCat("yres=", height));

    return {"-device", absl::StrJoin(params, ",")};
}

}  // namespace android::goldfish
