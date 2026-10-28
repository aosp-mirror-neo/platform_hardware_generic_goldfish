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

#include "android/crashreport/crash_system.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"

#include "android/base/testing/test_temp_dir.h"
#include "android/status/status_matcher_macros.h"
#include "goldfish/file/file.h"

namespace android::crashreport {
namespace {

namespace file = android::base::file;
using ::absl_testing::StatusIs;
using ::android::base::TestTempDir;
using ::testing::AllOf;
using ::testing::HasSubstr;

TEST(CrashSystemTest, EnsureDatabaseDirectoryCreatesMissingParents) {
    TestTempDir temp_dir("crash_system_test");
    // Like /tmp/crash-report.db on a Windows drive without C:\tmp, under a non-ASCII directory.
    const fs::path missing_parent =
            temp_dir.Path() / fs::path(std::u8string_view(u8"täst_中文_🚀"));
    const fs::path database_path = missing_parent / "tmp" / "crash-report.db";

    ASSERT_OK_AND_ASSIGN(fs::path created, CrashSystem::ensureDatabaseDirectory(database_path));

    EXPECT_TRUE(created.is_absolute());
    EXPECT_TRUE(file::is_dir(created));
    EXPECT_TRUE(file::is_dir(database_path));
#ifndef _WIN32
    ASSERT_OK_AND_ASSIGN(unsigned parent_mode, file::mode(missing_parent));
    EXPECT_EQ(parent_mode & 0777, 0700u);
    ASSERT_OK_AND_ASSIGN(unsigned database_mode, file::mode(database_path));
    EXPECT_EQ(database_mode & 0777, 0700u);
#endif
}

TEST(CrashSystemTest, EnsureDatabaseDirectoryAcceptsExistingDirectory) {
    TestTempDir temp_dir("crash_system_test");
    const fs::path database_path = temp_dir.Path() / "crash-report.db";
    ASSERT_TRUE(temp_dir.MakeSubDir("crash-report.db"));

    EXPECT_OK(CrashSystem::ensureDatabaseDirectory(database_path));
    EXPECT_OK(CrashSystem::ensureDatabaseDirectory(database_path));
    EXPECT_TRUE(file::is_dir(database_path));
}

TEST(CrashSystemTest, EnsureDatabaseDirectoryRejectsRegularFile) {
    TestTempDir temp_dir("crash_system_test");
    const fs::path database_path = temp_dir.Path() / "crash-report.db";
    ASSERT_TRUE(temp_dir.MakeSubFile("crash-report.db"));

    EXPECT_THAT(CrashSystem::ensureDatabaseDirectory(database_path),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr(database_path.string()),
                               HasSubstr("exists but is not a directory"))));
    EXPECT_TRUE(file::is_file(database_path));
}

TEST(CrashSystemTest, EnsureDatabaseDirectoryReportsPathItCannotCreate) {
#ifdef _WIN32
    // mkdir_recursive() calls the throwing fs::exists(), which recent Windows
    // versions can make throw for a path below a regular file.
    GTEST_SKIP() << "Runs on POSIX only.";
#endif
    TestTempDir temp_dir("crash_system_test");
    ASSERT_TRUE(temp_dir.MakeSubFile("not-a-directory"));
    const fs::path database_path = temp_dir.Path() / "not-a-directory" / "crash-report.db";

    auto created = CrashSystem::ensureDatabaseDirectory(database_path);

    ASSERT_FALSE(created.ok());
    EXPECT_THAT(created.status().message(),
                AllOf(HasSubstr("Cannot create crash report database directory"),
                      HasSubstr(database_path.string()),
                      HasSubstr("ANDROID_EMU_CRASH_REPORTING_DATABASE")));
}

}  // namespace
}  // namespace android::crashreport
