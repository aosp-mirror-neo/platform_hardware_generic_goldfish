// Copyright (C) 2016 The Android Open Source Project
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

#include "goldfish/memory/shared_memory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "android/base/bazel_info.h"
#include "android/process/command.h"

namespace goldfish::memory {

namespace {

std::string GetUniqueName(const std::string& prefix) {
    static int counter = 0;
    std::string name = prefix + "_" + std::to_string(getpid()) + "_" + std::to_string(++counter);
    return (std::filesystem::temp_directory_path() / name).string();
}

#ifdef _WIN32
#define EXE ".exe"
#else
#define EXE ""
#endif

std::string GetHelperExe() {
    return android::base::Bazel::RunfilesPath(
            "goldfish+/emulator/libs/shared_memory/shared_memory_test_helper" EXE);
}

}  // namespace

TEST(SharedMemory, ShareVisibleWithinSameProc) {
    const auto user_read_only =
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::string unique_name = GetUniqueName("test_share");
    std::string message = "Hello World!";
    SharedMemory mWriter(unique_name, message.size());
    SharedMemory mReader(unique_name, message.size());

    ASSERT_FALSE(mWriter.IsOpen());
    ASSERT_FALSE(mReader.IsOpen());

    ASSERT_FALSE(mWriter.IsOpen());
    ASSERT_FALSE(mReader.IsOpen());

    auto s1 = mWriter.Create(user_read_only);
    ASSERT_TRUE(s1.ok()) << s1;

    auto s2 = mReader.Open(SharedMemory::AccessMode::kReadOnly);
    ASSERT_TRUE(s2.ok()) << s2;

    ASSERT_TRUE(mWriter.IsOpen());
    ASSERT_TRUE(mReader.IsOpen());

    memcpy(*mWriter, message.c_str(), message.size());
    std::string read(static_cast<const char*>(*mReader));
    ASSERT_EQ(message, read);
}

TEST(SharedMemory, MoveConstructor) {
    std::string unique_name = GetUniqueName("test_move_ctor");
    std::string message = "Moving data";
    SharedMemory source(unique_name, message.size());
    EXPECT_TRUE(
            source.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                    .ok());
    memcpy(*source, message.c_str(), message.size());

    SharedMemory dest(std::move(source));

    // Source should be empty/closed
    EXPECT_FALSE(source.IsOpen());
    EXPECT_EQ(0, source.Size());
    EXPECT_EQ(nullptr, *source);

    // Dest should have data
    ASSERT_TRUE(dest.IsOpen());
    EXPECT_EQ(message.size(), dest.Size());
    std::string read(static_cast<const char*>(*dest));
    EXPECT_EQ(message, read);
}

TEST(SharedMemory, MoveAssignment) {
    std::string unique_name = GetUniqueName("test_move_assign");
    std::string message = "Assigning data";
    SharedMemory source(unique_name, message.size());
    EXPECT_TRUE(
            source.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                    .ok());
    memcpy(*source, message.c_str(), message.size());

    SharedMemory dest("dummy", 123);
    dest = std::move(source);

    // Source should be empty
    EXPECT_FALSE(source.IsOpen());
    EXPECT_EQ(0, source.Size());

    // Dest should have data
    ASSERT_TRUE(dest.IsOpen());
    EXPECT_EQ(message.size(), dest.Size());
    std::string read(static_cast<const char*>(*dest));
    EXPECT_EQ(message, read);
}

TEST(SharedMemory, BasicPersistence) {
    std::string path = GetUniqueName("test_persistence");
    std::string message = "Persisted content";
    {
        SharedMemory writer(path, message.size());
        auto s1 = writer.Create(std::filesystem::perms::owner_read |
                                std::filesystem::perms::owner_write);
        ASSERT_TRUE(s1.ok()) << s1;
        memcpy(*writer, message.c_str(), message.size());

        // Verify file created
        ASSERT_TRUE(std::filesystem::exists(path));

        SharedMemory reader(path, message.size());
        auto s2 = reader.Open(SharedMemory::AccessMode::kReadOnly);
        ASSERT_TRUE(s2.ok()) << s2;

        std::string read(static_cast<const char*>(*reader));
        EXPECT_EQ(message, read);
    }
    // Writer destructor should have removed the file (ownership)
    // Note: Behavior differs on Win32 vs Posix regarding when deletion happens,
    // but SharedMemory implementation calls remove/DeleteFile on destruction if owner.
    EXPECT_FALSE(std::filesystem::exists(path)) << "File should be removed by owner destructor";
}

TEST(SharedMemory, CrossProcess) {
    std::string unique_name = GetUniqueName("x_read");
    std::string message = "Cross process data";
    SharedMemory writer(unique_name, message.size());
    EXPECT_TRUE(
            writer.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                    .ok());
    memcpy(*writer, message.c_str(), message.size());

    // Launch helper to read
    auto proc = android::base::Command::Create({GetHelperExe(), "--name", unique_name, "--size",
                                                std::to_string(message.size()), "--mode", "read",
                                                "--data", message})
                        .Execute();

    EXPECT_NE(proc, nullptr);
    EXPECT_EQ(proc->WaitFor(std::chrono::seconds(15)), std::future_status::ready);
    EXPECT_EQ(proc->ExitCode(), 0);
}

TEST(SharedMemory, CrossProcessWrite) {
    std::string unique_name = GetUniqueName("x_write");
    std::string message = "Writer data";
    size_t size = 1024;

    // Launch helper to write and sleep for 15 seconds to keep it alive
    auto proc = android::base::Command::Create({GetHelperExe(), "--name", unique_name, "--size",
                                                std::to_string(size), "--mode", "write", "--data",
                                                message, "--sleep", "15000"})
                        .Execute();

    EXPECT_NE(proc, nullptr);

    // Wait for helper to both create and populate the shared memory mapping.
    SharedMemory reader(unique_name, size);
    for (int i = 0; i < 100; ++i) {  // Try for up to 10 seconds
        if (!reader.IsOpen()) {
            (void)reader.Open(SharedMemory::AccessMode::kReadOnly);
        }
        if (reader.IsOpen() &&
            std::string_view(static_cast<const char*>(*reader)) == message) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(reader.IsOpen()) << "Failed to open shared memory created by helper";

    std::string read(static_cast<const char*>(*reader));
    EXPECT_EQ(message, read);

    // Terminate helper now that we are done
    proc->Terminate();
}

TEST(SharedMemory, CreateNoMapping) {
    const auto user_read_write =
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::string name = GetUniqueName("test_no_map");
    SharedMemory mem(name, 256);

    ASSERT_FALSE(mem.IsOpen());

    EXPECT_TRUE(mem.CreateNoMapping(user_read_write).ok());
    ASSERT_TRUE(mem.IsOpen());
    ASSERT_FALSE(mem.IsMapped());
    ASSERT_NE(SharedMemory::kInvalidHandle, mem.GetFd());

    mem.Close();
    ASSERT_FALSE(mem.IsOpen());
}

TEST(SharedMemory, DestructionPolicy) {
    std::string path_keep = GetUniqueName("test_keep");

    // Test kKeep
    {
        SharedMemory mem(path_keep, 128, SharedMemory::DestructionPolicy::kKeep);
        ASSERT_TRUE(
                mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());
        ASSERT_TRUE(std::filesystem::exists(path_keep));
    }
    // Should still exist after destruction
    EXPECT_TRUE(std::filesystem::exists(path_keep)) << "File should persist with kKeep";
    // Cleanup
    std::filesystem::remove(path_keep);

    std::string path_destroy = GetUniqueName("test_destroy");

    // Create file normally first
    {
        std::ofstream f(path_destroy);
        f << "data";
    }
    ASSERT_TRUE(std::filesystem::exists(path_destroy));

    {
        // Open with kDestroy
        SharedMemory mem(path_destroy, 4, SharedMemory::DestructionPolicy::kDestroy);
        // We use Open here. kDestroy should trigger unlink on Close even if we executed Open.
        ASSERT_TRUE(mem.Open(SharedMemory::AccessMode::kReadOnly).ok());
    }
    // Should be gone
    EXPECT_FALSE(std::filesystem::exists(path_destroy)) << "File should be removed with kDestroy";
}

TEST(SharedMemory, FileUriSupport) {
    // Get a standard path
    std::string path = GetUniqueName("test_uri");
    // Convert to URI. logic: file:// + absolute_path
    // On POSIX, path starts with /, so we get file:///path/to/file
    std::string uri = "file://" + path;

    SharedMemory writer(uri, 128);
    auto status =
            writer.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    ASSERT_TRUE(status.ok()) << "Failed to create with URI: " << status;

    // Verify the file exists at the *original* path, not the URI path
    EXPECT_TRUE(std::filesystem::exists(path)) << "File should exist at system path: " << path;

    writer.Close();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(SharedMemory, OpenNonExistentReturnsNotFound) {
    std::string path = GetUniqueName("non_existent");
    SharedMemory mem(path, 128);
    auto status = mem.Open(SharedMemory::AccessMode::kReadWrite);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kNotFound);
}

TEST(SharedMemory, OpenTooSmallSegmentReturnsOutOfRange) {
    std::string path = GetUniqueName("test_too_small");
    size_t initial_size = 128;
    {
        SharedMemory mem(path, initial_size, SharedMemory::DestructionPolicy::kKeep);
        ASSERT_TRUE(
                mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());
    }

    // Now try to open it with a larger size
    size_t larger_size = 256;
    SharedMemory mem_larger(path, larger_size);
    auto status = mem_larger.Open(SharedMemory::AccessMode::kReadWrite);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kOutOfRange);

    // Cleanup
    std::filesystem::remove(path);
}

TEST(SharedMemory, FileSizeCorrectlySet) {
    size_t size = 8192;
    std::string path = GetUniqueName("test_filesize");
    {
        SharedMemory writer(path, size);
        auto s1 = writer.Create(std::filesystem::perms::owner_read |
                                std::filesystem::perms::owner_write);
        ASSERT_TRUE(s1.ok()) << s1;

        EXPECT_EQ(std::filesystem::file_size(path), size);

        SharedMemory reader(path, size);
        auto s2 = reader.Open(SharedMemory::AccessMode::kReadOnly);
        ASSERT_TRUE(s2.ok()) << s2;
        EXPECT_EQ(std::filesystem::file_size(path), size);
    }
    EXPECT_FALSE(std::filesystem::exists(path));
}

}  // namespace goldfish::memory
