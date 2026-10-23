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
#include "emulator/plugin/grpc/input/v2_input_validator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

#include "android/emulation/control/keyboard/key_code_resolver.h"
#include "android/status/status_macros.h"

namespace goldfish::grpc::v2 {

namespace {

using ::android::emulation::control::keyboard::ResolveEvdevKeyCode;
using ::android::emulation::v2::AxisEvent;
using ::android::emulation::v2::AxisType;
using ::android::emulation::v2::ButtonMask;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::KeyEvent;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::ToolType;
using ::goldfish::display::IMultiDisplay;

constexpr int32_t kSupportedButtonMask =
        static_cast<int32_t>(ButtonMask::BUTTON_MASK_STYLUS_PRIMARY);

absl::Status ValidateKeyIdentifier(const KeyEvent& key) {
    if (ResolveEvdevKeyCode(key) != 0) {
        return absl::OkStatus();
    }
    switch (key.key_identifier_case()) {
    case KeyEvent::kDomCode:
        return absl::InvalidArgumentError(
                absl::StrFormat("Invalid KeyEvent.dom_code '%s': must be a known W3C UI Events "
                                "KeyboardEvent.code string (e.g., 'KeyA', 'Enter', 'ArrowUp').",
                                key.dom_code()));
    case KeyEvent::kAndroidKeycode:
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid KeyEvent.android_keycode '%d': must be a supported Android "
                "system keycode (3=HOME, 4=BACK, 24=VOLUME_UP, 25=VOLUME_DOWN, 26=POWER, "
                "27=CAMERA, 82=MENU, 84=SEARCH, 187=APP_SWITCH). Use KeyEvent.dom_code "
                "(e.g., 'KeyA') or KeyEvent.evdev_code for general keyboard keys.",
                key.android_keycode()));
    case KeyEvent::kEvdevCode:
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid KeyEvent.evdev_code '%d': must be a positive Linux evdev KEY_* "
                "code (e.g., 30 for KEY_A, 116 for KEY_POWER).",
                key.evdev_code()));
    case KeyEvent::KEY_IDENTIFIER_NOT_SET:
        break;
    }
    return absl::InvalidArgumentError(
            "Missing KeyEvent.key_identifier: set one of 'dom_code' (e.g., 'KeyA'), "
            "'android_keycode' (e.g., 3 for HOME), or 'evdev_code' (e.g., 30 for KEY_A).");
}

absl::Status ValidateAxisEvent(const AxisEvent& axis, bool hw_rotary_input) {
    if (axis.axis() == AxisType::AXIS_TYPE_UNSPECIFIED) {
        return absl::InvalidArgumentError(
                "Invalid AxisEvent.axis 'AXIS_TYPE_UNSPECIFIED': must specify a valid "
                "AxisType such as 'AXIS_TYPE_ROTARY_ENCODER'.");
    }
    if (AxisType_Name(axis.axis()).empty()) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid AxisEvent.axis '%d': must specify a valid AxisType such as "
                "'AXIS_TYPE_ROTARY_ENCODER'.",
                static_cast<int>(axis.axis())));
    }
    if (!std::isfinite(axis.value())) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid AxisEvent.value '%g': must be a finite number.", axis.value()));
    }
    if (axis.axis() == AxisType::AXIS_TYPE_ROTARY_ENCODER) {
        if (!hw_rotary_input) {
            return absl::FailedPreconditionError(
                    "Cannot dispatch AxisEvent for 'AXIS_TYPE_ROTARY_ENCODER': rotary encoder "
                    "input is not supported yet because no guest rotary device is registered. "
                    "Send pointer, key, or text events instead.");
        }
        return absl::OkStatus();
    }
    return absl::FailedPreconditionError(
            absl::StrFormat("Cannot dispatch AxisEvent for '%s': axis input is not supported on "
                            "this device. Query CapabilitiesService for supported input features.",
                            AxisType_Name(axis.axis())));
}

