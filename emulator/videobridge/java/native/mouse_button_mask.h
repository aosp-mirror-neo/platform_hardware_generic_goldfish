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

#include <cstdint>

namespace goldfish::videobridge::java {

// Ordinals of the Kotlin `com.android.emulator.webrtc.MouseButton` enum.
// These must stay in sync with MouseButton.kt.
inline constexpr int kMouseButtonNoneOrdinal = 0;
inline constexpr int kMouseButtonLeftOrdinal = 1;
inline constexpr int kMouseButtonRightOrdinal = 2;
inline constexpr int kMouseButtonMiddleOrdinal = 3;

// Bits of the `MouseEvent.buttons` bitmask.
inline constexpr int32_t kMouseButtonPrimaryBit = 1 << 0;
inline constexpr int32_t kMouseButtonSecondaryBit = 1 << 1;
inline constexpr int32_t kMouseButtonMiddleBit = 1 << 2;

/**
 * Converts a Kotlin `MouseButton` ordinal into a `MouseEvent.buttons` mask.
 *
 * @param ordinal The `MouseButton.ordinal` value received over JNI.
 * @return The button bitmask, or 0 for NONE and unknown ordinals.
 */
constexpr int32_t MouseButtonOrdinalToMask(int ordinal) {
    switch (ordinal) {
    case kMouseButtonLeftOrdinal:
        return kMouseButtonPrimaryBit;
    case kMouseButtonRightOrdinal:
        return kMouseButtonSecondaryBit;
    case kMouseButtonMiddleOrdinal:
        return kMouseButtonMiddleBit;
    case kMouseButtonNoneOrdinal:
    default:
        return 0;
    }
}

}  // namespace goldfish::videobridge::java
