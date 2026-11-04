// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/cpu/cpu_accelerator.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
// clang-format off
#include <windows.h>
#include <WinHvPlatform.h>
// clang-format on
#undef ERROR
#endif

#ifdef __linux__
#include <unistd.h>
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/cleanup/cleanup.h"

#include "android/base/system.h"
#include "android/base/testing/test_system.h"
#include "android/base/testing/test_temp_dir.h"
#include "cpu_accelerator_internal.h"
#include "goldfish/file/file.h"

namespace android {
namespace {

using ::testing::AnyOf;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StartsWith;

constexpr std::string_view kHyperVWarning =
        "Please disable Hyper-V before using the Android Emulator.  "
        "Start a command prompt as Administrator, run 'bcdedit /set "
        "hypervisorlaunchtype off', reboot.";

TEST(CpuAcceleratorTest, CpuAcceleratorToString) {
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_NONE), "tcg");
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_KVM), "kvm");
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_HAX), "hax (deprecated)");
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_HVF), "hvf");
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_WHPX), "whpx");
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_AEHD), "aehd");
    EXPECT_EQ(CpuAcceleratorToString(CPU_ACCELERATOR_MAX), "tcg");
}

TEST(CpuAcceleratorTest, ParseMacOsVersionString) {
    std::string status;
    EXPECT_EQ(parseMacOSVersionString("macOS 14.5", &status), base::Version(14, 5, 0));
    EXPECT_TRUE(status.empty());

    EXPECT_EQ(parseMacOSVersionString("Mac OS X 10.15.7", &status), base::Version(10, 15, 7));
    EXPECT_TRUE(status.empty());

    EXPECT_EQ(parseMacOSVersionString("Error: sw_vers failed", &status), base::Version(0, 0, 0));
    EXPECT_EQ(status, "Internal error: failed to parse OS version 'Error: sw_vers failed'");

    status.clear();
    EXPECT_EQ(parseMacOSVersionString("14.5", &status), base::Version(0, 0, 0));
    EXPECT_EQ(status, "Internal error: failed to parse OS version '14.5'");

    status.clear();
    EXPECT_EQ(parseMacOSVersionString("macOS beta", &status), base::Version(0, 0, 0));
    EXPECT_EQ(status, "Internal error: failed to parse OS version 'macOS beta'");
}

TEST(CpuAcceleratorTest, SetCurrentCpuAcceleratorForTestingOverridesGlobalState) {
    SetCurrentCpuAcceleratorForTesting(CPU_ACCELERATOR_WHPX, ANDROID_CPU_ACCELERATION_READY,
                                       "WHPX (10.0.26200) is installed and usable.", "10.0.26200");
    EXPECT_EQ(GetCurrentCpuAccelerator(), CPU_ACCELERATOR_WHPX);
    EXPECT_EQ(GetCurrentCpuAcceleratorStatusCode(), ANDROID_CPU_ACCELERATION_READY);
    EXPECT_EQ(GetCurrentCpuAcceleratorStatus(), "WHPX (10.0.26200) is installed and usable.");
    EXPECT_EQ(GetCurrentCpuAcceleratorVersion(), base::Version(10, 0, 26200));

    ResetCurrentCpuAccelerator(CPU_ACCELERATOR_NONE);
    EXPECT_EQ(GetCurrentCpuAccelerator(), CPU_ACCELERATOR_NONE);
    EXPECT_EQ(GetCurrentCpuAcceleratorVersion(), base::Version::invalid());
}

#ifdef _WIN32

int fake_module_anchor = 0;
int free_library_calls = 0;

HMODULE FakeModuleHandle() {
    return reinterpret_cast<HMODULE>(&fake_module_anchor);
}

BOOL RecordFreeLibrary(HMODULE module) {
    if (module == FakeModuleHandle()) {
        ++free_library_calls;
    }
    return TRUE;
}

std::optional<_OSVERSIONINFOEXW> FakeWindows11Version() {
    _OSVERSIONINFOEXW ver = {};
    ver.dwOSVersionInfoSize = sizeof(ver);
    ver.dwMajorVersion = 10;
    ver.dwMinorVersion = 0;
    ver.dwBuildNumber = 26200;
    return ver;
}

