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
#include "android/emulation/control/keyboard/key_event_sender.h"

#include <cstdint>
#include <ios>
#include <memory>
#include <string_view>

#include "absl/log/log.h"

#include "android/emulation/control/keyboard/key_conversion.h"
#include "emulator_controller.pb.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/parsing/utf8_iterator.h"
#include "key_event_emitter.h"

extern "C" {
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "ui/input.h"


// Forward declarations for qemu bindings.
typedef struct QKbdState QKbdState;
QKbdState* qkbd_state_init(QemuConsole* con);  // NOLINT(readability-identifier-naming)
void qkbd_state_free(QKbdState* kbd);        // NOLINT(readability-identifier-naming)

// IWYU pragma: end_keep
// clang-format on
}

namespace android {
namespace emulation {
namespace control {
namespace keyboard {

using goldfish::async::EventLoop;

// Necessary on Windows.
#undef send

namespace {

// Translates keyboard requests into QEMU key events, handing each to `emit`.
// Lives only for the duration of one EmitKeyboardEvent() or EmitUtf8String().
class KeyEventConverter {
  public:
    explicit KeyEventConverter(QemuKeyEmitter emit) : emit_(emit) {}

  private:
    void EmitAsciiEvents(unsigned short character, KeyboardEvent::KeyEventType event_type) {
        if (event_type == KeyboardEvent::keydown || event_type == KeyboardEvent::keypress) {
            for (const auto& event : ascii_to_qcode(character, /*down=*/true)) {
                emit_(event);
            }
        }
        if (event_type == KeyboardEvent::keyup || event_type == KeyboardEvent::keypress) {
            for (const auto& event : ascii_to_qcode(character, /*down=*/false)) {
                emit_(event);
            }
        }
    }

    void SendKeyCode(int32_t code, const KeyboardEvent::KeyCodeType code_type,
                     const KeyboardEvent::KeyEventType event_type) {
        KeyCodeType type = static_cast<KeyCodeType>(code_type);
        QKeyCode qcode = keycode_to_qcode(code, type);
        if (qcode >= Q_KEY_CODE__MAX) {
            LOG(QFATAL) << "Invalid QKeyCode - too big: " << qcode;
        }
        if (event_type == KeyboardEvent::keydown || event_type == KeyboardEvent::keypress) {
            emit_({qcode, /*down=*/true});
        }
        if (event_type == KeyboardEvent::keyup || event_type == KeyboardEvent::keypress) {
            emit_({qcode, /*down=*/false});
        }
    }

  public:
    void DispatchKeyboardEvent(const KeyboardEvent& request) {
        VLOG(1) << "Handling " << request.ShortDebugString();
        if (!request.key().empty()) {
            const DomKeyResolution resolved = resolve_dom_key(request.key());
            switch (resolved.kind) {
            case DomKeyResolution::Kind::kNonPrintable:
                SendKeyCode(resolved.evdev, KeyboardEvent::Evdev, request.eventtype());
                break;
            case DomKeyResolution::Kind::kCharacter:
                if (resolved.character > 0xFF) {
                    VLOG(1) << "Ignoring non ascii character, we cannot translate it Key="
                            << request.key();
                    return;
                }
                EmitAsciiEvents(static_cast<unsigned short>(resolved.character),
                                request.eventtype());
                break;
            case DomKeyResolution::Kind::kNone: {
                if (request.key() == "Dead") {
                    break;
                }
                const uint32_t evdev = dom_code_to_evdev(request.key());
                if (evdev > 0) {
                    SendKeyCode(evdev, KeyboardEvent::Evdev, request.eventtype());
                } else {
                    LOG(WARNING) << "Dropped keyboard event: '" << request.key()
                                 << "' is not a valid W3C DOM key/code. "
                                 << "Use a standard DOM string (e.g. 'KeyA', 'Enter'), "
                                 << "an evdev 'keyCode', or the 'text' field.";
                }
                break;
            }
            }
        }

        if (!request.text().empty()) {
            SendUtf8String(request.text());
        }

        if (request.keycode() > 0) {
            SendKeyCode(request.keycode(), request.codetype(), request.eventtype());
        }
    }

    void SendUtf8String(std::string_view utf8) {
        VLOG(1) << "Sending utf8 string: " << utf8;
        // We need to convert every individual character to a sequence of evdev
        // events. This is due to the fact that a single character can be
        // translated into multiple ev dev events in case a modifier is needed.
        // For example: 'e' -> gives one ev dev event [18]. 'E' ->  gives two
        // [42, 18] (shift + e). The end result is that the sequence "Ee" will be
        // translated as [42, 18, 18]. If we deliver this sequence as down + up, we
        // would see "EE" in android. By splitting this up in individual chars we
        // guarantee that this doesn't happen.
        goldfish::parsing::Utf8Iterator utf8_it(utf8);
        for (int32_t codepoint = utf8_it(); codepoint >= 0; codepoint = utf8_it()) {
            if (IsTypeableCodePoint(codepoint)) {
                EmitAsciiEvents(static_cast<unsigned short>(codepoint), KeyboardEvent::keypress);
            } else {
                VLOG(1) << "Skipping untypeable UTF-8 codepoint U+" << std::hex << codepoint
                        << std::dec;
            }
        }
    }

  private:
    QemuKeyEmitter emit_;
};

}  // namespace

void EmitKeyboardEvent(const KeyboardEvent& request, QemuKeyEmitter emit) {
    KeyEventConverter(emit).DispatchKeyboardEvent(request);
}

void EmitUtf8String(std::string_view utf8, QemuKeyEmitter emit) {
    KeyEventConverter(emit).SendUtf8String(utf8);
}

class KeyEventSenderImpl : public IKeyEventSender {
  public:
    KeyEventSenderImpl(QemuConsole* con, EventLoop* qemu_loop) : qemu_loop_(qemu_loop) {
        keyboard_state_ = qkbd_state_init(con);
    }
    // Queued key events capture keyboard_state_, so it is freed on the loop after them. If the loop
    // no longer accepts tasks, it is leaked rather than freed under a pending event.
    ~KeyEventSenderImpl() {
        qemu_loop_->Post([kbd = keyboard_state_] { qkbd_state_free(kbd); }).IgnoreError();
    }

    void send(const KeyboardEvent request) override {
        EmitKeyboardEvent(request, [this](QemuKeyEvent event) { PostKeyEvent(event); });
    }

  private:
    void PostKeyEvent(QemuKeyEvent event) {
        qemu_loop_->Post([event, kbd = keyboard_state_] { event.send(kbd); }).IgnoreError();
    }

    EventLoop* qemu_loop_;
    ::QKbdState* keyboard_state_;
};

std::unique_ptr<IKeyEventSender> createKeyEventSender(QemuConsole* console, EventLoop* qemu_loop) {
    return std::make_unique<KeyEventSenderImpl>(console, qemu_loop);
}

}  // namespace keyboard
}  // namespace control
}  // namespace emulation
}  // namespace android
