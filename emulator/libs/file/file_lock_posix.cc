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

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"
#include "android/base/eintr_wrapper.h"
#include "emulator/libs/file/file_lock_internal.h"
#include "emulator/libs/file/posix_fd_io.h"
#include "goldfish/file/file.h"
#include "goldfish/file/scoped_fd.h"

namespace android::base::file {
namespace {

using internal::ParsePidString;
using internal::WriteAllToFd;

constexpr absl::Duration kUnparsablePidStaleAge = absl::Seconds(60);
constexpr absl::Duration kPollStepIncrement = absl::Milliseconds(50);
constexpr absl::Duration kMaxPollStep = absl::Milliseconds(200);
constexpr int kMaxImmediateRetries = 16;

// Serializes link/unlink inspection across threads within the same process so
// two in-process threads never race on inode verification before unlink().
absl::Mutex& ProcessLockTableMutex() {
    static absl::NoDestructor<absl::Mutex> mu;
    return *mu;
}

#ifdef __linux__
bool IsLinuxProcessZombieOrDead(int pid) {
    const absl::StatusOr<std::string> stat_contents =
            android::base::file::read_whole_file(absl::StrCat("/proc/", pid, "/stat"));
    if (!stat_contents.ok()) {
        return false;
    }
    const size_t close_paren = stat_contents->rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= stat_contents->size()) {
        return false;
    }
    const char state = (*stat_contents)[close_paren + 2];
    return state == 'Z' || state == 'X';
}
#endif

LockHolderState DefaultCheckLockHolderPosix(int pid) {
    if (pid <= 0) {
        return LockHolderState::kDead;
    }
    if (pid == static_cast<int>(::getpid())) {
        return LockHolderState::kAliveEmulator;
    }

    const int kill_rc = HANDLE_EINTR(::kill(pid, 0));
    if (kill_rc != 0) {
        if (errno == EPERM) {
            return LockHolderState::kAliveEmulator;
        }
        return LockHolderState::kDead;
    }

#ifdef __APPLE__
    char exe_buf[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath(pid, exe_buf, sizeof(exe_buf)) <= 0) {
        const int recheck_rc = HANDLE_EINTR(::kill(pid, 0));
        if (recheck_rc != 0 && errno == ESRCH) {
            return LockHolderState::kDead;
        }
        return LockHolderState::kAliveEmulator;
    }
    char self_buf[PROC_PIDPATHINFO_MAXSIZE] = {};
    std::string my_stem;
    if (proc_pidpath(::getpid(), self_buf, sizeof(self_buf)) > 0) {
        my_stem = std::filesystem::path(self_buf).stem().string();
    }
    return ScopedFileLock::IsKnownLockHolderExecutable(exe_buf, my_stem)
                   ? LockHolderState::kAliveEmulator
                   : LockHolderState::kAliveUnrelatedProcess;
#elif defined(__linux__)
    if (IsLinuxProcessZombieOrDead(pid)) {
        return LockHolderState::kDead;
    }

    const absl::StatusOr<std::filesystem::path> exe =
            android::base::file::read_symlink(absl::StrCat("/proc/", pid, "/exe"));
    if (!exe.ok()) {
        const int recheck_rc = HANDLE_EINTR(::kill(pid, 0));
        if (recheck_rc != 0 && errno == ESRCH) {
            return LockHolderState::kDead;
        }
        // Unreadable /proc/<pid>/exe (e.g. EACCES/EPERM): conservatively treat
        // the live PID as active rather than deleting a valid lock.
        return LockHolderState::kAliveEmulator;
    }

    const absl::StatusOr<std::filesystem::path> self_exe =
            android::base::file::read_symlink("/proc/self/exe");
    const std::string my_stem = self_exe.ok() ? self_exe->stem().string() : std::string();
    return ScopedFileLock::IsKnownLockHolderExecutable(exe->string(), my_stem)
                   ? LockHolderState::kAliveEmulator
                   : LockHolderState::kAliveUnrelatedProcess;
#else
#error "Unexpected platform"
#endif
}

absl::Time StatMtime(const struct stat& st) {
#ifdef __APPLE__
    return absl::TimeFromTimespec(st.st_mtimespec);
#else
    return absl::TimeFromTimespec(st.st_mtim);
#endif
}

struct ObservedLockFile {
    bool vanished = false;
    bool stale = false;
    std::optional<int> holder_pid;
    uint64_t dev = 0;
    uint64_t ino = 0;
};

ObservedLockFile InspectExistingLockPosix(const std::filesystem::path& lock_path, absl::Time now,
                                          const std::function<LockHolderState(int)>& check_holder) {
    ObservedLockFile obs;
    struct stat st_link {};
    const int lstat_rc = HANDLE_EINTR(::lstat(lock_path.c_str(), &st_link));
    if (lstat_rc != 0) {
        if (errno == ENOENT) {
            obs.vanished = true;
        }
        return obs;
    }

    if (S_ISDIR(st_link.st_mode)) {
        LOG(WARNING) << "Removing stale Win32 directory lock artifact: " << lock_path.string();
        const std::filesystem::path win_pid = lock_path / "pid";
        (void)HANDLE_EINTR(::unlink(win_pid.c_str()));
        const int rmdir_rc = HANDLE_EINTR(::rmdir(lock_path.c_str()));
        if (rmdir_rc == 0) {
            obs.vanished = true;
        }
        return obs;
    }

    ScopedFd fd(HANDLE_EINTR(::open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)));
    if (!fd.ok()) {
        if (errno == ENOENT) {
            obs.vanished = true;
        }
        return obs;
    }

