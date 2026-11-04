#include "goldfish/file/file.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

#include "goldfish/file/scoped_fd.h"
#endif

#include "absl/status/status_matchers.h"

#include "android/base/eintr_wrapper.h"
#include "android/base/testing/test_temp_dir.h"

#define EXPECT_OK(x) EXPECT_THAT(x, absl_testing::IsOk())

namespace android::base {

TEST(File, pathIsDir) {
    TestTempDir tempDir("path_opts");
    EXPECT_FALSE(file::is_dir(tempDir.Path() / "foo"));
    EXPECT_FALSE(file::is_dir(tempDir.Path() / "foo/"));
#ifdef _WIN32
    EXPECT_FALSE(file::is_dir(tempDir.Path() / "foo\\"));
#endif

    EXPECT_TRUE(tempDir.MakeSubDir("foo"));

    EXPECT_TRUE(file::is_dir(tempDir.Path() / "foo"));
    EXPECT_TRUE(file::is_dir(tempDir.Path() / "foo/"));
#ifdef _WIN32
    EXPECT_TRUE(file::is_dir(tempDir.Path() / "foo\\"));
#endif
}

TEST(File, pathOperations) {
    TestTempDir tempDir("path_opts");
    auto fooPath = tempDir.Path() / "foo";

    EXPECT_FALSE(file::exists(fooPath));
    EXPECT_FALSE(file::is_file(fooPath));
    EXPECT_FALSE(file::is_dir(fooPath));
    EXPECT_FALSE(file::can_read(fooPath));
    EXPECT_FALSE(file::can_write(fooPath));
    EXPECT_FALSE(file::can_exec(fooPath));
    EXPECT_THAT(file::file_size(fooPath), absl_testing::StatusIs(absl::StatusCode::kInternal));

    EXPECT_OK(file::touch(tempDir.Path() / "foo"));

    EXPECT_TRUE(file::exists(fooPath));
    EXPECT_TRUE(file::is_file(fooPath));
    EXPECT_FALSE(file::is_dir(fooPath));

    // NOTE: Windows doesn't have 'execute' permission bits.
    // Any readable file can be executed. Also any writable file
    // is readable.
    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR)));
    EXPECT_TRUE(file::can_read(fooPath));
    EXPECT_TRUE(file::can_write(fooPath));
    EXPECT_TRUE(file::can_exec(fooPath));

    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IRUSR)));
    EXPECT_TRUE(file::can_read(fooPath));
    EXPECT_FALSE(file::can_write(fooPath));
#ifdef _WIN32
    EXPECT_TRUE(file::can_exec(fooPath));
#else
    EXPECT_FALSE(file::can_exec(fooPath));
#endif

    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IWUSR)));
#ifdef _WIN32
    EXPECT_TRUE(file::can_read(fooPath));
    EXPECT_TRUE(file::can_write(fooPath));
    EXPECT_TRUE(file::can_exec(fooPath));
#else
    EXPECT_FALSE(file::can_read(fooPath));
    EXPECT_TRUE(file::can_write(fooPath));
    EXPECT_FALSE(file::can_exec(fooPath));
#endif

    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IXUSR)));
#ifdef _WIN32
    EXPECT_TRUE(file::can_read(fooPath));
#else
    EXPECT_FALSE(file::can_read(fooPath));
#endif
    EXPECT_FALSE(file::can_write(fooPath));
    EXPECT_TRUE(file::can_exec(fooPath));

    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IRUSR | S_IWUSR)));
    EXPECT_TRUE(file::can_read(fooPath));
    EXPECT_TRUE(file::can_write(fooPath));
#ifdef _WIN32
    EXPECT_TRUE(file::can_exec(fooPath));
#else
    EXPECT_FALSE(file::can_exec(fooPath));
#endif

    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IRUSR | S_IXUSR)));
    EXPECT_TRUE(file::can_read(fooPath));
    EXPECT_FALSE(file::can_write(fooPath));
    EXPECT_TRUE(file::can_exec(fooPath));

    EXPECT_FALSE(HANDLE_EINTR(chmod(fooPath.string().c_str(), S_IWUSR | S_IXUSR)));
#ifdef _WIN32
    EXPECT_TRUE(file::can_read(fooPath));
#else
    EXPECT_FALSE(file::can_read(fooPath));
#endif
    EXPECT_TRUE(file::can_write(fooPath));
    EXPECT_TRUE(file::can_exec(fooPath));

    EXPECT_THAT(file::file_size(fooPath), absl_testing::IsOkAndHolds(0U));

    std::ofstream fooFile(fooPath);
    ASSERT_TRUE(bool(fooFile));
    fooFile << "Some non-zero data";
    fooFile.close();
    EXPECT_THAT(file::file_size(fooPath), absl_testing::IsOkAndHolds(18U));

    // Test file deletion
    EXPECT_OK(file::rm(fooPath));
    EXPECT_THAT(file::file_size(fooPath), absl_testing::StatusIs(absl::StatusCode::kInternal));
}

TEST(File, scandDirEntries) {
    static const char* const kExpected[] = {"fifth", "first", "fourth", "second", "sixth", "third"};
    static const char* const kInput[] = {"first", "second", "third", "fourth", "fifth", "sixth"};
    const size_t kCount = 6;

    TestTempDir myDir("scanDirEntries");
    for (size_t n = 0; n < kCount; ++n) {
        file::touch(myDir.Path() / kInput[n]).IgnoreError();
    }

    auto entries = file::scan_dir(myDir.Path());

    ASSERT_EQ(kCount, entries.size());
    for (size_t n = 0; n < kCount; ++n) {
        EXPECT_STREQ(kExpected[n], entries[n].string().c_str()) << "#" << n;
    }
}

