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

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

#include "absl/status/statusor.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"
#include "goldfish/base/unique_handle.h"

#ifdef _WIN32
#include "android/base/scoped_file_handle.h"
#endif

namespace android::base::file {

enum class LockHolderState : uint8_t {
    kDead,
    kAliveEmulator,
    kAliveUnrelatedProcess,
};

struct FileLockOptions {
    absl::Clock& clock = absl::Clock::GetRealClock();
    // POSIX-only stale-PID classifier used to decide whether an existing
    // `<target>.lock` belongs to a live emulator. Unused on Windows, where lock
    // liveness is enforced directly by the kernel via the open file handle.
    std::function<LockHolderState(int pid)> check_holder_fn;
    std::optional<int> current_pid_override;
};

// Exclusive, PID-advertising file lock compatible with Android Studio and
// legacy QEMU2 (`filelock.cpp`).
//
// Given |target_path| (for example `<avd>/hardware-qemu.ini` or
// `<avd>/snapshot.lock`), the on-disk lock artifact is:
//   - POSIX: `<target_path>.lock` regular file containing the ASCII decimal PID.
//   - Windows: `<target_path>.lock\pid` file held open with `GENERIC_WRITE` and
//     `FILE_SHARE_READ` (`bInheritHandle = FALSE`).
class ScopedFileLock {
  public:
    static std::filesystem::path LockPathForTarget(const std::filesystem::path& target_path);

    // Reads the PID currently recorded in `<target_path>.lock` (or
    // `<target_path>.lock\pid` on Windows), if present and parsable.
    static std::optional<int> ReadLockHolderPid(const std::filesystem::path& target_path);

    // Returns true if |exe_path| has an executable stem belonging to QEMU10,
    // QEMU2 (`emulator*`, `qemu-system-*`, `launcher`), or |current_exe_stem|.
    static bool IsKnownLockHolderExecutable(std::string_view exe_path,
                                            std::string_view current_exe_stem);

    static absl::StatusOr<ScopedFileLock> Acquire(
            const std::filesystem::path& target_path,
            absl::Duration timeout = absl::ZeroDuration(),
            const FileLockOptions& options = {});

    ScopedFileLock(ScopedFileLock&& other) noexcept = default;
    ScopedFileLock& operator=(ScopedFileLock&& other) noexcept = default;
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;

    const std::filesystem::path& LockPath() const noexcept { return lock_path_; }

  private:
#ifdef _WIN32
    struct LockDeleter {
        struct Empty {};
        explicit LockDeleter(Empty) {}
        LockDeleter(std::filesystem::path lock_dir, std::filesystem::path pid_file);
        void operator()(HANDLE handle) const noexcept;

        std::filesystem::path lock_dir;
        std::filesystem::path pid_file;
    };
    using LockHandle = ::goldfish::base::UniqueHandle<HANDLE, nullptr, LockDeleter>;
#else
    struct LockDeleter {
        struct Empty {};
        explicit LockDeleter(Empty) {}
        LockDeleter(std::filesystem::path lock_path, uint64_t owned_dev, uint64_t owned_ino);
        void operator()(bool) const noexcept;

        std::filesystem::path lock_path;
        uint64_t owned_dev = 0;
        uint64_t owned_ino = 0;
    };
    using LockHandle = ::goldfish::base::UniqueHandle<bool, false, LockDeleter>;
#endif

    ScopedFileLock(std::filesystem::path lock_path, LockHandle handle);

    std::filesystem::path lock_path_;
    LockHandle handle_;
};

}  // namespace android::base::file
