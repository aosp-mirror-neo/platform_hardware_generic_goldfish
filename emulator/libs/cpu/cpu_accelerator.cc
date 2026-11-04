// Copyright (C) 2014 The Android Open Source Project
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

#define CPU_ACCELERATOR_PRIVATE

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
// IWYU pragma: end_keep
// clang-format on
#include <windows.h>
#include <winioctl.h>
// IWYU pragma: end_keep
// clang-format on

#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <cstring>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#endif

#include <cstdio>
#include <filesystem>
#include <string_view>

#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"

#include "android/base/system.h"
#include "cpu_accelerator_internal.h"
#include "goldfish/file/file.h"
#include "x86_cpuid.h"

#ifdef _WIN32
#include "android/base/scoped_file_handle.h"
#include "android/base/win32_unicode_string.h"
#include "android/base/win32_utils.h"
#include "windows_installer.h"
#else
#include "goldfish/file/scoped_fd.h"
#endif

#ifdef __APPLE__
#include "apple_cpu_accelerator.h"
#endif

#include <array>

// NOTE: This source file must be independent of the rest of QEMU, as such
//       it should not include / reuse any QEMU source file or function
//       related to KVM or HAX.

#ifdef __linux__
#define HAVE_KVM 1

#elif defined(_WIN32)
#define HAVE_WHPX 1
// AEHD is not in qemu 8, and crashes clang
#define HAVE_AEHD 0

#elif defined(__APPLE__)
#define HAVE_HVF 1
#ifdef __arm64__
#define APPLE_SILICON 1
#endif

#else
#error "Unsupported host platform!"
#endif

