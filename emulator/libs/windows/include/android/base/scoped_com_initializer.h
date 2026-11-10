// Copyright 2026 The Android Open Source Project
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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

#include "goldfish/base/unique_handle.h"

namespace android::base {

namespace internal {
struct ComInitializerDeleter {
    void operator()(HRESULT hr) const noexcept {
        if (SUCCEEDED(hr)) {
            ::CoUninitialize();
        }
    }
};
}  // namespace internal

/**
 * @brief RAII wrapper for Windows COM library initialization on the calling thread.
 *
 * Calls CoInitializeEx(nullptr, coinit) on construction and balances successful
 * initializations (S_OK or S_FALSE) with CoUninitialize() on destruction via
 * goldfish::base::UniqueHandle.
 *
 * When CoInitializeEx returns RPC_E_CHANGED_MODE, COM is already initialized on
 * the thread with a different concurrency model: COM remains usable for
 * thread-neutral APIs (so ok() / operator bool() returns true), but the COM
 * reference count was not incremented and CoUninitialize() is not called.
 */
class ScopedComInitializer
        : public ::goldfish::base::UniqueHandle<HRESULT, E_UNEXPECTED,
                                                internal::ComInitializerDeleter> {
    using Super =
            ::goldfish::base::UniqueHandle<HRESULT, E_UNEXPECTED, internal::ComInitializerDeleter>;

  public:
    explicit ScopedComInitializer(DWORD coinit = COINIT_APARTMENTTHREADED) noexcept
            : Super(::CoInitializeEx(nullptr, coinit)) {}

    // Returns true if CoInitializeEx succeeded (S_OK or S_FALSE) and this
    // instance owns a COM initialization reference that will be released
    // via CoUninitialize().
    bool succeeded() const noexcept { return SUCCEEDED(get()); }

    // Returns true if COM is available on the calling thread, including when
    // it was already initialized with a different apartment model
    // (RPC_E_CHANGED_MODE).
    bool ok() const noexcept { return succeeded() || get() == RPC_E_CHANGED_MODE; }
    explicit operator bool() const noexcept { return ok(); }

    HRESULT hr() const noexcept { return get(); }

    void reset() noexcept { Super::reset(); }
};

}  // namespace android::base
