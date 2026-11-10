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
#include "touch_encoding.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "slot_registry.h"
#include "standard-headers/linux/input-event-codes.h"
#include "standard-headers/linux/input.h"

namespace goldfish::grpc::v2 {

namespace {

using ::android::emulation::control::EvDevEvent;
using ::android::emulation::control::kMtsPointerUp;
using ::android::emulation::control::kMtsPressureMax;
using ::android::emulation::v2::ButtonMask;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::ToolType;

constexpr uint32_t kEvAbsMax = 0x7FFF;
// Default touch contact radius (~4% of normalized range [0, 32767]).
constexpr uint32_t kDefaultTouchMajor = 0x0500;

/**
 * @brief Normalizes a normalized float value [0.0, 1.0] to Linux evdev ABS range [0, range_max].
 *
 * @param range_max The maximum value the guest device advertises for the axis.
 */
constexpr uint32_t NormalizeToEvAbs(float value, uint32_t range_max = kEvAbsMax) {
    if (std::isnan(value)) {
        return 0;
    }
    return static_cast<uint32_t>(std::clamp(value, 0.0F, 1.0F) * static_cast<float>(range_max));
}

constexpr float kRadToDeg = 180.0F / 3.14159265358979323846F;
constexpr float kMaxEvdevDegrees = 90.0F;

constexpr uint32_t ClampEvdevDegrees(float degrees) {
    if (std::isnan(degrees)) {
        return 0;
    }
    const float clamped = std::clamp(std::round(degrees), -kMaxEvdevDegrees, kMaxEvdevDegrees);
    return static_cast<uint32_t>(static_cast<int32_t>(clamped));
}

/**
 * @brief Converts orientation radians [-π/2, π/2] to Linux evdev ABS_MT_ORIENTATION degrees [-90,
 * 90].
 *
 * Android's MotionEvent and W3C Pointer Events L3 define orientation in radians [-π/2, π/2].
 * The Linux evdev multi-touch driver (virtio-input-android with MTS_ORIENTATION_RANGE_MAX = 90)
 * expects integer degrees in the range [-90, 90].
 */
constexpr uint32_t RadiansToEvdevOrientation(float orientation_radians) {
    return ClampEvdevDegrees(orientation_radians * kRadToDeg);
}

struct EvdevTilt {
    uint32_t tilt_x;
    uint32_t tilt_y;
};

/**
 * @brief Converts stylus tilt radians [0, π/2] and orientation radians [-π/2, π/2] to Linux evdev
 * ABS_TILT_X and ABS_TILT_Y degrees [-90, 90].
 */
inline EvdevTilt RadiansToEvdevTilt(float tilt_radians, float orientation_radians) {
    if (std::isnan(tilt_radians) || std::isnan(orientation_radians)) {
        return {0, 0};
    }
    const float tilt_degrees = tilt_radians * kRadToDeg;
    const float tilt_x = std::sin(orientation_radians) * tilt_degrees;
    const float tilt_y = -std::cos(orientation_radians) * tilt_degrees;
    return {ClampEvdevDegrees(tilt_x), ClampEvdevDegrees(tilt_y)};
}

uint32_t MapToolType(ToolType tool_type) {
    switch (tool_type) {
    case ToolType::TOOL_TYPE_STYLUS:
        return MT_TOOL_PEN;
    case ToolType::TOOL_TYPE_ERASER:
        // Linux evdev has no MT_TOOL_ERASER constant. In Android's
        // MultiTouchInputMapper::syncTouch, MT_TOOL_PEN maps directly to
        // MotionEvent.TOOL_TYPE_STYLUS, whereas MT_TOOL_MAX maps to
        // ToolType::UNKNOWN, causing TouchInputMapper::cookPointerData to fall
        // back to mTouchButtonAccumulator.getToolType(), which reads
        // BTN_TOOL_RUBBER to produce MotionEvent.TOOL_TYPE_ERASER.
        return MT_TOOL_MAX;
    case ToolType::TOOL_TYPE_PALM:
        return MT_TOOL_PALM;
    case ToolType::TOOL_TYPE_FINGER:
    case ToolType::TOOL_TYPE_MOUSE:
    case ToolType::TOOL_TYPE_UNSPECIFIED:
    default:
        return MT_TOOL_FINGER;
    }
}

}  // namespace

bool IsStylusOrEraser(ToolType tool_type) {
    return tool_type == ToolType::TOOL_TYPE_STYLUS || tool_type == ToolType::TOOL_TYPE_ERASER;
}

void AppendStylusKeysReset(::android::emulation::control::EvDevEvents& events) {
    events.push_back({EV_KEY, BTN_TOOL_RUBBER, 0});
    events.push_back({EV_KEY, BTN_STYLUS, 0});
}

void AppendContactDownOrMove(const PointerEvent::Pointer& pointer, PointerAction action, int slot,
                             bool new_contact, ::android::emulation::control::EvDevEvents& events) {
    if (IsStylusOrEraser(pointer.tool_type())) {
        const uint32_t is_rubber = pointer.tool_type() == ToolType::TOOL_TYPE_ERASER ? 1U : 0U;
        const uint32_t is_stylus_button_pressed =
                (pointer.button_mask() & ButtonMask::BUTTON_MASK_STYLUS_PRIMARY) != 0 ? 1U : 0U;
        events.push_back({EV_KEY, BTN_TOOL_RUBBER, is_rubber});
        events.push_back({EV_KEY, BTN_STYLUS, is_stylus_button_pressed});
    }
    events.push_back({EV_ABS, ABS_MT_SLOT, static_cast<uint32_t>(slot)});
    if (new_contact) {
        events.push_back({EV_ABS, ABS_MT_TRACKING_ID, static_cast<uint32_t>(slot)});
    }
    events.push_back({EV_ABS, ABS_MT_POSITION_X, NormalizeToEvAbs(pointer.x())});
    events.push_back({EV_ABS, ABS_MT_POSITION_Y, NormalizeToEvAbs(pointer.y())});
    events.push_back({EV_ABS, ABS_MT_TOOL_TYPE, MapToolType(pointer.tool_type())});

    const bool is_hover = (action == PointerAction::POINTER_ACTION_HOVER_MOVE ||
                           action == PointerAction::POINTER_ACTION_HOVER_ENTER);
    const bool has_pressure = pointer.has_pressure() && pointer.pressure() > 0.0F;
    const float pressure = is_hover ? 0.0F : (has_pressure ? pointer.pressure() : 1.0F);
    events.push_back({EV_ABS, ABS_MT_PRESSURE, NormalizeToEvAbs(pressure, kMtsPressureMax)});

    const uint32_t touch_major =
            pointer.size() > 0.0F ? NormalizeToEvAbs(pointer.size()) : kDefaultTouchMajor;
    events.push_back({EV_ABS, ABS_MT_TOUCH_MAJOR, touch_major});
    events.push_back({EV_ABS, ABS_MT_TOUCH_MINOR, touch_major});

    if (!std::isnan(pointer.orientation_radians()) && pointer.orientation_radians() != 0.0F) {
        events.push_back({EV_ABS, ABS_MT_ORIENTATION,
                          RadiansToEvdevOrientation(pointer.orientation_radians())});
    }
    // Linux evdev ABS_TILT_X/Y are global (non-slotted) digitizer tablet axes rather
    // than per-slot Multi-Touch Protocol B properties. We only emit tilt when the
    // transducer is an active pen (stylus/eraser) physically tilted relative to the
    // screen normal, decomposing 3D spherical tilt into 2D planar X/Y degrees.
    if (pointer.tilt_radians() > 0.0F && IsStylusOrEraser(pointer.tool_type())) {
        const auto tilt = RadiansToEvdevTilt(pointer.tilt_radians(), pointer.orientation_radians());
        events.push_back({EV_ABS, ABS_TILT_X, tilt.tilt_x});
        events.push_back({EV_ABS, ABS_TILT_Y, tilt.tilt_y});
    }
}

void AppendContactUp(int slot, ::android::emulation::control::EvDevEvents& events,
                     bool clear_stylus_keys) {
    events.push_back({EV_ABS, ABS_MT_SLOT, static_cast<uint32_t>(slot)});
    events.push_back({EV_ABS, ABS_MT_TRACKING_ID, static_cast<uint32_t>(kMtsPointerUp)});
    if (clear_stylus_keys) {
        AppendStylusKeysReset(events);
    }
}

void AppendContactCancel(int slot, ::android::emulation::control::EvDevEvents& palm_events,
                         ::android::emulation::control::EvDevEvents& lift_events,
                         bool clear_stylus_keys) {
    palm_events.push_back({EV_ABS, ABS_MT_SLOT, static_cast<uint32_t>(slot)});
    palm_events.push_back({EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM});
    AppendContactUp(slot, lift_events, clear_stylus_keys);
}

bool IsContactReleased(PointerAction action, const PointerEvent::Pointer& p) {
    if (action == PointerAction::POINTER_ACTION_CANCEL ||
        action == PointerAction::POINTER_ACTION_HOVER_EXIT) {
        return true;
    }
    if (action == PointerAction::POINTER_ACTION_UP) {
        return !(p.has_pressure() && p.pressure() > 0.0F);
    }
    if (action == PointerAction::POINTER_ACTION_DOWN ||
        action == PointerAction::POINTER_ACTION_HOVER_ENTER ||
        action == PointerAction::POINTER_ACTION_HOVER_MOVE) {
        return false;
    }
    return p.has_pressure() && p.pressure() <= 0.0F;
}

}  // namespace goldfish::grpc::v2
