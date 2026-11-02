// Copyright 2014 The Android Open Source Project
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

#if !defined(_WIN32) && !defined(_WIN64)
#error "Only compile this file when targeting Windows!"
#endif

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include "goldfish/base/unique_handle.h"

namespace android::base {

namespace internal {
struct WinHandleDeleter {
    void operator()(HANDLE handle) const {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            ::CloseHandle(handle);
        }
    }
};
}  // namespace internal

/**
 * @brief RAII wrapper for Windows HANDLEs closed via CloseHandle().
 *
 * This class normalizes both nullptr and INVALID_HANDLE_VALUE to nullptr,
 * providing move semantics and consistent 'if (handle)' checks.
 */
class ScopedFileHandle
        : public goldfish::base::UniqueHandle<HANDLE, nullptr, internal::WinHandleDeleter> {
    using Super = goldfish::base::UniqueHandle<HANDLE, nullptr, internal::WinHandleDeleter>;

  public:
    ScopedFileHandle() : Super() {}

    explicit ScopedFileHandle(HANDLE handle)
            : Super(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}

    // Maintain backward compatibility for any old .ok() or .close() calls
    bool ok() const { return Super::ok(); }
    void close() { reset(); }
};

using ScopedEventHandle = ScopedFileHandle;

}  // namespace android::base
