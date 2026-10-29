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

#include "android/emulation/control/ev_dev_event.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2 {

// Encodes v2 touch contacts as Linux multi-touch protocol B evdev events.
//
// These functions are pure: they hold no state and never allocate slots. The
// caller owns slot allocation and passes in the slot a contact occupies. The
// slot number doubles as the contact's ABS_MT_TRACKING_ID. Events are
// appended to `events`; the caller terminates the report (SYN_REPORT).

/**
 * @brief Returns whether `tool_type` is an active pen tool (`TOOL_TYPE_STYLUS`
 * or `TOOL_TYPE_ERASER`) that drives global `BTN_TOOL_RUBBER` / `BTN_STYLUS`
 * `EV_KEY` state on the touch device.
 */
bool IsStylusOrEraser(::android::emulation::v2::ToolType tool_type);

/**
 * @brief Appends `EV_KEY` events resetting `BTN_TOOL_RUBBER` and `BTN_STYLUS`
 * to 0 so subsequent finger contacts do not inherit stale pen/eraser state in
 * Android's `TouchButtonAccumulator`.
 */
void AppendStylusKeysReset(::android::emulation::control::EvDevEvents& events);

/**
 * @brief Appends the events for a contact going down, moving, or hovering.
 *
 * Emits `BTN_TOOL_RUBBER` and `BTN_STYLUS` for stylus/eraser contacts, then
 * ABS_MT_SLOT `slot`, then ABS_MT_TRACKING_ID `slot` if `new_contact`, then
 * position, tool type, pressure (scaled to kMtsPressureMax; 0 while hovering),
 * touch major/minor, orientation (if non-zero) and, for a tilted stylus or
 * eraser, ABS_TILT_X/Y.
 *
 * @param pointer The contact's position and properties.
 * @param action The pointer action; hover actions encode zero pressure.
 * @param slot The multi-touch slot the contact occupies; must be >= 0.
 * @param new_contact Whether the contact just acquired `slot` and needs a
 *     tracking ID.
 * @param events The event list to append to.
 */
void AppendContactDownOrMove(const ::android::emulation::v2::PointerEvent::Pointer& pointer,
                             ::android::emulation::v2::PointerAction action, int slot,
                             bool new_contact, ::android::emulation::control::EvDevEvents& events);

/**
 * @brief Appends the events lifting the contact in `slot`.
 *
 * Emits ABS_MT_SLOT `slot`, then ABS_MT_TRACKING_ID kMtsPointerUp (-1), and
 * optionally resets `BTN_TOOL_RUBBER` and `BTN_STYLUS` to 0 when lifting the
 * last stylus/eraser contact on the device.
 *
 * @param slot The multi-touch slot of the contact to lift; must be >= 0.
 * @param events The event list to append to.
 * @param clear_stylus_keys Whether to reset `BTN_TOOL_RUBBER` and `BTN_STYLUS`.
 */
void AppendContactUp(int slot, ::android::emulation::control::EvDevEvents& events,
                     bool clear_stylus_keys = false);

/**
 * @brief Appends the two-report event sequence cancelling the contact in
 * `slot`.
 *
 * Appends `ABS_MT_SLOT slot` and `ABS_MT_TOOL_TYPE MT_TOOL_PALM` to
 * `palm_events`, and `ABS_MT_SLOT slot` and `ABS_MT_TRACKING_ID
 * kMtsPointerUp` (-1) (plus optional stylus key resets) to `lift_events`. The
 * caller must flush `palm_events` (`SYN_REPORT`) before `lift_events`:
 * Android's `MultiTouchMotionAccumulator` clears a slot immediately upon
 * `ABS_MT_TRACKING_ID = -1` before `SYN_REPORT` syncs `TouchInputMapper`, so
 * `MT_TOOL_PALM` and the lift must arrive in separate frames for the guest to
 * dispatch a cancelled `MotionEvent` (`FLAG_CANCELED`).
 *
 * @param slot The multi-touch slot of the contact to cancel; must be >= 0.
 * @param palm_events First frame: marks `slot` as `MT_TOOL_PALM`.
 * @param lift_events Second frame: lifts `slot` (`ABS_MT_TRACKING_ID = -1`).
 * @param clear_stylus_keys Whether to reset `BTN_TOOL_RUBBER` and `BTN_STYLUS`
 *     in `lift_events`.
 */
void AppendContactCancel(int slot, ::android::emulation::control::EvDevEvents& palm_events,
                         ::android::emulation::control::EvDevEvents& lift_events,
                         bool clear_stylus_keys = false);

/**
 * @brief Determines if a pointer action or pressure indicator signifies a touch release /
 * cancel.
 *
 * - On CANCEL or HOVER_EXIT, all pointers in the event are released.
 * - On POINTER_ACTION_UP, pointers lift unless they explicitly report positive contact pressure
 *   (`p.has_pressure() && p.pressure() > 0.0F`), supporting multi-touch partial finger lifts.
 * - On POINTER_ACTION_DOWN, HOVER_ENTER, and HOVER_MOVE, contacts remain active.
 * - On POINTER_ACTION_MOVE, contacts remain active when `pressure` is omitted
 *   (`!p.has_pressure()`, which defaults to 1.0) and only lift if `pressure` is explicitly set
 *   to `<= 0.0F`.
 */
bool IsContactReleased(::android::emulation::v2::PointerAction action,
                       const ::android::emulation::v2::PointerEvent::Pointer& p);

}  // namespace goldfish::grpc::v2
