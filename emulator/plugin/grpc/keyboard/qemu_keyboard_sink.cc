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
#include "android/emulation/control/keyboard/qemu_keyboard_sink.h"

#include <bitset>
#include <string_view>

#include "absl/log/check.h"

#include "android/emulation/control/keyboard/key_conversion.h"
#include "key_event_emitter.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace android::emulation::control::keyboard {

namespace {

// Each key-up plus its SYN_REPORT consumes 2 descriptors on the 64-entry
// virtio-input event virtqueue (`virtio-input.c`), so pace bursts of 24 keys
// (48 descriptors) with a 1 ms `kbd_queue` delay.
constexpr int kKeyReleaseBatchSize = 24;

}  // namespace

QemuKeyboardSink::QemuKeyboardSink(QemuConsole* console, ::goldfish::async::EventLoop& qemu_loop)
        : console_(console), qemu_loop_(qemu_loop), keyboard_state_(qkbd_state_init(console)) {}

QemuKeyboardSink::~QemuKeyboardSink() {
    qkbd_state_free(keyboard_state_);
}

void QemuKeyboardSink::WriteKey(uint32_t evdev_code, bool down) {
    DCHECK(qemu_loop_.IsOnLoopThread());
    QemuKeyEvent{evdev_to_qcode(evdev_code), down}.send(keyboard_state_);
}

void QemuKeyboardSink::TypeText(std::string_view utf8) {
    DCHECK(qemu_loop_.IsOnLoopThread());
    EmitUtf8String(utf8, [this](QemuKeyEvent event) { event.send(keyboard_state_); });
}

void QemuKeyboardSink::Reset() {
    DCHECK(qemu_loop_.IsOnLoopThread());
    qkbd_state_free(keyboard_state_);
    keyboard_state_ = qkbd_state_init(console_);

    // QKbdState is host-only (not part of VMState), so keys held in the guest
    // when a snapshot was saved have 0 bits in `keyboard_state_->keys` after
    // load and would be filtered out by `qkbd_state_key_event()`. Send key-ups
    // for every qcode a Linux key maps to directly; the guest ignores releases
    // for keys that are already up.
    //
    // Walks the Linux key codes through `qemu_input_linux_to_qcode()` rather
    // than QEMU's qcode-to-Linux table: on Windows the plugin can only import
    // functions from the QEMU executable, not data.
    std::bitset<Q_KEY_CODE__MAX> released;
    int sent_in_batch = 0;
    for (unsigned int linux_code = 1; linux_code <= KEY_MAX; ++linux_code) {
        const int qcode = qemu_input_linux_to_qcode(linux_code);
        if (qcode <= Q_KEY_CODE_UNMAPPED || qcode >= Q_KEY_CODE__MAX || released.test(qcode)) {
            continue;  // Unmapped, or released for an earlier Linux code.
        }
        released.set(qcode);
        if (sent_in_batch == kKeyReleaseBatchSize) {
            qemu_input_event_send_key_delay(1);
            sent_in_batch = 0;
        }
        qemu_input_event_send_key_qcode(console_, static_cast<QKeyCode>(qcode), /*down=*/false);
        ++sent_in_batch;
    }
}

}  // namespace android::emulation::control::keyboard
