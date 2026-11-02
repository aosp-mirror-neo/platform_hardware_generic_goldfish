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

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"
#include "android/base/eintr_wrapper.h"
#include "emulator/libs/file/shared_file_lock_internal.h"

namespace android::base::file {

void ScopedSharedFileLock::FdDeleter::operator()(int fd) const noexcept {
    if (fd >= 0) {
        (void)IGNORE_EINTR(::close(fd));
    }
}

absl::StatusOr<ScopedSharedFileLock> ScopedSharedFileLock::Acquire(
        const std::filesystem::path& lock_path,
        LockShareMode mode,
        absl::Duration timeout,
        const SharedFileLockOptions& options) {
    int raw_fd = -1;
    int open_err = 0;
    if (mode == LockShareMode::kSharedRead) {
        raw_fd = HANDLE_EINTR(::open(lock_path.c_str(), O_RDONLY | O_CLOEXEC));
        if (raw_fd < 0) {
            open_err = errno;
            if (open_err == ENOENT) {
                raw_fd = HANDLE_EINTR(
                        ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
                if (raw_fd < 0) {
                    open_err = errno;
                }
            }
        }
    } else {
        raw_fd = HANDLE_EINTR(::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (raw_fd < 0) {
            open_err = errno;
        }
    }

    if (raw_fd < 0) {
        return absl::ErrnoToStatus(
                open_err,
                absl::StrCat("Failed to open shared lock file '", lock_path.string(), "'"));
    }
    LockHandle fd(raw_fd);

    const absl::Time deadline = options.clock.TimeNow() + std::max(timeout, absl::ZeroDuration());
    const absl::Duration poll_interval =
            std::max(options.poll_interval, absl::Milliseconds(1));
    const int flock_op =
            (mode == LockShareMode::kSharedRead ? LOCK_SH : LOCK_EX) | LOCK_NB;

    for (;;) {
        const int flock_rc = HANDLE_EINTR(::flock(fd.get(), flock_op));
        if (flock_rc == 0) {
            return ScopedSharedFileLock(lock_path, mode, std::move(fd));
        }
        const int flock_err = errno;
        if (flock_err != EWOULDBLOCK && flock_err != EAGAIN) {
            return absl::ErrnoToStatus(
                    flock_err, absl::StrCat("Failed to flock '", lock_path.string(), "'"));
        }

        const absl::Duration remaining = deadline - options.clock.TimeNow();
        if (remaining <= absl::ZeroDuration()) {
            return internal::FormatSharedLockTimeoutError(lock_path, mode, timeout);
        }

        options.clock.Sleep(std::min(remaining, poll_interval));
    }
}

}  // namespace android::base::file