    struct stat st_fd {};
    const int fstat_rc = HANDLE_EINTR(::fstat(fd.get(), &st_fd));
    if (fstat_rc != 0) {
        return obs;
    }
    obs.dev = static_cast<uint64_t>(st_fd.st_dev);
    obs.ino = static_cast<uint64_t>(st_fd.st_ino);

    char buf[32] = {};
    const ssize_t n = HANDLE_EINTR(::read(fd.get(), buf, sizeof(buf) - 1));
    if (n > 0) {
        obs.holder_pid = ParsePidString(std::string_view(buf, static_cast<size_t>(n)));
        if (obs.holder_pid.has_value()) {
            const LockHolderState state = check_holder(*obs.holder_pid);
            obs.stale = (state != LockHolderState::kAliveEmulator);
            return obs;
        }
    }

    // No parsable PID (e.g. concurrent writer or truncated crash artifact):
    // treat as fresh until 60 seconds after last modification.
    if (now - StatMtime(st_fd) >= kUnparsablePidStaleAge) {
        obs.stale = true;
    }
    return obs;
}

}  // namespace

std::optional<int> ScopedFileLock::ReadLockHolderPid(const std::filesystem::path& target_path) {
    const std::filesystem::path lock_path = LockPathForTarget(target_path);
    ScopedFd fd(HANDLE_EINTR(::open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)));
    if (!fd.ok()) {
        return std::nullopt;
    }
    char buf[32] = {};
    const ssize_t n = HANDLE_EINTR(::read(fd.get(), buf, sizeof(buf) - 1));
    if (n <= 0) {
        return std::nullopt;
    }
    return ParsePidString(std::string_view(buf, static_cast<size_t>(n)));
}

