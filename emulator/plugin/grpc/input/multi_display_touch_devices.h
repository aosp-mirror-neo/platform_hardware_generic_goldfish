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
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"

#include "contact_table.h"
#include "goldfish/display/abstract_multi_display.h"
#include "touch_device.h"

namespace goldfish::grpc::v2 {

/**
 * @brief The guest touch devices of the emulator's displays.
 *
 * Each display has its own virtio input device, identified by the display's ID.
 * `Resolve()` maps a requested display to the display that receives its
 * touches, applying the foldable remap of display 0 to 1. `Find()` returns the
 * sink that writes reports to a display's device synchronously and reports
 * whether its guest driver is active.
 *
 * ### Thread Safety:
 * Called only on the QEMU event loop, like `TouchDevices`.
 *
 * ### Ownership:
 * `multi_display` must outlive this object. Sinks are owned by this object and
 * cached per device, so a pointer returned by `Find()` stays valid for this
 * object's lifetime, even after its display is removed. A sink holds only a
 * weak reference to its display, which `Find()` rebinds to the display that
 * currently has the device's ID; a sink whose display is gone is inactive and
 * writes nothing.
 */
class MultiDisplayTouchDevices : public TouchDevices {
  public:
    /**
     * @brief Constructs `MultiDisplayTouchDevices` backed by `multi_display`.
     * @param has_hinge Whether the device is a foldable, from the hardware
     *        configuration.
     */
    MultiDisplayTouchDevices(::goldfish::display::IMultiDisplay& multi_display, bool has_hinge);
    ~MultiDisplayTouchDevices() override;

    /**
     * @brief Resolves `display_id` to the active touch device that receives its
     * touches.
     * @return The ID of the display that receives touches for `display_id`:
     *         display 1 for display 0 on a folded foldable. `INVALID_ARGUMENT`
     *         for a negative ID, or the error of a missing or inactive display.
     */
    absl::StatusOr<TouchDeviceId> Resolve(int32_t display_id) override;

    /**
     * @brief Looks up the touch device sink for `device`.
     * @return The sink of the display `device`, or `nullptr` if there is no
     *         such display.
     */
    TouchDeviceSink* Find(TouchDeviceId device) override;

    /**
     * @return The IDs of all existing displays, including display 0 on a folded
     *         foldable, in ascending order.
     */
    std::vector<TouchDeviceId> Devices() override;

  private:
    class DisplaySink;

    ::goldfish::display::IMultiDisplay& multi_display_;
    absl::flat_hash_map<TouchDeviceId, std::unique_ptr<DisplaySink>> sinks_;
    const bool has_hinge_;
};

}  // namespace goldfish::grpc::v2
