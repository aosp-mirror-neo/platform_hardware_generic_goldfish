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
#include <string_view>

#include "emulator/plugin/grpc/input/keyboard_sink.h"
#include "goldfish/async/event_loop.h"

extern "C" {
typedef struct QemuConsole QemuConsole;
typedef struct QKbdState QKbdState;
}

namespace android::emulation::control::keyboard {

/**
 * @brief The guest keyboard behind QEMU's keyboard state, as a `KeyboardSink`.
 *
 * Each key is sent to QEMU before the call returns, so keys and touches
 * written in order on the QEMU loop reach the guest in that order. Text is
 * translated as `IKeyEventSender` translates it: a press and release per ASCII
 * character, with modifiers as needed.
 *
 * ### Thread Safety:
 * `WriteKey()`, `TypeText()`, and `Reset()` run only on the QEMU loop.
 *
 * ### Ownership:
 * Owns its QEMU keyboard state. Destroy the sink on the QEMU loop, or after
 * the loop has stopped, so the state is never freed while QEMU uses it.
 */
class QemuKeyboardSink : public ::goldfish::grpc::v2::KeyboardSink {
  public:
    /**
     * @brief Constructs a `QemuKeyboardSink` attached to `console`.
     * @param console The console that receives the keys. May be null, as with
     *     `createKeyEventSender()`.
     * @param qemu_loop The QEMU loop; must outlive the sink.
     */
    QemuKeyboardSink(QemuConsole* console, ::goldfish::async::EventLoop& qemu_loop);
    ~QemuKeyboardSink() override;

    QemuKeyboardSink(const QemuKeyboardSink&) = delete;
    QemuKeyboardSink& operator=(const QemuKeyboardSink&) = delete;

    void WriteKey(uint32_t evdev_code, bool down) override;
    void TypeText(std::string_view utf8) override;
    void Reset() override;

  private:
    QemuConsole* const console_;
    ::goldfish::async::EventLoop& qemu_loop_;
    QKbdState* keyboard_state_;
};

}  // namespace android::emulation::control::keyboard
