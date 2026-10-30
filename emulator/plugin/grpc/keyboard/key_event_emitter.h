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

#include <string_view>

#include "absl/functional/function_ref.h"

#include "android/emulation/control/keyboard/key_conversion.h"
#include "emulator_controller.pb.h"

// Translation of keyboard requests into QEMU key events, shared by the key
// senders of the keyboard package.
//
// Internal to the keyboard package: other code should use IKeyEventSender or
// a goldfish::grpc::v2::KeyboardSink instead.
namespace android::emulation::control::keyboard {

/**
 * @brief Receives each QEMU key event a translation produces, in order.
 *
 * The receiver decides how the event reaches QEMU, for example posted to the
 * QEMU loop or sent at once.
 */
using QemuKeyEmitter = absl::FunctionRef<void(QemuKeyEvent)>;

/**
 * @brief Translates the DOM key, text, and key code of `request` into QEMU key
 * events.
 */
void EmitKeyboardEvent(const KeyboardEvent& request, QemuKeyEmitter emit);

/**
 * @brief Translates `utf8` into a press and release per character, with
 * modifiers as needed. Only ASCII characters are translated.
 */
void EmitUtf8String(std::string_view utf8, QemuKeyEmitter emit);

}  // namespace android::emulation::control::keyboard
