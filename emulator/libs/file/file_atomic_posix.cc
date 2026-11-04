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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "android/base/eintr_wrapper.h"
#include "emulator/libs/file/posix_fd_io.h"
#include "goldfish/file/file.h"
#include "goldfish/file/file_atomic.h"
#include "goldfish/file/scoped_fd.h"

#ifndef O_BINARY
#ifdef _O_BINARY
#define O_BINARY _O_BINARY
#else
#define O_BINARY 0  // If this isn't defined, the platform doesn't need it.
#endif
#endif

namespace android::base::file {

namespace {

absl::Status MapCreatePrivateFileErrno(int err, std::string_view context, std::string_view hint) {
    const absl::Status errno_status = absl::ErrnoToStatus(err, context);
    const std::string detail = absl::StrCat(errno_status.message(), ". ", hint);
    if (err == EACCES || err == EPERM) {
        return absl::PermissionDeniedError(detail);
    }
    return absl::InternalError(detail);
}

}  // namespace

absl::Status CreatePrivateFileExclusive(const fs::path& path, std::string_view content) noexcept {
    // Generate a unique temporary file path cleanly in the same directory.
    std::string tmp_name_template = absl::StrCat(path.string(), ".tmp.XXXXXX");

    // Open with 0600 permissions and close-on-exec flag, tmp_name_template is modified in place.
    const int raw_fd = ::mkostemp(tmp_name_template.data(), O_CLOEXEC);
    if (raw_fd < 0) {
        const int err = errno;
        return MapCreatePrivateFileErrno(
                err,
                absl::StrCat("Failed to write to '", path.string(),
                             "': could not create temporary file in '",
                             path.parent_path().string(), "'"),
                "Ensure the parent directory exists, is writable, and has sufficient disk space.");
    }
    ScopedFd fd(raw_fd);

    // Setup cleanup to remove the temporary file.
    // We use hard links so we can always remove it.
    absl::Cleanup remove_tmp_file = [tmp_name_template] {
        if (auto s = rm(tmp_name_template); !s.ok()) {
            LOG(WARNING) << "Failed to remove temporary file '" << tmp_name_template
                         << "': " << s.message()
                         << ". You might have to manually remove it to reclaim space.";
        }
    };

    if (absl::Status status = internal::WriteAllToFd(fd.get(), content, tmp_name_template);
        !status.ok()) {
        const std::string detail = absl::StrCat(
                status.message(), ". Please check if the disk is full or permissions changed.");
        if (absl::IsPermissionDenied(status)) {
            return absl::PermissionDeniedError(detail);
        }
        return absl::InternalError(detail);
    }

    // We are going to create a hardlink, which means that path will point
    // to the same (refcounted) inode as tmp_name_template, if path already points to something
    // this will fail. Since it is a hardlink we can safely remove tmp_name_template, as it
    // will merely decrease the refcount.
    const int link_rc = HANDLE_EINTR(::link(tmp_name_template.c_str(), path.c_str()));
    if (link_rc != 0) {
        const int err = errno;
        if (err == EEXIST) {
            return absl::AlreadyExistsError(absl::StrCat(
                    "The file '", path.string(),
                    "' already exists and cannot be overwritten for security reasons."));
        }
        return MapCreatePrivateFileErrno(
                err, absl::StrCat("Failed to finalize creation of '", path.string(), "'"),
                "Check permissions or if another process is modifying this directory.");
    }

    return absl::OkStatus();
}

}  // namespace android::base::file
