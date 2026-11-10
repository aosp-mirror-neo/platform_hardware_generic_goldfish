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

#include "goldfish/devices/sensor/sensor_device.h"

#include <algorithm>
#include <chrono>
#include <cstdbool>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"

#include "android/goldfish/device_type.h"
#include "android/goldfish/hardware_config.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/devices/connector_registry.h"
#include "goldfish/devices/qemud/qemud.h"
#include "goldfish/sensors/android_sensor.h"

namespace goldfish::devices::sensor {

using goldfish::sensors::AndroidSensor;
using goldfish::sensors::PhysicalModel;
using goldfish::sensors::PhysicalParameter;
using goldfish::sensors::SensorData;
using goldfish::sensors::SensorValue;
using goldfish::sensors::vec3;
using goldfish::sensors::vec4;

namespace {  // Anonymous namespace for internal helpers

// Sensor information
struct SensorInfo {
    const std::string_view name;
    int id{0};
};

// Serialized sensor data
struct SerializedSensor {
    size_t measurement_id{0};
    char value[127];
    uint8_t length{0};
};

// Sensor data with enabled state
struct Sensor {
    bool enabled{false};
    SerializedSensor serialized;
};

/* a helper function that replaces commas (,) with points (.).
 * Each sensor string must be processed this way before being
 * sent into the guest. This is because the system locale may
 * cause decimal values to be formatted with a comma instead of
 * a decimal point, but that would not be parsed correctly
 * within the guest.
 */
void SanitizeSensorString(char* string, int maxlen) {
    for (int i = 0; i < maxlen && string[i] != '\0'; i++) {
        if (string[i] == ',') {
            string[i] = '.';
        }
    }
}

const char* GetSensorWireName(const AndroidSensor sensor_id) {
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) \
    case AndroidSensor::x:                 \
        return w;

    switch (sensor_id) {
        GOLDFISH_SENSORS_LIST

    case AndroidSensor::MAX_SENSORS:
        break;
    }
#undef GOLDFISH_SENSOR_DEF

    LOG(QFATAL) << "Unexpected sensor_id: " << static_cast<int>(sensor_id);
}

SerializedSensor SerializeSensorData(const AndroidSensor sensor_id, const SensorData& d) {
    const char* name = GetSensorWireName(sensor_id);

    SerializedSensor serialized;

    switch (d.value.size()) {
    case 1:
        serialized.length = ::snprintf(serialized.value, sizeof(serialized.value), "%s:%g:%zu",
                                       name, d.value[0], d.measurement_id);
        SanitizeSensorString(serialized.value, serialized.length);
        return serialized;

    case 3:
        serialized.length =
                ::snprintf(serialized.value, sizeof(serialized.value), "%s:%g:%g:%g:%zu", name,
                           d.value[0], d.value[1], d.value[2], d.measurement_id);
        SanitizeSensorString(serialized.value, serialized.length);
        return serialized;

    case 4:
        serialized.length =
                ::snprintf(serialized.value, sizeof(serialized.value), "%s:%g:%g:%g:%g:%zu", name,
                           d.value[0], d.value[1], d.value[2], d.value[3], d.measurement_id);
        SanitizeSensorString(serialized.value, serialized.length);
        return serialized;

    default:
        LOG(QFATAL) << "Unexpected SensorValue size: " << d.value.size();
    }
}

}  // anonymous namespace

