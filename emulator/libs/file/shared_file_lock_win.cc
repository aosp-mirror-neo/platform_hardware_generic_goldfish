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

#if defined(UNICODE) || defined(_UNICODE)
#error We rely on UTF-8 codepage in the manifest
#endif

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"
#include "android/base/scoped_file_handle.h"
#include "android/base/win32_utils.h"
#include "emulator/libs/file/shared_file_lock_internal.h"

namespace android::base::file {

absl::StatusOr<ScopedSharedFileLock> ScopedSharedFileLock::Acquire(
        const std::filesystem::path& lock_path,
        LockShareMode mode,
        absl::Duration timeout,
        const SharedFileLockOptions& options) {
    const std::string lock_path_str = lock_path.string();
    const absl::Time deadline = options.clock.TimeNow() + std::max(timeout, absl::ZeroDuration());
    const absl::Duration poll_interval =
            std::max(options.poll_interval, absl::Milliseconds(1));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = FALSE;

    for (;;) {
        android::base::ScopedFileHandle handle;
        if (mode == LockShareMode::kSharedRead) {
            handle = android::base::ScopedFileHandle(
                    ::CreateFileA(lock_path_str.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!handle.ok() && ::GetLastError() == ERROR_FILE_NOT_FOUND) {
                android::base::ScopedFileHandle create_handle(
                        ::CreateFileA(lock_path_str.c_str(), /*dwDesiredAccess=*/0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
                create_handle.close();
                handle = android::base::ScopedFileHandle(
                        ::CreateFileA(lock_path_str.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            }
        } else {
            handle = android::base::ScopedFileHandle(
                    ::CreateFileA(lock_path_str.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  /*dwShareMode=*/0, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
        }

        if (handle.ok()) {
            return ScopedSharedFileLock(lock_path, mode, std::move(handle));
        }

        const DWORD last_err = ::GetLastError();
        if (last_err != ERROR_SHARING_VIOLATION && last_err != ERROR_LOCK_VIOLATION) {
            return absl::InternalError(absl::StrCat(
                    "Failed to open shared lock file '", lock_path_str,
                    "': ", android::base::Win32Utils::getErrorString(last_err)));
        }

        const absl::Duration remaining = deadline - options.clock.TimeNow();
        if (remaining <= absl::ZeroDuration()) {
            return internal::FormatSharedLockTimeoutError(lock_path, mode, timeout);
        }

        options.clock.Sleep(std::min(remaining, poll_interval));
    }
}

}  // namespace android::base::file