HRESULT WINAPI FakeWhvCapabilityPresent(WHV_CAPABILITY_CODE capability_code,
                                        VOID* capability_buffer, UINT32 capability_buffer_size,
                                        UINT32* written_size) {
    if (capability_code != WHvCapabilityCodeHypervisorPresent || capability_buffer == nullptr ||
        capability_buffer_size < sizeof(WHV_CAPABILITY)) {
        return E_INVALIDARG;
    }
    std::memset(capability_buffer, 0, capability_buffer_size);
    auto* cap = static_cast<WHV_CAPABILITY*>(capability_buffer);
    cap->HypervisorPresent = TRUE;
    if (written_size != nullptr) {
        *written_size = sizeof(WHV_CAPABILITY);
    }
    return S_OK;
}

HRESULT WINAPI FakeWhvCapabilityNotPresent(WHV_CAPABILITY_CODE capability_code,
                                           VOID* capability_buffer, UINT32 capability_buffer_size,
                                           UINT32* written_size) {
    if (capability_code != WHvCapabilityCodeHypervisorPresent || capability_buffer == nullptr ||
        capability_buffer_size < sizeof(WHV_CAPABILITY)) {
        return E_INVALIDARG;
    }
    std::memset(capability_buffer, 0, capability_buffer_size);
    auto* cap = static_cast<WHV_CAPABILITY*>(capability_buffer);
    cap->HypervisorPresent = FALSE;
    if (written_size != nullptr) {
        *written_size = sizeof(WHV_CAPABILITY);
    }
    return S_OK;
}

HRESULT WINAPI FakeWhvCapabilityFailed(WHV_CAPABILITY_CODE /*capability_code*/,
                                       VOID* /*capability_buffer*/,
                                       UINT32 /*capability_buffer_size*/,
                                       UINT32* /*written_size*/) {
    return E_FAIL;
}

TEST(CpuAcceleratorTest, ProbeWhpxClearsPreexistingHyperVWarningWhenReady) {
    free_library_calls = 0;
    SetCurrentCpuAcceleratorForTesting(CPU_ACCELERATOR_WHPX, ANDROID_CPU_ACCELERATION_ERROR, "init",
                                       "0.0.0");

    internal::WhpxOps ops{
        .load_library = [](LPCWSTR) -> HMODULE { return FakeModuleHandle(); },
        .get_proc_address = [](HMODULE, LPCSTR proc_name) -> void* {
            if (std::string_view(proc_name) == "WHvGetCapability") {
                return reinterpret_cast<void*>(&FakeWhvCapabilityPresent);
            }
            return nullptr;
        },
        .free_library = &RecordFreeLibrary,
        .get_windows_version = &FakeWindows11Version,
    };

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeWHPX(&status, ops), ANDROID_CPU_ACCELERATION_READY);
    EXPECT_EQ(status, "WHPX (10.0.26200) is installed and usable.");
    EXPECT_EQ(free_library_calls, 1);
    EXPECT_EQ(GetCurrentCpuAcceleratorVersion(), base::Version(10, 0, 26200));
}

TEST(CpuAcceleratorTest, ProbeWhpxClearsPreexistingStatusWhenHypervisorNotPresent) {
    free_library_calls = 0;
    internal::WhpxOps ops{
        .load_library = [](LPCWSTR) -> HMODULE { return FakeModuleHandle(); },
        .get_proc_address = [](HMODULE, LPCSTR) -> void* {
            return reinterpret_cast<void*>(&FakeWhvCapabilityNotPresent);
        },
        .free_library = &RecordFreeLibrary,
        .get_windows_version = &FakeWindows11Version,
    };

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeWHPX(&status, ops), ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED);
    EXPECT_EQ(status, "WHPX: No accelerator found, hr=00000000.");
    EXPECT_EQ(free_library_calls, 1);
}

