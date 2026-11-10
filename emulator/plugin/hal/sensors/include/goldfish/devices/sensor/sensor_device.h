// Copyright 2024 The Android Open Source Project
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

#include "android/goldfish/device_type.h"
#include "android/goldfish/hardware_config.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/devices/connector_registry.h"
#include "goldfish/devices/internal/hal_plug.h"
#include "goldfish/sensors/physical_model.h"
#include "goldfish/system/virtual_clock.h"

namespace goldfish::devices::sensor {

using goldfish::async::EventLoop;
using goldfish::sensors::PhysicalModel;

using namespace std::string_view_literals;

// A Qemud based sensor emulator.
class ISensorDevice : public HalPlug, public std::enable_shared_from_this<ISensorDevice> {
  public:
    ~ISensorDevice() override = default;

    // Name under which you should register this in qemud
    static constexpr std::string_view kServiceName = "sensors"sv;

    /**
     * @brief Registers the sensor device with the connector registry.
     *
     * This function registers the sensor device with the provided
     * `IConnectorRegistry` instance, making it available for connection
     * through the qemud pipe.  The provided `Avd` object supplies
     * configuration information for the sensor device, while the `Looper`
     * instance manages the event loop for asynchronous operations.
     *
     * @param registry The `IConnectorRegistry` instance to register with.
     * @param avd The `Avd` object containing the AVD configuration.
     * @param looper The `Looper` instance to use for asynchronous operations.
     *
     * @note The `avd` and `looper` objects are expected to remain valid for
     * the lifetime of the registry.  Their lifecycles should be managed
     * externally to ensure they outlive the registry.
     */
    static void RegisterDevice(PhysicalModel* pm, IConnectorRegistry* registry,
                               android::goldfish::DeviceType avd_type, int avd_api,
                               const android::goldfish::HardwareConfig& hw, EventLoop* client_loop,
                               EventLoop* qemu_loop,
                               const ::goldfish::system::VirtualClock& clock);
};

}  // namespace goldfish::devices::sensor