TEST(File, scanDirEntriesWithFullPaths) {
    static const char* const kExpected[] = {"fifth", "first", "fourth", "second", "sixth", "third"};
    static const char* const kInput[] = {"first", "second", "third", "fourth", "fifth", "sixth"};
    const size_t kCount = 6;

    TestTempDir myDir("scanDirEntriesFull");
    for (size_t n = 0; n < kCount; ++n) {
        file::touch(myDir.Path() / kInput[n]).IgnoreError();
    }

    auto entries = file::scan_dir(myDir.Path(), true);

    ASSERT_EQ(kCount, entries.size());
    for (size_t n = 0; n < kCount; ++n) {
        std::string expected(myDir.Path().string());
        expected += fs::path::preferred_separator;
        expected += kExpected[n];
        EXPECT_STREQ(expected.c_str(), entries[n].string().c_str()) << "#" << n;
    }
}

TEST(File, copyIfMissing) {
    const android::base::TestTempDir tmpdir("CopyIfMissing");
    const fs::path existing = tmpdir.Path() / "existing";
    const fs::path missing = tmpdir.Path() / "missing";
    const fs::path src = tmpdir.Path() / "src";

    {
        std::ofstream f(existing);
        ASSERT_TRUE(f);
        f << "existing";
    }
    {
        std::ofstream f(src);
        ASSERT_TRUE(f);
        f << "src";
    }

    EXPECT_THAT(file::copy_if_missing(existing, src), absl_testing::IsOk());
    {
        std::ifstream f(existing);
        ASSERT_TRUE(f);
        std::string contents;
        f >> contents;
        EXPECT_EQ(contents, "existing");
    }

    EXPECT_THAT(file::copy_if_missing(missing, src), absl_testing::IsOk());
    {
        std::ifstream f(missing);
        ASSERT_TRUE(f);
        std::string contents;
        f >> contents;
        EXPECT_EQ(contents, "src");
    }
}

TEST(File, mvFileOverwritesExisting) {
    const android::base::TestTempDir tmpdir("MvFileOverwrites");
    const fs::path dst = tmpdir.Path() / "dst";
    const fs::path src = tmpdir.Path() / "src";

    {
        std::ofstream f(dst);
        ASSERT_TRUE(f);
        f << "old";
    }
    {
        std::ofstream f(src);
        ASSERT_TRUE(f);
        f << "new_content";
    }

    EXPECT_THAT(file::mv_file(src, dst), absl_testing::IsOk());
    EXPECT_FALSE(file::exists(src));
    EXPECT_THAT(file::read_whole_file(dst, false), absl_testing::IsOkAndHolds("new_content"));
}

TEST(File, freeSpaceAndDiskPressureLimit) {
    EXPECT_EQ(file::kDiskPressureLimit, 2_GiB);

    const android::base::TestTempDir tmpdir("FreeSpace");
    const fs::path unicode_dir = tmpdir.Path() / "测试_🚀_é";
    ASSERT_THAT(file::mkdir_recursive(unicode_dir, 0755), absl_testing::IsOk());

    const absl::StatusOr<StorageCapacity> space = file::free_space(unicode_dir);
    ASSERT_THAT(space, absl_testing::IsOk());
    EXPECT_GT(space->Bytes(), 0U);
    EXPECT_LT(space->Bytes(), static_cast<std::uintmax_t>(-1));

    EXPECT_THAT(file::free_space(unicode_dir / "non_existent_subpath"),
                absl_testing::StatusIs(absl::StatusCode::kInternal));
}

TEST(File, readSymlink) {
    const android::base::TestTempDir tmpdir("ReadSymlink");
    const fs::path unicode_dir = tmpdir.Path() / "测试_🚀_é";
    ASSERT_THAT(file::mkdir_recursive(unicode_dir, 0755), absl_testing::IsOk());

    const fs::path target_file = unicode_dir / "target_测试.txt";
    ASSERT_THAT(file::touch(target_file), absl_testing::IsOk());

    EXPECT_THAT(file::read_symlink(unicode_dir / "missing_link"),
                absl_testing::StatusIs(absl::StatusCode::kInternal));
    EXPECT_THAT(file::read_symlink(target_file),
                absl_testing::StatusIs(absl::StatusCode::kInternal));

#ifndef _WIN32
    const fs::path link_path = unicode_dir / "link_🚀";
    ASSERT_EQ(HANDLE_EINTR(::symlink(target_file.c_str(), link_path.c_str())), 0);
    EXPECT_TRUE(file::is_link(link_path));
    EXPECT_THAT(file::read_symlink(link_path), absl_testing::IsOkAndHolds(target_file));
#endif
}

#ifndef _WIN32
TEST(File, scopedFdClosesAndMovesHandle) {
    file::ScopedFd empty;
    EXPECT_FALSE(empty.ok());
    EXPECT_EQ(empty.get(), -1);

    int pipe_fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipe_fds), 0);
    const int read_raw = pipe_fds[0];
    const int write_raw = pipe_fds[1];

    {
        file::ScopedFd read_fd(read_raw);
        file::ScopedFd write_fd(write_raw);
        EXPECT_TRUE(read_fd.ok());
        EXPECT_EQ(read_fd.get(), read_raw);
        ASSERT_GE(::fcntl(read_raw, F_GETFD), 0);
        ASSERT_GE(::fcntl(write_raw, F_GETFD), 0);

        file::ScopedFd moved = std::move(read_fd);
        EXPECT_FALSE(read_fd.ok());
        EXPECT_EQ(moved.get(), read_raw);
        EXPECT_GE(::fcntl(read_raw, F_GETFD), 0);
    }

    errno = 0;
    EXPECT_EQ(::fcntl(read_raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    errno = 0;
    EXPECT_EQ(::fcntl(write_raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
#endif

}  // namespace android::base
