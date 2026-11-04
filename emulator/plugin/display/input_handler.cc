// Copyright 2026 The Android Open Source Project
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

#include "goldfish/display/input_handler.h"

#include "absl/types/span.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qom/object.h"
}
#include "virtio_bridge.h"
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::display {

namespace {

// Event Types
constexpr int kEvSyn = 0x00;

// Synchronization Events
constexpr int kSynReport = 0x00;

}  // namespace

void InputHandler::SendEvDevReport(VirtIOInputHID* vhid, absl::Span<const EvDevEvent> events) {
    if (!vhid) return;
    for (const EvDevEvent& ev : events) {
        virtio_input_send_evdev(vhid, ev.type, ev.code, ev.value);
    }
    virtio_input_send_evdev(vhid, kEvSyn, kSynReport, 0);
}

}  // namespace goldfish::display
