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

#include "goldfish/audio/goldfish_audio_mux.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include <gmodule.h>
extern "C" {
#include "google/system/aemu_func_defs.h"
}
// IWYU pragma: end_keep
// clang-format on

extern "C" {

G_MODULE_EXPORT void GF_REGISTER_TYPES_FUNC(void) {
    goldfish::audio::audio_mux_register_types();
}

G_MODULE_EXPORT void GF_STARTUP_FUNC(int /*argc*/, char** /*argv*/) {}

G_MODULE_EXPORT void GF_SHUTDOWN_FUNC(void) {}

}  // extern "C"
