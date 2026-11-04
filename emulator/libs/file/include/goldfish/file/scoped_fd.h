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

#ifdef _WIN32
#error "Only compile this file on POSIX platforms!"
#endif

#include <unistd.h>

#include "android/base/eintr_wrapper.h"
#include "goldfish/base/unique_handle.h"

namespace android::base::file {

struct FdDeleter {
    void operator()(int fd) const noexcept {
        if (fd >= 0) {
            (void)IGNORE_EINTR(::close(fd));
        }
    }
};

using ScopedFd = ::goldfish::base::UniqueHandle<int, -1, FdDeleter>;

}  // namespace android::base::file
