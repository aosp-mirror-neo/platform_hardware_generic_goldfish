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

#include "goldfish/file/shared_file_lock.h"

#include <filesystem>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "emulator/libs/file/shared_file_lock_internal.h"

namespace android::base::file {

namespace internal {

absl::Status FormatSharedLockTimeoutError(const std::filesystem::path& lock_path,
                                          LockShareMode mode,
                                          absl::Duration timeout) {
    return absl::UnavailableError(absl::StrCat(
            "Failed to acquire ",
            mode == LockShareMode::kSharedRead ? "shared" : "exclusive",
            " lock on '", lock_path.string(), "' within ",
            absl::FormatDuration(timeout), ": held by another process"));
}

}  // namespace internal

ScopedSharedFileLock::ScopedSharedFileLock(std::filesystem::path lock_path,
                                           LockShareMode mode,
                                           LockHandle handle)
        : lock_path_(std::move(lock_path)), mode_(mode), handle_(std::move(handle)) {}

}  // namespace android::base::file