absl::Status ValidatePointerBoundsAndFinite(const PointerEvent::Pointer& pointer, int index,
                                            int32_t max_touch_points) {
    if (pointer.pointer_id() < 0 || pointer.pointer_id() >= max_touch_points) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid Pointer.pointer_id '%d' at pointers[%d]: must be in range [0, %d].",
                pointer.pointer_id(), index, max_touch_points - 1));
    }
    const std::pair<absl::string_view, float> fields[] = {
        {"x", pointer.x()},
        {"y", pointer.y()},
        {"pressure", pointer.pressure()},
        {"size", pointer.size()},
        {"orientation_radians", pointer.orientation_radians()},
        {"tilt_radians", pointer.tilt_radians()},
        {"delta_x", pointer.delta_x()},
        {"delta_y", pointer.delta_y()},
    };
    for (const auto& [field, value] : fields) {
        if (!std::isfinite(value)) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Invalid Pointer.%s '%g' for pointer_id '%d' at pointers[%d]: must be a "
                    "finite number.",
                    field, value, pointer.pointer_id(), index));
        }
    }
    return absl::OkStatus();
}

absl::Status ValidatePointerCapabilities(const PointerEvent::Pointer& pointer, int index) {
    if (pointer.delta_x() != 0.0F || pointer.delta_y() != 0.0F) {
        return absl::FailedPreconditionError(absl::StrFormat(
                "Cannot dispatch Pointer with relative motion (delta_x='%g', delta_y='%g') "
                "for pointer_id '%d' at pointers[%d]: relative mouse motion "
                "(INPUT_FEATURE_RELATIVE_MOUSE) is not supported on this device. Use normalized "
                "absolute x/y coordinates instead.",
                pointer.delta_x(), pointer.delta_y(), pointer.pointer_id(), index));
    }
    if (pointer.tool_type() == ToolType::TOOL_TYPE_MOUSE) {
        return absl::FailedPreconditionError(absl::StrFormat(
                "Cannot dispatch Pointer with tool_type 'TOOL_TYPE_MOUSE' for pointer_id '%d' "
                "at pointers[%d]: mouse pointer input (INPUT_FEATURE_MOUSE_POINTER) is not "
                "supported on this device. Use 'TOOL_TYPE_FINGER' instead.",
                pointer.pointer_id(), index));
    }
    if ((pointer.button_mask() & ~kSupportedButtonMask) != 0) {
        return absl::FailedPreconditionError(absl::StrFormat(
                "Cannot dispatch Pointer with unsupported button_mask '0x%x' for pointer_id "
                "'%d' at pointers[%d]: only 'BUTTON_MASK_STYLUS_PRIMARY' is supported on this "
                "device. Clear unsupported button_mask bits or query CapabilitiesService for "
                "supported input features.",
                pointer.button_mask(), pointer.pointer_id(), index));
    }
    return absl::OkStatus();
}

}  // namespace

V2InputValidator::V2InputValidator(IMultiDisplay& multi_display)
        : V2InputValidator(multi_display, Options{}) {}

V2InputValidator::V2InputValidator(IMultiDisplay& multi_display, Options options)
        : multi_display_(&multi_display), options_(options) {}

