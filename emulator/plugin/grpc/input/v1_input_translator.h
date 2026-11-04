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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "android/emulation/control/ev_dev_event.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator_controller.pb.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/display.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2 {

// Translates a legacy v1 MouseEvent into a v2 InputEvent.
//
// Implementations share per-display `held_pointers` with TouchEvent translation
// so that session pointer ID 0 on a given display can transition seamlessly
// between single-finger mouse drags and multi-touch gestures (such as Android
// Studio's Ctrl-pinch overlay).
class MouseEventTranslator {
  public:
    virtual ~MouseEventTranslator() = default;

    virtual std::optional<::android::emulation::v2::InputEvent> Translate(
            const ::android::emulation::control::MouseEvent& event,
            ::goldfish::display::IMultiDisplay& multi_display, bool has_hinge,
            absl::flat_hash_map<int32_t, ::goldfish::display::Dimensions>& held_pointers) = 0;
};

// Translates v1 MouseEvent messages into single-finger touch PointerEvents
// using session pointer ID 0 and TOOL_TYPE_FINGER.
class SingleFingerMouseTranslator final : public MouseEventTranslator {
  public:
    std::optional<::android::emulation::v2::InputEvent> Translate(
            const ::android::emulation::control::MouseEvent& event,
            ::goldfish::display::IMultiDisplay& multi_display, bool has_hinge,
            absl::flat_hash_map<int32_t, ::goldfish::display::Dimensions>& held_pointers) override;
};

// Stateful per-stream translator converting legacy v1 EmulatorController input
// messages into canonical v2 InputEvent protos for InputDispatcher.
//
// Pointer and external touch-identifier state is scoped per `display_id` so a
// single shared stream (such as Android Studio's streamInputEvent connection)
// can drive independent gestures across multiple displays without colliding.
//
// Not thread-safe; each gRPC stream or WebRTC data channel session should own
// its own V1InputTranslator instance (or serialize access externally).
class V1InputTranslator {
  public:
    static constexpr int32_t kMouseAndTouchZeroPointerId = 0;
    static constexpr int32_t kMaxTouchPointers = 10;
    static constexpr int32_t kPenPointerId = 10;
    static constexpr size_t kMaxBufferedAndroidEvents = 256;

    explicit V1InputTranslator(::goldfish::display::IMultiDisplay& multi_display,
                               bool has_hinge = false);
    V1InputTranslator(::goldfish::display::IMultiDisplay& multi_display, bool has_hinge,
                      std::unique_ptr<MouseEventTranslator> mouse_translator);

    // Clears all held pointer mappings, external touch IDs, and pending raw
    // AndroidEvent buffers across every display.
    void Reset();

    // Clears held pointer mappings, external touch IDs, and pending raw
    // AndroidEvent buffers for `display_id` without disturbing other displays.
    void ResetDisplay(int32_t display_id);

    // Synchronizes translator state with asynchronous InputDispatcher resets
    // (idle timeout, snapshot load, display removal, or guest driver reset).
    void SyncResetTracker(const SessionResetTracker& tracker);

    // Translates a v1 TouchEvent into zero, one, or two v2 PointerEvent
    // messages (splitting simultaneous new press + release into DOWN then UP).
    std::vector<::android::emulation::v2::InputEvent> TranslateTouch(
            const ::android::emulation::control::TouchEvent& event);

    // Translates a v1 MouseEvent via the configured MouseEventTranslator.
    std::optional<::android::emulation::v2::InputEvent> TranslateMouse(
            const ::android::emulation::control::MouseEvent& event);

    // Translates a v1 PenEvent into v2 PointerEvent messages using
    // TOOL_TYPE_STYLUS or TOOL_TYPE_ERASER on session pointer ID 10.
    std::vector<::android::emulation::v2::InputEvent> TranslatePen(
            const ::android::emulation::control::PenEvent& event);

    // Translates a v1 KeyboardEvent into a sequence of v2 KeyEvent and/or
    // TextEvent messages.
    std::vector<::android::emulation::v2::InputEvent> TranslateKeyboard(
            const ::android::emulation::control::KeyboardEvent& event) const;

    // Buffers deprecated v1 AndroidEvent wired-headset switch tuples
    // (EV_SW SW_HEADPHONE_INSERT and SW_MICROPHONE_INSERT) per display and
    // flushes them to IDisplay::SendEvDevReport upon receiving EV_SYN/SYN_REPORT.
    // All other AndroidEvent types and codes are dropped so raw evdev cannot
    // bypass InputDispatcher's multi-touch slot and held-key tracking.
    absl::Status DispatchAndroidEvent(const ::android::emulation::control::AndroidEvent& event);

    // Translates a polymorphic v1 InputEvent envelope. If the envelope carries
    // an AndroidEvent, dispatches it via DispatchAndroidEvent and returns {}.
    std::vector<::android::emulation::v2::InputEvent> TranslateInputEvent(
            const ::android::emulation::control::InputEvent& event);

  private:
    struct DisplayPointerState {
        absl::flat_hash_map<int32_t, ::goldfish::display::Dimensions> held_pointers;
        absl::flat_hash_map<int32_t, int32_t> external_touch_id_to_pointer_id;
    };

    static std::optional<int32_t> ResolveTouchPointerId(DisplayPointerState& state,
                                                        int32_t v1_identifier, bool is_down,
                                                        uint32_t batch_direct_mask);
    static void ReleaseTouchPointerId(DisplayPointerState& state, int32_t v1_identifier,
                                      int32_t pointer_id);
    void PruneDisplayStateIfEmpty(int32_t display_id);

    ::goldfish::display::IMultiDisplay* multi_display_;
    bool has_hinge_;
    std::unique_ptr<MouseEventTranslator> mouse_translator_;
    absl::flat_hash_map<int32_t, DisplayPointerState> display_states_;
    absl::flat_hash_map<int32_t, ::android::emulation::control::EvDevEvents> android_event_buffers_;
    uint64_t last_reset_version_ = 0;
    uint64_t last_full_generation_ = 0;
    absl::flat_hash_map<int32_t, uint64_t> last_display_generations_;
};

}  // namespace goldfish::grpc::v2
