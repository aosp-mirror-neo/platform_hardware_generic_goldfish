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

#include "absl/status/statusor.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"

#ifdef _WIN32
#include "android/base/scoped_file_handle.h"
#else
#include "goldfish/base/unique_handle.h"
#endif

namespace android::base::file {

enum class LockShareMode : uint8_t {
    kSharedRead,
    kExclusiveWrite,
};

struct SharedFileLockOptions {
    absl::Clock& clock = absl::Clock::GetRealClock();
    absl::Duration poll_interval = absl::Milliseconds(200);
};

// Cross-process 1-Writer XOR N-Readers lock compatible with QEMU2's
// `FileShareOpen` / `multiinstance.lock`.
//
// - POSIX: opens |lock_path| with `O_CLOEXEC` and applies `flock(LOCK_SH)` or
//   `flock(LOCK_EX)`.
// - Windows: opens |lock_path| via `CreateFileA` (`bInheritHandle = FALSE`)
//   with `GENERIC_READ` + `FILE_SHARE_READ` (`kSharedRead`) or
//   `GENERIC_READ | GENERIC_WRITE` + `dwShareMode = 0` (`kExclusiveWrite`).
class ScopedSharedFileLock {
  public:
    static absl::StatusOr<ScopedSharedFileLock> Acquire(
            const std::filesystem::path& lock_path,
            LockShareMode mode,
            absl::Duration timeout = absl::ZeroDuration(),
            const SharedFileLockOptions& options = {});

    ScopedSharedFileLock(ScopedSharedFileLock&& other) noexcept = default;
    ScopedSharedFileLock& operator=(ScopedSharedFileLock&& other) noexcept = default;
    ScopedSharedFileLock(const ScopedSharedFileLock&) = delete;
    ScopedSharedFileLock& operator=(const ScopedSharedFileLock&) = delete;

    LockShareMode Mode() const noexcept { return mode_; }
    const std::filesystem::path& LockPath() const noexcept { return lock_path_; }

  private:
#ifdef _WIN32
    using LockHandle = android::base::ScopedFileHandle;
#else
    struct FdDeleter {
        struct Empty {};
        FdDeleter() = default;
        explicit FdDeleter(Empty) {}
        void operator()(int fd) const noexcept;
    };
    using LockHandle = ::goldfish::base::UniqueHandle<int, -1, FdDeleter>;
#endif

    ScopedSharedFileLock(std::filesystem::path lock_path,
                         LockShareMode mode,
                         LockHandle handle);

    std::filesystem::path lock_path_;
    LockShareMode mode_ = LockShareMode::kExclusiveWrite;
    LockHandle handle_;
};

}  // namespace android::base::file