class SensorDevice : public ISensorDevice {
  public:
    SensorDevice(PhysicalModel* pm, android::goldfish::DeviceType avd_type, int avd_api,
                 const android::goldfish::HardwareConfig& hw, EventLoop* event_loop,
                 const ::goldfish::system::VirtualClock& clock)
            : physical_model_(pm)
            , loop_(event_loop)
            , clock_(clock)
            , qemud_parser_([this](const void* data, size_t size) {
                return HandleMessage(std::string_view(static_cast<const char*>(data), size));
            }) {
        // Initialize sensors based on AVD configuration
        if (hw.hw_accelerometer) {
            sensors_[static_cast<size_t>(AndroidSensor::ACCELERATION)].enabled = true;
        }
        if (hw.hw_accelerometer_uncalibrated) {
            sensors_[static_cast<size_t>(AndroidSensor::ACCELERATION_UNCALIBRATED)].enabled = true;
        }
        if (hw.hw_gyroscope) {
            sensors_[static_cast<size_t>(AndroidSensor::GYROSCOPE)].enabled = true;
        }
        if (hw.hw_sensors_proximity) {
            sensors_[static_cast<size_t>(AndroidSensor::PROXIMITY)].enabled = true;
        }
        if (hw.hw_sensors_magnetic_field) {
            sensors_[static_cast<size_t>(AndroidSensor::MAGNETIC_FIELD)].enabled = true;
        }
        if (hw.hw_sensors_magnetic_field_uncalibrated) {
            sensors_[static_cast<size_t>(AndroidSensor::MAGNETIC_FIELD_UNCALIBRATED)].enabled =
                    true;
        }
        if (hw.hw_sensors_gyroscope_uncalibrated) {
            sensors_[static_cast<size_t>(AndroidSensor::GYROSCOPE_UNCALIBRATED)].enabled = true;
        }
        if (hw.hw_sensors_orientation) {
            sensors_[static_cast<size_t>(AndroidSensor::ORIENTATION)].enabled = true;
        }
        if (hw.hw_sensors_temperature) {
            sensors_[static_cast<size_t>(AndroidSensor::TEMPERATURE)].enabled = true;
        }
        if (hw.hw_sensors_light) {
            sensors_[static_cast<size_t>(AndroidSensor::LIGHT)].enabled = true;
        }
        if (hw.hw_sensors_pressure) {
            sensors_[static_cast<size_t>(AndroidSensor::PRESSURE)].enabled = true;
        }
        if (hw.hw_sensors_humidity) {
            sensors_[static_cast<size_t>(AndroidSensor::HUMIDITY)].enabled = true;
        }
        if (hw.hw_sensors_rgbclight) {
            sensors_[static_cast<size_t>(AndroidSensor::RGBC_LIGHT)].enabled = true;
        }
        if (hw.hw_sensor_hinge) {
            sensors_[static_cast<size_t>(AndroidSensor::HINGE_ANGLE0)].enabled = true;
            switch (hw.hw_sensor_hinge_count) {
            case 3:
                sensors_[static_cast<size_t>(AndroidSensor::HINGE_ANGLE2)].enabled = true;
            case 2:
                sensors_[static_cast<size_t>(AndroidSensor::HINGE_ANGLE1)].enabled = true;
            default:;
            }
        }

        const bool modern_wear_device =
                avd_type == android::goldfish::DeviceType::kWear && avd_api >= 28;

        if (hw.hw_sensors_heart_rate || modern_wear_device) {
            sensors_[static_cast<size_t>(AndroidSensor::HEART_RATE)].enabled = true;
        }

        if (hw.hw_sensors_wrist_tilt || modern_wear_device) {
            sensors_[static_cast<size_t>(AndroidSensor::WRIST_TILT)].enabled = true;
        }

        /*
         * TODO: move `SetPhysicalParameterValue` elsewhere so we are not overriding
         * the universe in the code which provides sensor values for the guest side.
         */

        constexpr float kPressure = 1013.25F;  // One "standard atmosphere"
        SetPhysicalParameterValue(PhysicalParameter::PRESSURE, &kPressure, 1U,
                                  PhysicalInterpolation::kSmooth);

        constexpr float kProximity = 1.F;
        SetPhysicalParameterValue(PhysicalParameter::PROXIMITY, &kProximity, 1U,
                                  PhysicalInterpolation::kStep);

        enabled_mask_ = 0;
        for (size_t nn = 0; nn < static_cast<size_t>(AndroidSensor::MAX_SENSORS); nn++) {
            if (sensors_[nn].enabled) {
                enabled_mask_ |= (1 << nn);
            }
        }
    }

    ~SensorDevice() override = default;

    void OnClose() override {
        VLOG(1) << "Bye bye! Sensors shutting down";
        timer_.reset();

        // Make sure we don't get destroyed while a timer is active.
        // By posting with a self reference we guarantee that we remain alive
        // until the timer has completed been cleaned up (b/443556478)
        if (self_) {
            loop_->Post([this] { self_.reset(); }).IgnoreError();
        }
    }

    void OnConnect() override {
        VLOG(1) << "Starting sensor ticks" << *this;
        this->self_ = shared_from_this();
        // Note, the timer will be scheduled after the guest requests it.
        timer_ = loop_->CreateTimer([this] {
            Tick();
            return true;
        });
    };

    void Send(std::string_view msg) {
        auto encoded = qemud::EncodeQemudPacket(msg);
        VLOG(2) << "Sending " << encoded;
        Socket()->Send(encoded);
    }

