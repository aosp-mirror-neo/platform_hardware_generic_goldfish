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

#include "absl/status/status.h"

#include "emulator/plugin/grpc/input/slot_registry.h"
#include "goldfish/display/abstract_multi_display.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2 {

/**
 * @brief Validates AEMU v2 InputEvent messages against protocol bounds and device capabilities.
 *
 * Shared by gRPC `InputServiceImpl` and the WebRTC `input_v2` data channel so that both
 * transports enforce identical capability and argument checks before dispatching events to
 * `IInputDispatcher`.
 *
 * ### Thread Safety:
 * `Validate` is const and thread-safe assuming `multi_display` is thread-safe.
 *
 * ### Ownership:
 * Does not own `multi_display`, which must outlive the validator.
 */
class V2InputValidator {
  public:
    static constexpr int32_t kDefaultMaxTouchPoints =
            static_cast<int32_t>(::android::emulation::control::kMtsPointersNum);

    struct Options {
        bool hw_sensor_hinge = false;
        bool hw_keyboard = true;
        bool hw_rotary_input = false;
        int32_t max_touch_points = kDefaultMaxTouchPoints;
    };

    explicit V2InputValidator(::goldfish::display::IMultiDisplay& multi_display);
    V2InputValidator(::goldfish::display::IMultiDisplay& multi_display, Options options);

    /**
     * @brief Validates `event` against argument invariants and configured device capabilities.
     *
     * @param event The input event to validate.
     * @return `absl::OkStatus()` if valid, `INVALID_ARGUMENT` for malformed fields or unknown
     *     identifiers, or `FAILED_PRECONDITION` for capabilities not supported by the device.
     */
    absl::Status Validate(const ::android::emulation::v2::InputEvent& event) const;

  private:
    absl::Status ValidatePointerEvent(const ::android::emulation::v2::PointerEvent& pointer) const;

    ::goldfish::display::IMultiDisplay* multi_display_;
    Options options_;
};

}  // namespace goldfish::grpc::v2
