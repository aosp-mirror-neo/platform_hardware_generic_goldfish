// Copyright 2023 The Android Open Source Project
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
#include "android/goldfish/config_dirs.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

#include "absl/log/check.h"
#include "absl/log/log.h"

#include "android/base/system.h"
#include "goldfish/file/file.h"

namespace android::goldfish {

namespace fs = std::filesystem;

using android::base::System;

namespace {

// Name of the Android configuration directory under $HOME.
constexpr std::string_view kAndroidSubDir = ".android";
// Subdirectory for AVD data files.
constexpr std::string_view kAvdSubDir = "avd";

// Returns true if `path` is a non-empty, readable directory.
bool IsReadableDir(const fs::path& path) {
    return !path.empty() && base::file::is_dir(path) && base::file::can_read(path);
}

}  // namespace

// static
auto ConfigDirs::GetUserDirectory() -> fs::path {
    fs::path home = System::Get()->EnvGet("ANDROID_EMULATOR_HOME");
    if (!home.empty()) {
        return home;
    }

    // New key: ANDROID_PREFS_ROOT
    home = System::Get()->EnvGet("ANDROID_PREFS_ROOT");
    if (!home.empty()) {
        // In v1.9 emulator was changed to use $ANDROID_SDK_HOME/.android
        // directory, but Android Studio has always been using $ANDROID_SDK_HOME
        // directly. Put a workaround here to make sure it works both ways,
        // preferring the one from AS.
        auto home_new_way = fs::path(home) / kAndroidSubDir;
        return base::file::is_dir(home_new_way) ? home_new_way : home;
    }  // Old key that is deprecated (ANDROID_SDK_HOME)
    home = System::Get()->EnvGet("ANDROID_SDK_HOME");
    if (!home.empty()) {
        auto home_old_way = fs::path(home) / kAndroidSubDir;
        return base::file::exists(home_old_way) ? home_old_way : home;
    }

    home = android::base::System::Get()->GetHomeDirectory();
    if (home.empty()) {
        return fs::temp_directory_path();
    }
    return home / kAndroidSubDir;
}

// static
auto ConfigDirs::GetAvdRootDirectory() -> fs::path {
    // The search order here should match that in AndroidLocation.java
    // in Android Studio. Otherwise, Studio and the Emulator may find
    // different AVDs. Or one may find an AVD when the other doesn't.
    fs::path avd_root = System::Get()->EnvGet("ANDROID_AVD_HOME");
    if (!avd_root.empty() && base::file::is_dir(avd_root)) {
        return avd_root;
    }

    // No luck with ANDROID_AVD_HOME, try ANDROID_PREFS_ROOT/ANDROID_SDK_HOME
    for (std::string_view env_var : {"ANDROID_PREFS_ROOT", "ANDROID_SDK_HOME"}) {
        avd_root = GetAvdRootDirectoryWithPrefsRoot(System::Get()->EnvGet(env_var));
        if (!avd_root.empty()) {
            return avd_root;
        }
    }

    // ANDROID_PREFS_ROOT/ANDROID_SDK_HOME is defined but bad. In this case,
    // Android Studio tries $TEST_TMPDIR, $USER_HOME, and
    // $HOME. We'll do the same.
    for (std::string_view env_var : {"TEST_TMPDIR", "USER_HOME", "HOME"}) {
        const fs::path env_dir = System::Get()->EnvGet(env_var);
        if (env_dir.empty()) {
            continue;
        }
        const fs::path candidate_root = env_dir / kAndroidSubDir;
        if (IsValidAvdRoot(candidate_root)) {
            return candidate_root / kAvdSubDir;
        }
    }

    // No luck with ANDROID_AVD_HOME, ANDROID_SDK_HOME,
    // TEST_TMPDIR, USER_HOME, or HOME. Try even more.
    return GetUserDirectory() / kAvdSubDir;
}

// static
auto ConfigDirs::GetSdkRootDirectoryByEnv(bool verbose) -> fs::path {
    LOG_IF(INFO, verbose) << "checking ANDROID_HOME for valid sdk root.";
    std::string sdk_root = System::Get()->EnvGet("ANDROID_HOME");
    LOG_IF(INFO, verbose) << "ANDROID_HOME: " << sdk_root;

    if (!sdk_root.empty() && IsValidSdkRoot(sdk_root, verbose)) {
        return sdk_root;
    }

    LOG_IF(INFO, verbose) << "checking ANDROID_SDK_ROOT for valid sdk root.";

    // ANDROID_HOME is not good. Try ANDROID_SDK_ROOT.
    sdk_root = System::Get()->EnvGet("ANDROID_SDK_ROOT");
    if (!sdk_root.empty()) {
        // Unquote a possibly "quoted" path.
        if (sdk_root[0] == '"') {
            DCHECK(sdk_root.back() == '"');
            sdk_root.erase(0, 1);
            sdk_root.pop_back();
        }
        if (IsValidSdkRoot(sdk_root, verbose)) {
            return sdk_root;
        }
    }

    LOG_IF(WARNING, verbose) << "ANDROID_SDK_ROOT is missing.";
    return {};
}

auto ConfigDirs::GetSdkRootDirectoryByPath(const fs::path& launcher_dir, bool verbose) -> fs::path {
    fs::path sdk_root = launcher_dir;
    for (int i = 0; i < 3; ++i) {
        sdk_root = sdk_root.parent_path();
        LOG_IF(INFO, verbose) << "guessed sdk root: " << sdk_root.string();
        if (IsValidSdkRoot(sdk_root, verbose)) {
            return sdk_root;
        }
        LOG_IF(INFO, verbose) << "guessed sdk root " << sdk_root.string()
                              << " does not seem to be valid";
    }
    LOG_IF(WARNING, verbose) << "invalid sdk root:" << sdk_root.string();
    return {};
}

fs::path ConfigDirs::GetSdkRootDefault(bool verbose) {
    auto home = android::base::System::Get()->GetHomeDirectory();
    if (home.empty()) {
        LOG_IF(WARNING, verbose) << "User home directory not known";
        return home;
    }
    fs::path sdk_root;
#if defined(_WIN32)
    sdk_root = home / "AppData" / "Local" / "Android" / "Sdk";
#elif defined(__linux__)
    sdk_root = home / "Android" / "Sdk";
#elif defined(__APPLE__)
    sdk_root = home / "Library" / "Android" / "sdk";
#endif
    if (IsValidSdkRoot(sdk_root, verbose)) {
        return sdk_root;
    }
    return {};
}

// static
auto ConfigDirs::GetSdkRootDirectory(const fs::path& launcher_dir, bool verbose) -> fs::path {
    auto sdk_root = GetSdkRootDirectoryByEnv(verbose);
    if (!sdk_root.empty()) {
        return sdk_root;
    }

    LOG_IF(WARNING, verbose) << "Cannot find valid sdk root from environment "
                                "variable ANDROID_HOME nor ANDROID_SDK_ROOT,"
                                "trying to infer from emulator's path.";
    // Otherwise, infer from the path of the emulator's binary.
    sdk_root = GetSdkRootDirectoryByPath(launcher_dir, verbose);
    if (!sdk_root.empty()) {
        return sdk_root;
    }

    // Fall back to default installation location.
    return GetSdkRootDefault(verbose);
}

// static
auto ConfigDirs::IsValidSdkRoot(const fs::path& root_path, bool verbose) -> bool {
    if (root_path.empty()) {
        LOG_IF(WARNING, verbose) << "empty sdk root";
        return false;
    }
    if (!base::file::is_dir(root_path)) {
        LOG_IF(WARNING, verbose) << root_path << " is not a directory, and cannot be sdk root";
        return false;
    }
    if (!base::file::can_read(root_path)) {
        LOG_IF(WARNING, verbose) << root_path << " is not readable, and cannot be sdk root";
        return false;
    }
    const fs::path platform_tools_path = root_path / "platform-tools";
    if (!base::file::is_dir(platform_tools_path)) {
        LOG_IF(WARNING, verbose) << "platform-tools subdirectory is missing under " << root_path
                                 << ", please install it";
        return false;
    }

    return true;
}

// static
auto ConfigDirs::IsValidAvdRoot(const fs::path& avd_path) -> bool {
    return IsReadableDir(avd_path) && IsReadableDir(avd_path / kAvdSubDir);
}

auto ConfigDirs::GetAvdRootDirectoryWithPrefsRoot(const fs::path& path) -> fs::path {
    if (path.empty()) {
        return {};
    }

    // ANDROID_PREFS_ROOT is defined
    if (IsValidAvdRoot(path)) {
        // ANDROID_PREFS_ROOT is good
        return path / kAvdSubDir;
    }

    const fs::path avd_root = path / kAndroidSubDir;
    if (IsValidAvdRoot(avd_root)) {
        // ANDROID_PREFS_ROOT/.android is good
        return avd_root / kAvdSubDir;
    }

    return {};
}

}  // namespace android::goldfish