    /*
     * - when the qemu-specific sensors HAL module starts, it sends
     *   "list-sensors"
     *
     * - this code replies with a string containing an integer corresponding
     *   to a bitmap of available hardware sensors in the current AVD
     *   configuration (e.g. "1" a.k.a. (1 << ANDROID_SENSOR_ACCELERATION))
     *
     * - the HAL module sends "set:<sensor>:<flag>" to enable or disable
     *   the report of a given sensor state. <sensor> must be the name of
     *   a given sensor (e.g. "accelerometer"), and <flag> must be either
     *   "1" (to enable) or "0" (to disable).
     *
     * - Once at least one sensor is "enabled", this code should periodically
     *   send information about the corresponding enabled sensors. The default
     *   period is 200ms.
     *
     * - the HAL module sends "set-delay:<delay>", where <delay> is an integer
     *   corresponding to a time delay in milli-seconds. This corresponds to
     *   a new interval between sensor events sent by this code to the HAL
     *   module.
     *
     * - the HAL module can also send a "wake" command. This code should simply
     *   send the "wake" back to the module. This is used internally to wake a
     *   blocking read that happens in a different thread. This ping-pong makes
     *   the code in the HAL module very simple.
     *
     * - each timer Tick, this code sends sensor reports in the following
     *   format (each line corresponds to a different line sent to the module):
     *
     *      acceleration:<x>:<y>:<z>
     *      magnetic-field:<x>:<y>:<z>
     *      orientation:<azimuth>:<pitch>:<roll>
     *      temperature:<celsius>
     *      light:<lux>
     *      pressure:<hpa>
     *      humidity:<percent>
     *      sync:<time_us>
     *
     *   Where each line before the sync:<time_us> is optional and will only
     *   appear if the corresponding sensor has been enabled by the HAL module.
     *
     *   Note that <time_us> is the VM time in micro-seconds when the report
     *   was "taken" by this code. This is adjusted by the HAL module to
     *   emulated system time (using the first sync: to compute an adjustment
     *   offset).
     */
    void OnReceive(std::string_view data) override {
        qemud_parser_.OnReceive(data.data(), data.size());
    }

    bool HandleMessage(std::string_view msg) {
        DCHECK(timer_)
                << "onReceive must have been called before onConnected was called, this "
                   "means we are operating on an unconnected socket, and the guest will not "
                   "receive the expected response! Logcat will likely show a crashed sensor hal.";

        VLOG(2) << "Received message from sensor HAL: " << msg;
        if (msg == "list-sensors") {
            const std::string response = std::to_string(enabled_mask_);
            Send(response);
            return true;
        }

        if (msg == "wake") {
            Send("wake");
            return true;
        }

        if (absl::ConsumePrefix(&msg, "set-delay:")) {
            int32_t delay_ms;
            if (absl::SimpleAtoi(msg, &delay_ms)) {
                delay_ = absl::Milliseconds(delay_ms);
                if (enabled_mask_ != 0) {
                    // Trigger a Tick to apply the new delay immediately.
                    Tick();
                }
                return true;
            }
            VLOG(1) << "Ignoring 'set-delay' command with invalid delay value: '" << msg << "'";
            return true;
        }

        if (absl::ConsumePrefix(&msg, "set:")) {
            std::vector<std::string_view> parts = absl::StrSplit(msg, ':');
            if (parts.size() != 2) {
                VLOG(1) << "Ignoring malformed 'set' command. Expected format "
                        << "'set:<sensor>:<0|1>', but received: 'set:" << msg << "'";
                return true;
            }

            const int id = SensorIdFromName(parts[0]);
            if (id < 0 || id >= static_cast<int>(AndroidSensor::MAX_SENSORS)) {
                VLOG(1) << "Ignoring 'set' command for unknown sensor: '" << parts[0] << "'";
                return true;
            }

            if (!sensors_[id].enabled) {
                VLOG(1) << "Ignoring 'set' command for sensor '" << parts[0]
                        << "' which is not enabled by AVD configuration.";
                return true;
            }

            const bool enabled = (parts[1] == "1");
            if (enabled) {
                enabled_mask_ |= (1 << id);
            } else {
                enabled_mask_ &= ~(1 << id);
            }

            // Trigger a Tick to apply the new mask configuration immediately.
            Tick();
            return true;
        }

        if (absl::ConsumePrefix(&msg, "time:")) {
            int64_t guest_time_ns;
            if (absl::SimpleAtoi(msg, &guest_time_ns)) {
                const absl::Duration now = clock_.Now();
                time_offset_ = absl::Nanoseconds(guest_time_ns) - now;
                return true;
            }
            VLOG(1) << "Ignoring 'time' command with invalid timestamp value: '" << msg << "'";
            return true;
        }

        VLOG(1) << "Ignoring unknown command from sensor HAL: " << msg;
        return true;
    }

  protected:
    void AbslStringifyImpl(absl::FormatSink& s) const override {
        absl::Format(&s, "[SensorDevice socket=%v]", *Socket());
    }

