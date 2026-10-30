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
#include "goldfish/devices/gps/gps_device.h"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/avd_universe/gps/location.h"
#include "goldfish/devices/qemud/qemud.h"

namespace goldfish::devices::gps {

using goldfish::avd_universe::gps::Location;
using goldfish::avd_universe::gps::ObservableLocation;
using LocationUpdateSubscription =
        std::unique_ptr<android::base::eventing::ScopedEventCallback<ObservableLocation, Location>>;

namespace {
std::string ToNmeaCoord(double degrees, bool is_latitude) {
    const double d = std::abs(degrees);
    const int dint = static_cast<int>(d);
    const double m = (d - dint) * 60.0;
    const int mint = static_cast<int>(m);
    const int fint = static_cast<int>(10000 * (m - mint));

    if (is_latitude) {
        // latitude from 0 to 90 need 2 digits
        return absl::StrFormat("%02d%02d.%04d", dint, mint, fint);
    }
    // longitude from 0 to 180, need 3 digits
    return absl::StrFormat("%03d%02d.%04d", dint, mint, fint);
}

std::string CalculateChecksum(const std::string& sentence_content) {
    unsigned char checksum = 0;
    for (const char c : sentence_content) {
        checksum ^= c;
    }
    return absl::StrFormat("%02X", checksum);
}

std::string FormatGPGGA(const Location& loc, absl::Time now) {
    const std::string time_str = absl::FormatTime("%H%M%S", now, absl::UTCTimeZone());

    const std::string lat_str = ToNmeaCoord(loc.latitude, true);

    const char lat_dir = (loc.latitude >= 0) ? 'N' : 'S';

    const std::string lon_str = ToNmeaCoord(loc.longitude, false);
    const char lon_dir = (loc.longitude >= 0) ? 'E' : 'W';

    const int fix_quality = loc.satellites > 0 ? 1 : 0;
    const double hdop = loc.accuracy > 0.0 ? loc.accuracy : 1.0;

    const std::string body = absl::StrFormat("GPGGA,%s,%s,%c,%s,%c,%d,%02d,%.1f,%.2f,M,0.0,M,,",
                                             time_str, lat_str, lat_dir, lon_str, lon_dir,
                                             fix_quality, loc.satellites, hdop, loc.altitude);

    return absl::StrFormat("$%s*%s\r\n", body, CalculateChecksum(body));
}

std::string FormatGPRMC(const Location& loc, absl::Time now) {
    const std::string time_str = absl::FormatTime("%H%M%S", now, absl::UTCTimeZone());

    const std::string date_str = absl::FormatTime("%d%m%y", now, absl::UTCTimeZone());

    const std::string lat_str = ToNmeaCoord(loc.latitude, true);
    const char lat_dir = (loc.latitude >= 0) ? 'N' : 'S';
    const std::string lon_str = ToNmeaCoord(loc.longitude, false);
    const char lon_dir = (loc.longitude >= 0) ? 'E' : 'W';

    const char status = loc.satellites > 0 ? 'A' : 'V';
    const double speed_knots = loc.speed * 1.94384;

    const std::string body =
            absl::StrFormat("GPRMC,%s,%c,%s,%c,%s,%c,%.2f,%.2f,%s,0.0,W", time_str, status, lat_str,
                            lat_dir, lon_str, lon_dir, speed_knots, loc.bearing, date_str);

    return absl::StrFormat("$%s*%s\r\n", body, CalculateChecksum(body));
}
}  // namespace

// Executes all HalPlug callbacks, location updates, and the 1 Hz virtual-clock
// timer on `qemu_loop`.
class GpsDevice : public IGpsDevice, public std::enable_shared_from_this<GpsDevice> {
  public:
    explicit GpsDevice(EventLoop* qemu_loop) : loop_(qemu_loop) {
        DCHECK(loop_ != nullptr);
        VLOG(1) << "Gps device has been created";
    }

    void OnConnect() override {
        DCHECK(loop_->IsOnLoopThread());
        VLOG(1) << "Gps device has been connected";

        one_second_timer_ = loop_->ScheduleRepeating(
                [weak_self = weak_from_this()] {
                    if (const auto self = weak_self.lock()) {
                        self->OneSecondTick();
                        return true;
                    }
                    return false;
                },
                absl::Seconds(1), absl::Seconds(1));
    }

    void OnClose() override {
        DCHECK(loop_->IsOnLoopThread());
        VLOG(1) << "Gps device has been disconnected";

        location_update_subscription_.reset();
        one_second_timer_.reset();
    }

    void OnReceive(std::string_view data) override {
        DCHECK(loop_->IsOnLoopThread());
        VLOG(1) << "The guest is (unexpectedly) sending data to the Gps device: " << data;
    }

    void SetInitialLocation(const Location& location) { location_ = location; }

    void Send(const Location& location) {
        DCHECK(loop_->IsOnLoopThread());
        // Ignore location updates that were queued on `qemu_loop` before `OnClose()`.
        if (!location_update_subscription_) {
            return;
        }
        VLOG(1) << "Setting the location to:" << location;
        location_ = location;
        OneSecondTick();
    }

    void SetLocationUpdateSubscription(LocationUpdateSubscription s) {
        location_update_subscription_ = std::move(s);
    }

  private:
    void SendImpl(std::string_view msg) {
        auto encoded = qemud::EncodeQemudPacket(msg);
        VLOG(2) << "Sending " << encoded;
        Socket()->Send(std::move(encoded));
    }

    void OneSecondTick() {
        DCHECK(loop_->IsOnLoopThread());
        const absl::Time now = absl::Now();
        const std::string gpgga = FormatGPGGA(location_, now);
        const std::string gprmc = FormatGPRMC(location_, now);
        SendImpl(gpgga);
        SendImpl(gprmc);
    }

    LocationUpdateSubscription location_update_subscription_;
    EventLoop* const loop_;
    EventLoop::ScopedTimer one_second_timer_;
    Location location_{};
};

void IGpsDevice::RegisterDevice(ObservableLocation* observable_location,
                                IConnectorRegistry* registry, EventLoop* qemu_loop) {
    // Both `HalPlug` callbacks and the 1 Hz `QemuTimer` execute on `qemu_loop`
    // so timer creation and cancellation run synchronously on the loop thread.
    registry->RegisterHalQemuDevice(
            std::string(GpsDevice::kServiceName), qemu_loop, qemu_loop,
            [observable_location, qemu_loop](std::string_view /*args*/) {
                auto dev = std::make_shared<GpsDevice>(qemu_loop);
                std::weak_ptr<GpsDevice> weak_dev = dev;

                // Post location updates to `qemu_loop` before locking `weak_dev`
                // so `GpsDevice` state is only accessed on `qemu_loop` and
                // `weak_dev` is never locked while `ObservableLocation` holds
                // its internal callback mutex.
                auto location_update_subscription = MakeScopedCallback(
                        *observable_location,
                        [weak_dev = std::move(weak_dev), qemu_loop](const Location& location) {
                            qemu_loop
                                    ->Post([weak_dev, location] {
                                        if (const auto dev = weak_dev.lock()) {
                                            dev->Send(location);
                                        }
                                    })
                                    .IgnoreError();
                        });

                dev->SetInitialLocation(observable_location->GetValue());
                dev->SetLocationUpdateSubscription(std::move(location_update_subscription));

                return dev;
            });
}

}  // namespace goldfish::devices::gps
