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
#include <optional>
#include <string>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

#include "goldfish/eventing/event_sources.h"
#include "input_service.pb.h"
#include "input_session_id.h"

namespace goldfish::grpc::v2 {

/**
 * @brief The single owner of guest touch and keyboard state: applies the
 * pointer, key, and text events of every input session to the touch devices and
 * the keyboard.
 *
 * Events are applied in the order they were dispatched, so keys and touches
 * reach the guest in that order. Each contact `(session, pointer)` holds a
 * multi-touch slot on the device it was created on, and stays bound to that
 * device until released. Each key is down in the guest while at least one
 * session holds it.
 *
 * Everything that reaches the guest is reported, once and in the order it was
 * applied, as an `InputActivity` on `activity()`.
 *
 * ### Thread Safety:
 * All methods are thread-safe.
 */
class IInputDispatcher {
  public:
    struct SessionOptions {
        /** @brief Human sessions may use the slots reserved for human input. */
        bool human = false;
        /** @brief The `input_source_id` stamped on every activity of the session. */
        int32_t source_id = 0;
        /**
         * @brief Whether `DOWN` for a pointer the session already holds is a
         * client error rather than a move. Set by APIs whose clients must lift
         * a pointer before pressing it again (v2); others may resend presses.
         */
        bool reject_down_on_held = false;
        /**
         * @brief If positive, how long the session may go without an operation
         * before its held contacts and keys are released as on
         * `CloseSession()`, while the session stays open.
         */
        absl::Duration idle_timeout = absl::ZeroDuration();
    };

    using ActivityEventSource =
            ::android::base::eventing::CallbackEventSource<::android::emulation::v2::InputActivity>;

    /** @brief The result of one `Dispatch*()` operation. */
    struct DispatchResult {
        absl::Status status;
        /**
         * @brief When the operation reached the guest: the `dispatched_at_nanos`
         * of its activity (elapsed on the host monotonic clock). Unset if it
         * reported none, such as when it was rejected or changed nothing.
         */
        std::optional<absl::Duration> dispatched_at;
    };

    /** @brief Called once with the result of the operation; see `DispatchPointer()`. */
    using DispatchCallback = absl::AnyInvocable<void(DispatchResult) &&>;

    virtual ~IInputDispatcher() = default;

    /**
     * @brief Activity, with the source ID of the session and
     * `dispatched_at_nanos` read right after the write to the guest:
     * - Pointer: the event on the requested display, listing only the pointers
     *   written to a device; nothing if none was. A pointer-less release that
     *   released contacts is a `CANCEL` of their pointer IDs.
     * - Key: each applied `DOWN`, `UP`, and `PRESS`, even an `UP` the guest
     *   didn't get because another session holds the key. A stray `UP` applies
     *   nothing.
     * - Text: non-empty text.
     * - `CloseSession()`: a synthesized `CANCEL` per display the released
     *   contacts were requested on, and a synthesized `KEY_ACTION_UP` (as
     *   `evdev_code`) per released key.
     * - Driver reset: when a touch device's guest driver is reset or
     *   re-probed, which makes the guest forget its contacts, an active-change
     *   push posts an immediate resync onto the loop (and any pointer
     *   operation or `CloseSession()` already ahead on the loop resyncs first)
     *   so held contacts on that device are forgotten without writing to the
     *   guest and a synthesized `CANCEL` is reported per session and requested
     *   display.
     * - Snapshot load: a synthesized `CANCEL` per session and requested display
     *   of the contacts held before the load, and a synthesized `KEY_ACTION_UP`
     *   per session and held key.
     * - Display removal: a synthesized `CANCEL` per session and requested
     *   display of the contacts bound to the removed display's device.
     *
     * Rejected events report nothing.
     *
     * Callbacks run in order on a dedicated thread. They must not block, and
     * must not call back into the dispatcher synchronously.
     */
    virtual ActivityEventSource& activity() = 0;

    /**
     * @brief Stops the dispatcher, for emulator exit, when the emulator may
     * never apply what is dispatched.
     *
     * Completes every pending operation with `UNAVAILABLE` on the calling
     * thread and delivers the queued activity before it returns. From then on
     * every method is a no-op: `OpenSession()` still returns an ID and
     * `Dispatch*()` completes with `UNAVAILABLE` on the calling thread.
     *
     * Idempotent and callable from any thread.
     */
    virtual void Shutdown() = 0;

