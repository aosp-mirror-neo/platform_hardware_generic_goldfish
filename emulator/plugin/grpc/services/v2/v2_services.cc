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

#include "goldfish/grpc/v2/v2_services.h"

#include <memory>
#include <vector>

#include "battery/battery_service_impl.h"
#include "discovery/capabilities_service_impl.h"
#include "environment/sensors_service_impl.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd/global_event_loop.h"
#include "hypervisor/hypervisor_service_impl.h"
#include "input/input_service_impl.h"
#include "location/location_service_impl.h"
#include "operations/operations_service_impl.h"
#include "system/system_service_impl.h"

namespace goldfish::grpc::v2 {

using ::android::goldfish::VmOperations;
using ::goldfish::avd::AvdUniverse;
using ::grpc::Service;

std::vector<std::shared_ptr<Service>> CreateV2Services(AvdUniverse& avd_universe,
                                                       VmOperations* vm_operations) {
    std::vector<std::shared_ptr<Service>> services;

    services.emplace_back(std::make_shared<CapabilitiesServiceImpl>(avd_universe));
    services.emplace_back(
            std::make_shared<SensorsServiceImpl>(avd_universe.GetSensorsPhysicalModel()));
    services.emplace_back(std::make_shared<SystemServiceImpl>(
            avd_universe.GetGuestStatus(), &avd_universe.GetGrpcNotificationChannel(),
            vm_operations, &avd_universe.GetClipboardChannel(),
            &avd_universe.GetFingerprintSensor()));

    const auto& hw = avd_universe.Props().hw_config;
    services.emplace_back(std::make_shared<LocationServiceImpl>(
            avd_universe.GetLocation(),
            LocationServiceImpl::Options{.has_gps = hw.hw_gps,
                                         .event_loop = ::goldfish::avd::globalEventLoop()}));
    services.emplace_back(std::make_shared<BatteryServiceImpl>(
            avd_universe.GetBattery(), BatteryServiceImpl::Options{.has_battery = hw.hw_battery}));
    services.emplace_back(std::make_shared<InputServiceImpl>(
            avd_universe.GetInputDispatcher(), avd_universe.GetMultiDisplay(),
            InputServiceImpl::Options{.hw_sensor_hinge = hw.hw_sensor_hinge,
                                      .hw_keyboard = hw.hw_keyboard,
                                      .event_loop = ::goldfish::avd::globalEventLoop()}));

    if (vm_operations != nullptr) {
        auto operations_service = std::make_shared<OperationsServiceImpl>();
        services.emplace_back(operations_service);
        services.emplace_back(
                std::make_shared<HypervisorServiceImpl>(*vm_operations, operations_service));
    }

    return services;
}

}  // namespace goldfish::grpc::v2
