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

#include <string_view>

#include "absl/status/status.h"

#include "gfxstream/host/features.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "render-utils/renderer_enums.h"

// Pure functions that turn environment/config strings into gfxstream settings.
// None of them reads the environment: the caller passes the raw values so the
// functions can be unit-tested with a table.

namespace goldfish::legacy {

// Maps the ANDROID_EMU_RENDERER value to the gfxstream backend. Every software
// mode resolves to lavapipe (SwiftShader is being removed, b/570473107), and
// host GPU is not wired up on the goldfish_pipe path below API 37 yet, so it
// falls back to lavapipe with a warning instead of silently changing the mode.
// An empty value selects lavapipe.
SelectedRenderer ParseRendererBackend(std::string_view env_value);

// Fills `features` with the goldfish_pipe defaults (GL only, no Vulkan, BAR 1
// DMA through goldfish_address_space) and then applies `renderer_features`,
// a ','/';'-separated list of "<FeatureName>:enabled|disabled" entries.
// `egl_on_egl` is the ANDROID_EGL_ON_EGL=1 switch.
absl::Status BuildFeatureSet(std::string_view renderer_features, bool egl_on_egl,
                             gfxstream::host::FeatureSet* features);

// Parses the ANDROID_EMU_GOLDFISH_GPU_VERIFY override ("off", "log", "enforce",
// "strict", or 0-3). An empty or unknown value keeps `default_mode`.
GoldfishGpuVerifyMode ParseVerifyMode(std::string_view env_value,
                                      GoldfishGpuVerifyMode default_mode);

std::string_view VerifyModeName(GoldfishGpuVerifyMode mode);

}  // namespace goldfish::legacy
