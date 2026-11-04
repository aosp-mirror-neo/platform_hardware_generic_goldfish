#include "host_info.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"

#include "android/base/system.h"
#include "android/cpu/cpu_accelerator.h"
#include "android/cpu/cpu_brand.h"
#include "android/goldfish/avd.h"
#include "android/goldfish/hardware_config.h"
#include "goldfish/file/file.h"
#include "goldfish/metrics/studio_config.h"
#include "vulkan/vulkan.h"

namespace android::goldfish {

namespace {

template <typename T>
concept HasEmulatorDeviceName = requires(T t) {
    typename T::EmulatorDeviceName;
    t.set_device_name(T::UNKNOWN_EMULATOR_DEVICE_NAME);
};

template <typename T>
void SetDeviceNameIfSupported(T& avd_info, const std::string& hw_device_name) {
    if constexpr (HasEmulatorDeviceName<T>) {
        using DeviceNameEnum = typename T::EmulatorDeviceName;
        static const auto device_name_map = absl::flat_hash_map<std::string_view, DeviceNameEnum>({
            {"resizable", T::RESIZABLE},
            {"7.6in Foldable", T::FOLDABLE_7_6_IN},
            {"foldable_4_3", T::FOLDABLE_4_3},
            {"foldable_1_1", T::FOLDABLE_1_1},
            {"small_phone", T::SMALL_PHONE},
            {"medium_phone", T::MEDIUM_PHONE},
            {"medium_tablet", T::MEDIUM_TABLET},
            {"pixel_c", T::PIXEL_C},
            {"pixel", T::PIXEL},
            {"pixel_xl", T::PIXEL_XL},
            {"pixel_2", T::PIXEL_2},
            {"pixel_2_xl", T::PIXEL_2_XL},
            {"pixel_3", T::PIXEL_3},
            {"pixel_3_xl", T::PIXEL_3_XL},
            {"pixel_3a", T::PIXEL_3A},
            {"pixel_3a_xl", T::PIXEL_3A_XL},
            {"pixel_4", T::PIXEL_4},
            {"pixel_4_xl", T::PIXEL_4_XL},
            {"pixel_4a", T::PIXEL_4A},
            {"pixel_5", T::PIXEL_5},
            {"pixel_6", T::PIXEL_6},
            {"pixel_6_pro", T::PIXEL_6_PRO},
            {"pixel_6a", T::PIXEL_6A},
            {"pixel_7_pro", T::PIXEL_7_PRO},
            {"pixel_7", T::PIXEL_7},
            {"pixel_7a", T::PIXEL_7A},
            {"pixel_8_pro", T::PIXEL_8_PRO},
            {"pixel_8", T::PIXEL_8},
            {"pixel_8a", T::PIXEL_8A},
            {"pixel_9", T::PIXEL_9},
            {"pixel_9a", T::PIXEL_9A},
            {"pixel_9_pro", T::PIXEL_9_PRO},
            {"pixel_9_pro_xl", T::PIXEL_9_PRO_XL},
            {"pixel_9_pro_fold", T::PIXEL_9_PRO_FOLD},
            {"pixel_10", T::PIXEL_10},
            {"pixel_10_pro", T::PIXEL_10_PRO},
            {"pixel_10_pro_xl", T::PIXEL_10_PRO_XL},
            {"pixel_10_pro_fold", T::PIXEL_10_PRO_FOLD},
            {"pixel_10a", T::PIXEL_10A},
            {"pixel_11_pro_fold", T::PIXEL_11_PRO_FOLD},
            {"pixel_fold", T::PIXEL_FOLD},
            {"pixel_tablet", T::PIXEL_TABLET},
            {"automotive_1024p_landscape", T::AUTOMOTIVE_1024P_LANDSCAPE},
            {"automotive_1080p_landscape", T::AUTOMOTIVE_1080P_LANDSCAPE},
            {"automotive_1408p_landscape_with_play", T::AUTOMOTIVE_1408P_LANDSCAPE_WITH_PLAY},
            {"automotive_1408p_landscape_with_google_apis",
             T::AUTOMOTIVE_1408P_LANDSCAPE_WITH_GOOGLE_APIS},
            {"automotive_portrait", T::AUTOMOTIVE_PORTRAIT},
            {"automotive_distant_display", T::AUTOMOTIVE_DISTANT_DISPLAY},
            {"automotive_distant_display_with_play", T::AUTOMOTIVE_DISTANT_DISPLAY_WITH_PLAY},
            {"automotive_ultrawide", T::AUTOMOTIVE_ULTRAWIDE_DISPLAY},
            {"automotive_large_portrait", T::AUTOMOTIVE_LARGE_PORTRAIT},
            {"desktop_small", T::DESKTOP_SMALL},
            {"desktop_medium", T::DESKTOP_MEDIUM},
            {"desktop_large", T::DESKTOP_LARGE},
            {"tv_4k", T::TV_4K},
            {"tv_1080p", T::TV_1080P},
            {"tv_720p", T::TV_720P},
            {"wearos_large_round", T::WEAROS_LARGE_ROUND},
            {"wearos_small_round", T::WEAROS_SMALL_ROUND},
            {"wearos_rect", T::WEAROS_RECT},
            {"wearos_square", T::WEAROS_SQUARE},
            {"xr_headset_device", T::XR_HEADSET_DEVICE},
            {"xr_glasses_device", T::XR_GLASSES_DEVICE},
            {"ai_glasses_device", T::AI_GLASSES_DEVICE},
            {"ai_glasses_displayless", T::AI_GLASSES_AUDIO_DEVICE},
        });
        if (auto i = device_name_map.find(hw_device_name); i != device_name_map.end()) {
            avd_info.set_device_name(i->second);
        } else {
            avd_info.set_device_name(T::UNKNOWN_EMULATOR_DEVICE_NAME);
        }
    }
}

android_studio::EmulatorDetails::GuestCpuArchitecture toClearcutLogGuestArch(
        const std::string& hw_cpu_arch) {
    using namespace std::literals;
    using android_studio::EmulatorDetails;
    constexpr auto map = std::array{
        std::pair{"x86"sv, EmulatorDetails::X86},   std::pair{"x86_64"sv, EmulatorDetails::X86_64},
        std::pair{"arm"sv, EmulatorDetails::ARM},   std::pair{"arm64"sv, EmulatorDetails::ARM_64},
        std::pair{"mips"sv, EmulatorDetails::MIPS}, std::pair{"mips64"sv, EmulatorDetails::MIPS_64},
    };

    for (const auto& [name, enu] : map) {
        if (name == hw_cpu_arch) {
            return enu;
        }
    }
    return EmulatorDetails::UNKNOWN_GUEST_CPU_ARCHITECTURE;
}

android_studio::EmulatorAvdInfo::EmulatorAvdProperty toClearcutLogAvdProperty(DeviceType flavor) {
    switch (flavor) {
        using enum DeviceType;
    case kPhone:
        return android_studio::EmulatorAvdInfo::PHONE_AVD;
    case kDesktop:
        return android_studio::EmulatorAvdInfo::DESKTOP_AVD;
    case kTv:
        return android_studio::EmulatorAvdInfo::TV_AVD;
    case kWear:
        return android_studio::EmulatorAvdInfo::WEAR_AVD;
    case kAndroidAuto:
        return android_studio::EmulatorAvdInfo::ANDROIDAUTO_AVD;
    case kXr:
        return android_studio::EmulatorAvdInfo::XR_AVD;
    case kGlasses:
        return android_studio::EmulatorAvdInfo::XR_GLASSES_AVD;
    case kUnknown:
        return android_studio::EmulatorAvdInfo::UNKNOWN_EMULATOR_AVD_FLAG;
    }
    return android_studio::EmulatorAvdInfo::UNKNOWN_EMULATOR_AVD_FLAG;
}

void FillAvdFileInfo(android_studio::EmulatorAvdFile& avd_file,
                     android_studio::EmulatorAvdFile::EmulatorAvdFileKind kind,
                     const fs::path& path, bool is_custom) {
    avd_file.set_kind(kind);
    // TODO avd_file.set_size(size);
    // TODO avd_file.set_creation_timestamp(*creationTime / 1000000);
    avd_file.set_location(is_custom ? android_studio::EmulatorAvdFile::CUSTOM
                                    : android_studio::EmulatorAvdFile::STANDARD);
}

void FillAvdInfo(android_studio::EmulatorAvdInfo& avd_info, const Avd& avd) {
    const auto& hw = avd.Hw();

    // AVD name is a user-generated data, so won't report it.
    avd_info.set_api_level(avd.ApiLevel());

    avd_info.set_image_kind(avd.ImageKind());

    avd_info.set_arch(toClearcutLogGuestArch(hw.hw_cpu_arch));

    // TODO if (inAndroidBuild()) return;
    // no real AVD, so no creation times or file infos.

    /* TODO if (auto creationTime = getAvdCreationTimeSec(
                getConsoleAgents()->settings->avdInfo())) {
        avd_info.set_creation_timestamp(*creationTime);
        VERBOSE_PRINT(metrics, "AVD creation timestamp %ld", *creationTime);
    }*/

    if (auto t = avd.BuildTimestamp(); t > 0) {
        avd_info.set_build_timestamp(t);
    }
    avd_info.set_build_id(avd.BuildFingerprint());

    SetDeviceNameIfSupported(avd_info, hw.hw_device_name);

    FillAvdFileInfo(*avd_info.add_files(), android_studio::EmulatorAvdFile::KERNEL,
                    /*path=*/hw.kernel_path, /*is_custom=TODO*/ false);
    FillAvdFileInfo(*avd_info.add_files(), android_studio::EmulatorAvdFile::RAMDISK,
                    /*path=*/hw.disk_ramdisk_path, /*is_custom=TODO*/ false);
    FillAvdFileInfo(*avd_info.add_files(), android_studio::EmulatorAvdFile::SYSTEM,
                    /*path=*/hw.disk_systemPartition_path, /*is_custom=TODO*/ false);

    avd_info.add_properties(toClearcutLogAvdProperty(avd.GetDeviceType()));
}

}  // namespace

bool IsRunningInCi() {
    auto sys = android::base::System::Get();
    auto isTruthy = [](std::string_view val) {
        if (val.empty()) return false;
        if (val == "1") return true;
        if (val.size() == 4) {
            return (val[0] == 't' || val[0] == 'T') && (val[1] == 'r' || val[1] == 'R') &&
                   (val[2] == 'u' || val[2] == 'U') && (val[3] == 'e' || val[3] == 'E');
        }
        return false;
    };
    if (isTruthy(sys->EnvGet("CI"))) return true;
    if (isTruthy(sys->EnvGet("CONTINUOUS_INTEGRATION"))) return true;

    static constexpr std::array kCiEnvVars = {
        "GITHUB_ACTIONS", "GITLAB_CI", "JENKINS_URL",      "TF_BUILD",           "CIRCLECI",
        "TRAVIS",         "BUILDKITE", "TEAMCITY_VERSION", "CODEBUILD_BUILD_ID",
    };

    return std::any_of(kCiEnvVars.begin(), kCiEnvVars.end(),
                       [&](const char* var) { return sys->EnvTest(var); });
}

bool IsAndroidCliDefined() {
    return android::base::System::Get()->EnvGet("ANDROID_CLI") == "1";
}

bool IsRunningInContainer() {
#if defined(__linux__)
    if (android::base::file::exists("/.dockerenv")) return true;
    if (android::base::file::exists("/run/.containerenv")) return true;
    if (android::base::file::exists("/run/systemd/container")) return true;
    if (android::base::System::Get()->EnvTest("container")) return true;
    if (android::base::System::Get()->EnvTest("KUBERNETES_SERVICE_HOST")) return true;
#endif
    // Backward compatibility with the legacy container image
    if (android::base::file::exists("/android/sdk/launch-emulator.sh") &&
        android::base::file::exists("/tmp/pulseverbose.log")) {
        return true;
    }
    return false;
}

namespace {

void FillHost(android_studio::EmulatorHost& host) {
    host.set_os_bit_count(64);
    const android::AndroidCpuInfoFlags cpuFlags = android::GetCpuInfo().first;
    host.set_virt_support(cpuFlags & ANDROID_CPU_INFO_VIRT_SUPPORTED);
    host.set_running_in_vm(cpuFlags & ANDROID_CPU_INFO_VM);
    host.set_running_in_ci(IsRunningInCi());
    host.set_android_cli_defined(IsAndroidCliDefined());
    host.set_cpu_manufacturer((cpuFlags & ANDROID_CPU_INFO_INTEL) ? "INTEL"
                              : (cpuFlags & ANDROID_CPU_INFO_AMD) ? "AMD"
                                                                  : "OTHER");

#if defined(__x86_64__)
    auto x86_cpuid = android::GetX86Cpuid();
    host.set_cpuid_stepping(x86_cpuid.cpuid_stepping);
    host.set_cpuid_model(x86_cpuid.cpuid_model);
    host.set_cpuid_family(x86_cpuid.cpuid_family);
    host.set_cpuid_type(x86_cpuid.cpuid_type);
    host.set_cpuid_extmodel(x86_cpuid.cpuid_extmodel);
    host.set_cpuid_extfamily(x86_cpuid.cpuid_extfamily);

    host.set_cpu_architecture("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    host.set_cpu_architecture("arm64");
#endif

    // x86_64 CPU brand name returned by CPUID can at most have 48 bytes
    // including the NULL terminator. But I don't see a definition on the
    // ARM64 side. To get it safer, use a larger buffer to hold the name.
    // So far, I have not seen a CPU whose name is 100 bytes long.
    // TODO(b/496311138): Update the implementations to check that the buffer is big enough.
    char cpuBrandName[100];
    if (!android::cpu::GetCpuBrandName(cpuBrandName)) {
        host.set_cpu_brandname(cpuBrandName);
    }
}

void FillFeatureFlagState(android_studio::EmulatorFeatureFlagState& feature_flag_state,
                          const FeatureFlags& feature_flags) {
    for (const auto feature : feature_flags.GetEnabledNonOverride()) {
        feature_flag_state.add_attempted_enabled_feature_flags(feature);
    }
    for (const auto feature : feature_flags.GetEnabledOverride()) {
        feature_flag_state.add_user_overridden_enabled_features(feature);
    }
    for (const auto feature : feature_flags.GetDisabledOverride()) {
        feature_flag_state.add_user_overridden_disabled_features(feature);
    }
    for (const auto feature : feature_flags.GetEnabled()) {
        feature_flag_state.add_resulting_enabled_features(feature);
    }
}

android_studio::EmulatorDetails::VulkanIcd GetVulkanIcd(std::string_view vk_icd) {
    if (vk_icd == "") {  // Use hardware when vk_icd is null
        return android_studio::EmulatorDetails::HOST_DEFAULT_VK;
    } else if (vk_icd == "swiftshader") {
        return android_studio::EmulatorDetails::SWIFTSHADER_VK;
    } else if (vk_icd == "lavapipe") {
        return android_studio::EmulatorDetails::LAVAPIPE_VK;
    } else if (vk_icd == "moltenvk") {
        return android_studio::EmulatorDetails::MOLTEN_VK;
    } else if (vk_icd == "kosmickrisp") {
        return android_studio::EmulatorDetails::KOSMICKRISP_VK;
    } else {
        return android_studio::EmulatorDetails::UNKNOWN_VK;
    }
}

void FillDetails(android_studio::EmulatorDetails& details, const Avd& avd,
                 const FeatureFlags& feature_flags, long launcher_pid, long qemu_pid,
                 bool metrics_collection_opt, bool fuchsia_opt, bool openglAlive,
                 std::string_view vk_icd, std::span<const VulkanGpuDeviceInfo> host_gpus,
                 const std::optional<VulkanGpuDeviceInfo>& selected_gpu) {
    FillAvdInfo(*details.mutable_avd_info(), avd);
    FillFeatureFlagState(*details.mutable_feature_flag_state(), feature_flags);

    const auto& hw = avd.Hw();

    details.set_session_phase(android_studio::EmulatorDetails::RUNNING_GENERAL);
    details.set_is_opengl_alive(openglAlive);
    details.set_guest_arch(toClearcutLogGuestArch(hw.hw_cpu_arch));
    details.set_guest_api_level(avd.ApiLevel());

    // TODO: Check that CpuAccelerator enum +1 is the same
    // as the proto enum.
    details.set_hypervisor(static_cast<android_studio::EmulatorDetails::EmulatorHypervisor>(
            android::GetCurrentCpuAccelerator() + 1));

    details.set_guest_gpu_enabled(hw.hw_gpu_enabled);
    // TODO Set renderer details.
    /* details.set_renderer()
    if (hw.hw_gpu_enabled) {
        fillGuestGlMetrics(event);
        if (openglAlive) {
                renderer->fillGLESUsages(details.mutable_gles_usages());
        }
    }*/

    details.set_vulkan_icd(GetVulkanIcd(vk_icd));

    int i = 0;
    for (const auto& gpu : host_gpus) {
        const auto& props = gpu.properties;
        const std::string_view device_name(props.deviceName,
                                           strnlen(props.deviceName, sizeof(props.deviceName)));
        VLOG(1) << absl::StrFormat(
                "Found Vulkan device: %d: name: '%s', apiVersion: %d.%d.%d, vendorID: 0x%x, "
                "deviceID: 0x%x, driverVersion: 0x%x",
                i++, device_name, VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
                props.vendorID, props.deviceID, props.driverVersion);

        auto* hostGpu = details.add_host_gpu();
        hostGpu->set_device_id(absl::StrFormat("0x%04x", props.deviceID));
        hostGpu->set_make(absl::StrFormat("0x%04x", props.vendorID));
        hostGpu->set_model(std::string(device_name));
        hostGpu->set_renderer(std::string(device_name));
        hostGpu->set_revision_id(absl::StrFormat("0x%x", props.driverVersion));
        hostGpu->set_version(absl::StrFormat("%d.%d.%d", VK_API_VERSION_MAJOR(props.apiVersion),
                                             VK_API_VERSION_MINOR(props.apiVersion),
                                             VK_API_VERSION_PATCH(props.apiVersion)));
    }

    if (selected_gpu.has_value()) {
        const auto& props = selected_gpu->properties;
        const std::string_view device_name(props.deviceName,
                                           strnlen(props.deviceName, sizeof(props.deviceName)));
        auto* vkGpu = details.mutable_active_vulkan_host_gpu();
        vkGpu->set_vendor_id(static_cast<int32_t>(props.vendorID));
        vkGpu->set_device_id(static_cast<int32_t>(props.deviceID));
        const int32_t proto_device_type = static_cast<int32_t>(props.deviceType) + 1;
        if (android_studio::EmulatorGpuVkInfo::PhysicalDeviceType_IsValid(proto_device_type)) {
            vkGpu->set_device_type(
                    static_cast<android_studio::EmulatorGpuVkInfo::PhysicalDeviceType>(
                            proto_device_type));
        } else {
            vkGpu->set_device_type(android_studio::EmulatorGpuVkInfo::UNKNOWN_PHYSICAL_DEVICE_TYPE);
        }
        vkGpu->set_api_version(static_cast<int32_t>(props.apiVersion));
        vkGpu->set_driver_version(static_cast<int32_t>(props.driverVersion));
        vkGpu->set_gpu_memory(static_cast<int64_t>(selected_gpu->device_local_memory_bytes));
        vkGpu->set_device_name(std::string(device_name));
    }

    // Check for container environment
    bool isContainer = IsRunningInContainer();

    if (isContainer && metrics_collection_opt) {
        details.mutable_used_features()->set_launch_type(
                android_studio::EmulatorFeatures::CONTAINER);
    }

    if (fuchsia_opt) {
        details.mutable_used_features()->set_launch_type(android_studio::EmulatorFeatures::FUCHSIA);
    }

    details.set_emu_pid(launcher_pid);
    details.set_qemu_pid(qemu_pid);
    details.set_has_elevated_privileges(
            android::base::System::CurrentProcessHasElevatedPrivileges());
}

android_studio::ProductDetails::SoftwareLifeCycleChannel ToProductChannel(
        ::goldfish::metrics::studio::UpdateChannel channel) {
    using android_studio::ProductDetails;
    using ::goldfish::metrics::studio::UpdateChannel;
    switch (channel) {
    case UpdateChannel::kStable:
        return ProductDetails::STABLE;
    case UpdateChannel::kBeta:
        return ProductDetails::BETA;
    case UpdateChannel::kDev:
        return ProductDetails::DEV;
    case UpdateChannel::kCanary:
        return ProductDetails::CANARY;
    case UpdateChannel::kUnknown:
        break;
    }
    return ProductDetails::UNKNOWN_LIFE_CYCLE_CHANNEL;
}
}  // namespace

void FillEmulatorHostEvent(android_studio::AndroidStudioEvent& event, const Avd& avd,
                           const FeatureFlags& feature_flags, long launcher_pid, long qemu_pid,
                           bool metrics_collection_opt, bool fuchsia_opt, std::string_view vk_icd,
                           std::span<const VulkanGpuDeviceInfo> host_gpus,
                           const std::optional<VulkanGpuDeviceInfo>& selected_gpu) {
    event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_HOST);

    auto& product = *event.mutable_product_details();
    product.set_channel(ToProductChannel(::goldfish::metrics::studio::GetUpdateChannel()));

#ifdef __aarch64__
    product.set_os_architecture(android_studio::ProductDetails::ARM);
#else
    product.set_os_architecture(android_studio::ProductDetails::X86_64);
#endif

    FillHost(*event.mutable_emulator_host());
    FillDetails(*event.mutable_emulator_details(), avd, feature_flags, launcher_pid, qemu_pid,
                metrics_collection_opt, fuchsia_opt, false, vk_icd, host_gpus, selected_gpu);
}

}  // namespace android::goldfish
