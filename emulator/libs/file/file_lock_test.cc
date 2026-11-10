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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#undef ERROR
#undef CreateProcess
#endif

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"
#include "android/base/testing/test_temp_dir.h"
#include "android/process/command.h"
#include "emulator/libs/file/lock_test_utils.h"
#include "goldfish/file/file.h"
#include "goldfish/file/testing/auto_advancing_clock.h"

namespace android::base::file {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::android::base::file::testing::AutoAdvancingClock;
using ::android::base::file::testing::SleepExecutablePath;
using ::android::base::file::testing::WaitForChildExec;
using ::testing::HasSubstr;

int CurrentPid() {
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

TEST(ScopedFileLockTest, AcquiresReadsPidAndReleasesOnDestruction) {
    const TestTempDir temp_dir("file_lock_basic");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";
    const std::filesystem::path expected_lock = temp_dir.Path() / "hardware-qemu.ini.lock";

    EXPECT_EQ(ScopedFileLock::LockPathForTarget(target), expected_lock);
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), std::nullopt);

    {
        absl::StatusOr<ScopedFileLock> lock = ScopedFileLock::Acquire(target);
        ASSERT_THAT(lock, IsOk());
        EXPECT_EQ(lock->LockPath(), expected_lock);
        EXPECT_TRUE(android::base::file::exists(expected_lock));
        EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
    }

    EXPECT_FALSE(android::base::file::exists(expected_lock));
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), std::nullopt);
}

TEST(ScopedFileLockTest, SnapshotLockTargetUsesDoubleLockSuffixMatchingQemu2) {
    const TestTempDir temp_dir("file_lock_snapshot");
    const std::filesystem::path target = temp_dir.Path() / "snapshot.lock";
    const std::filesystem::path expected_lock = temp_dir.Path() / "snapshot.lock.lock";

    absl::StatusOr<ScopedFileLock> lock = ScopedFileLock::Acquire(target);
    ASSERT_THAT(lock, IsOk());
    EXPECT_EQ(lock->LockPath(), expected_lock);
    EXPECT_TRUE(android::base::file::exists(expected_lock));
}

TEST(ScopedFileLockTest, HandlesNonAsciiTargetPath) {
    const TestTempDir temp_dir("file_lock_unicode");
    const std::filesystem::path unicode_dir = temp_dir.Path() / "测试_🚀_é";
    ASSERT_THAT(android::base::file::mkdir_recursive(unicode_dir, 0755), IsOk());

    const std::filesystem::path target = unicode_dir / "hardware-qemu.ini";
    {
        absl::StatusOr<ScopedFileLock> lock = ScopedFileLock::Acquire(target);
        ASSERT_THAT(lock, IsOk());
        EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
    }

    EXPECT_FALSE(android::base::file::exists(ScopedFileLock::LockPathForTarget(target)));
}

TEST(ScopedFileLockTest, MoveConstructorAndMoveAssignmentTransferOwnership) {
    const TestTempDir temp_dir("file_lock_move");
    const std::filesystem::path target_a = temp_dir.Path() / "a.ini";
    const std::filesystem::path target_b = temp_dir.Path() / "b.ini";

    {
        absl::StatusOr<ScopedFileLock> lock_a = ScopedFileLock::Acquire(target_a);
        ASSERT_THAT(lock_a, IsOk());

        ScopedFileLock moved_a(std::move(*lock_a));
        EXPECT_TRUE(android::base::file::exists(ScopedFileLock::LockPathForTarget(target_a)));

        {
            absl::StatusOr<ScopedFileLock> lock_b = ScopedFileLock::Acquire(target_b);
            ASSERT_THAT(lock_b, IsOk());
            moved_a = std::move(*lock_b);
        }

        // Reassignment must have released target_a and retained target_b.
        EXPECT_EQ(moved_a.LockPath(), ScopedFileLock::LockPathForTarget(target_b));
        EXPECT_FALSE(android::base::file::exists(ScopedFileLock::LockPathForTarget(target_a)));
        EXPECT_TRUE(android::base::file::exists(ScopedFileLock::LockPathForTarget(target_b)));
    }

    EXPECT_FALSE(android::base::file::exists(ScopedFileLock::LockPathForTarget(target_b)));
}