namespace android {

namespace fs = std::filesystem;

using base::Version;

// For detecting that the cpu can run with fast virtualization
// without requiring workarounds such as resetting SMP=1.
//
// Technically, we need  rdmsr to detect ept/ug support,
// but that instruction is only available
// as root. So just say yes if the processor has features that were
// only introduced the same time as the feature.
// EPT: popcnt
// UG: aes + pclmulqsq
bool hasModernX86VirtualizationFeatures() {
    uint32_t cpuid_function1_ecx;
    android_get_x86_cpuid(1, 0, nullptr, nullptr, &cpuid_function1_ecx, nullptr);

    uint32_t popcnt_support = 1 << 23;
    uint32_t aes_support = 1 << 25;
    uint32_t pclmulqsq_support = 1 << 1;

    bool eptSupport = (cpuid_function1_ecx & popcnt_support) != 0u;
    bool ugSupport = ((cpuid_function1_ecx & aes_support) != 0u) &&
                     ((cpuid_function1_ecx & pclmulqsq_support) != 0u);

    return eptSupport && ugSupport;
}

struct GlobalState {
    bool probed;
    bool testing;
    CpuAccelerator accel;
    char status[256];
    char version[256];
    AndroidCpuAcceleration status_code;
    std::array<bool, CPU_ACCELERATOR_MAX> supported_accelerators;
};

GlobalState gGlobals = {
    false, false, CPU_ACCELERATOR_NONE, {'\0'}, {'\0'}, ANDROID_CPU_ACCELERATION_ERROR, {}};

// Windows Hypervisor Platform (WHPX) support

#if HAVE_WHPX

// IWYU pragma: end_keep
// clang-format on
#include <WinHvPlatform.h>
#include <windows.h>
// IWYU pragma: end_keep
// clang-format on

/* debugging */
#define WHPX_DBG(...) (void)0

AndroidCpuAcceleration ProbeWHPX(std::string* status, const internal::WhpxOps& ops) {
    status->clear();

    WHPX_DBG(
            "Checking whether Windows Hypervisor Platform (WHPX) is "
            "available.");

    HMODULE win_hv_platform = ops.load_library(L"WinHvPlatform.dll");
    if (win_hv_platform == nullptr) {
        WHPX_DBG("Could not load library WinHvPlatform.dll");
        status->assign("Could not load library 'WinHvPlatform.dll'.");
        return ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED;
    }
    const auto free_win_hv =
            absl::MakeCleanup([&ops, win_hv_platform] { ops.free_library(win_hv_platform); });

    WHPX_DBG("WinHvPlatform.dll found. Looking for WHvGetCapability...");
    using WhvGetCapabilityFn = HRESULT(WINAPI*)(WHV_CAPABILITY_CODE, VOID*, UINT32, UINT32*);
    auto get_capability = reinterpret_cast<WhvGetCapabilityFn>(
            ops.get_proc_address(win_hv_platform, "WHvGetCapability"));
    if (get_capability == nullptr) {
        WHPX_DBG("Could not load library function 'WHvGetCapability'.");
        status->assign("Could not load library function 'WHvGetCapability'.");
        return ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED;
    }

    WHPX_DBG("WHvGetCapability found. Querying WHPX capabilities...");
    WHV_CAPABILITY whpx_cap = {};
    UINT32 whpx_cap_size = 0;
    HRESULT hr = get_capability(WHvCapabilityCodeHypervisorPresent, &whpx_cap, sizeof(whpx_cap),
                                &whpx_cap_size);
    if (FAILED(hr) || !whpx_cap.HypervisorPresent) {
        WHPX_DBG(
                "WHvGetCapability failed. hr=0x%08lx "
                "whpx_cap.HypervisorPresent? %d\n",
                hr, whpx_cap.HypervisorPresent);
        status->assign(absl::StrFormat("WHPX: No accelerator found, hr=%08lx.", hr));
        return ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED;
    }

    auto ver = ops.get_windows_version();
    if (!ver) {
        WHPX_DBG("Could not extract Windows version.");
        status->assign("Could not extract Windows version.");
        return ANDROID_CPU_ACCELERATION_ERROR;
    }

    const std::string version_str = absl::StrFormat("%lu.%lu.%lu", ver->dwMajorVersion,
                                                    ver->dwMinorVersion, ver->dwBuildNumber);

    WHPX_DBG("WHPX (%s) is installed and usable.", version_str.c_str());
    status->assign(absl::StrFormat("WHPX (%s) is installed and usable.", version_str));
    GlobalState* g = &gGlobals;
    ::snprintf(g->version, sizeof(g->version), "%s", version_str.c_str());
    return ANDROID_CPU_ACCELERATION_READY;
}

AndroidCpuAcceleration ProbeWHPX(std::string* status) {
    return ProbeWHPX(status, internal::WhpxOps{});
}

#endif  // HAVE_WHPX

/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
/////
/////   Linux KVM support.
/////
/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

#if HAVE_KVM

#include <linux/kvm.h>

#define KVM_DEVICE_NAME_ENV "ANDROID_EMULATOR_KVM_DEVICE"

// Return true iff KVM is installed and usable on this machine.
// |*status| will be set to a small status string explaining the
// status of KVM on success or failure.
AndroidCpuAcceleration ProbeKVM(std::string* status) {
    status->clear();

    const char* kvm_device = getenv(KVM_DEVICE_NAME_ENV);
    if (nullptr == kvm_device) {
        kvm_device = "/dev/kvm";
    }
    // Check that kvm device exists.
    if (!base::file::exists(kvm_device)) {
        // kvm device does not exist
        bool cpu_ok = android_get_x86_cpuid_vmx_support() || android_get_x86_cpuid_svm_support();
        if (!cpu_ok) {
            status->assign("KVM requires a CPU that supports vmx or svm");
            return ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT;
        }
        absl::StrAppendFormat(status,
                              "%s is not found: VT disabled in BIOS or KVM kernel "
                              "module not loaded",
                              kvm_device);
        return ANDROID_CPU_ACCELERATION_DEV_NOT_FOUND;
    }

    // Check that kvm device can be opened.
    if (!base::file::can_read(kvm_device)) {
        constexpr char kEtcGroupsPath[] = "/etc/group";
        std::string etc_groups_kvm_line("LINE_NOT_FOUND");
        if (auto content_or = base::file::read_whole_file(kEtcGroupsPath, /*binary=*/true);
            content_or.ok()) {
            for (std::string_view line : absl::StrSplit(*content_or, '\n')) {
                if (absl::StartsWith(line, "kvm:")) {
                    etc_groups_kvm_line = std::string(line);
                }
            }
        }

        absl::StrAppendFormat(status,
                              "This user doesn't have permissions to use KVM (%s).\n"
                              "The KVM line in /etc/group is: [%s]\n"
                              "\n"
                              "If the current user has KVM permissions,\n"
                              "the KVM line in /etc/group should end with \":\" followed by "
                              "your username.\n"
                              "\n"
                              "If we see LINE_NOT_FOUND, the kvm group may need to be "
                              "created along with permissions:\n"
                              "    sudo groupadd -r kvm\n"
                              "    # Then ensure /lib/udev/rules.d/50-udev-default.rules "
                              "contains something like:\n"
                              "    # KERNEL==\"kvm\", GROUP=\"kvm\", MODE=\"0660\"\n"
                              "    # and then run:\n"
                              "    sudo gpasswd -a $USER kvm\n"
                              "\n"
                              "If we see kvm:... but no username at the end, running the "
                              "following command may allow KVM access:\n"
                              "    sudo gpasswd -a $USER kvm\n"
                              "\n"
                              "You may need to log out and back in for changes to take "
                              "effect.\n",
                              kvm_device, etc_groups_kvm_line.c_str());

        // There are issues with ensuring the above is actually printed. Print
        // it now.
        fprintf(stderr, "%s: %s\n", __func__, status->c_str());
        return ANDROID_CPU_ACCELERATION_DEV_PERMISSION;
    }

    // Open the file.
    base::file::ScopedFd fd(TEMP_FAILURE_RETRY(open(kvm_device, O_RDWR)));
    if (!fd.ok()) {
        absl::StrAppendFormat(status, "Could not open %s : %s", kvm_device, strerror(errno));
        return ANDROID_CPU_ACCELERATION_DEV_OPEN_FAILED;
    }

    // Extract KVM version number.
    int version = ::ioctl(fd.get(), KVM_GET_API_VERSION, 0);
    if (version < 0) {
        status->assign("Could not extract KVM version: ");
        status->append(strerror(errno));
        return ANDROID_CPU_ACCELERATION_DEV_IOCTL_FAILED;
    }

    // Compare to minimum supported version
    if (version < KVM_API_VERSION) {
        absl::StrAppendFormat(status, "KVM version too old: %d (expected at least %d)\n", version,
                              KVM_API_VERSION);
        return ANDROID_CPU_ACCELERATION_DEV_OBSOLETE;
    }

    // Profit!
    absl::StrAppendFormat(status, "KVM (version %d) is installed and usable.", version);
    GlobalState* g = &gGlobals;
    ::snprintf(g->version, sizeof(g->version), "%d", version);
    return ANDROID_CPU_ACCELERATION_READY;
}

#endif  // HAVE_KVM

/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
/////
/////  Darwin Hypervisor.framework support.
/////
/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

#if HAVE_HVF

Version currentMacOSVersion(std::string* status) {
    std::string osProductVersion = base::System::Get()->GetOsName();
    return parseMacOSVersionString(osProductVersion, status);
}

AndroidCpuAcceleration ProbeHVF(std::string* status) {
    status->clear();

    AndroidCpuAcceleration res = ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT;

    // Hypervisor.framework is only supported on OS X 10.10 and above.
    auto macOsVersion = currentMacOSVersion(status);
    if (macOsVersion < Version(10, 10, 0)) {
        status->append(
                "Hypervisor.Framework is only supported"
                "on OS X 10.10 and above");
        return res;
    }

#ifndef APPLE_SILICON
    // HVF need EPT and UG
    if (!android::hasModernX86VirtualizationFeatures()) {
        status->assign(
                "CPU doesn't support EPT and/or UG features "
                "needed for Hypervisor.Framework");
        return res;
    }
#endif

    // HVF supported
    res = ANDROID_CPU_ACCELERATION_READY;

    GlobalState* g = &gGlobals;
    int maj = macOsVersion.component<Version::kMajor>();
    int min = macOsVersion.component<Version::kMinor>();
    ::snprintf(g->version, sizeof(g->version), "%d.%d", maj, min);
    status->append(absl::StrFormat("Hypervisor.Framework OS X Version %d.%d", maj, min));
    return res;
}
#endif  // HAVE_HVF

// Windows AEHD hypervisor support (Not in QEMU 8)

#if HAVE_AEHD

// Windows IOCTL code to extract AEHD version.
#define FILE_DEVICE_AEHD 0xE3E3
#define AEHD_GET_API_VERSION CTL_CODE(FILE_DEVICE_AEHD, 0x00, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * ProbeAEHDCpu: returns ANDROID_CPU_ACCELERATION_READY if the CPU supports
 * AEHD requirements.
 *
 * Otherwise returns some other AndroidCpuAcceleration status and sets
 * |status| to a user-understandable error string
 */
AndroidCpuAcceleration ProbeAEHDCpu(std::string* status) {
    char vendor_id[16];
    android_get_x86_cpuid_vendor_id(vendor_id, sizeof(vendor_id));

    auto cpu_vendor_type = android_get_x86_cpuid_vendor_id_type(vendor_id);
    if (cpu_vendor_type == VENDOR_ID_AMD && !android_get_x86_cpuid_svm_support() ||
        cpu_vendor_type == VENDOR_ID_INTEL && !android_get_x86_cpuid_vmx_support()) {
        status->assign(
                "Android Emulator requires an Intel/AMD processor with virtualization "
                "extension support.  "
                "(Virtualization extension is not supported)");
        return ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT;
    }

    return ANDROID_CPU_ACCELERATION_READY;
}

AndroidCpuAcceleration ProbeAEHD(std::string* status) {
    status->clear();

    AndroidCpuAcceleration cpu = ProbeAEHDCpu(status);
    if (cpu != ANDROID_CPU_ACCELERATION_READY) return cpu;

    base::ScopedFileHandle aehd(CreateFileA("\\\\.\\AEHD", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (aehd.ok()) goto success;
    base::ScopedFileHandle gvm(CreateFileA("\\\\.\\gvm", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!aehd.ok() && !gvm.ok()) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            status->assign(
                    "Android Emulator hypervisor driver is not installed"
                    " on this machine");
            return ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED;
        } else if (err == ERROR_ACCESS_DENIED) {
            status->assign("Unable to open AEHD device: ERROR_ACCESS_DENIED");
            return ANDROID_CPU_ACCELERATION_DEV_PERMISSION;
        }
        absl::StrAppendFormat(status,
                              "Opening Android Emulator hypervisor driver"
                              " failed: %u",
                              err);
        return ANDROID_CPU_ACCELERATION_DEV_OPEN_FAILED;
    }

success:
    int version;

    DWORD dSize = 0;
    BOOL ret = DeviceIoControl(aehd.ok() ? aehd.get() : gvm.get(), AEHD_GET_API_VERSION, NULL, 0,
                               &version, sizeof(version), &dSize, (LPOVERLAPPED)NULL);
    if (!ret) {
        DWORD err = GetLastError();
        absl::StrAppendFormat(status, "Could not extract AEHD version: %u", err);
        return ANDROID_CPU_ACCELERATION_DEV_IOCTL_FAILED;
    }

    // Profit!
    absl::StrAppendFormat(status, "AEHD (version %d.%d) is installed and usable.", version >> 16,
                          version & 0xFFFF);
    GlobalState* g = &gGlobals;
    ::snprintf(g->version, sizeof(g->version), "%d", version);
    return ANDROID_CPU_ACCELERATION_READY;
}

#endif  // HAVE_AEHD

CpuAccelerator GetCurrentCpuAccelerator() {
    GlobalState* g = &gGlobals;

    if (g->probed || g->testing) {
        return g->accel;
    }

    g->supported_accelerators = {};

    std::string status;
    AndroidCpuAcceleration status_code = ANDROID_CPU_ACCELERATION_ERROR;

#if !HAVE_KVM
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    if (!android::hasModernX86VirtualizationFeatures()) {
        // TODO: Support snapshots when UG is not supported.
        LOG(WARNING) << "Quick Boot / Snapshots not supported on this "
                        "machine. "
                        "A CPU with EPT + UG features is currently needed. "
                        "We will address this in a future release.";
        // featurecontrol::setEnabledOverride(featurecontrol::FastSnapshotV1,
        // false);
    }
#endif
#endif

#if HAVE_KVM
    status_code = ProbeKVM(&status);
    if (status_code == ANDROID_CPU_ACCELERATION_READY) {
        g->accel = CPU_ACCELERATOR_KVM;
        g->supported_accelerators[CPU_ACCELERATOR_KVM] = true;
    }
#elif HAVE_HVF || HAVE_WHPX || HAVE_AEHD
    auto hv_status = GetHyperVStatus();
    if (hv_status.first == ANDROID_HYPERV_RUNNING) {
#if HAVE_WHPX
        auto ver = android::base::Win32Utils::getWindowsVersion();
        if (ver && (ver->dwMajorVersion > 10 ||
                    (ver->dwMajorVersion == 10 && ver->dwBuildNumber >= 17134))) {
            status_code = ProbeWHPX(&status);
            if (status_code == ANDROID_CPU_ACCELERATION_READY) {
                g->accel = CPU_ACCELERATOR_WHPX;
                g->supported_accelerators[CPU_ACCELERATOR_WHPX] = true;
            } else {
                status = "Hyper-V detected and Windows Hypervisor Platform "
                         "is available. "
                         "Please ensure both the \"Hyper-V\" and \"Windows "
                         "Hypervisor Platform\" "
                         "features are enabled in \"Turn Windows features "
                         "on or off\".";
            }
        } else {
            status = "Hyper-V detected and Windows Hypervisor Platform is "
                     "not available. "
                     "Please upgrade to Windows 10 1803 or later and "
                     "ensure both "
                     "the \"Hyper-V\" and \"Windows Hypervisor Platform\" "
                     "features are enabled "
                     "in \"Turn Windows features on or off\".";
            status_code = ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED;
        }
#endif
    } else {

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        char vendor_id[16];
        CpuVendorIdType vid_type;

        android_get_x86_cpuid_vendor_id(vendor_id, sizeof(vendor_id));
        vid_type = android_get_x86_cpuid_vendor_id_type(vendor_id);
        if (vid_type != VENDOR_ID_AMD && vid_type != VENDOR_ID_INTEL) {
            absl::StrAppendFormat(&status,
                                  "Android Emulator requires an Intel or AMD processor "
                                  "with "
                                  "virtualization extension support.  Your CPU: '%s'",
                                  vendor_id);
            status_code = ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT;
        } else {
#if HAVE_AEHD
            status_code = ProbeAEHD(&status);
            if (status_code == ANDROID_CPU_ACCELERATION_READY) {
                g->accel = CPU_ACCELERATOR_AEHD;
                g->supported_accelerators[CPU_ACCELERATOR_AEHD] = true;
            }
#endif
        }
#endif

#if HAVE_WHPX && !HAVE_AEHD
        if (status_code != ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT) {
            status = absl::StrFormat(
                    "WHPX CPU acceleration is unavailable (%s). Please ensure "
                    "both the \"Hyper-V\" and \"Windows Hypervisor Platform\" "
                    "features are enabled in \"Turn Windows features on or "
                    "off\".",
                    hv_status.second);
            status_code = ANDROID_CPU_ACCELERATION_ACCEL_NOT_INSTALLED;
        }
#endif

#if HAVE_HVF

        std::string status_hvf;
        AndroidCpuAcceleration status_code_hvf = ProbeHVF(&status_hvf);
        if (status_code_hvf == ANDROID_CPU_ACCELERATION_READY) {
#ifdef APPLE_SILICON
            g->accel = CPU_ACCELERATOR_HVF;
            g->supported_accelerators[CPU_ACCELERATOR_KVM] = false;
#endif
            g->supported_accelerators[CPU_ACCELERATOR_HVF] = true;
            // TODO(jansene): Switch to HVF as default option if/when
            // appropriate.
            if (status_code != ANDROID_CPU_ACCELERATION_READY) {
                g->accel = CPU_ACCELERATOR_HVF;
                status_code = status_code_hvf;
                status = status_hvf;
            }
        }
#endif  // HAVE_HVF
    }
#else   // !HAVE_KVM && !(HAVE_HVF || HAVE_AEHD || HAVE_WHPX)
    status = "This system does not support CPU acceleration.";
#endif  // !HAVE_KVM && !(HAVE_HVF || HAVE_AEHD || HAVE_WHPX)

    // cache status
    g->probed = true;
    g->status_code = status_code;
    ::snprintf(g->status, sizeof(g->status), "%s", status.c_str());

    return g->accel;
}

void ResetCurrentCpuAccelerator(CpuAccelerator accel) {
    GlobalState* g = &gGlobals;
    g->accel = accel;
}

bool GetCurrentAcceleratorSupport(CpuAccelerator type) {
    GlobalState* g = &gGlobals;

    if (!g->probed && !g->testing) {
        // Force detection of the current CPU accelerator.
        GetCurrentCpuAccelerator();
    }

    return g->supported_accelerators[type];
}

std::string GetCurrentCpuAcceleratorStatus() {
    GlobalState* g = &gGlobals;

    if (!g->probed && !g->testing) {
        // Force detection of the current CPU accelerator.
        GetCurrentCpuAccelerator();
    }

    return {g->status};
}

AndroidCpuAcceleration GetCurrentCpuAcceleratorStatusCode() {
    GlobalState* g = &gGlobals;

    if (!g->probed && !g->testing) {
        // Force detection of the current CPU accelerator.
        GetCurrentCpuAccelerator();
    }

    return g->status_code;
}

void SetCurrentCpuAcceleratorForTesting(CpuAccelerator accel, AndroidCpuAcceleration status_code,
                                        const char* status, const char* version) {
    GlobalState* g = &gGlobals;

    g->testing = true;
    g->accel = accel;
    g->status_code = status_code;
    ::snprintf(g->status, sizeof(g->status), "%s", status);
    ::snprintf(g->version, sizeof(g->version), "%s", version ? version : "Unknown version");
}

std::pair<AndroidHyperVStatus, std::string> GetHyperVStatus() {
#ifndef _WIN32
    // this was easy
    return std::make_pair(ANDROID_HYPERV_ABSENT, "Hyper-V runs only on Windows");
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)  // Windows ARM
    // There is no "cpuid" for Windows ARM to check Hyper-V. However, according
    // to Windows ARM devices (X Elite processors) experience, Hyper-V is always
    // on.
    return std::make_pair(ANDROID_HYPERV_RUNNING, "Hyper-V is enabled");
#else                                                                  // _WIN32
    char vendor_id[16];
    android_get_x86_cpuid_vmhost_vendor_id(vendor_id, sizeof(vendor_id));
    const auto vmType = android_get_x86_cpuid_vendor_vmhost_type(vendor_id);
    if (vmType == VENDOR_VM_HYPERV) {
        // The simple part: there's currently a Hyper-V hypervisor running.
        // Let's find out if we're in a host or guest.
        // Hyper-V has a CPUID function 0x40000003 which returns a set of
        // supported features in ebx register. Ebx[0] is a 'CreatePartitions'
        // feature bit, which is only enabled in host as of now
        uint32_t ebx;
        android_get_x86_cpuid(0x40000003, 0, nullptr, &ebx, nullptr, nullptr);
        if (ebx & 0x1) {
            return std::make_pair(ANDROID_HYPERV_RUNNING, "Hyper-V is enabled");
        } else {
            // TODO: update this part when Hyper-V officially implements
            // nesting support
            return std::make_pair(ANDROID_HYPERV_ABSENT,
                                  "Running in a guest Hyper-V VM, Hyper-V is not supported");
        }
    } else if (vmType != VENDOR_VM_NOTVM) {
        // some CPUs may return something strange even if we're not under a VM,
        // so let's double-check it
        if (android_get_x86_cpuid_is_vcpu()) {
            return std::make_pair(ANDROID_HYPERV_ABSENT,
                                  "Running in a guest VM, Hyper-V is not supported");
        }
    }

    // Now the hard part: we know Hyper-V is not running. We need to find out if
    // it's installed.
    // The only reliable way of detecting it is to query the list of optional
    // features through the WMI and check if Hyper-V is installed there. But it
    // runs for tens of seconds, and can be even slower under memory pressure.
    // Instead, let's take a shortcut: Hyper-V engine file is vmms.exe. If it's
    // installed it has to be in system32 directory. So we can just check if
    // it's there.
    android::base::Win32UnicodeString winPath(MAX_PATH);
    UINT size = ::GetWindowsDirectoryW(winPath.data(), winPath.size() + 1);
    if (size > winPath.size()) {
        winPath.resize(size);
        size = ::GetWindowsDirectoryW(winPath.data(), winPath.size() + 1);
    }
    if (size == 0) {
        // Last chance call
        winPath = L"C:\\Windows";
    } else if (winPath.size() != size) {
        winPath.resize(size);
    }

    fs::path winPathStd(winPath.toString());
#ifdef __x86_64__
    fs::path sysPath = winPathStd / "System32";
#else
    // For the 32-bit application everything's a little bit more complicated:
    // the main Hyper-V executable is 64-bit on 64-bit OS; but we're running
    // under file system redirector which redirects access into 32-bit System32.
    // even more: if we're running under 32-bit Windows, there's no 64-bit
    // directory. So we need to select the proper one here.
    // First, try a symlink which only exists on 64-bit Windows and leads to
    // the native, 64-bit directory
    fs::path sysPath = winPathStd / "Sysnative";

    // check only if path exists: path_is_dir() would fail as it's not a
    // directory but a symlink
    if (!base::file::exists(sysPath)) {
        // If it doesn't exist, we're on 32-bit Windows and let's just use
        // the plain old System32
        sysPath = winPathStd / "System32";
    }
#endif
    fs::path hyperVExe = sysPath / "vmms.exe";

    if (base::file::is_file(hyperVExe)) {
        // hyper-v is installed but not running
        return std::make_pair(ANDROID_HYPERV_INSTALLED, "Hyper-V is disabled");
    }

    // not a slightest sign of it
    return std::make_pair(ANDROID_HYPERV_ABSENT, "Hyper-V is not installed");
#endif  // _WIN32
}

std::pair<AndroidCpuInfoFlags, std::string> GetCpuInfo() {
    int flags = 0;
    std::string status;

    char vendor_id[13];
    android_get_x86_cpuid_vendor_id(vendor_id, sizeof(vendor_id));
    switch (android_get_x86_cpuid_vendor_id_type(vendor_id)) {
    case VENDOR_ID_AMD:
        flags |= ANDROID_CPU_INFO_AMD;
        status += "AMD CPU\n";
        if (android_get_x86_cpuid_svm_support()) {
            flags |= ANDROID_CPU_INFO_VIRT_SUPPORTED;
            status += "Virtualization is supported\n";
        }
        break;
    case VENDOR_ID_INTEL:
        flags |= ANDROID_CPU_INFO_INTEL;
        status += "Intel CPU\n";
        if (android_get_x86_cpuid_vmx_support()) {
            flags |= ANDROID_CPU_INFO_VIRT_SUPPORTED;
            status += "Virtualization is supported\n";
        }
        break;
    default:
#ifdef APPLE_SILICON
        flags |= ANDROID_CPU_INFO_APPLE;
        status += "Apple CPU\n";
        status += "Virtualization is supported\n";  // we have not found
                                                    // otherwise on apple cpu
#else
        flags |= ANDROID_CPU_INFO_OTHER;
        status += "Other CPU: ";
        status += vendor_id;
#endif
        status += '\n';
        break;
    }

    if (android_get_x86_cpuid_is_vcpu()) {
        if (android_get_x86_cpuid_hyperv_root()) {
            status += "Hyper-V Root Partition\n";
        } else {
            status += "Inside a VM\n";
            flags |= ANDROID_CPU_INFO_VM;
        }
    } else {
        status += "Bare metal\n";
    }

    flags |= ANDROID_CPU_INFO_64_BIT;
    status += "64-bit CPU\n";

    return std::make_pair(static_cast<AndroidCpuInfoFlags>(flags), status);
}

std::string CpuAcceleratorToString(CpuAccelerator type) {
    switch (type) {
    case CPU_ACCELERATOR_KVM:
        return "kvm";
    case CPU_ACCELERATOR_HAX:
        return "hax (deprecated)";
    case CPU_ACCELERATOR_HVF:
        return "hvf";
    case CPU_ACCELERATOR_WHPX:
        return "whpx";
    case CPU_ACCELERATOR_AEHD:
        return "aehd";
    case CPU_ACCELERATOR_NONE:
    case CPU_ACCELERATOR_MAX:
        return "tcg";
    }
    return "";
}

Version GetCurrentCpuAcceleratorVersion() {
    GlobalState* g = &gGlobals;

    if (!g->probed && !g->testing) {
        // Force detection of the current CPU accelerator.
        GetCurrentCpuAccelerator();
    }
    return (g->accel != CPU_ACCELERATOR_NONE) ? Version(g->version) : Version::invalid();
}

Version parseMacOSVersionString(const std::string& str, std::string* status) {
    size_t pos = str.rfind(' ');
    if (strncmp("Error: ", str.c_str(), 7) == 0 || pos == std::string::npos) {
        status->append(absl::StrFormat("Internal error: failed to parse OS version '%s'", str));
        return {0, 0, 0};
    }

    auto ver = Version(std::string(str.c_str() + pos + 1, str.size() - pos - 1));
    if (!ver.isValid()) {
        status->append(absl::StrFormat("Internal error: failed to parse OS version '%s'", str));
        return {0, 0, 0};
    }

    return ver;
}

#ifdef __x86_64__
X86Cpuid GetX86Cpuid() {
    uint32_t cpuid_mfs;
    android_get_x86_cpuid(1, 0, &cpuid_mfs, nullptr, nullptr, nullptr);
    return {
        .cpuid_stepping = cpuid_mfs & 0x0000000f,
        .cpuid_model = (cpuid_mfs & 0x000000f0) >> 4,
        .cpuid_family = (cpuid_mfs & 0x00000f00) >> 8,
        .cpuid_type = (cpuid_mfs & 0x00003000) >> 12,
        .cpuid_extmodel = (cpuid_mfs & 0x000f0000) >> 16,
        .cpuid_extfamily = (cpuid_mfs & 0x0ff00000) >> 20,
    };
}
#endif

}  // namespace android
