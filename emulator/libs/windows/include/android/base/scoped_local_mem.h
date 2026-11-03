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
#error "Only compile this file when targeting Windows!"
#endif

#include <windows.h>

#include "goldfish/base/unique_handle.h"

namespace android::base {

struct LocalDeleter {
    void operator()(void* ptr) const {
        if (ptr) {
            ::LocalFree(ptr);
        }
    }
};

/**
 * @brief RAII wrapper for memory allocated via LocalAlloc or returned by
 *        Win32 APIs that require LocalFree (e.g., SIDs, ACLs).
 */
using ScopedLocalMem = goldfish::base::UniqueHandle<void*, nullptr, LocalDeleter>;

}  // namespace android::base
