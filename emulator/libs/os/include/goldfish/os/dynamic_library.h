/* Copyright 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <filesystem>

#include "goldfish/base/unique_handle.h"

#if defined(__linux__)
#include <dlfcn.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32)
#include <Windows.h>
#else
#error unknown OS
#endif

namespace goldfish::os {

struct DynamicLibrary {
    DynamicLibrary() = default;
    explicit DynamicLibrary(const std::filesystem::path& path);
    DynamicLibrary(DynamicLibrary&& rhs) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& rhs) noexcept;

    bool ok() const;  // NOLINT
    void* operator[](const char*) const;

    friend void swap(DynamicLibrary& lhs, DynamicLibrary& rhs) noexcept;  // NOLINT

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  private:
#if defined(_WIN32)
    struct HandleDeleter {
        void operator()(HMODULE) const;
    };
    using LibraryHandle = goldfish::base::UniqueHandle<HMODULE, nullptr, HandleDeleter>;
#else
    struct HandleDeleter {
        void operator()(void*) const;
    };
    using LibraryHandle = goldfish::base::UniqueHandle<void*, nullptr, HandleDeleter>;
#endif

    LibraryHandle handle_;
};

}  // namespace goldfish::os
