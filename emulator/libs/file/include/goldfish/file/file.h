// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

// NOLINTBEGIN
#include <filesystem>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "goldfish/file/storage_capacity.h"

namespace fs = std::filesystem;

namespace android::base::file {

absl::StatusOr<fs::path> make_absolute(const fs::path& path) noexcept;
absl::StatusOr<fs::path> make_relative(const fs::path& path, const fs::path& base_path) noexcept;
absl::StatusOr<fs::path> make_canonical(const fs::path& path) noexcept;
absl::StatusOr<fs::path> read_symlink(const fs::path& path) noexcept;

bool exists(const fs::path& path) noexcept;

bool is_file(const fs::path& path) noexcept;
bool is_dir(const fs::path& path) noexcept;
bool is_link(const fs::path& path) noexcept;

bool can_read(const fs::path& path) noexcept;
bool can_write(const fs::path& path) noexcept;
bool can_exec(const fs::path& path) noexcept;

inline constexpr StorageCapacity kDiskPressureLimit = 2_GiB;

absl::StatusOr<StorageCapacity> file_size(const fs::path& path) noexcept;
absl::StatusOr<StorageCapacity> free_space(const fs::path& path) noexcept;
absl::StatusOr<absl::Time> last_write_time(const fs::path& path) noexcept;

std::vector<fs::path> scan_dir(const fs::path& dirPath, bool fullPath = false) noexcept;
std::vector<fs::path> scan_dir_recursive(const fs::path& dirPath) noexcept;

absl::StatusOr<unsigned> mode(const fs::path& path) noexcept;

// TODO(b/465404199): Consider changing API to take mode as fs::perms instead of int.
absl::Status chmod(const fs::path& path, unsigned octalMode) noexcept;

absl::Status mkdir(const fs::path& path, unsigned octalMode) noexcept;
absl::Status mkdir_recursive(const fs::path& path, unsigned octalMode) noexcept;

absl::Status rm(const fs::path& path) noexcept;
absl::Status rm_recursive(const fs::path& path) noexcept;

absl::Status cp_file(const fs::path& from, const fs::path& to, bool overwrite = false) noexcept;
absl::Status cp_recursive(const fs::path& from, const fs::path& to,
                          bool overwrite = false) noexcept;

absl::Status mv_file(const fs::path& from, const fs::path& to,
                     bool fallback_to_copy_rm = true) noexcept;

absl::Status touch(const fs::path& path) noexcept;

absl::StatusOr<std::string> read_whole_file(const fs::path& path, bool binary = false) noexcept;

absl::Status copy_if_missing(const std::filesystem::path& dst, const std::filesystem::path& src);

// NOLINTEND
}  // namespace android::base::file
