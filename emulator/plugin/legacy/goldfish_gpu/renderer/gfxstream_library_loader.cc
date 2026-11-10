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

#include "goldfish/legacy/goldfish_gpu/renderer/gfxstream_library_loader.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "goldfish/file/file.h"

namespace goldfish::legacy {
namespace {

// Text of the OS loader's last error, for the DynamicLibrary that was just
// constructed (goldfish::os::DynamicLibrary only exposes ok()).
std::string LastDynamicLoaderError() {
#if defined(_WIN32)
    const DWORD code = ::GetLastError();
    char* buffer = nullptr;
    const DWORD len =
            ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = (len > 0 && buffer != nullptr) ? std::string(buffer, len) : std::string();
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return absl::StrCat("error ", code, text.empty() ? "" : ": ", text);
#else
    const char* err = ::dlerror();
    return err != nullptr ? std::string(err) : std::string("dlopen failed");
#endif
}

}  // namespace

const char* GfxstreamLibraryName() {
#if defined(_WIN32)
    return "gfxstream_backend.dll";
#elif defined(__APPLE__)
    return "libgfxstream_backend.dylib";
#else
    return "libgfxstream_backend.so";
#endif
}

std::filesystem::path CurrentModuleDirectory() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&CurrentModuleDirectory), &module)) {
        return {};
    }
    wchar_t wpath[4096];
    const DWORD len = ::GetModuleFileNameW(module, wpath, 4096);
    if (len == 0 || len >= 4096) {
        return {};
    }
    return std::filesystem::path(std::wstring(wpath, len)).parent_path();
#else
    Dl_info info = {};
    if (::dladdr(reinterpret_cast<const void*>(&CurrentModuleDirectory), &info) == 0 ||
        info.dli_fname == nullptr) {
        return {};
    }
    return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

std::vector<std::filesystem::path> GfxstreamLibraryCandidates(
        std::string_view lib_dir, const std::filesystem::path& plugin_dir) {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path lib_dir_path{std::string(lib_dir)};
    if (!lib_dir.empty()) {
        candidates.emplace_back(lib_dir_path / GfxstreamLibraryName());
    }
    if (!plugin_dir.empty() && plugin_dir != lib_dir_path) {
        candidates.emplace_back(plugin_dir / GfxstreamLibraryName());
    }
    if (lib_dir.empty()) {
        candidates.emplace_back(GfxstreamLibraryName());
    }
    return candidates;
}

void ApplySoftwareEglDefaults(android::base::System* system,
                              const std::filesystem::path& plugin_dir) {
#if defined(__linux__)
    if (system->EnvGet("EGL_PLATFORM").empty()) {
        system->EnvSet("EGL_PLATFORM", "surfaceless");
    }
    if (system->EnvGet("LIBGL_ALWAYS_SOFTWARE").empty()) {
        system->EnvSet("LIBGL_ALWAYS_SOFTWARE", "1");
    }
#endif
    if (system->EnvGet("VK_DRIVER_FILES").empty() && system->EnvGet("VK_ICD_FILENAMES").empty()) {
        std::vector<std::filesystem::path> roots;
        const std::string lib_dir = system->EnvGet("ANDROID_EMULATOR_LIBRARY_DIR");
        if (!lib_dir.empty()) {
            roots.emplace_back(lib_dir);
        }
        const std::string launcher_dir = system->EnvGet("ANDROID_EMULATOR_LAUNCHER_DIR");
        if (!launcher_dir.empty()) {
            roots.push_back(std::filesystem::path(launcher_dir) / "lib64");
        }
        if (!plugin_dir.empty()) {
            roots.push_back(plugin_dir);
        }
        for (const std::filesystem::path& root : roots) {
            for (const std::filesystem::path& candidate :
                 {root / "vulkan" / "lvp_icd.json", root / "lvp_icd.json"}) {
                if (android::base::file::is_file(candidate)) {
                    const std::string icd_path = candidate.string();
                    system->EnvSet("VK_DRIVER_FILES", icd_path);
                    system->EnvSet("VK_ICD_FILENAMES", icd_path);
                    return;
                }
            }
        }
    }
}

absl::StatusOr<gfxstream::RenderLibPtr> GfxstreamLibraryLoader::Load(std::string_view lib_dir) {
    const std::vector<std::filesystem::path> candidates =
            GfxstreamLibraryCandidates(lib_dir, CurrentModuleDirectory());

    absl::MutexLock lock(&mu_);
    if (!library_.ok()) {
        std::vector<std::string> errors;
        for (const std::filesystem::path& candidate : candidates) {
            goldfish::os::DynamicLibrary library(candidate);
            if (library.ok()) {
                library_ = std::move(library);
                path_ = candidate;
                break;
            }
            errors.push_back(absl::StrCat(candidate.string(), ": ", LastDynamicLoaderError()));
        }
        if (!library_.ok()) {
            return absl::NotFoundError(absl::StrCat(
                    "failed to load ", GfxstreamLibraryName(), " (ANDROID_EMULATOR_LIBRARY_DIR=",
                    lib_dir.empty() ? "<unset>" : lib_dir, "): ", absl::StrJoin(errors, "; ")));
        }
    }
    const std::string path = path_.string();
    using InitLibraryFn = gfxstream::RenderLibPtr (*)();
    auto init_library = reinterpret_cast<InitLibraryFn>(library_["initLibrary"]);
    if (init_library == nullptr) {
        return absl::InternalError(absl::StrCat("symbol 'initLibrary' not found in '", path, "'"));
    }
    gfxstream::RenderLibPtr render_lib = init_library();
    if (!render_lib) {
        return absl::InternalError(
                absl::StrCat("initLibrary() returned nullptr from '", path, "'"));
    }
    return render_lib;
}

std::filesystem::path GfxstreamLibraryLoader::LoadedPath() const {
    absl::MutexLock lock(&mu_);
    return path_;
}

}  // namespace goldfish::legacy
