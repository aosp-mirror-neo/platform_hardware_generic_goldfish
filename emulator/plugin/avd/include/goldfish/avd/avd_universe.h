// Copyright 2024 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "android/goldfish/vm_interface.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd_universe/battery/battery_state.h"
#include "goldfish/avd_universe/clipboard/clipboard_data.h"
#include "goldfish/avd_universe/fingerprint/fingerprint_sensor.h"
#include "goldfish/avd_universe/gps/location.h"
#include "goldfish/avd_universe/grpc/grpc_notification_channel.h"
#include "goldfish/avd_universe/guest_status/guest_status.h"
#include "goldfish/avd_universe/vehicle/vehicle_data.h"
#include "goldfish/devices/multidisplay/multidisplay_device.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/sensors/physical_model.h"
#include "goldfish/snapshottable/snapshottable.h"
#include "goldfish/system/virtual_clock.h"

namespace goldfish::avd {

/**
 * @brief This struct represents the whole AVD state.
 *
 * All AVD specific data should live here. This
 * explicitly describes the data lifetime and also
 * allows running several AVDs simultaneously
 * (in this case `GetAvd()` should be adjusted to
 * receive some form of an AVD id).
 *
 * The instance of this type is available between
 * the `avd_info_realize` and `avd_info_unrealize` events.
 */
struct AvdUniverse : public snapshottable::Snapshottable {
    virtual ~AvdUniverse() = default;

    const AvdProperties& Props() const { return *props_; }

    avd_universe::battery::ObservableBattery& GetBattery() { return battery_; }
    avd_universe::clipboard::ClipboardChannel& GetClipboardChannel() { return clipboard_channel_; }
    avd_universe::vehicle::VehicleChannel& GetVehicleChannel() { return vehicle_channel_; }
    avd_universe::fingerprint::ObservableFingerprintSensor& GetFingerprintSensor() {
        return fingerprint_sensor_;
    }
    avd_universe::grpc::GrpcNotificationEventSource& GetGrpcNotificationChannel() {
        return grpc_notification_event_source_;
    }
    avd_universe::guest_status::GuestStatus& GetGuestStatus() { return guest_status_; }
    avd_universe::gps::ObservableLocation& GetLocation() { return location_; }
    sensors::PhysicalModel& GetSensorsPhysicalModel() { return sensors_physical_model_; }

    virtual async::EventLoop& GetQemuEventLoop() = 0;
    virtual metrics::MetricsReporter& GetMetricsReporter() = 0;

    /**
     * @brief Controls and observes the VM of this AVD.
     * @return A reference that stays valid for the lifetime of the universe.
     */
    virtual android::goldfish::VmOperations& GetVmOperations() = 0;

    void SetActiveMultiDisplayDevice(
            std::shared_ptr<devices::multidisplay::MultiDisplayDevice> device)
            ABSL_LOCKS_EXCLUDED(device_mutex_);
    std::shared_ptr<devices::multidisplay::MultiDisplayDevice> GetActiveMultiDisplayDevice()
            ABSL_LOCKS_EXCLUDED(device_mutex_);

    void SetNetsimEndpoint(std::string_view endpoint) ABSL_LOCKS_EXCLUDED(device_mutex_);
    std::string GetNetsimEndpoint() const ABSL_LOCKS_EXCLUDED(device_mutex_);

    virtual display::IMultiDisplay& GetMultiDisplay() const = 0;

    /**
     * @brief Applies the input of every input session to the guest.
     * @return A thread-safe dispatcher, shared with input clients that may
     *     outlive the universe, such as WebRTC channels. It is shut down when
     *     the gRPC server stops at emulator exit, or at the latest when the
     *     universe is destroyed.
     */
    virtual std::shared_ptr<::goldfish::grpc::v2::IInputDispatcher> GetInputDispatcher() = 0;

    void SetGenericSnapshotInProgress(bool in_progress) {
        generic_snapshot_in_progress_ = in_progress;
    }
    bool IsGenericSnapshotInProgress() const { return generic_snapshot_in_progress_; }

    AvdUniverse(std::unique_ptr<AvdProperties> props,
                const ::goldfish::system::VirtualClock& virtual_clock);
    AvdUniverse(const AvdUniverse&) = delete;
    AvdUniverse(AvdUniverse&&) = delete;
    AvdUniverse& operator=(const AvdUniverse&) = delete;
    AvdUniverse& operator=(AvdUniverse&&) = delete;

  protected:
    const std::unique_ptr<const AvdProperties> props_;
    bool generic_snapshot_in_progress_{false};

    mutable absl::Mutex device_mutex_;
    std::shared_ptr<devices::multidisplay::MultiDisplayDevice> active_multi_display_device_
            ABSL_GUARDED_BY(device_mutex_);
    std::string netsim_endpoint_ ABSL_GUARDED_BY(device_mutex_);

    avd_universe::grpc::GrpcNotificationEventSource grpc_notification_event_source_;
    avd_universe::battery::ObservableBattery battery_;
    avd_universe::clipboard::ClipboardChannel clipboard_channel_;
    avd_universe::vehicle::VehicleChannel vehicle_channel_;
    avd_universe::fingerprint::ObservableFingerprintSensor fingerprint_sensor_;
    avd_universe::guest_status::GuestStatus guest_status_;
    avd_universe::gps::ObservableLocation location_;
    sensors::PhysicalModel sensors_physical_model_;
};

}  // namespace goldfish::avd
