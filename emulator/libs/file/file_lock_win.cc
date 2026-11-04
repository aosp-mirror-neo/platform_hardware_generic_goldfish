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

#if defined(UNICODE) || defined(_UNICODE)
#error We rely on UTF-8 codepage in the manifest
#endif

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"
#include "android/base/scoped_file_handle.h"
#include "android/base/win32_utils.h"
#include "emulator/libs/file/file_lock_internal.h"
#include "goldfish/file/file.h"

namespace android::base::file {
namespace {

using internal::ParsePidString;

constexpr absl::Duration kPollStepIncrement = absl::Milliseconds(50);
constexpr absl::Duration kMaxPollStep = absl::Milliseconds(200);
constexpr int kMaxImmediateRetries = 16;

// Serializes directory and PID-file operations across threads within the same
// process so in-process lock release never overlaps CreateDirectoryA,
// CreateFileA, or ReadPidFileWin.
absl::Mutex& ProcessLockTableMutex() {
    static absl::NoDestructor<absl::Mutex> mu;
    return *mu;
}

std::optional<int> ReadPidFileWin(const std::string& pid_file_str) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = FALSE;

    android::base::ScopedFileHandle read_handle(
            ::CreateFileA(pid_file_str.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!read_handle.ok()) {
        return std::nullopt;
    }

    char buf[32] = {};
    DWORD bytes_read = 0;
    if (!::ReadFile(read_handle.get(), buf, sizeof(buf) - 1, &bytes_read, nullptr) ||
        bytes_read == 0) {
        return std::nullopt;
    }
    return ParsePidString(std::string_view(buf, static_cast<size_t>(bytes_read)));
}

}  // namespace

std::optional<int> ScopedFileLock::ReadLockHolderPid(const std::filesystem::path& target_path) {
    const std::filesystem::path lock_dir = LockPathForTarget(target_path);
    const std::string pid_file_str = (lock_dir / "pid").string();
    const absl::MutexLock lock(ProcessLockTableMutex());
    return ReadPidFileWin(pid_file_str);
}

absl::StatusOr<ScopedFileLock> ScopedFileLock::Acquire(
        const std::filesystem::path& target_path,
        absl::Duration timeout,
        const FileLockOptions& options) {
    std::filesystem::path lock_dir = LockPathForTarget(target_path);
    std::filesystem::path pid_file = lock_dir / "pid";
    const std::string lock_dir_str = lock_dir.string();
    const std::string pid_file_str = pid_file.string();

    const absl::Time deadline = options.clock.TimeNow() + std::max(timeout, absl::ZeroDuration());
    absl::Duration poll_step = absl::ZeroDuration();
    int immediate_retries = 0;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = FALSE;

    for (;;) {
        std::optional<int> holder_pid;
        {
            const absl::MutexLock lock(ProcessLockTableMutex());
            if (android::base::file::is_file(lock_dir)) {
                // Remove stale POSIX regular-file lock artifact on shared filesystems.
                (void)::DeleteFileA(lock_dir_str.c_str());
            }

            bool dir_ready = true;
            if (!::CreateDirectoryA(lock_dir_str.c_str(), nullptr)) {
                const DWORD dir_err = ::GetLastError();
                if (dir_err == ERROR_ACCESS_DENIED) {
                    // Another process is releasing `lock_dir` and NTFS holds it
                    // in DELETE_PENDING until the last handle closes.
                    dir_ready = false;
                    if (immediate_retries < kMaxImmediateRetries) {
                        ++immediate_retries;
                        continue;
                    }
                } else if (dir_err != ERROR_ALREADY_EXISTS) {
                    return absl::InternalError(absl::StrCat(
                            "Failed to create lock directory '", lock_dir_str,
                            "': ", android::base::Win32Utils::getErrorString(dir_err)));
                }
            }

            if (dir_ready) {
                android::base::ScopedFileHandle handle(
                        ::CreateFileA(pid_file_str.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (handle.ok()) {
                    const int my_pid = options.current_pid_override.value_or(
                            static_cast<int>(::GetCurrentProcessId()));
                    const std::string pid_str = absl::StrCat(my_pid);
                    DWORD written = 0;
                    if (!::WriteFile(handle.get(), pid_str.data(),
                                     static_cast<DWORD>(pid_str.size()), &written, nullptr) ||
                        written != static_cast<DWORD>(pid_str.size())) {
                        const DWORD write_err = ::GetLastError();
                        handle.close();
                        (void)::DeleteFileA(pid_file_str.c_str());
                        (void)::RemoveDirectoryA(lock_dir_str.c_str());
                        return absl::InternalError(absl::StrCat(
                                "Failed to write PID to '", pid_file_str,
                                "': ", android::base::Win32Utils::getErrorString(write_err)));
                    }
                    (void)::FlushFileBuffers(handle.get());
                    std::filesystem::path owned_dir = lock_dir;
                    return ScopedFileLock(
                            std::move(lock_dir),
                            LockHandle(handle.release(),
                                       LockDeleter(std::move(owned_dir), std::move(pid_file))));
                }

                const DWORD last_err = ::GetLastError();
                if (last_err == ERROR_PATH_NOT_FOUND || last_err == ERROR_FILE_NOT_FOUND) {
                    // Another process released the lock (deleting `pid_file` and
                    // removing `lock_dir`) between `CreateDirectoryA` and
                    // `CreateFileA`. Retry immediately up to `kMaxImmediateRetries`
                    // to recreate `lock_dir`.
                    if (immediate_retries < kMaxImmediateRetries) {
                        ++immediate_retries;
                        continue;
                    }
                } else if (last_err != ERROR_SHARING_VIOLATION &&
                           last_err != ERROR_ACCESS_DENIED &&
                           last_err != ERROR_LOCK_VIOLATION) {
                    (void)::RemoveDirectoryA(lock_dir_str.c_str());
                    return absl::InternalError(absl::StrCat(
                            "Failed to open lock file '", pid_file_str,
                            "': ", android::base::Win32Utils::getErrorString(last_err)));
                }

                holder_pid = ReadPidFileWin(pid_file_str);
                if (last_err == ERROR_ACCESS_DENIED) {
                    // Clear FILE_ATTRIBUTE_READONLY if a crashed process or external
                    // tool left an unlocked stale `pid` file read-only so
                    // CREATE_ALWAYS can overwrite it on the next retry.
                    (void)::SetFileAttributesA(pid_file_str.c_str(), FILE_ATTRIBUTE_NORMAL);
                }
            }
        }

        const absl::Duration remaining = deadline - options.clock.TimeNow();
        if (remaining <= absl::ZeroDuration()) {
            {
                const absl::MutexLock lock(ProcessLockTableMutex());
                // Remove `lock_dir` if it is empty (e.g. recreated after a
                // cross-process holder released); no-op when `pid` exists inside.
                (void)::RemoveDirectoryA(lock_dir_str.c_str());
            }
            const std::string holder_suffix =
                    holder_pid.has_value() ? absl::StrCat(" (held by PID ", *holder_pid, ")") : "";
            return absl::UnavailableError(
                    absl::StrCat("Failed to acquire lock '", lock_dir_str, "'", holder_suffix));
        }

        poll_step = std::min({remaining, kMaxPollStep, poll_step + kPollStepIncrement});
        options.clock.Sleep(poll_step);
        immediate_retries = 0;
    }
}

ScopedFileLock::LockDeleter::LockDeleter(std::filesystem::path lock_dir,
                                         std::filesystem::path pid_file)
        : lock_dir(std::move(lock_dir)), pid_file(std::move(pid_file)) {}

void ScopedFileLock::LockDeleter::operator()(HANDLE handle) const noexcept {
    const std::string pid_file_str = pid_file.string();
    const std::string lock_dir_str = lock_dir.string();
    const absl::MutexLock lock(ProcessLockTableMutex());
    (void)::CloseHandle(handle);
    (void)::DeleteFileA(pid_file_str.c_str());
    (void)::RemoveDirectoryA(lock_dir_str.c_str());
}

}  // namespace android::base::file