TEST(ScopedFileLockTest, ContendingAcquireFailsWithHolderPidInMessage) {
    const TestTempDir temp_dir("file_lock_contention");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";

    absl::StatusOr<ScopedFileLock> first = ScopedFileLock::Acquire(target);
    ASSERT_THAT(first, IsOk());

    const absl::StatusOr<ScopedFileLock> second =
            ScopedFileLock::Acquire(target, absl::ZeroDuration());
    EXPECT_THAT(second, StatusIs(absl::StatusCode::kUnavailable,
                                 HasSubstr(absl::StrCat("PID ", CurrentPid()))));
}

TEST(ScopedFileLockTest, TimeoutRetriesAndSucceedsDeterministicallyWhenReleased) {
    const TestTempDir temp_dir("file_lock_timeout_ok");
    const std::filesystem::path target = temp_dir.Path() / "snapshot.lock";

    absl::StatusOr<ScopedFileLock> acquired_holder = ScopedFileLock::Acquire(target);
    ASSERT_THAT(acquired_holder, IsOk());
    std::optional<ScopedFileLock> holder(*std::move(acquired_holder));

    int sleep_calls = 0;
    AutoAdvancingClock clock(absl::FromUnixSeconds(1000), [&](absl::Duration) {
        if (++sleep_calls == 3) {
            holder.reset();
        }
    });

    const FileLockOptions opts{.clock = clock};

    absl::StatusOr<ScopedFileLock> waiter =
            ScopedFileLock::Acquire(target, absl::Seconds(10), opts);
    ASSERT_THAT(waiter, IsOk());
    EXPECT_EQ(clock.SleepCalls(), 3);
}

TEST(ScopedFileLockTest, TimeoutExpiresDeterministicallyViaTestClock) {
    const TestTempDir temp_dir("file_lock_timeout_expire");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";

    absl::StatusOr<ScopedFileLock> holder = ScopedFileLock::Acquire(target);
    ASSERT_THAT(holder, IsOk());

    AutoAdvancingClock clock(absl::FromUnixSeconds(1000));
    const FileLockOptions opts{.clock = clock};

    const absl::StatusOr<ScopedFileLock> waiter =
            ScopedFileLock::Acquire(target, absl::Seconds(2), opts);
    EXPECT_THAT(waiter, StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_EQ(clock.TotalSlept(), absl::Seconds(2));
}

TEST(ScopedFileLockTest, RecognizesQemu10AndQemu2ExecutableNames) {
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable("/sdk/emulator/emulator", ""));
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable("C:\\sdk\\emulator\\emulator.exe", ""));
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable(
            "/sdk/emulator/qemu/linux-x86_64/qemu-system-x86_64", ""));
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable(
            "/sdk/emulator/qemu/linux-x86_64/qemu-system-aarch64-headless", ""));
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable(
            "/sdk/emulator/qemu-system-x86_64 (deleted)", ""));
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable("/path/to/launcher", ""));
    EXPECT_TRUE(ScopedFileLock::IsKnownLockHolderExecutable("/tmp/custom_test_bin",
                                                            "custom_test_bin"));

    EXPECT_FALSE(ScopedFileLock::IsKnownLockHolderExecutable("", ""));
    EXPECT_FALSE(ScopedFileLock::IsKnownLockHolderExecutable("/usr/bin/sleep_emu",
                                                             "file_lock_test"));
    EXPECT_FALSE(ScopedFileLock::IsKnownLockHolderExecutable("/bin/bash", "file_lock_test"));
    EXPECT_FALSE(ScopedFileLock::IsKnownLockHolderExecutable("/sbin/init", "file_lock_test"));
}

