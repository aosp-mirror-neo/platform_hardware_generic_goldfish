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

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "absl/algorithm/container.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"

#include "android/base/eintr_wrapper.h"
#include "android/status/status_macros.h"
#include "goldfish/file/file.h"
#include "goldfish/file/storage_capacity.h"

// NOLINTBEGIN
namespace android::base::file {

absl::StatusOr<fs::path> make_absolute(const fs::path& path) noexcept {
    std::error_code ec;
    if (fs::path abs = fs::absolute(path, ec); !ec) {
        return abs;
    }
    return absl::InternalError(
            absl::StrCat("Failed to make path absolute: ", path.string(), " - ", ec.message()));
}

absl::StatusOr<fs::path> make_relative(const fs::path& path, const fs::path& base_path) noexcept {
    std::error_code ec;
    if (fs::path rel = fs::relative(path, base_path, ec); !ec) {
        return rel;
    }
    return absl::InternalError(
            absl::StrCat("Failed to make path relative: ", path.string(), " - ", ec.message()));
}

absl::StatusOr<fs::path> make_canonical(const fs::path& path) noexcept {
    std::error_code ec;
    if (fs::path canon = fs::canonical(path, ec); !ec) {
        return canon;
    }
    return absl::InternalError(
            absl::StrCat("Failed to make path canonical: ", path.string(), " - ", ec.message()));
}

absl::StatusOr<fs::path> read_symlink(const fs::path& path) noexcept {
    std::error_code ec;
    if (fs::path target = fs::read_symlink(path, ec); !ec) {
        return target;
    }
    return absl::InternalError(
            absl::StrCat("Failed to read symlink: ", path.string(), " - ", ec.message()));
}

bool exists(const fs::path& path) noexcept {
    std::error_code ec;
    // Ignore EC - false is returned when there's an error too.
    return fs::exists(path, ec);
    // Alternatively:
    // int ret = path_access(path, F_OK);
    // return (ret == 0) || (errno != ENOENT);
}

bool is_file(const fs::path& path) noexcept {
    std::error_code ec;
    // Ignore EC - false is returned when there's an error too.
    return fs::is_regular_file(path, ec);
}

bool is_dir(const fs::path& path) noexcept {
    std::error_code ec;
    // Ignore EC - false is returned when there's an error too.
    return fs::is_directory(path, ec);
}

bool is_link(const fs::path& path) noexcept {
    std::error_code ec;
    // Ignore EC - false is returned when there's an error too.
    return fs::is_symlink(path, ec);
}

namespace {

#ifdef _WIN32
static int GetWin32Mode(int mode) {
    // Convert |mode| to win32 permission bits.
    int win32mode = 0x0;

    if ((mode & R_OK) || (mode & X_OK)) {
        win32mode |= 0x4;
    }
    if (mode & W_OK) {
        win32mode |= 0x2;
    }

    return win32mode;
}
#endif

int path_access(const fs::path& path, int mode) noexcept {
#ifdef _WIN32
    // Always use w version, even when UNICODE not defined.
    return _waccess(path.wstring().c_str(), GetWin32Mode(mode));
#else   // !_WIN32
    return HANDLE_EINTR(access(path.c_str(), mode));
#endif  // !_WIN32
}

}  // namespace

bool can_read(const fs::path& path) noexcept {
    return path_access(path, R_OK) == 0;
}

bool can_write(const fs::path& path) noexcept {
    return path_access(path, W_OK) == 0;
}

bool can_exec(const fs::path& path) noexcept {
    return path_access(path, X_OK) == 0;
}

absl::StatusOr<StorageCapacity> file_size(const fs::path& path) noexcept {
    std::error_code ec;
    if (std::uintmax_t size = fs::file_size(path, ec); !ec) {
        return size;
    }
    return absl::InternalError(
            absl::StrCat("Failed to get size of: ", path.string(), " - ", ec.message()));
}

absl::StatusOr<StorageCapacity> free_space(const fs::path& path) noexcept {
    std::error_code ec;
    if (fs::space_info info = fs::space(path, ec); !ec) {
        if (info.available == static_cast<std::uintmax_t>(-1)) {
            return absl::InternalError(
                    absl::StrCat("Failed to determine free space of: ", path.string()));
        }
        return StorageCapacity(static_cast<StorageCapacity::ValueType>(info.available));
    }
    return absl::InternalError(
            absl::StrCat("Failed to get free space of: ", path.string(), " - ", ec.message()));
}

absl::StatusOr<absl::Time> last_write_time(const fs::path& path) noexcept {
    std::error_code ec;
    if (std::filesystem::file_time_type ftime = std::filesystem::last_write_time(path, ec); !ec) {
        // TODO (whollins): use c++20 clock_cast when our version of clang supports it:
        // auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        auto system_time = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        ftime.time_since_epoch()));
        return absl::FromChrono(system_time);
    }
    return absl::InternalError(
            absl::StrCat("Failed to get last_wire_time of: ", path.string(), " - ", ec.message()));
}

namespace {
template <typename it_type>
std::vector<fs::path> scan_dir_impl(const fs::path& dirPath, bool fullPath) noexcept {
    std::error_code ec;

    std::vector<fs::path> x;
    for (const auto& e : it_type(dirPath, ec)) {
        if (fullPath) {
            // This will be relative if dirPath is relative.
            x.push_back(e.path());
        } else {
            x.push_back(e.path().filename());
        }
    }
    absl::c_sort(x);

    return x;
}
}  // namespace

std::vector<fs::path> scan_dir(const fs::path& dirPath, bool fullPath) noexcept {
    return scan_dir_impl<std::filesystem::directory_iterator>(dirPath, fullPath);
}

std::vector<fs::path> scan_dir_recursive(const fs::path& dirPath) noexcept {
    return scan_dir_impl<std::filesystem::recursive_directory_iterator>(dirPath, /*fullPath=*/true);
}

namespace {
fs::perms octal_mode_to_perms(unsigned octalMode) noexcept {
    fs::perms mode = fs::perms::none;

    // Owner permissions
    mode |= (octalMode & 0400) ? fs::perms::owner_read : fs::perms::none;
    mode |= (octalMode & 0200) ? fs::perms::owner_write : fs::perms::none;
    mode |= (octalMode & 0100) ? fs::perms::owner_exec : fs::perms::none;

    // Group permissions
    mode |= (octalMode & 0040) ? fs::perms::group_read : fs::perms::none;
    mode |= (octalMode & 0020) ? fs::perms::group_write : fs::perms::none;
    mode |= (octalMode & 0010) ? fs::perms::group_exec : fs::perms::none;

    // Others permissions
    mode |= (octalMode & 0004) ? fs::perms::others_read : fs::perms::none;
    mode |= (octalMode & 0002) ? fs::perms::others_write : fs::perms::none;
    mode |= (octalMode & 0001) ? fs::perms::others_exec : fs::perms::none;

    return mode;
}

unsigned perms_to_octal_mode(fs::perms perms) noexcept {
    return static_cast<unsigned>(perms);
}
}  // namespace

absl::StatusOr<unsigned> mode(const fs::path& path) noexcept {
    std::error_code ec;
    if (fs::file_status stat = fs::status(path, ec); !ec) {
        return perms_to_octal_mode(stat.permissions());
    }
    return absl::InternalError(
            absl::StrCat("Failed to get file mode of: ", path.string(), " - ", ec.message()));
}

absl::Status chmod(const fs::path& path, unsigned octalMode) noexcept {
    if (std::error_code ec;
        fs::permissions(path, octal_mode_to_perms(octalMode), fs::perm_options::replace, ec), ec) {
        return absl::InternalError(
                absl::StrCat("Failed to chmod: ", path.string(), " - ", ec.message()));
    }
    return absl::OkStatus();
}

absl::Status mkdir(const fs::path& path, unsigned octalMode) noexcept {
    if (std::error_code ec; !fs::create_directory(path, ec)) {
        return absl::InternalError(
                absl::StrCat("Failed to mkdir: ", path.string(), " - ", ec.message()));
    }
    return chmod(path, octalMode);
}

absl::Status mkdir_recursive(const fs::path& path, unsigned octalMode) noexcept {
    // We don't use fs::create_directories here as it doesn't set permissions.
    if (fs::exists(path)) {
        return absl::OkStatus();
    }
    if (!path.has_parent_path()) {
        // Hit the root
        return absl::OkStatus();
    }
    if (auto s = mkdir_recursive(path.parent_path(), octalMode); !s.ok()) {
        return s;
    }
    if (!path.has_filename()) {
        // Ignore trailing separator
        return absl::OkStatus();
    }
    return mkdir(path, octalMode);
}

absl::Status rm(const fs::path& path) noexcept {
    if (std::error_code ec; fs::remove(path, ec), ec) {
        return absl::InternalError(
                absl::StrCat("Failed to rm: ", path.string(), " - ", ec.message()));
    }
    return absl::OkStatus();
}

absl::Status rm_recursive(const fs::path& path) noexcept {
    if (std::error_code ec; fs::remove_all(path, ec), ec) {
        return absl::InternalError(
                absl::StrCat("Failed to rm recursively: ", path.string(), " - ", ec.message()));
    }
    return absl::OkStatus();
}

absl::Status cp_file(const fs::path& from, const fs::path& to, bool overwrite) noexcept {
    fs::copy_options opt =
            overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    if (std::error_code ec; fs::copy_file(from, to, opt, ec), ec) {
        return absl::InternalError(absl::StrCat("Failed to copy_file: ", from.string(), "->",
                                                to.string(), " - ", ec.message()));
    }
    return absl::OkStatus();
}

absl::Status cp_recursive(const fs::path& from, const fs::path& to, bool overwrite) noexcept {
    fs::copy_options opt =
            overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    if (std::error_code ec;
        fs::copy(from, to, opt | std::filesystem::copy_options::recursive, ec), ec) {
        return absl::InternalError(absl::StrCat("Failed to copy recursively: ", from.string(), "->",
                                                to.string(), " - ", ec.message()));
    }
    return absl::OkStatus();
}

absl::Status mv_file(const fs::path& from, const fs::path& to, bool fallback_to_copy_rm) noexcept {
    std::error_code ec;
    if (fs::rename(from, to, ec); !ec) {
        return absl::OkStatus();
    }
    if (!fallback_to_copy_rm) {
        return absl::InternalError(absl::StrCat("Failed to atomically rename file: ", from.string(),
                                                "->", to.string(), " - ", ec.message()));
    }
    // fs::rename can fail if files are on different disks or locked on Windows, so fallback to copy then rm.
    VLOG(1) << "fs::rename failed for " << from.string() << " -> " << to.string()
            << " - reverting to slower copy-then-delete: " << ec.message();
    RETURN_IF_ERROR(cp_file(from, to, /*overwrite=*/true));
    return rm(from);
}

absl::Status touch(const fs::path& path) noexcept {
    std::ofstream f(path);
    if (f) {
        return absl::OkStatus();
    }
    return absl::DataLossError(absl::StrCat("Unable to create file: ", path.string()));
}

absl::StatusOr<std::string> read_whole_file(const fs::path& path, bool binary) noexcept {
    std::ifstream ifs(path, binary ? (std::ios::in | std::ios::binary) : std::ios::in);
    if (ifs.fail()) {
        return absl::UnavailableError(absl::StrCat("failed to open file: ", path.string()));
    }
    std::string data{std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    if (ifs.fail()) {
        return absl::UnavailableError(
                absl::StrCat("failed while reading from file: ", path.string()));
    }

    return data;
}

absl::Status copy_if_missing(const fs::path& dst, const fs::path& src) {
    if (fs::exists(dst)) {
        return absl::OkStatus();
    }

    std::error_code ec;
    if (!fs::copy_file(src, dst, ec)) {
        return absl::InternalError(ec.message());
    }

    return absl::OkStatus();
}

// NOLINTEND
}  // namespace android::base::file