  private:
    // Helper functions to get sensor/parameter ID from name
    static int SensorIdFromName(std::string_view name) {
        for (int i = 0; i < static_cast<int>(AndroidSensor::MAX_SENSORS); i++) {
            if (kSensors[i].name == name) {
                return i;
            }
        }
        return -1;
    }

    void SetPhysicalParameterValue(PhysicalParameter parameter, const float* val,
                                   const size_t count, PhysicalInterpolation interpolation_mode) {
        physical_model_->SetPhysicalParameterValue(parameter, val, count, interpolation_mode);
    }

    bool Enabled(size_t sensor_id) const { return (enabled_mask_ & (1U << sensor_id)) != 0; }

    void Tick() {
        // Grab the guest time before sending any sensor data:
        // the android.hardware CTS requires sync times to be no greater than the
        // time of the sensor event arrival. Since the CTS enforces this property,
        // other code may also rely on it.
        DCHECK(loop_->IsOnLoopThread()) << "Tick must be called from the event loop!";
        const absl::Duration now = clock_.Now();
        physical_model_->SetCurrentTime(absl::ToInt64Nanoseconds(now));
        for (size_t sensor_id = 0; sensor_id < static_cast<size_t>(AndroidSensor::MAX_SENSORS);
             ++sensor_id) {
            if (!Enabled(sensor_id)) {
                continue;
            }

            const SensorData d =
                    physical_model_->GetSensorData(static_cast<AndroidSensor>(sensor_id));
            Sensor& s = sensors_[sensor_id];

            s.serialized = SerializeSensorData(static_cast<AndroidSensor>(sensor_id), d);
            Send(std::string_view(s.serialized.value, s.serialized.length));
        }

        Send(absl::StrFormat("guest-sync:%d", absl::ToInt64Nanoseconds(now + time_offset_)));
        Send(absl::StrFormat("sync:%d", absl::ToInt64Microseconds(now)));

        if (enabled_mask_ == 0) return;

        // Rearm the timer to fire a little bit early, so we can sustain the
        // requested frequency. Also make sure we have at least a minimal delay,
        // otherwise this timer would hijack the main loop thread and won't allow
        // guest to ever run.
        // Note: While there is some overhead in this code, it is signifcantly less
        //  than 1ms. Just delay by exactly (delay_ms) below to keep the actual rate
        //  as close to the desired rate as possible.
        // Note2: Let's cap the minimal Tick interval to 10ms, to make sure:
        // - We never overload the main QEMU loop.
        // - Some CTS hardware test cases require a limit on the maximum update
        // rate,
        //   which has been known to be in the low 100's of Hz.
        delay_ = std::clamp(delay_, absl::Milliseconds(10), absl::Hours(1));

        DCHECK(self_) << "Self reference should have been set, otherwise we are scheduling a "
                         "callback where we can disappear from (i.e. Tick could be called with "
                         "this == nullptr)!";
        timer_->Schedule(delay_, delay_);
    }

    PhysicalModel* const physical_model_;
    EventLoop* const loop_;
    const ::goldfish::system::VirtualClock& clock_;
    qemud::Parser qemud_parser_;
    EventLoop::ScopedTimer timer_;
    absl::Duration time_offset_;
    absl::Duration delay_{absl::Milliseconds(800)};

    // We are having callbacks in a timer, we want to make sure we never
    // delete ourselves.
    std::shared_ptr<ISensorDevice> self_;

    Sensor sensors_[static_cast<size_t>(AndroidSensor::MAX_SENSORS)];
    uint32_t enabled_mask_{0};

    // Sensor and Physical Parameter information arrays
    static constexpr SensorInfo kSensors[static_cast<size_t>(AndroidSensor::MAX_SENSORS)] = {
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) {y, static_cast<int>(AndroidSensor::x)},
        GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF
    };
};

void ISensorDevice::RegisterDevice(PhysicalModel* pm, IConnectorRegistry* registry,
                                   android::goldfish::DeviceType avd_type, int avd_api,
                                   const android::goldfish::HardwareConfig& hw,
                                   EventLoop* client_loop, EventLoop* qemu_loop,
                                   const ::goldfish::system::VirtualClock& clock) {
    registry->RegisterHalQemuDevice(
            std::string(ISensorDevice::kServiceName), client_loop, qemu_loop,
            [pm, avd_type, avd_api, &hw, client_loop, &clock](std::string_view /*args*/) {
                return std::make_shared<SensorDevice>(pm, avd_type, avd_api, hw, client_loop,
                                                      clock);
            });
}

}  // namespace goldfish::devices::sensor
