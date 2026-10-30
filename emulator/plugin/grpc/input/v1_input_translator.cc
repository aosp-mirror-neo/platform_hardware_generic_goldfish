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
#include "emulator/plugin/grpc/input/v1_input_translator.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "android/emulation/control/ev_dev_event.h"
#include "android/emulation/control/keyboard/key_conversion.h"
#include "android/status/status_macros.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator_controller.pb.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/display.h"
#include "input_service.pb.h"
#include "standard-headers/linux/input-event-codes.h"

namespace goldfish::grpc::v2 {

namespace {

using ::android::emulation::control::AndroidEvent;
using ::android::emulation::control::EvDevEvent;
using ::android::emulation::control::EvDevEvents;
using ::android::emulation::control::KeyboardEvent;
using ::android::emulation::control::MouseEvent;
using ::android::emulation::control::Pen;
using ::android::emulation::control::PenEvent;
using ::android::emulation::control::Touch;
using ::android::emulation::control::TouchEvent;
using ::android::emulation::control::keyboard::ascii_to_qcode;
using ::android::emulation::control::keyboard::dom_code_to_evdev;
using ::android::emulation::control::keyboard::DomKeyResolution;
using ::android::emulation::control::keyboard::evdev_to_dom_code;
using ::android::emulation::control::keyboard::keycode_to_evdev;
using ::android::emulation::control::keyboard::KeyCodeType;
using ::android::emulation::control::keyboard::qcode_to_evdev;
using ::android::emulation::control::keyboard::resolve_dom_key;
using ::android::emulation::v2::ButtonMask;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::KeyAction;
using ::android::emulation::v2::KeyEvent;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::ToolType;
using ::goldfish::display::Dimensions;
using ::goldfish::display::DisplayId;
using ::goldfish::display::IMultiDisplay;

constexpr float kMaxV1Pressure = 1024.0F;
constexpr float kMaxV1TouchMajor = 32767.0F;
constexpr float kDegreesToRadians = std::numbers::pi_v<float> / 180.0F;
constexpr int32_t kLeftMouseButtonMask = 1;
constexpr int32_t kMaxAsciiCharacter = 0xFF;

std::optional<Dimensions> ResolveDisplayDimensions(IMultiDisplay& multi_display, int32_t display_id,
                                                   bool has_hinge) {
    if (display_id < 0) {
        return std::nullopt;
    }
    const auto display =
            multi_display.GetActiveDisplay(static_cast<DisplayId>(display_id), has_hinge);
    if (!display.ok() || !*display) {
        return std::nullopt;
    }
    const Dimensions dims = (*display)->GetDimensions();
    if (dims.width == 0 || dims.height == 0) {
        return std::nullopt;
    }
    return dims;
}

float NormalizeCoord(int32_t pixel, uint32_t extent) {
    return std::clamp(static_cast<float>(pixel) / static_cast<float>(extent), 0.0F, 1.0F);
}

float NormalizePressure(int32_t pressure) {
    if (pressure <= 0) {
        return 0.0F;
    }
    return std::clamp(static_cast<float>(pressure) / kMaxV1Pressure, 0.0F, 1.0F);
}

PointerEvent::Pointer BuildPointer(int32_t pointer_id, ToolType tool_type, int32_t button_mask,
                                   const Touch& touch, Dimensions dims) {
    PointerEvent::Pointer p;
    p.set_pointer_id(pointer_id);
    p.set_tool_type(tool_type);
    p.set_button_mask(button_mask);
    p.set_x(NormalizeCoord(touch.x(), dims.width));
    p.set_y(NormalizeCoord(touch.y(), dims.height));
    p.set_pressure(NormalizePressure(touch.pressure()));
    if (touch.touch_major() > 0) {
        p.set_size(
                std::clamp(static_cast<float>(touch.touch_major()) / kMaxV1TouchMajor, 0.0F, 1.0F));
    }
    if (touch.orientation() != 0) {
        p.set_orientation_radians(static_cast<float>(touch.orientation()) * kDegreesToRadians);
    }
    return p;
}

InputEvent MakePointerInputEvent(int32_t display_id, PointerAction action,
                                 std::vector<PointerEvent::Pointer> pointers) {
    InputEvent out;
    PointerEvent* pe = out.mutable_pointer();
    pe->set_display_id(display_id);
    pe->set_action(action);
    for (PointerEvent::Pointer& p : pointers) {
        *pe->add_pointers() = std::move(p);
    }
    return out;
}

void AppendKeyEvent(std::vector<InputEvent>& out, uint32_t evdev, KeyAction action) {
    if (evdev == 0) {
        return;
    }
    InputEvent ev;
    KeyEvent* key = ev.mutable_key();
    key->set_action(action);
    const std::string_view dom_code = evdev_to_dom_code(evdev);
    if (!dom_code.empty()) {
        key->set_dom_code(std::string(dom_code));
    } else {
        key->set_evdev_code(static_cast<int32_t>(evdev));
    }
    out.push_back(std::move(ev));
}

void AppendKeyEventsForEventType(std::vector<InputEvent>& out, uint32_t evdev,
                                 KeyboardEvent::KeyEventType event_type) {
    if (event_type == KeyboardEvent::keydown || event_type == KeyboardEvent::keypress) {
        AppendKeyEvent(out, evdev, KeyAction::KEY_ACTION_DOWN);
    }
    if (event_type == KeyboardEvent::keyup || event_type == KeyboardEvent::keypress) {
        AppendKeyEvent(out, evdev, KeyAction::KEY_ACTION_UP);
    }
}

void AppendAsciiEvents(std::vector<InputEvent>& out, unsigned short character,
                       KeyboardEvent::KeyEventType event_type) {
    if (event_type == KeyboardEvent::keydown || event_type == KeyboardEvent::keypress) {
        for (const auto& qevent : ascii_to_qcode(character, /*down=*/true)) {
            AppendKeyEvent(out, qcode_to_evdev(qevent.code),
                           qevent.down ? KeyAction::KEY_ACTION_DOWN : KeyAction::KEY_ACTION_UP);
        }
    }
    if (event_type == KeyboardEvent::keyup || event_type == KeyboardEvent::keypress) {
        for (const auto& qevent : ascii_to_qcode(character, /*down=*/false)) {
            AppendKeyEvent(out, qcode_to_evdev(qevent.code),
                           qevent.down ? KeyAction::KEY_ACTION_DOWN : KeyAction::KEY_ACTION_UP);
        }
    }
}

bool IsFilteredMultiTouchEvent(uint16_t type, uint16_t code) {
    if (type == EV_ABS && code >= ABS_MT_SLOT && code <= ABS_MT_TOOL_Y) {
        return true;
    }
    if (type == EV_KEY &&
        (code == BTN_TOUCH || (code >= BTN_TOOL_PEN && code <= BTN_TOOL_QUADTAP))) {
        return true;
    }
    return false;
}

std::optional<InputEvent> TranslateSinglePenEvent(
        const Pen& pen, int32_t display_id, IMultiDisplay& multi_display, bool has_hinge,
        absl::flat_hash_map<int32_t, Dimensions>& held_pointers) {
    const Touch& location = pen.location();
    const bool is_down = location.pressure() > 0;
    const auto held_it = held_pointers.find(V1InputTranslator::kPenPointerId);
    const bool was_held = held_it != held_pointers.end();

    if (!is_down && !was_held) {
        return std::nullopt;
    }

    Dimensions dims{};
    PointerAction action = PointerAction::POINTER_ACTION_UNSPECIFIED;
    if (!was_held && is_down) {
        const std::optional<Dimensions> resolved =
                ResolveDisplayDimensions(multi_display, display_id, has_hinge);
        if (!resolved.has_value()) {
            return std::nullopt;
        }
        dims = *resolved;
        held_pointers[V1InputTranslator::kPenPointerId] = dims;
        action = PointerAction::POINTER_ACTION_DOWN;
    } else if (was_held && is_down) {
        dims = held_it->second;
        action = PointerAction::POINTER_ACTION_MOVE;
    } else {
        dims = held_it->second;
        held_pointers.erase(held_it);
        action = PointerAction::POINTER_ACTION_UP;
    }

    const ToolType tool_type =
            pen.rubber_pointer() ? ToolType::TOOL_TYPE_ERASER : ToolType::TOOL_TYPE_STYLUS;
    const int32_t button_mask =
            pen.button_pressed() ? static_cast<int32_t>(ButtonMask::BUTTON_MASK_STYLUS_PRIMARY)
                                 : static_cast<int32_t>(ButtonMask::BUTTON_MASK_NONE);
    PointerEvent::Pointer p =
            BuildPointer(V1InputTranslator::kPenPointerId, tool_type, button_mask, location, dims);
    return MakePointerInputEvent(display_id, action, {std::move(p)});
}

}  // namespace

std::optional<InputEvent> SingleFingerMouseTranslator::Translate(
        const MouseEvent& event, IMultiDisplay& multi_display, bool has_hinge,
        absl::flat_hash_map<int32_t, Dimensions>& held_pointers) {
    constexpr int32_t kPointerId = V1InputTranslator::kMouseAndTouchZeroPointerId;
    const bool button_down = (event.buttons() & kLeftMouseButtonMask) != 0;
    const auto held_it = held_pointers.find(kPointerId);
    const bool was_down = held_it != held_pointers.end();

    if (!button_down && !was_down) {
        return std::nullopt;
    }

    Dimensions dims{};
    PointerAction action = PointerAction::POINTER_ACTION_UNSPECIFIED;
    if (!was_down && button_down) {
        const std::optional<Dimensions> resolved =
                ResolveDisplayDimensions(multi_display, event.display(), has_hinge);
        if (!resolved.has_value()) {
            return std::nullopt;
        }
        dims = *resolved;
        held_pointers[kPointerId] = dims;
        action = PointerAction::POINTER_ACTION_DOWN;
    } else if (was_down && button_down) {
        dims = held_it->second;
        action = PointerAction::POINTER_ACTION_MOVE;
    } else {
        dims = held_it->second;
        held_pointers.erase(held_it);
        action = PointerAction::POINTER_ACTION_UP;
    }

    PointerEvent::Pointer p;
    p.set_pointer_id(kPointerId);
    p.set_tool_type(ToolType::TOOL_TYPE_FINGER);
    p.set_x(NormalizeCoord(event.x(), dims.width));
    p.set_y(NormalizeCoord(event.y(), dims.height));
    p.set_pressure(button_down ? 1.0F : 0.0F);
    return MakePointerInputEvent(event.display(), action, {std::move(p)});
}

V1InputTranslator::V1InputTranslator(IMultiDisplay& multi_display, bool has_hinge)
        : V1InputTranslator(multi_display, has_hinge,
                            std::make_unique<SingleFingerMouseTranslator>()) {}

V1InputTranslator::V1InputTranslator(IMultiDisplay& multi_display, bool has_hinge,
                                     std::unique_ptr<MouseEventTranslator> mouse_translator)
        : multi_display_(&multi_display)
        , has_hinge_(has_hinge)
        , mouse_translator_(std::move(mouse_translator)) {}

void V1InputTranslator::Reset() {
    display_states_.clear();
    android_event_buffers_.clear();
}

void V1InputTranslator::ResetDisplay(int32_t display_id) {
    display_states_.erase(display_id);
    android_event_buffers_.erase(display_id);
}

void V1InputTranslator::SyncResetTracker(const SessionResetTracker& tracker) {
    if (tracker.Version() == last_reset_version_) {
        return;
    }
    SessionResetTracker::Snapshot snapshot = tracker.ReadSnapshot();
    if (snapshot.full_generation != last_full_generation_) {
        Reset();
        last_full_generation_ = snapshot.full_generation;
        last_display_generations_ = std::move(snapshot.display_generations);
        last_reset_version_ = snapshot.version;
        return;
    }
    for (const auto& [display_id, generation] : snapshot.display_generations) {
        const auto prev_it = last_display_generations_.find(display_id);
        const uint64_t prev_gen =
                (prev_it != last_display_generations_.end()) ? prev_it->second : 0;
        if (generation != prev_gen) {
            ResetDisplay(display_id);
        }
    }
    last_display_generations_ = std::move(snapshot.display_generations);
    last_reset_version_ = snapshot.version;
}

std::optional<int32_t> V1InputTranslator::ResolveTouchPointerId(DisplayPointerState& state,
                                                                int32_t v1_identifier, bool is_down,
                                                                uint32_t batch_direct_mask) {
    if (v1_identifier >= 0 && v1_identifier < kMaxTouchPointers) {
        if (!is_down && !state.held_pointers.contains(v1_identifier)) {
            return std::nullopt;
        }
        return v1_identifier;
    }

    const auto existing = state.external_touch_id_to_pointer_id.find(v1_identifier);
    if (existing != state.external_touch_id_to_pointer_id.end()) {
        return existing->second;
    }
    if (!is_down) {
        return std::nullopt;
    }

    for (int32_t candidate = kMaxTouchPointers - 1; candidate >= 0; --candidate) {
        if ((batch_direct_mask & (1U << candidate)) != 0 ||
            state.held_pointers.contains(candidate)) {
            continue;
        }
        const bool used_by_external =
                std::any_of(state.external_touch_id_to_pointer_id.begin(),
                            state.external_touch_id_to_pointer_id.end(),
                            [candidate](const auto& entry) { return entry.second == candidate; });
        if (!used_by_external) {
            state.external_touch_id_to_pointer_id[v1_identifier] = candidate;
            return candidate;
        }
    }
    return std::nullopt;
}

void V1InputTranslator::ReleaseTouchPointerId(DisplayPointerState& state, int32_t v1_identifier,
                                              int32_t pointer_id) {
    state.held_pointers.erase(pointer_id);
    state.external_touch_id_to_pointer_id.erase(v1_identifier);
}

void V1InputTranslator::PruneDisplayStateIfEmpty(int32_t display_id) {
    const auto it = display_states_.find(display_id);
    if (it != display_states_.end() && it->second.held_pointers.empty() &&
        it->second.external_touch_id_to_pointer_id.empty()) {
        display_states_.erase(it);
    }
}

std::vector<InputEvent> V1InputTranslator::TranslateTouch(const TouchEvent& event) {
    const int32_t display_id = event.display();
    DisplayPointerState& state = display_states_[display_id];

    uint32_t batch_direct_mask = 0;
    for (const Touch& touch : event.touches()) {
        if (touch.pressure() > 0 && touch.identifier() >= 0 &&
            touch.identifier() < kMaxTouchPointers) {
            batch_direct_mask |= (1U << touch.identifier());
        }
    }

    std::vector<PointerEvent::Pointer> active_pointers;
    std::vector<PointerEvent::Pointer> released_pointers;
    std::vector<PointerEvent::Pointer> all_pointers;
    bool has_new_down = false;
    bool has_held_move = false;
    bool has_release = false;

    for (const Touch& touch : event.touches()) {
        const bool is_down = touch.pressure() > 0;
        const std::optional<int32_t> pointer_id =
                ResolveTouchPointerId(state, touch.identifier(), is_down, batch_direct_mask);
        if (!pointer_id.has_value()) {
            continue;
        }

        const auto held_it = state.held_pointers.find(*pointer_id);
        Dimensions dims{};
        if (held_it == state.held_pointers.end()) {
            const std::optional<Dimensions> resolved =
                    ResolveDisplayDimensions(*multi_display_, display_id, has_hinge_);
            if (!resolved.has_value()) {
                ReleaseTouchPointerId(state, touch.identifier(), *pointer_id);
                continue;
            }
            dims = *resolved;
            state.held_pointers[*pointer_id] = dims;
            has_new_down = true;
        } else {
            dims = held_it->second;
            has_held_move = has_held_move || is_down;
            if (!is_down) {
                has_release = true;
                ReleaseTouchPointerId(state, touch.identifier(), *pointer_id);
            }
        }

        PointerEvent::Pointer p = BuildPointer(*pointer_id, ToolType::TOOL_TYPE_FINGER,
                                               ButtonMask::BUTTON_MASK_NONE, touch, dims);
        if (is_down) {
            active_pointers.push_back(p);
        } else {
            released_pointers.push_back(p);
        }
        all_pointers.push_back(std::move(p));
    }

    PruneDisplayStateIfEmpty(display_id);
    if (all_pointers.empty()) {
        return {};
    }

    if (has_new_down && has_release) {
        std::vector<InputEvent> out;
        out.reserve(2);
        out.push_back(MakePointerInputEvent(display_id, PointerAction::POINTER_ACTION_DOWN,
                                            std::move(active_pointers)));
        out.push_back(MakePointerInputEvent(display_id, PointerAction::POINTER_ACTION_UP,
                                            std::move(released_pointers)));
        return out;
    }

    PointerAction action = PointerAction::POINTER_ACTION_MOVE;
    if (has_new_down) {
        action = PointerAction::POINTER_ACTION_DOWN;
    } else if (!has_held_move && has_release) {
        action = PointerAction::POINTER_ACTION_UP;
    }
    return {MakePointerInputEvent(display_id, action, std::move(all_pointers))};
}

std::optional<InputEvent> V1InputTranslator::TranslateMouse(const MouseEvent& event) {
    const int32_t display_id = event.display();
    std::optional<InputEvent> translated = mouse_translator_->Translate(
            event, *multi_display_, has_hinge_, display_states_[display_id].held_pointers);
    PruneDisplayStateIfEmpty(display_id);
    return translated;
}

std::vector<InputEvent> V1InputTranslator::TranslatePen(const PenEvent& event) {
    const int32_t display_id = event.display();
    DisplayPointerState& state = display_states_[display_id];
    std::vector<InputEvent> out;
    for (const Pen& pen : event.events()) {
        std::optional<InputEvent> translated = TranslateSinglePenEvent(
                pen, display_id, *multi_display_, has_hinge_, state.held_pointers);
        if (translated.has_value()) {
            out.push_back(std::move(*translated));
        }
    }
    PruneDisplayStateIfEmpty(display_id);
    return out;
}

std::vector<InputEvent> V1InputTranslator::TranslateKeyboard(const KeyboardEvent& event) const {
    std::vector<InputEvent> out;

    if (!event.key().empty()) {
        const DomKeyResolution resolved = resolve_dom_key(event.key());
        switch (resolved.kind) {
        case DomKeyResolution::Kind::kNonPrintable:
            AppendKeyEventsForEventType(out, resolved.evdev, event.eventtype());
            break;
        case DomKeyResolution::Kind::kCharacter:
            if (resolved.character <= kMaxAsciiCharacter) {
                AppendAsciiEvents(out, static_cast<unsigned short>(resolved.character),
                                  event.eventtype());
            } else {
                VLOG(1) << "Ignoring non-ASCII character Key=" << event.key();
            }
            break;
        case DomKeyResolution::Kind::kNone:
            if (event.key() != "Dead") {
                const uint32_t evdev = dom_code_to_evdev(event.key());
                if (evdev > 0) {
                    AppendKeyEventsForEventType(out, evdev, event.eventtype());
                } else {
                    VLOG(1) << "Dropped keyboard event: '" << event.key()
                            << "' is not a valid W3C DOM key/code. "
                            << "Use a standard DOM string (e.g. 'KeyA', 'Enter'), "
                            << "an evdev 'keyCode', or the 'text' field.";
                }
            }
            break;
        }
    }

    if (!event.text().empty()) {
        InputEvent text_event;
        text_event.mutable_text()->set_text(event.text());
        out.push_back(std::move(text_event));
    }

    if (event.keycode() > 0) {
        const auto code_type = static_cast<KeyCodeType>(event.codetype());
        const uint32_t evdev = keycode_to_evdev(static_cast<uint32_t>(event.keycode()), code_type);
        AppendKeyEventsForEventType(out, evdev, event.eventtype());
    }

    return out;
}

absl::Status V1InputTranslator::DispatchAndroidEvent(const AndroidEvent& event) {
    const auto type = static_cast<uint16_t>(event.type());
    const auto code = static_cast<uint16_t>(event.code());
    const auto value = static_cast<uint32_t>(event.value());
    const int32_t display_id = event.display();

    if (type == EV_SYN && code == SYN_REPORT) {
        const auto buf_it = android_event_buffers_.find(display_id);
        if (buf_it == android_event_buffers_.end() || buf_it->second.empty()) {
            return absl::OkStatus();
        }
        EvDevEvents batch = std::move(buf_it->second);
        android_event_buffers_.erase(buf_it);

        if (display_id < 0) {
            return absl::InvalidArgumentError(
                    absl::StrCat("Failed to dispatch AndroidEvent: display (", display_id,
                                 ") must be >= 0; specify a non-negative display ID"));
        }
        ASSIGN_OR_RETURN(auto display, multi_display_->GetActiveDisplay(
                                               static_cast<DisplayId>(display_id), has_hinge_));
        if (!display) {
            return absl::UnavailableError(absl::StrCat(
                    "Failed to dispatch AndroidEvent on display ", display_id,
                    ": display is no longer available; verify the display is active before sending "
                    "raw evdev events"));
        }
        display->SendEvDevReport(std::move(batch));
        return absl::OkStatus();
    }

    if (IsFilteredMultiTouchEvent(type, code)) {
        return absl::OkStatus();
    }

    EvDevEvents& buffer = android_event_buffers_[display_id];
    if (buffer.size() < kMaxBufferedAndroidEvents) {
        buffer.push_back(EvDevEvent{type, code, value});
    } else {
        VLOG(1) << "Dropping AndroidEvent on display " << display_id << ": buffer full ("
                << kMaxBufferedAndroidEvents << " events without SYN_REPORT)";
    }
    return absl::OkStatus();
}

std::vector<InputEvent> V1InputTranslator::TranslateInputEvent(
        const ::android::emulation::control::InputEvent& event) {
    switch (event.type_case()) {
    case ::android::emulation::control::InputEvent::kKeyEvent:
        return TranslateKeyboard(event.key_event());
    case ::android::emulation::control::InputEvent::kTouchEvent:
        return TranslateTouch(event.touch_event());
    case ::android::emulation::control::InputEvent::kMouseEvent: {
        std::optional<InputEvent> mouse = TranslateMouse(event.mouse_event());
        if (!mouse.has_value()) {
            return {};
        }
        return {std::move(*mouse)};
    }
    case ::android::emulation::control::InputEvent::kPenEvent:
        return TranslatePen(event.pen_event());
    case ::android::emulation::control::InputEvent::kWheelEvent:
        VLOG(1) << "Dropping unsupported v1 WheelEvent";
        return {};
    case ::android::emulation::control::InputEvent::kAndroidEvent:
        if (absl::Status status = DispatchAndroidEvent(event.android_event()); !status.ok()) {
            VLOG(1) << "Failed to dispatch AndroidEvent: " << status;
        }
        return {};
    default:
        VLOG(1) << "Dropping unsupported v1 InputEvent type_case=" << event.type_case();
        return {};
    }
    return {};
}

}  // namespace goldfish::grpc::v2