absl::Status V2InputValidator::Validate(const InputEvent& event) const {
    if (event.timestamp_nanos() < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid InputEvent.timestamp_nanos '%d': must be >= 0 (or omit timestamp_nanos "
                "for immediate dispatch).",
                event.timestamp_nanos()));
    }

    switch (event.event_case()) {
    case InputEvent::kPointer:
        return ValidatePointerEvent(event.pointer());
    case InputEvent::kKey:
        if (!options_.hw_keyboard) {
            return absl::FailedPreconditionError(
                    "Cannot dispatch KeyEvent: hardware keyboard support (hw.keyboard) is disabled "
                    "on this device. Enable hw.keyboard in the device hardware configuration or "
                    "check that CapabilitiesService reports INPUT_FEATURE_KEYBOARD.");
        }
        return ValidateKeyIdentifier(event.key());
    case InputEvent::kAxis:
        return ValidateAxisEvent(event.axis(), options_.hw_rotary_input);
    case InputEvent::kText:
        if (!options_.hw_keyboard) {
            return absl::FailedPreconditionError(
                    "Cannot dispatch TextEvent: keyboard/IME text input (hw.keyboard) is disabled "
                    "on this device. Enable hw.keyboard in the device hardware configuration or "
                    "check that CapabilitiesService reports INPUT_FEATURE_KEYBOARD.");
        }
        return absl::OkStatus();
    case InputEvent::EVENT_NOT_SET:
        break;
    }
    return absl::InvalidArgumentError(
            "Missing InputEvent.event payload: set one of 'pointer', 'key', 'axis', or 'text'.");
}

absl::Status V2InputValidator::ValidatePointerEvent(const PointerEvent& pointer) const {
    if (pointer.display_id() < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid PointerEvent.display_id '%d': must be >= 0 (use '0' for the primary "
                "display).",
                pointer.display_id()));
    }
    if (static_cast<size_t>(pointer.display_id()) >= IMultiDisplay::kMaxDisplays) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Cannot dispatch PointerEvent to display_id '%d': exceeds maximum supported "
                "display_id '%d'. Specify '0' for the primary display, or query "
                "CapabilitiesService for active secondary display IDs.",
                pointer.display_id(), IMultiDisplay::kMaxDisplays - 1));
    }
    const auto display = multi_display_->GetActiveDisplay(
            static_cast<uint32_t>(pointer.display_id()), options_.hw_sensor_hinge);
    if (!display.ok()) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Cannot dispatch PointerEvent to display_id '%d': %s. Specify '0' for the primary "
                "display, or query CapabilitiesService for active secondary display IDs.",
                pointer.display_id(), display.status().message()));
    }

    // NaN and infinity have no evdev representation.
    if (!std::isfinite(pointer.scroll_x()) || !std::isfinite(pointer.scroll_y())) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Invalid PointerEvent scroll (scroll_x='%g', scroll_y='%g'): must be finite "
                "numbers.",
                pointer.scroll_x(), pointer.scroll_y()));
    }

    const int32_t max_touch_points = std::min(options_.max_touch_points, kDefaultMaxTouchPoints);
    for (int i = 0; i < pointer.pointers_size(); ++i) {
        RETURN_IF_ERROR(ValidatePointerBoundsAndFinite(pointer.pointers(i), i, max_touch_points));
    }

    if (pointer.action() == PointerAction::POINTER_ACTION_RELATIVE_MOVE) {
        return absl::FailedPreconditionError(
                "Cannot dispatch PointerEvent with action 'POINTER_ACTION_RELATIVE_MOVE': "
                "relative mouse motion (INPUT_FEATURE_RELATIVE_MOUSE) is not supported on this "
                "device. Use normalized absolute x/y coordinates with 'POINTER_ACTION_DOWN' or "
                "'POINTER_ACTION_MOVE' instead.");
    }
    if (pointer.action() == PointerAction::POINTER_ACTION_SCROLL || pointer.scroll_x() != 0.0F ||
        pointer.scroll_y() != 0.0F) {
        return absl::FailedPreconditionError(absl::StrFormat(
                "Cannot dispatch PointerEvent scroll (action='%s', scroll_x='%g', scroll_y='%g'): "
                "mouse wheel scrolling is not supported on this device. Omit scroll fields or use "
                "a touch drag gesture instead.",
                PointerAction_Name(pointer.action()), pointer.scroll_x(), pointer.scroll_y()));
    }

    for (int i = 0; i < pointer.pointers_size(); ++i) {
        RETURN_IF_ERROR(ValidatePointerCapabilities(pointer.pointers(i), i));
    }
    return absl::OkStatus();
}

}  // namespace goldfish::grpc::v2
