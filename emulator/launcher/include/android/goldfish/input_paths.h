// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <filesystem>

#include "absl/status/statusor.h"

#include "android/cmdline_option.h"

namespace android::goldfish {

namespace fs = std::filesystem;

// These paths represent read-only inputs providing configuration and data to the emulator
// All paths should be canonicalised so as not to use
struct EmulatorPaths {
    fs::path launcher_binary;

    fs::path launcher_directory;
    fs::path binary_directory;
    fs::path library_directory;
    fs::path lib64_directory;
    fs::path bios_directory;
    fs::path advanced_features;
    fs::path feature_patterns;

    fs::path qemu_system_x86_binary;
    fs::path qemu_system_arm_binary;
    fs::path qemu_system_riscv_binary;
    fs::path netsim_binary;
    fs::path crashpad_handler_binary;
    fs::path fishtank_binary;

    bool HasNetsim() const { return !netsim_binary.empty(); }
    bool HasFishtank() const { return !fishtank_binary.empty(); }
};

struct UserPaths {
    fs::path user_directory;
    fs::path avd_directory;
    fs::path sdk_directory;
    fs::path discovery_directory;
    fs::path tmp_directory;
};

struct SystemImagePaths {
    fs::path build_properties;
    fs::path vendor_build_properties;
    fs::path advanced_features;
    fs::path verified_boot_params;

    fs::path data_dir;

    fs::path kernel_cmdline;
    fs::path kernel_image;
    fs::path ramdisk_image;
    fs::path system_image;
    fs::path vendor_image;
    fs::path encryption_key_image;
};

// TODO
/*struct AvdContentPaths {
    fs::path kernel_image;
    fs::path ramdisk_image;

    fs::path system_image;
    fs::path vendor_image;
    fs::path encryption_key_image;
    fs::path data_image;
    fs::path cache_image;
    fs::path sdcard_image;
};*/

absl::StatusOr<EmulatorPaths> ResolveEmulatorPaths(bool verbose);
absl::StatusOr<UserPaths> ResolveUserPaths(const fs::path& launcher_dir, bool verbose);
absl::StatusOr<SystemImagePaths> ResolveSystemImagePaths(const std::vector<fs::path>& search_paths,
                                                         const AndroidOptions& opts, bool android_build);

}  // namespace android::goldfish
