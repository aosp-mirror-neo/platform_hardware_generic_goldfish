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

#include <filesystem>
#include <string_view>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "android/base/system.h"
#include "goldfish/os/dynamic_library.h"
#include "render-utils/RenderLib.h"

// Finds and loads libgfxstream_backend and resolves its `initLibrary` entry
// point. The library handle is kept for the lifetime of the loader (the
// RenderLib it produced refers into it).

namespace goldfish::legacy {

// Platform file name of the gfxstream backend library.
const char* GfxstreamLibraryName();

// Directory containing the running plugin (empty when unknown).
std::filesystem::path CurrentModuleDirectory();

// Where to look for the backend, in order (same lookup as
// gfxstream_backend_loader.c): the packaged library directory first, then
// next to this plugin, and the dynamic loader's own search path only when no
// packaged directory was given, so that a packaged emulator whose copy fails
// to load reports that rather than silently picking up a foreign
// libgfxstream_backend.
std::vector<std::filesystem::path> GfxstreamLibraryCandidates(
        std::string_view lib_dir, const std::filesystem::path& plugin_dir);

// On Linux the bundled Mesa libEGL.so defaults to _EGL_PLATFORM_X11 and
// libgallium_dri.so is built with only GALLIUM_LLVMPIPE (swrast). QEMU runs
// without an X11 display, so select the surfaceless EGL platform and the
// software rasterizer unless the user already set them.
//
// On macOS and Windows the bundled libEGL is ANGLE-on-Vulkan
// (ANGLE_DEFAULT_PLATFORM=vulkan). Because GoldfishGpu disables guest Vulkan
// (features.Vulkan = false), gfxstream's FrameBuffer::Impl::Create() skips
// vk::vkDispatch() / initIcdPaths(), which would otherwise translate
// ANDROID_EMU_VK_ICD=lavapipe into VK_DRIVER_FILES / VK_ICD_FILENAMES. Point
// both variables at the bundled lvp_icd.json when neither is set so ANGLE's
// Vulkan loader selects Lavapipe instead of an unsupported host ICD.
void ApplySoftwareEglDefaults(android::base::System* system,
                              const std::filesystem::path& plugin_dir = CurrentModuleDirectory());

class GfxstreamLibraryLoader {
  public:
    // Loads (once) the first candidate for `lib_dir` (ANDROID_EMULATOR_LIBRARY_DIR)
    // and calls its initLibrary(). NOT_FOUND lists every candidate and the OS
    // loader's error for it; INTERNAL when the symbol is missing or returns
    // null.
    absl::StatusOr<gfxstream::RenderLibPtr> Load(std::string_view lib_dir) ABSL_LOCKS_EXCLUDED(mu_);

    // Path of the library that was loaded, or empty.
    std::filesystem::path LoadedPath() const ABSL_LOCKS_EXCLUDED(mu_);

  private:
    mutable absl::Mutex mu_;
    goldfish::os::DynamicLibrary library_ ABSL_GUARDED_BY(mu_);
    std::filesystem::path path_ ABSL_GUARDED_BY(mu_);
};

}  // namespace goldfish::legacy
