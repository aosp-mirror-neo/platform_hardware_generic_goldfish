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

#pragma once

#include <optional>
#include <string>

#include "android/cpu/cpu_accelerator.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#undef ERROR

#include "android/base/win32_utils.h"
#endif

namespace android {

#ifdef _WIN32
namespace internal {

struct WhpxOps {
    HMODULE (*load_library)(LPCWSTR lib_file_name) = [](LPCWSTR lib_file_name) -> HMODULE {
        return ::LoadLibraryW(lib_file_name);
    };
    void* (*get_proc_address)(HMODULE module, LPCSTR proc_name) = [](HMODULE module,
                                                                     LPCSTR proc_name) -> void* {
        return reinterpret_cast<void*>(::GetProcAddress(module, proc_name));
    };
    BOOL (*free_library)(HMODULE module) = [](HMODULE module) -> BOOL {
        return ::FreeLibrary(module);
    };
    std::optional<_OSVERSIONINFOEXW> (*get_windows_version)() =
            &android::base::Win32Utils::getWindowsVersion;
};

}  // namespace internal

AndroidCpuAcceleration ProbeWHPX(std::string* status);
AndroidCpuAcceleration ProbeWHPX(std::string* status, const internal::WhpxOps& ops);
#endif

#ifdef __linux__
AndroidCpuAcceleration ProbeKVM(std::string* status);
#endif

#ifdef __APPLE__
AndroidCpuAcceleration ProbeHVF(std::string* status);
#endif

}  // namespace android