absl::StatusOr<ScopedFileLock> ScopedFileLock::Acquire(
        const std::filesystem::path& target_path,
        absl::Duration timeout,
        const FileLockOptions& options) {
    std::filesystem::path lock_path = LockPathForTarget(target_path);
    std::string temp_template = absl::StrCat(target_path.string(), ".tmp.XXXXXX");

    const int raw_temp_fd = ::mkostemp(temp_template.data(), O_CLOEXEC);
    if (raw_temp_fd < 0) {
        const int err = errno;
        return absl::ErrnoToStatus(
                err, absl::StrCat("Failed to create temporary lock file for '",
                                  lock_path.string(), "'"));
    }
    ScopedFd temp_fd(raw_temp_fd);

    const std::filesystem::path temp_path(temp_template);
    absl::Cleanup cleanup_temp = [&temp_path]() {
        (void)HANDLE_EINTR(::unlink(temp_path.c_str()));
    };

    const int my_pid = options.current_pid_override.value_or(static_cast<int>(::getpid()));
    const std::string pid_str = absl::StrCat(my_pid);
    if (absl::Status status = WriteAllToFd(temp_fd.get(), pid_str, temp_path); !status.ok()) {
        return status;
    }

    struct stat st_temp {};
    const int fstat_temp_rc = HANDLE_EINTR(::fstat(temp_fd.get(), &st_temp));
    if (fstat_temp_rc != 0) {
        const int err = errno;
        return absl::ErrnoToStatus(
                err, absl::StrCat("Failed to stat temporary lock file '",
                                  temp_path.string(), "'"));
    }
    temp_fd.reset();

    const auto check_holder = options.check_holder_fn
                                      ? options.check_holder_fn
                                      : std::function<LockHolderState(int)>(
                                                DefaultCheckLockHolderPosix);

    const absl::Time deadline = options.clock.TimeNow() + std::max(timeout, absl::ZeroDuration());
    absl::Duration poll_step = absl::ZeroDuration();
    int immediate_retries = 0;

    for (;;) {
        {
            const absl::MutexLock lock(ProcessLockTableMutex());
            const int link_rc = HANDLE_EINTR(::link(temp_path.c_str(), lock_path.c_str()));
            if (link_rc == 0) {
                std::filesystem::path owned_path = lock_path;
                return ScopedFileLock(
                        std::move(lock_path),
                        LockHandle(true, LockDeleter(std::move(owned_path),
                                                     static_cast<uint64_t>(st_temp.st_dev),
                                                     static_cast<uint64_t>(st_temp.st_ino))));
            }
            const int err = errno;
            if (err != EEXIST) {
                return absl::ErrnoToStatus(
                        err, absl::StrCat("Failed to link lock file '", lock_path.string(), "'"));
            }
        }

        const ObservedLockFile obs =
                InspectExistingLockPosix(lock_path, options.clock.TimeNow(), check_holder);
        if (obs.vanished && immediate_retries < kMaxImmediateRetries) {
            ++immediate_retries;
            continue;
        }
        if (obs.stale && immediate_retries < kMaxImmediateRetries) {
            ++immediate_retries;
            const absl::MutexLock lock(ProcessLockTableMutex());
            struct stat st_cur {};
            const int lstat_cur_rc = HANDLE_EINTR(::lstat(lock_path.c_str(), &st_cur));
            if (lstat_cur_rc == 0 &&
                static_cast<uint64_t>(st_cur.st_dev) == obs.dev &&
                static_cast<uint64_t>(st_cur.st_ino) == obs.ino) {
                VLOG(1) << "Removing stale lock file: " << lock_path.string();
                (void)HANDLE_EINTR(::unlink(lock_path.c_str()));
            }
            continue;
        }

        const absl::Duration remaining = deadline - options.clock.TimeNow();
        if (remaining <= absl::ZeroDuration()) {
            const std::string holder_suffix =
                    obs.holder_pid.has_value()
                            ? absl::StrCat(" (held by PID ", *obs.holder_pid, ")")
                            : "";
            return absl::UnavailableError(absl::StrCat(
                    "Failed to acquire lock '", lock_path.string(), "'", holder_suffix));
        }

        poll_step = std::min({remaining, kMaxPollStep, poll_step + kPollStepIncrement});
        options.clock.Sleep(poll_step);
        immediate_retries = 0;
    }
}

ScopedFileLock::LockDeleter::LockDeleter(std::filesystem::path lock_path,
                                         uint64_t owned_dev,
                                         uint64_t owned_ino)
        : lock_path(std::move(lock_path)), owned_dev(owned_dev), owned_ino(owned_ino) {}

void ScopedFileLock::LockDeleter::operator()(bool) const noexcept {
    const absl::MutexLock lock(ProcessLockTableMutex());
    struct stat st_cur {};
    const int lstat_rc = HANDLE_EINTR(::lstat(lock_path.c_str(), &st_cur));
    if (lstat_rc == 0 &&
        static_cast<uint64_t>(st_cur.st_dev) == owned_dev &&
        static_cast<uint64_t>(st_cur.st_ino) == owned_ino) {
        (void)HANDLE_EINTR(::unlink(lock_path.c_str()));
    }
}

}  // namespace android::base::file