#ifndef _WIN32
TEST(ScopedFileLockTest, ParsesQemu2NulTerminatedPidAndRecoversDeadHolder) {
    const TestTempDir temp_dir("file_lock_qemu2_nul");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";
    const std::filesystem::path lock_file = ScopedFileLock::LockPathForTarget(target);

    {
        std::ofstream out(lock_file, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        constexpr char kQemu2WirePid[] = {'9', '8', '7', '6', '5', '\0'};
        out.write(kQemu2WirePid, sizeof(kQemu2WirePid));
    }

    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), 98765);

    FileLockOptions opts;
    opts.check_holder_fn = [](int pid) {
        EXPECT_EQ(pid, 98765);
        return LockHolderState::kDead;
    };

    absl::StatusOr<ScopedFileLock> recovered = ScopedFileLock::Acquire(target, absl::ZeroDuration(), opts);
    ASSERT_THAT(recovered, IsOk());
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
}

TEST(ScopedFileLockTest, RecoversStaleLockWhenPidReusedByLiveUnrelatedProcess) {
    const TestTempDir temp_dir("file_lock_pid_reuse");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";
    const std::filesystem::path lock_file = ScopedFileLock::LockPathForTarget(target);

    auto unrelated_proc = Command::Create({SleepExecutablePath(), "--sleep", "60s"}).Execute();
    ASSERT_NE(unrelated_proc, nullptr);
    ASSERT_TRUE(unrelated_proc->IsAlive());
    WaitForChildExec(unrelated_proc->pid(), "sleep_emu");

    {
        std::ofstream out(lock_file, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << unrelated_proc->pid();
    }

    // Even though |unrelated_proc| is alive, its executable is `sleep_emu`,
    // not an emulator binary, so ScopedFileLock must treat the lock as stale.
    absl::StatusOr<ScopedFileLock> recovered = ScopedFileLock::Acquire(target);
    ASSERT_THAT(recovered, IsOk());
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
    EXPECT_TRUE(unrelated_proc->IsAlive());
    EXPECT_TRUE(unrelated_proc->Terminate());
}

TEST(ScopedFileLockTest, UnparsablePidFreshBefore60SecondsAndStaleAfter60Seconds) {
    const TestTempDir temp_dir("file_lock_unparsable");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";
    const std::filesystem::path lock_file = ScopedFileLock::LockPathForTarget(target);

    {
        std::ofstream out(lock_file, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "not-a-pid";
    }

    constexpr time_t kFixedMtimeSec = 1'700'000'000;
    const struct timespec times[2] = {
            {.tv_sec = kFixedMtimeSec, .tv_nsec = 0},
            {.tv_sec = kFixedMtimeSec, .tv_nsec = 0},
    };
    ASSERT_EQ(::utimensat(AT_FDCWD, lock_file.c_str(), times, 0), 0);
    const absl::Time mtime = absl::FromUnixSeconds(kFixedMtimeSec);

    absl::SimulatedClock clock;
    const FileLockOptions opts{.clock = clock};

    clock.SetTime(mtime + absl::Seconds(30));
    EXPECT_THAT(ScopedFileLock::Acquire(target, absl::ZeroDuration(), opts),
                StatusIs(absl::StatusCode::kUnavailable));

    clock.SetTime(mtime + absl::Seconds(60) - absl::Milliseconds(1));
    EXPECT_THAT(ScopedFileLock::Acquire(target, absl::ZeroDuration(), opts),
                StatusIs(absl::StatusCode::kUnavailable));

    clock.SetTime(mtime + absl::Seconds(60));
    absl::StatusOr<ScopedFileLock> recovered =
            ScopedFileLock::Acquire(target, absl::ZeroDuration(), opts);
    ASSERT_THAT(recovered, IsOk());
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
}

TEST(ScopedFileLockTest, RemovesStaleWin32DirectoryLockOnPosix) {
    const TestTempDir temp_dir("file_lock_win32_dir");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";
    const std::filesystem::path lock_dir = ScopedFileLock::LockPathForTarget(target);

    ASSERT_THAT(android::base::file::mkdir_recursive(lock_dir, 0755), IsOk());
    ASSERT_THAT(android::base::file::touch(lock_dir / "pid"), IsOk());

    absl::StatusOr<ScopedFileLock> lock = ScopedFileLock::Acquire(target);
    ASSERT_THAT(lock, IsOk());
    EXPECT_TRUE(android::base::file::is_file(lock_dir));
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
}

TEST(ScopedFileLockTest, ResetsImmediateRetryBudgetAfterPollSleep) {
    const TestTempDir temp_dir("file_lock_retry_budget");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";
    const std::filesystem::path lock_file = ScopedFileLock::LockPathForTarget(target);

    {
        std::ofstream out(lock_file, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << 42424;
    }

    AutoAdvancingClock clock(absl::FromUnixSeconds(1000));
    int holder_checks = 0;
    const FileLockOptions opts{
        .clock = clock,
        .check_holder_fn =
                [&](int pid) {
                    EXPECT_EQ(pid, 42424);
                    ++holder_checks;
                    if (holder_checks <= 16) {
                        // Replace `lock_file` with a fresh inode before the stale-unlink inode
                        // guard runs, exhausting the 16-attempt immediate retry budget in the
                        // first poll cycle.
                        const std::filesystem::path replacement =
                                temp_dir.Path() / absl::StrCat("replacement_", holder_checks);
                        std::ofstream out(replacement, std::ios::binary);
                        out << 42424;
                        out.close();
                        std::filesystem::rename(replacement, lock_file);
                    }
                    return LockHolderState::kDead;
                },
    };

    absl::StatusOr<ScopedFileLock> recovered =
            ScopedFileLock::Acquire(target, absl::Seconds(1), opts);
    ASSERT_THAT(recovered, IsOk());
    EXPECT_EQ(clock.TotalSlept(), absl::Milliseconds(50));
    EXPECT_EQ(ScopedFileLock::ReadLockHolderPid(target), CurrentPid());
}
#endif

TEST(ScopedFileLockTest, ChildProcessDoesNotInheritLockHandle) {
    const TestTempDir temp_dir("file_lock_cloexec");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";

    std::unique_ptr<ObservableProcess> child;
    {
        absl::StatusOr<ScopedFileLock> parent_lock = ScopedFileLock::Acquire(target);
        ASSERT_THAT(parent_lock, IsOk());

        child = Command::Create({SleepExecutablePath(), "--sleep", "60s"}).Execute();
        ASSERT_NE(child, nullptr);
        WaitForChildExec(child->pid(), "sleep_emu");
    }

    ASSERT_TRUE(child->IsAlive());

    absl::StatusOr<ScopedFileLock> reacquired = ScopedFileLock::Acquire(target);
    EXPECT_THAT(reacquired, IsOk());
    EXPECT_TRUE(child->Terminate());
}

TEST(ScopedFileLockTest, MultiThreadedContentionMaintainsMutualExclusion) {
    const TestTempDir temp_dir("file_lock_mt");
    const std::filesystem::path target = temp_dir.Path() / "hardware-qemu.ini";

    constexpr int kNumThreads = 8;
    constexpr int kAttemptsPerThread = 20;
    std::atomic<bool> start{false};
    std::atomic<int> active_holders{0};
    std::atomic<int> successful_acquisitions{0};

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kAttemptsPerThread; ++i) {
                absl::StatusOr<ScopedFileLock> lock =
                        ScopedFileLock::Acquire(target, absl::Milliseconds(25));
                if (lock.ok()) {
                    const int prev = active_holders.fetch_add(1, std::memory_order_acq_rel);
                    EXPECT_EQ(prev, 0);
                    successful_acquisitions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    active_holders.fetch_sub(1, std::memory_order_acq_rel);
                } else {
                    EXPECT_EQ(lock.status().code(), absl::StatusCode::kUnavailable)
                            << lock.status();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_GT(successful_acquisitions.load(), 0);
    EXPECT_EQ(active_holders.load(), 0);
    EXPECT_FALSE(android::base::file::exists(ScopedFileLock::LockPathForTarget(target)));
}

}  // namespace
}  // namespace android::base::file