TEST(CpuAcceleratorTest, ProbeWhpxClearsPreexistingStatusWhenCapabilityCallFails) {
    free_library_calls = 0;
    internal::WhpxOps ops{
        .load_library = [](LPCWSTR) -> HMODULE { return FakeModuleHandle(); },
        .get_proc_address = [](HMODULE, LPCSTR) -> void* {
            return reinterpret_cast<void*>(&FakeWhvCapabilityFailed);
        },
        .free_library = &RecordFreeLibrary,
        .get_windows_version = &FakeWindows11Version,
    };

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeWHPX(&status, ops), ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED);
    EXPECT_EQ(status, "WHPX: No accelerator found, hr=80004005.");
    EXPECT_EQ(free_library_calls, 1);
}

TEST(CpuAcceleratorTest, ProbeWhpxClearsPreexistingStatusWhenDllMissing) {
    free_library_calls = 0;
    internal::WhpxOps ops{
        .load_library = [](LPCWSTR) -> HMODULE { return nullptr; },
        .free_library = &RecordFreeLibrary,
    };

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeWHPX(&status, ops), ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED);
    EXPECT_EQ(status, "Could not load library 'WinHvPlatform.dll'.");
    EXPECT_EQ(free_library_calls, 0);
}

TEST(CpuAcceleratorTest, ProbeWhpxClearsPreexistingStatusWhenSymbolMissing) {
    free_library_calls = 0;
    internal::WhpxOps ops{
        .load_library = [](LPCWSTR) -> HMODULE { return FakeModuleHandle(); },
        .get_proc_address = [](HMODULE, LPCSTR) -> void* { return nullptr; },
        .free_library = &RecordFreeLibrary,
    };

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeWHPX(&status, ops), ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED);
    EXPECT_EQ(status, "Could not load library function 'WHvGetCapability'.");
    EXPECT_EQ(free_library_calls, 1);
}

TEST(CpuAcceleratorTest, ProbeWhpxClearsPreexistingStatusWhenWindowsVersionUnavailable) {
    free_library_calls = 0;
    internal::WhpxOps ops{
        .load_library = [](LPCWSTR) -> HMODULE { return FakeModuleHandle(); },
        .get_proc_address = [](HMODULE, LPCSTR) -> void* {
            return reinterpret_cast<void*>(&FakeWhvCapabilityPresent);
        },
        .free_library = &RecordFreeLibrary,
        .get_windows_version = []() -> std::optional<_OSVERSIONINFOEXW> { return std::nullopt; },
    };

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeWHPX(&status, ops), ANDROID_CPU_ACCELERATION_ERROR);
    EXPECT_EQ(status, "Could not extract Windows version.");
    EXPECT_EQ(free_library_calls, 1);
}

TEST(CpuAcceleratorTest, ProbeWhpxHostOverloadClearsPreexistingStatus) {
    std::string status(kHyperVWarning);
    ProbeWHPX(&status);
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
    EXPECT_FALSE(status.empty());
}

#endif  // _WIN32

#ifdef __linux__

constexpr std::string_view kKvmDeviceEnvVar = "ANDROID_EMULATOR_KVM_DEVICE";

TEST(CpuAcceleratorTest, ProbeKvmClearsPreexistingStatusWhenDeviceMissing) {
    base::TestTempDir temp_dir("cpu_accel_kvm_missing");
    const std::string prev_kvm_env = base::System::GetEnvironmentVariable(kKvmDeviceEnvVar);
    const auto restore_env = absl::MakeCleanup([&prev_kvm_env] {
        base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, prev_kvm_env);
    });

    const std::filesystem::path missing_path = temp_dir.MakeSubPath("kvm_\xc3\xa9_missing");
    base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, missing_path.string());

    std::string status(kHyperVWarning);
    const AndroidCpuAcceleration code = ProbeKVM(&status);
    EXPECT_THAT(code, AnyOf(ANDROID_CPU_ACCELERATION_DEV_NOT_FOUND,
                            ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT));
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
    if (code == ANDROID_CPU_ACCELERATION_DEV_NOT_FOUND) {
        EXPECT_THAT(status, StartsWith(missing_path.string()));
    }
}

