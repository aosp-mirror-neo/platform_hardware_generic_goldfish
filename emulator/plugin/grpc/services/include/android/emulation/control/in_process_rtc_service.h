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

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "android/emulation/control/keyboard/key_event_sender.h"
#include "emulator/plugin/grpc/input/v2_input_validator.h"
#include "goldfish/display/display.h"
#include "goldfish/videobridge/input_sender.h"

extern "C" {
typedef struct AudioBackend AudioBackend;
}

namespace goldfish::avd {
struct AvdUniverse;
}

namespace goldfish::display {
class IMultiDisplay;
}

namespace goldfish::grpc::v2 {
class IInputDispatcher;
}

namespace goldfish::sensors {
class PhysicalModel;
}

namespace android::emulation::control {

using ::goldfish::avd::AvdUniverse;
using ::goldfish::display::IMultiDisplay;
using ::goldfish::videobridge::DataChannelLabel;
using ::goldfish::videobridge::InputSender;
using ::goldfish::videobridge::InputSenderFactory;
using keyboard::IKeyEventSender;

/**
 * @brief Creates an `InputSender` factory configured for in-process event dispatching.
 *
 * Directs WebRTC v1 data-channel input events (touch, mouse, wheel) to the provided
 * `IMultiDisplay` instance, and keyboard events to the `IKeyEventSender`. Each v2 input
 * channel is its own human input session on `input_dispatcher`, which releases the
 * channel's touches and keys when the channel closes.
 *
 * @param input_dispatcher Receives v2 input; without one, v2 input is dropped.
 *
 * ### Thread Safety:
 * - The returned factory lambda is thread-safe and can be invoked on WebRTC signaling threads.
 */
InputSenderFactory CreateInProcessInputSenderFactory(
        IMultiDisplay& multidisplay, std::shared_ptr<IKeyEventSender> key_event_sender,
        std::shared_ptr<::goldfish::grpc::v2::IInputDispatcher> input_dispatcher = nullptr,
        ::goldfish::sensors::PhysicalModel* physical_model = nullptr,
        ::goldfish::grpc::v2::V2InputValidator::Options v2_validator_options = {});

/**
 * Instantiates the in-process WebRTC RtcServices (v1 and v2) bound to AvdUniverse.
 *
 * Data Flow & Coordination:
 * - Wires guest display rendering from IMultiDisplay (display_id) to InProcessVideoSource.
 * - Wires guest PCM audio recording from QEMU's AudioBackend to QemuAudioSource.
 * - Configures InProcessInputSender to inject remote user inputs directly into QEMU.
 * - Bundles all sources and handlers into a single Switchboard, then returns one gRPC service
 *   per signaling API version sharing that Switchboard.
 *
 * Lifetime:
 * - The returned gRPC Service instances manage the lifecycles of their underlying WebRTC
 *   pipeline, video sources, audio sources, and input senders.
 */
std::vector<std::shared_ptr<::grpc::Service>> CreateInProcessRtcServices(
        AvdUniverse& avd_universe, uint32_t display_id = 0, uint32_t console_index = 0,
        AudioBackend* audio_backend = nullptr);

}  // namespace android::emulation::control
