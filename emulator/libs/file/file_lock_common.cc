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

#include "goldfish/file/file_lock.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"
#include "emulator/libs/file/file_lock_internal.h"

namespace android::base::file {

namespace internal {

std::optional<int> ParsePidString(std::string_view raw) {
    if (const size_t nul = raw.find('\0'); nul != std::string_view::npos) {
        raw = raw.substr(0, nul);
    }
    raw = absl::StripAsciiWhitespace(raw);
    int pid = 0;
    if (raw.empty() || !absl::SimpleAtoi(raw, &pid) || pid <= 0) {
        return std::nullopt;
    }
    return pid;
}

}  // namespace internal

std::filesystem::path ScopedFileLock::LockPathForTarget(
        const std::filesystem::path& target_path) {
    std::filesystem::path lock_path = target_path;
    lock_path += ".lock";
    return lock_path;
}

bool ScopedFileLock::IsKnownLockHolderExecutable(std::string_view exe_path,
                                                 std::string_view current_exe_stem) {
    exe_path = absl::StripAsciiWhitespace(exe_path);
    exe_path = absl::StripSuffix(exe_path, " (deleted)");
    if (exe_path.empty()) {
        return false;
    }

    const size_t last_sep = exe_path.find_last_of("/\\");
    const std::string_view filename =
            last_sep == std::string_view::npos ? exe_path : exe_path.substr(last_sep + 1);
    if (filename.empty()) {
        return false;
    }

    std::string_view stem = filename;
    if (absl::EndsWithIgnoreCase(stem, ".exe")) {
        stem.remove_suffix(4);
    }

    if (!current_exe_stem.empty() &&
        (stem == current_exe_stem || filename == current_exe_stem)) {
        return true;
    }

    return absl::StartsWith(stem, "emulator") ||
           absl::StartsWith(stem, "qemu-system-") ||
           stem == "launcher";
}

ScopedFileLock::ScopedFileLock(std::filesystem::path lock_path, LockHandle handle)
        : lock_path_(std::move(lock_path)), handle_(std::move(handle)) {}

}  // namespace android::base::file