TEST(CpuAcceleratorTest, ProbeKvmClearsPreexistingStatusWhenPermissionDenied) {
    if (::geteuid() == 0) {
        return;
    }

    base::TestTempDir temp_dir("cpu_accel_kvm_perm");
    const std::string prev_kvm_env = base::System::GetEnvironmentVariable(kKvmDeviceEnvVar);
    const auto restore_env = absl::MakeCleanup([&prev_kvm_env] {
        base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, prev_kvm_env);
    });

    const std::filesystem::path unreadable_path = temp_dir.MakeSubPath("kvm_\xc3\xa9_unreadable");
    ASSERT_TRUE(base::file::touch(unreadable_path).ok());
    ASSERT_TRUE(base::file::chmod(unreadable_path, 0000).ok());
    const auto restore_perms = absl::MakeCleanup(
            [&unreadable_path] { base::file::chmod(unreadable_path, 0600).IgnoreError(); });
    base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, unreadable_path.string());

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeKVM(&status), ANDROID_CPU_ACCELERATION_DEV_PERMISSION);
    EXPECT_THAT(status, StartsWith("This user doesn't have permissions to use KVM"));
    EXPECT_THAT(status, HasSubstr(unreadable_path.string()));
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
}

TEST(CpuAcceleratorTest, ProbeKvmClearsPreexistingStatusWhenIoctlFails) {
    base::TestTempDir temp_dir("cpu_accel_kvm_ioctl");
    const std::string prev_kvm_env = base::System::GetEnvironmentVariable(kKvmDeviceEnvVar);
    const auto restore_env = absl::MakeCleanup([&prev_kvm_env] {
        base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, prev_kvm_env);
    });

    const std::filesystem::path regular_file = temp_dir.MakeSubPath("kvm_\xc3\xa9_regular");
    ASSERT_TRUE(base::file::touch(regular_file).ok());
    base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, regular_file.string());

    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeKVM(&status), ANDROID_CPU_ACCELERATION_DEV_IOCTL_FAILED);
    EXPECT_THAT(status, StartsWith("Could not extract KVM version:"));
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
}

TEST(CpuAcceleratorTest, ProbeKvmDefaultHostClearsPreexistingStatus) {
    const std::string prev_kvm_env = base::System::GetEnvironmentVariable(kKvmDeviceEnvVar);
    const auto restore_env = absl::MakeCleanup([&prev_kvm_env] {
        base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, prev_kvm_env);
    });
    base::System::SetEnvironmentVariable(kKvmDeviceEnvVar, "");

    std::string status(kHyperVWarning);
    ProbeKVM(&status);
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
    EXPECT_FALSE(status.empty());
}

#endif  // __linux__

#ifdef __APPLE__

class FakeMacSystem : public base::TestSystem {
  public:
    explicit FakeMacSystem(std::string os_name)
            : base::TestSystem("/progdir"), os_name_(std::move(os_name)) {}

    std::string GetOsName() override { return os_name_; }

  private:
    std::string os_name_;
};

TEST(CpuAcceleratorTest, ProbeHvfClearsPreexistingStatusWhenOsTooOld) {
    FakeMacSystem system("Mac OS X 10.9");
    std::string status(kHyperVWarning);
    EXPECT_EQ(ProbeHVF(&status), ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT);
    EXPECT_THAT(status, StartsWith("Hypervisor.Framework is only supported"));
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
}

TEST(CpuAcceleratorTest, ProbeHvfClearsPreexistingStatusWhenSupportedOs) {
    FakeMacSystem system("macOS 14.5");
    std::string status(kHyperVWarning);
    const AndroidCpuAcceleration code = ProbeHVF(&status);
    EXPECT_THAT(status, Not(HasSubstr("Please disable Hyper-V")));
#ifdef __arm64__
    EXPECT_EQ(code, ANDROID_CPU_ACCELERATION_READY);
    EXPECT_EQ(status, "Hypervisor.Framework OS X Version 14.5");
#else
    EXPECT_THAT(code,
                AnyOf(ANDROID_CPU_ACCELERATION_READY, ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT));
#endif
}

#endif  // __APPLE__

}  // namespace
}  // namespace android
