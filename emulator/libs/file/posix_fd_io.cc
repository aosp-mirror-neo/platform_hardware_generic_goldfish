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

#include "emulator/libs/file/posix_fd_io.h"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string_view>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "android/base/eintr_wrapper.h"

namespace android::base::file::internal {

absl::Status WriteAllToFd(int fd, std::string_view data, const std::filesystem::path& path) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = HANDLE_EINTR(::write(fd, data.data() + written, data.size() - written));
        if (n < 0) {
            const int err = errno;
            return absl::ErrnoToStatus(err,
                                       absl::StrCat("Failed to write to '", path.string(), "'"));
        }
        if (n == 0) {
            return absl::InternalError(
                    absl::StrCat("Unexpected zero-byte write to '", path.string(), "'"));
        }
        written += static_cast<size_t>(n);
    }
    return absl::OkStatus();
}

}  // namespace android::base::file::internal
