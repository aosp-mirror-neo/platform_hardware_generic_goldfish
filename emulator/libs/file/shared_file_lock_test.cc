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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#undef ERROR
#undef CreateProcess
#endif

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
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
using ::testing::AllOf;
using ::testing::HasSubstr;

TEST(ScopedSharedFileLockTest, SharedReadAllowsMultipleConcurrentReaders) {
    const TestTempDir temp_dir("shared_lock_readers");
    const std::filesystem::path lock_path = temp_dir.Path() / "multiinstance.lock";

    absl::StatusOr<ScopedSharedFileLock> reader_1 =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
    ASSERT_THAT(reader_1, IsOk());
    EXPECT_EQ(reader_1->Mode(), LockShareMode::kSharedRead);
    EXPECT_EQ(reader_1->LockPath(), lock_path);

    absl::StatusOr<ScopedSharedFileLock> reader_2 =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
    ASSERT_THAT(reader_2, IsOk());

    absl::StatusOr<ScopedSharedFileLock> reader_3 =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
    ASSERT_THAT(reader_3, IsOk());
}

TEST(ScopedSharedFileLockTest, EnforcesOneWriterXorManyReadersConflictMatrix) {
    const TestTempDir temp_dir("shared_lock_matrix");
    const std::filesystem::path lock_path = temp_dir.Path() / "multiinstance.lock";

    // Reader blocks Writer.
    {
        absl::StatusOr<ScopedSharedFileLock> reader_1 =
                ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
        ASSERT_THAT(reader_1, IsOk());
        {
            absl::StatusOr<ScopedSharedFileLock> reader_2 =
                    ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
            ASSERT_THAT(reader_2, IsOk());

            EXPECT_THAT(ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite),
                        StatusIs(absl::StatusCode::kUnavailable));
        }

        // reader_2 destroyed, reader_1 still held -> Writer remains blocked.
        EXPECT_THAT(ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite),
                    StatusIs(absl::StatusCode::kUnavailable));
    }

    // All readers released -> Writer now succeeds, and blocks both Reader and Writer.
    {
        absl::StatusOr<ScopedSharedFileLock> writer =
                ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite);
        ASSERT_THAT(writer, IsOk());
        EXPECT_EQ(writer->Mode(), LockShareMode::kExclusiveWrite);

        EXPECT_THAT(ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead),
                    StatusIs(absl::StatusCode::kUnavailable));
        EXPECT_THAT(ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite),
                    StatusIs(absl::StatusCode::kUnavailable));
    }

    // Writer released -> Reader succeeds again.
    absl::StatusOr<ScopedSharedFileLock> reader =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
    EXPECT_THAT(reader, IsOk());
}

TEST(ScopedSharedFileLockTest, HandlesNonAsciiLockPath) {
    const TestTempDir temp_dir("shared_lock_unicode");
    const std::filesystem::path unicode_dir = temp_dir.Path() / "测试_🚀_é";
    ASSERT_THAT(android::base::file::mkdir_recursive(unicode_dir, 0755), IsOk());

    const std::filesystem::path lock_path = unicode_dir / "multiinstance.lock";
    absl::StatusOr<ScopedSharedFileLock> writer =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite);
    ASSERT_THAT(writer, IsOk());
    EXPECT_TRUE(android::base::file::exists(lock_path));
}

TEST(ScopedSharedFileLockTest, MoveConstructorAndMoveAssignmentMaintainLock) {
    const TestTempDir temp_dir("shared_lock_move");
    const std::filesystem::path path_a = temp_dir.Path() / "a.lock";
    const std::filesystem::path path_b = temp_dir.Path() / "b.lock";

    absl::StatusOr<ScopedSharedFileLock> lock_a =
            ScopedSharedFileLock::Acquire(path_a, LockShareMode::kExclusiveWrite);
    ASSERT_THAT(lock_a, IsOk());

    ScopedSharedFileLock moved(std::move(*lock_a));
    EXPECT_EQ(moved.LockPath(), path_a);
    EXPECT_EQ(moved.Mode(), LockShareMode::kExclusiveWrite);
    EXPECT_THAT(ScopedSharedFileLock::Acquire(path_a, LockShareMode::kSharedRead),
                StatusIs(absl::StatusCode::kUnavailable));

    {
        absl::StatusOr<ScopedSharedFileLock> lock_b =
                ScopedSharedFileLock::Acquire(path_b, LockShareMode::kSharedRead);
        ASSERT_THAT(lock_b, IsOk());
        moved = std::move(*lock_b);
    }

    // Moving path_b into |moved| must have released path_a and kept path_b locked.
    EXPECT_EQ(moved.LockPath(), path_b);
    EXPECT_EQ(moved.Mode(), LockShareMode::kSharedRead);
    EXPECT_THAT(ScopedSharedFileLock::Acquire(path_a, LockShareMode::kExclusiveWrite), IsOk());
    EXPECT_THAT(ScopedSharedFileLock::Acquire(path_b, LockShareMode::kExclusiveWrite),
                StatusIs(absl::StatusCode::kUnavailable));
}

