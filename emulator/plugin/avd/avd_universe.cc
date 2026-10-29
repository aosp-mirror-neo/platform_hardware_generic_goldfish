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

#include "goldfish/avd/avd_universe.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/base/system.h"

namespace goldfish::avd {

AvdUniverse::AvdUniverse(std::unique_ptr<AvdProperties> props,
                         const ::goldfish::system::VirtualClock& virtual_clock)
        : props_(std::move(props))
        , sensors_physical_model_(props_->hw_config, virtual_clock) {
    guest_status_.Reset(
            absl::UnixEpoch() +
            absl::Milliseconds(android::base::System::Get()->GetProcessTimes().wall_clock_ms));
}

void AvdUniverse::SetActiveMultiDisplayDevice(
        std::shared_ptr<devices::multidisplay::MultiDisplayDevice> device) {
    absl::MutexLock lock(&device_mutex_);
    active_multi_display_device_ = std::move(device);
}

std::shared_ptr<devices::multidisplay::MultiDisplayDevice>
AvdUniverse::GetActiveMultiDisplayDevice() {
    absl::MutexLock lock(&device_mutex_);
    return active_multi_display_device_;
}

void AvdUniverse::SetNetsimEndpoint(std::string_view endpoint) {
    absl::MutexLock lock(&device_mutex_);
    netsim_endpoint_ = endpoint;
}

std::string AvdUniverse::GetNetsimEndpoint() const {
    absl::MutexLock lock(&device_mutex_);
    return netsim_endpoint_;
}

}  // namespace goldfish::avd
