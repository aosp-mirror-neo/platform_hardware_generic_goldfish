// Copyright (C) 2025 The Android Open Source Project
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

#include <cstdint>

namespace android::emulation::control {

/**
 * @brief Maximum number of pointers supported by multi-touch emulation.
 */
constexpr uint32_t kMtsPointersNum = 10;

/**
 * @brief Signals that a pointer is not tracked (or is "up").
 */
constexpr uint32_t kMtsPointerUp = -1;

/**
 * @brief Maximum ABS_MT_PRESSURE value (full contact pressure).
 *
 * Must match MTS_PRESSURE_RANGE_MAX advertised by virtio-input-android.
 */
constexpr uint32_t kMtsPressureMax = 0x400;

}  // namespace android::emulation::control
