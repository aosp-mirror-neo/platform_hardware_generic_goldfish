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

#pragma once

#ifndef _WIN32

#include <unistd.h>

#include <filesystem>
#include <string_view>

#include "absl/status/status.h"
#include "android/base/eintr_wrapper.h"
#include "goldfish/base/unique_handle.h"

namespace android::base::file::internal {

struct PosixFdDeleter {
    void operator()(int fd) const noexcept {
        if (fd >= 0) {
            (void)IGNORE_EINTR(::close(fd));
        }
    }
};

using ScopedFd = ::goldfish::base::UniqueHandle<int, -1, PosixFdDeleter>;

absl::Status WriteAllToFd(int fd, std::string_view data, const std::filesystem::path& path);

}  // namespace android::base::file::internal

#endif  // !_WIN32
