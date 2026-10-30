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

#pragma once

#include "absl/types/span.h"

#include "goldfish/display/ev_dev_event.h"

struct VirtIOInputHID;

namespace goldfish::display {

/**
 * @class InputHandler
 * @brief Utility class to send input events to virtio-input devices.
 */
class InputHandler {
  public:
    /**
     * @brief Writes `events`, then EV_SYN/SYN_REPORT, to the device in order.
     *
     * @param vhid The virtio-input device handle.
     * @param events The report's events, without the trailing SYN_REPORT.
     */
    static void SendEvDevReport(VirtIOInputHID* vhid, absl::Span<const EvDevEvent> events);
};

}  // namespace goldfish::display