    /**
     * @brief Opens a new input session with `options`.
     * @return A new session ID; IDs start at 1.
     */
    virtual InputSessionId OpenSession(SessionOptions options) = 0;

    /**
     * @brief Releases all contacts of `session` as cancelled (writing a palm
     * report followed by a lift report per active device), releases the keys
     * only it holds in ascending code order, and forgets it. Never rejected for
     * backpressure.
     */
    virtual void CloseSession(InputSessionId session) = 0;

    /**
     * @brief Applies `event` for `session`.
     *
     * - `RELATIVE_MOVE` and `SCROLL` change nothing and write nothing.
     * - `CANCEL` or `HOVER_EXIT` without pointers releases the session's
     *   contacts on the device `display_id` resolves to.
     * - A listed pointer the session holds moves on its bound device, or is
     *   lifted if `IsContactReleased()`. `DOWN` for a held pointer is a move,
     *   unless the session has `reject_down_on_held`: then that pointer is
     *   skipped, its contact is left as it was, and the event completes with
     *   `FAILED_PRECONDITION`; other pointers still apply.
     * - An unheld pointer becomes a new contact on the device `display_id`
     *   resolves to if the action is `DOWN`, `HOVER_ENTER`, or `HOVER_MOVE`,
     *   and is ignored otherwise. If no slot is free it is skipped and the
     *   event completes with `RESOURCE_EXHAUSTED`; other pointers still apply.
     *
     * A contact released by `CANCEL` turns into a palm in one report and lifts
     * in an immediate follow-up report, so the guest observes `MT_TOOL_PALM`
     * before the slot clears and cancels its gesture rather than firing a click
     * or drop.
     *
     * If a target device is missing or inactive the event completes with
     * `UNAVAILABLE`, and if `display_id` doesn't resolve with that error;
     * either way nothing is written or changed. An unknown session completes
     * with `FAILED_PRECONDITION`.
     *
     * @param done Runs once with the result. It is rejected without being
     *     applied with `RESOURCE_EXHAUSTED` if `session` has too many
     *     operations pending, and with `UNAVAILABLE` after `Shutdown()` or if
     *     `Shutdown()` runs before it is applied.
     */
    virtual void DispatchPointer(InputSessionId session,
                                 ::android::emulation::v2::PointerEvent event,
                                 DispatchCallback done) = 0;
    void DispatchPointer(InputSessionId session, ::android::emulation::v2::PointerEvent event) {
        DispatchPointer(session, std::move(event), nullptr);
    }

    /**
     * @brief Applies `event` for `session` to the key it maps to:
     * - `DOWN` makes the session a holder and always writes a press, so a
     *   client's autorepeat reaches the guest.
     * - `UP` from a holder removes it, and writes a release once no session
     *   holds the key. `UP` from a non-holder changes nothing.
     * - `PRESS`, `UNSPECIFIED`, and unknown actions are `DOWN` then `UP`, so a
     *   press while another session holds the key writes only a press.
     *
     * An unmapped key completes with `INVALID_ARGUMENT` and an unknown session
     * with `FAILED_PRECONDITION`; either way nothing is written.
     *
     * @param done As for `DispatchPointer()`.
     */
    virtual void DispatchKey(InputSessionId session, ::android::emulation::v2::KeyEvent event,
                             DispatchCallback done) = 0;
    void DispatchKey(InputSessionId session, ::android::emulation::v2::KeyEvent event) {
        DispatchKey(session, std::move(event), nullptr);
    }

    /**
     * @brief Types `utf8` for `session` unless it is empty; text holds no keys.
     * An unknown session completes with `FAILED_PRECONDITION`.
     *
     * @param done As for `DispatchPointer()`.
     */
    virtual void DispatchText(InputSessionId session, std::string utf8, DispatchCallback done) = 0;
    void DispatchText(InputSessionId session, std::string utf8) {
        DispatchText(session, std::move(utf8), nullptr);
    }
};

}  // namespace goldfish::grpc::v2