TEST(ScopedSharedFileLockTest, TimeoutRetriesAndAcquiresWhenWriterReleases) {
    const TestTempDir temp_dir("shared_lock_timeout_ok");
    const std::filesystem::path lock_path = temp_dir.Path() / "multiinstance.lock";

    absl::StatusOr<ScopedSharedFileLock> acquired_writer =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite);
    ASSERT_THAT(acquired_writer, IsOk());
    std::optional<ScopedSharedFileLock> writer(*std::move(acquired_writer));

    int sleep_calls = 0;
    AutoAdvancingClock clock(absl::FromUnixSeconds(2000), [&](absl::Duration) {
        if (++sleep_calls == 2) {
            writer.reset();
        }
    });

    const SharedFileLockOptions opts{
        .clock = clock,
        .poll_interval = absl::Seconds(1),
    };

    absl::StatusOr<ScopedSharedFileLock> reader = ScopedSharedFileLock::Acquire(
            lock_path, LockShareMode::kSharedRead, absl::Milliseconds(3500), opts);
    ASSERT_THAT(reader, IsOk());
    EXPECT_EQ(clock.SleepCalls(), 2);
}

TEST(ScopedSharedFileLockTest, TimeoutExpiresDeterministicallyViaTestClock) {
    const TestTempDir temp_dir("shared_lock_timeout_expire");
    const std::filesystem::path lock_path = temp_dir.Path() / "multiinstance.lock";

    absl::StatusOr<ScopedSharedFileLock> reader =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kSharedRead);
    ASSERT_THAT(reader, IsOk());

    AutoAdvancingClock clock(absl::FromUnixSeconds(2000));
    const SharedFileLockOptions opts{
        .clock = clock,
        .poll_interval = absl::Seconds(1),
    };

    EXPECT_THAT(ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite,
                                              absl::Milliseconds(3500), opts),
                StatusIs(absl::StatusCode::kUnavailable,
                         AllOf(HasSubstr("exclusive lock"), HasSubstr("3.5s"),
                               HasSubstr("held by another process"))));
    EXPECT_EQ(clock.TotalSlept(), absl::Milliseconds(3500));
}

TEST(ScopedSharedFileLockTest, ChildProcessDoesNotInheritOpenLockDescriptor) {
    const TestTempDir temp_dir("shared_lock_cloexec");
    const std::filesystem::path lock_path = temp_dir.Path() / "multiinstance.lock";

    std::unique_ptr<ObservableProcess> child;
    {
        absl::StatusOr<ScopedSharedFileLock> writer =
                ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite);
        ASSERT_THAT(writer, IsOk());

        child = Command::Create({SleepExecutablePath(), "--sleep", "60s"}).Execute();
        ASSERT_NE(child, nullptr);
        WaitForChildExec(child->pid(), "sleep_emu");
    }

    // If |child| had inherited the underlying FD/HANDLE, destroying |writer| in
    // the parent would leave the kernel flock / Win32 share lock held by |child|.
    ASSERT_TRUE(child->IsAlive());

    absl::StatusOr<ScopedSharedFileLock> reacquired =
            ScopedSharedFileLock::Acquire(lock_path, LockShareMode::kExclusiveWrite);
    EXPECT_THAT(reacquired, IsOk());
    EXPECT_TRUE(child->Terminate());
}

TEST(ScopedSharedFileLockTest, MultiThreadedReadersAndWritersPreserveInvariants) {
    const TestTempDir temp_dir("shared_lock_mt");
    const std::filesystem::path lock_path = temp_dir.Path() / "multiinstance.lock";

    constexpr int kReaderThreads = 4;
    constexpr int kWriterThreads = 3;
    constexpr int kIterations = 25;
    std::atomic<bool> start{false};
    std::atomic<int> active_readers{0};
    std::atomic<int> active_writers{0};
    std::atomic<int> reader_acquisitions{0};
    std::atomic<int> writer_acquisitions{0};

    std::vector<std::thread> threads;
    threads.reserve(kReaderThreads + kWriterThreads);

    for (int r = 0; r < kReaderThreads; ++r) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            SharedFileLockOptions opts;
            opts.poll_interval = absl::Milliseconds(2);
            for (int i = 0; i < kIterations; ++i) {
                absl::StatusOr<ScopedSharedFileLock> lock = ScopedSharedFileLock::Acquire(
                        lock_path, LockShareMode::kSharedRead, absl::Milliseconds(20), opts);
                if (lock.ok()) {
                    active_readers.fetch_add(1, std::memory_order_acq_rel);
                    EXPECT_EQ(active_writers.load(std::memory_order_acquire), 0);
                    reader_acquisitions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    active_readers.fetch_sub(1, std::memory_order_acq_rel);
                }
            }
        });
    }

    for (int w = 0; w < kWriterThreads; ++w) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            SharedFileLockOptions opts;
            opts.poll_interval = absl::Milliseconds(2);
            for (int i = 0; i < kIterations; ++i) {
                absl::StatusOr<ScopedSharedFileLock> lock = ScopedSharedFileLock::Acquire(
                        lock_path, LockShareMode::kExclusiveWrite, absl::Milliseconds(20), opts);
                if (lock.ok()) {
                    const int prev_writers =
                            active_writers.fetch_add(1, std::memory_order_acq_rel);
                    EXPECT_EQ(prev_writers, 0);
                    EXPECT_EQ(active_readers.load(std::memory_order_acquire), 0);
                    writer_acquisitions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    active_writers.fetch_sub(1, std::memory_order_acq_rel);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_GT(reader_acquisitions.load(), 0);
    EXPECT_GT(writer_acquisitions.load(), 0);
    EXPECT_EQ(active_readers.load(), 0);
    EXPECT_EQ(active_writers.load(), 0);
}

}  // namespace
}  // namespace android::base::file
