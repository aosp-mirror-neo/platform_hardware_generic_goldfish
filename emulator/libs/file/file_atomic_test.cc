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
#include "goldfish/file/file_atomic.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "android/base/system.h"
#include "goldfish/file/file.h"

namespace android::base::file {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

class FileAtomicTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Use a temporary directory for each test
        temp_dir_ = fs::temp_directory_path() / "FileAtomicTest";
        fs::create_directories(temp_dir_);
    }

    void TearDown() override { fs::remove_all(temp_dir_); }

    fs::path temp_dir_;
};

TEST_F(FileAtomicTest, WriteNewFile) {
    fs::path target = temp_dir_ / "test_file.txt";
    std::string content = "Hello, World!";

    auto status = CreatePrivateFileExclusive(target, content);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_TRUE(fs::exists(target));

    EXPECT_THAT(read_whole_file(target, /*binary=*/false), IsOkAndHolds(content));
}

TEST_F(FileAtomicTest, FailsIfFileExists) {
    fs::path target = temp_dir_ / "exclusive.txt";
    ASSERT_TRUE(CreatePrivateFileExclusive(target, "First").ok());

    // Subsequent write should fail
    auto status = CreatePrivateFileExclusive(target, "Second");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kAlreadyExists);

    EXPECT_THAT(read_whole_file(target, /*binary=*/false),
                IsOkAndHolds("First"));  // Content should not have changed
}

TEST_F(FileAtomicTest, PrivatePermissions) {
    fs::path target = temp_dir_ / "private.txt";
    ASSERT_TRUE(CreatePrivateFileExclusive(target, "private").ok());

    auto perms = fs::status(target).permissions();

#ifdef _WIN32
    // Windows permissions are handled via ACLs
#else
    // On POSIX, we expect 0600 (owner read/write)
    EXPECT_EQ(perms, fs::perms::owner_read | fs::perms::owner_write);
#endif
}

TEST_F(FileAtomicTest, FailsWithInternalErrorIfParentDirectoryMissing) {
    fs::path target = temp_dir_ / "missing_parent" / "test_file.txt";
    EXPECT_THAT(CreatePrivateFileExclusive(target, "content"),
                StatusIs(absl::StatusCode::kInternal));
}

#ifndef _WIN32
TEST_F(FileAtomicTest, FailsWithPermissionDeniedIfDirectoryReadOnly) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "Root bypasses POSIX directory permission checks.";
    }
    fs::path ro_dir = temp_dir_ / "read_only_dir";
    ASSERT_TRUE(mkdir(ro_dir, 0500).ok());

    EXPECT_THAT(CreatePrivateFileExclusive(ro_dir / "test_file.txt", "content"),
                StatusIs(absl::StatusCode::kPermissionDenied));

    ASSERT_TRUE(chmod(ro_dir, 0700).ok());
}
#endif

}  // namespace android::base::file
