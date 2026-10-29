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

#include "environment/sensors_service_impl.h"

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/grpc_event_stream_support.h"

namespace goldfish::grpc::v2 {

using ::android::emulation::v2::AmbientEnvironment;
using ::android::emulation::v2::DevicePose;
using ::android::emulation::v2::GetPostureStateRequest;
using ::android::emulation::v2::GetSensorsStateRequest;
using ::android::emulation::v2::HingeState;
using ::android::emulation::v2::ImuTelemetry;
using ::android::emulation::v2::PostureState;
using ::android::emulation::v2::PostureType;
using ::android::emulation::v2::RawSensorOverrides;
using ::android::emulation::v2::RgbcLight;
using ::android::emulation::v2::RollableState;
using ::android::emulation::v2::SensorsState;
using ::android::emulation::v2::SensorType;
using ::android::emulation::v2::StreamPostureStateRequest;
using ::android::emulation::v2::StreamSensorsStateRequest;
using ::android::emulation::v2::StreamSensorsUpdatesResponse;
using ::android::emulation::v2::TransitionMode;
using ::android::emulation::v2::UpdatePostureStateRequest;
using ::android::emulation::v2::UpdateSensorsStateRequest;
using ::android::emulation::v2::Vector3f;
using ::goldfish::sensors::AndroidSensor;
using ::goldfish::sensors::PhysicalModel;
using ::goldfish::sensors::PhysicalModelChangeEvent;

namespace {

void PopulateVector3f(const glm::vec3& src, Vector3f* dst) {
    dst->set_x(src.x);
    dst->set_y(src.y);
    dst->set_z(src.z);
}

glm::vec3 ToVec3(const Vector3f& src) {
    return glm::vec3(src.x(), src.y(), src.z());
}

bool IsFiniteVec3(const Vector3f& src) {
    return std::isfinite(src.x()) && std::isfinite(src.y()) && std::isfinite(src.z());
}

bool IsFiniteNonNegative(float val) {
    return std::isfinite(val) && val >= 0.0F;
}

bool IsFiniteInRange(float val, float min_val, float max_val) {
    return std::isfinite(val) && val >= min_val && val <= max_val;
}

::grpc::Status FoldableUnsupportedStatus() {
    return {::grpc::StatusCode::FAILED_PRECONDITION,
            "Device does not support foldable or rollable postures."};
}

bool IsNonChangingStateEvent(const PhysicalModelChangeEvent& event) {
    return event.type != PhysicalModelChangeEvent::Type::kPhysicalStateChanging;
}

using ParamGetter = float (PhysicalModel::*)(::ParameterValueType) const;
using ParamSetter = void (PhysicalModel::*)(float, ::PhysicalInterpolation);

constexpr std::array<ParamGetter, ::goldfish::sensors::kMaxHinges> kHingeGetters = {
    &PhysicalModel::GetParameterHingeAngle0,
    &PhysicalModel::GetParameterHingeAngle1,
    &PhysicalModel::GetParameterHingeAngle2,
};

constexpr std::array<ParamSetter, ::goldfish::sensors::kMaxHinges> kHingeSetters = {
    &PhysicalModel::SetTargetHingeAngle0,
    &PhysicalModel::SetTargetHingeAngle1,
    &PhysicalModel::SetTargetHingeAngle2,
};

constexpr std::array<ParamGetter, ::goldfish::sensors::kMaxRolls> kRollableGetters = {
    &PhysicalModel::GetParameterRollable0,
    &PhysicalModel::GetParameterRollable1,
};

constexpr std::array<ParamSetter, ::goldfish::sensors::kMaxRolls> kRollableSetters = {
    &PhysicalModel::SetTargetRollable0,
    &PhysicalModel::SetTargetRollable1,
};

::PhysicalInterpolation ToPhysicalInterpolation(TransitionMode mode) {
    switch (mode) {
    case TransitionMode::TRANSITION_MODE_STEP:
        return ::PhysicalInterpolation::kStep;
    case TransitionMode::TRANSITION_MODE_SMOOTH:
    case TransitionMode::TRANSITION_MODE_UNSPECIFIED:
    default:
        return ::PhysicalInterpolation::kSmooth;
    }
}

PostureType FromFoldablePosture(::goldfish::sensors::FoldablePostures posture) {
    switch (posture) {
    case ::goldfish::sensors::FoldablePostures::kClosed:
        return PostureType::POSTURE_TYPE_CLOSED;
    case ::goldfish::sensors::FoldablePostures::kHalfOpened:
        return PostureType::POSTURE_TYPE_HALF_OPENED;
    case ::goldfish::sensors::FoldablePostures::kOpened:
        return PostureType::POSTURE_TYPE_OPENED;
    case ::goldfish::sensors::FoldablePostures::kFlipped:
        return PostureType::POSTURE_TYPE_FLIPPED;
    case ::goldfish::sensors::FoldablePostures::kTent:
        return PostureType::POSTURE_TYPE_TENT;
    default:
        return PostureType::POSTURE_TYPE_UNSPECIFIED;
    }
}

absl::StatusOr<::goldfish::sensors::FoldablePostures> ToFoldablePosture(PostureType posture) {
    switch (posture) {
    case PostureType::POSTURE_TYPE_CLOSED:
        return ::goldfish::sensors::FoldablePostures::kClosed;
    case PostureType::POSTURE_TYPE_HALF_OPENED:
        return ::goldfish::sensors::FoldablePostures::kHalfOpened;
    case PostureType::POSTURE_TYPE_OPENED:
        return ::goldfish::sensors::FoldablePostures::kOpened;
    case PostureType::POSTURE_TYPE_FLIPPED:
        return ::goldfish::sensors::FoldablePostures::kFlipped;
    case PostureType::POSTURE_TYPE_TENT:
        return ::goldfish::sensors::FoldablePostures::kTent;
    default:
        return absl::InvalidArgumentError("Unsupported or unspecified PostureType.");
    }
}

constexpr std::array<std::pair<AndroidSensor, SensorType>, 7> kImuOverrideSensors{{
    {AndroidSensor::ACCELERATION, SensorType::SENSOR_TYPE_ACCELEROMETER},
    {AndroidSensor::GYROSCOPE, SensorType::SENSOR_TYPE_GYROSCOPE},
    {AndroidSensor::MAGNETIC_FIELD, SensorType::SENSOR_TYPE_MAGNETOMETER},
    {AndroidSensor::ORIENTATION, SensorType::SENSOR_TYPE_ORIENTATION},
    {AndroidSensor::ACCELERATION_UNCALIBRATED, SensorType::SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED},
    {AndroidSensor::GYROSCOPE_UNCALIBRATED, SensorType::SENSOR_TYPE_GYROSCOPE_UNCALIBRATED},
    {AndroidSensor::MAGNETIC_FIELD_UNCALIBRATED, SensorType::SENSOR_TYPE_MAGNETOMETER_UNCALIBRATED},
}};

}  // namespace

SensorsServiceImpl::SensorsServiceImpl(PhysicalModel& physical_model,
                                       const ::goldfish::system::SteadyClock& clock)
        : physical_model_(physical_model), clock_(clock) {}

SensorsState SensorsServiceImpl::BuildSensorsState() const {
    SensorsState state;
    state.set_timestamp_nanos(absl::ToInt64Nanoseconds(clock_.Now()));

    auto* pose = state.mutable_pose();
    PopulateVector3f(physical_model_.GetParameterRotation(::ParameterValueType::kCurrent),
                     pose->mutable_rotation_degrees());
    PopulateVector3f(physical_model_.GetParameterPosition(::ParameterValueType::kCurrent),
                     pose->mutable_position_meters());
    PopulateVector3f(physical_model_.GetParameterVelocity(::ParameterValueType::kCurrent),
                     pose->mutable_velocity_meters_per_second());
    pose->set_ambient_motion(
            physical_model_.GetParameterAmbientMotion(::ParameterValueType::kCurrent));

    auto* env = state.mutable_environment();
    env->set_light_lux(physical_model_.GetParameterLight(::ParameterValueType::kCurrent));
    const glm::vec4 rgbc = physical_model_.GetParameterRgbcLight(::ParameterValueType::kCurrent);
    auto* rgbc_msg = env->mutable_rgbc_light();
    rgbc_msg->set_red(rgbc.r);
    rgbc_msg->set_green(rgbc.g);
    rgbc_msg->set_blue(rgbc.b);
    rgbc_msg->set_clear(rgbc.a);
    env->set_proximity_centimeters(
            physical_model_.GetParameterProximity(::ParameterValueType::kCurrent));
    env->set_ambient_temperature_celsius(
            physical_model_.GetParameterTemperature(::ParameterValueType::kCurrent));
    env->set_pressure_hpa(physical_model_.GetParameterPressure(::ParameterValueType::kCurrent));
    env->set_relative_humidity_percent(
            physical_model_.GetParameterHumidity(::ParameterValueType::kCurrent));
    PopulateVector3f(physical_model_.GetParameterMagneticField(::ParameterValueType::kCurrent),
                     env->mutable_magnetic_field_ut());
    env->set_heart_rate_bpm(physical_model_.GetParameterHeartRate(::ParameterValueType::kCurrent));
    env->set_wrist_tilt(physical_model_.GetParameterWristTilt(::ParameterValueType::kCurrent));

    size_t measurement_id = 0;
    auto* imu = state.mutable_imu();
    PopulateVector3f(physical_model_.GetAccelerometer(&measurement_id),
                     imu->mutable_accelerometer());
    PopulateVector3f(physical_model_.GetGyroscope(&measurement_id), imu->mutable_gyroscope());
    PopulateVector3f(physical_model_.GetMagnetometer(&measurement_id), imu->mutable_magnetometer());
    PopulateVector3f(glm::degrees(physical_model_.GetOrientation(&measurement_id)),
                     imu->mutable_orientation_degrees());
    PopulateVector3f(physical_model_.GetAccelerometerUncalibrated(&measurement_id),
                     imu->mutable_accelerometer_uncalibrated());
    PopulateVector3f(physical_model_.GetGyroscopeUncalibrated(&measurement_id),
                     imu->mutable_gyroscope_uncalibrated());
    PopulateVector3f(physical_model_.GetMagnetometerUncalibrated(&measurement_id),
                     imu->mutable_magnetometer_uncalibrated());

    for (const auto& [android_sensor, proto_sensor] : kImuOverrideSensors) {
        if (physical_model_.IsSensorOverridden(android_sensor)) {
            imu->add_active_overrides(proto_sensor);
        }
    }

    return state;
}

PostureState SensorsServiceImpl::BuildPostureState() const {
    PostureState state;
    state.set_timestamp_nanos(absl::ToInt64Nanoseconds(clock_.Now()));
    if (!physical_model_.HasFoldableModel()) {
        return state;
    }

    state.set_posture(FromFoldablePosture(physical_model_.GetFoldablePosture()));

    const auto* config = physical_model_.GetFoldableConfig();
    const unsigned num_hinges = std::min<unsigned>(config != nullptr ? config->num_hinges : 1,
                                                   static_cast<unsigned>(kHingeGetters.size()));
    for (unsigned id = 0; id < num_hinges; ++id) {
        auto* hinge = state.add_hinges();
        hinge->set_hinge_id(id);
        hinge->set_angle_degrees(
                (physical_model_.*kHingeGetters[id])(::ParameterValueType::kCurrent));
    }

    const unsigned num_rolls = std::min<unsigned>(config != nullptr ? config->num_rolls : 0,
                                                  static_cast<unsigned>(kRollableGetters.size()));
    for (unsigned id = 0; id < num_rolls; ++id) {
        auto* rollable = state.add_rollables();
        rollable->set_rollable_id(id);
        rollable->set_extension_percent(
                (physical_model_.*kRollableGetters[id])(::ParameterValueType::kCurrent));
    }

    return state;
}

absl::Status SensorsServiceImpl::ApplySensorsUpdate(const UpdateSensorsStateRequest& request) {
    // Validate ranges and finiteness before mutating state.
    if (request.has_pose()) {
        const auto& pose = request.pose();
        if (pose.has_rotation_degrees() && !IsFiniteVec3(pose.rotation_degrees())) {
            return absl::InvalidArgumentError("rotation_degrees must be finite.");
        }
        if (pose.has_position_meters() && !IsFiniteVec3(pose.position_meters())) {
            return absl::InvalidArgumentError("position_meters must be finite.");
        }
        if (pose.has_velocity_meters_per_second() &&
            !IsFiniteVec3(pose.velocity_meters_per_second())) {
            return absl::InvalidArgumentError("velocity_meters_per_second must be finite.");
        }
        if (pose.has_ambient_motion() && !IsFiniteNonNegative(pose.ambient_motion())) {
            return absl::InvalidArgumentError("ambient_motion must be finite and >= 0.0.");
        }
    }
    if (request.has_environment()) {
        const auto& env = request.environment();
        if (env.has_light_lux() && !IsFiniteNonNegative(env.light_lux())) {
            return absl::InvalidArgumentError("light_lux must be finite and >= 0.0.");
        }
        if (env.has_rgbc_light()) {
            const auto& c = env.rgbc_light();
            if (!IsFiniteNonNegative(c.red()) || !IsFiniteNonNegative(c.green()) ||
                !IsFiniteNonNegative(c.blue()) || !IsFiniteNonNegative(c.clear())) {
                return absl::InvalidArgumentError("rgbc_light channels must be finite and >= 0.0.");
            }
        }
        if (env.has_proximity_centimeters() && !IsFiniteNonNegative(env.proximity_centimeters())) {
            return absl::InvalidArgumentError("proximity_centimeters must be finite and >= 0.0.");
        }
        if (env.has_ambient_temperature_celsius() &&
            !std::isfinite(env.ambient_temperature_celsius())) {
            return absl::InvalidArgumentError("ambient_temperature_celsius must be finite.");
        }
        if (env.has_pressure_hpa() && !IsFiniteNonNegative(env.pressure_hpa())) {
            return absl::InvalidArgumentError("pressure_hpa must be finite and >= 0.0.");
        }
        if (env.has_relative_humidity_percent() &&
            !IsFiniteInRange(env.relative_humidity_percent(), 0.0F, 100.0F)) {
            return absl::InvalidArgumentError(
                    "relative_humidity_percent must be finite and within [0.0, 100.0].");
        }
        if (env.has_magnetic_field_ut() && !IsFiniteVec3(env.magnetic_field_ut())) {
            return absl::InvalidArgumentError("magnetic_field_ut must be finite.");
        }
        if (env.has_heart_rate_bpm() && !IsFiniteNonNegative(env.heart_rate_bpm())) {
            return absl::InvalidArgumentError("heart_rate_bpm must be finite and >= 0.0.");
        }
        if (env.has_wrist_tilt() && !IsFiniteInRange(env.wrist_tilt(), 0.0F, 1.0F)) {
            return absl::InvalidArgumentError("wrist_tilt must be finite and within [0.0, 1.0].");
        }
    }
    if (request.has_raw_overrides()) {
        const auto& raw = request.raw_overrides();
        if ((raw.has_accelerometer() && !IsFiniteVec3(raw.accelerometer())) ||
            (raw.has_gyroscope() && !IsFiniteVec3(raw.gyroscope())) ||
            (raw.has_magnetometer() && !IsFiniteVec3(raw.magnetometer())) ||
            (raw.has_orientation_degrees() && !IsFiniteVec3(raw.orientation_degrees())) ||
            (raw.has_accelerometer_uncalibrated() &&
             !IsFiniteVec3(raw.accelerometer_uncalibrated())) ||
            (raw.has_gyroscope_uncalibrated() && !IsFiniteVec3(raw.gyroscope_uncalibrated())) ||
            (raw.has_magnetometer_uncalibrated() &&
             !IsFiniteVec3(raw.magnetometer_uncalibrated()))) {
            return absl::InvalidArgumentError("raw_overrides values must be finite.");
        }
    }

    const ::PhysicalInterpolation mode = ToPhysicalInterpolation(request.transition());

    if (request.clear_raw_overrides()) {
        physical_model_.ClearOverrides();
    }

    if (request.has_pose()) {
        const auto& pose = request.pose();
        if (pose.has_rotation_degrees()) {
            physical_model_.SetTargetRotation(ToVec3(pose.rotation_degrees()), mode);
        }
        if (pose.has_position_meters()) {
            physical_model_.SetTargetPosition(ToVec3(pose.position_meters()), mode);
        }
        if (pose.has_velocity_meters_per_second()) {
            physical_model_.SetTargetVelocity(ToVec3(pose.velocity_meters_per_second()), mode);
        }
        if (pose.has_ambient_motion()) {
            physical_model_.SetTargetAmbientMotion(pose.ambient_motion(), mode);
        }
    }

    if (request.has_environment()) {
        const auto& env = request.environment();
        if (env.has_light_lux()) {
            physical_model_.SetTargetLight(env.light_lux(), mode);
        }
        if (env.has_rgbc_light()) {
            const auto& c = env.rgbc_light();
            physical_model_.SetTargetRgbcLight(glm::vec4(c.red(), c.green(), c.blue(), c.clear()),
                                               mode);
        }
        if (env.has_proximity_centimeters()) {
            physical_model_.SetTargetProximity(env.proximity_centimeters(), mode);
        }
        if (env.has_ambient_temperature_celsius()) {
            physical_model_.SetTargetTemperature(env.ambient_temperature_celsius(), mode);
        }
        if (env.has_pressure_hpa()) {
            physical_model_.SetTargetPressure(env.pressure_hpa(), mode);
        }
        if (env.has_relative_humidity_percent()) {
            physical_model_.SetTargetHumidity(env.relative_humidity_percent(), mode);
        }
        if (env.has_magnetic_field_ut()) {
            physical_model_.SetTargetMagneticField(ToVec3(env.magnetic_field_ut()), mode);
        }
        if (env.has_heart_rate_bpm()) {
            physical_model_.SetTargetHeartRate(env.heart_rate_bpm(), mode);
        }
        if (env.has_wrist_tilt()) {
            physical_model_.SetTargetWristTilt(env.wrist_tilt(), mode);
        }
    }

    if (request.has_raw_overrides()) {
        const auto& raw = request.raw_overrides();
        if (raw.has_accelerometer()) {
            physical_model_.OverrideAccelerometer(ToVec3(raw.accelerometer()));
        }
        if (raw.has_gyroscope()) {
            physical_model_.OverrideGyroscope(ToVec3(raw.gyroscope()));
        }
        if (raw.has_magnetometer()) {
            physical_model_.OverrideMagnetometer(ToVec3(raw.magnetometer()));
        }
        if (raw.has_orientation_degrees()) {
            physical_model_.OverrideOrientation(glm::radians(ToVec3(raw.orientation_degrees())));
        }
        if (raw.has_accelerometer_uncalibrated()) {
            physical_model_.OverrideAccelerometerUncalibrated(
                    ToVec3(raw.accelerometer_uncalibrated()));
        }
        if (raw.has_gyroscope_uncalibrated()) {
            physical_model_.OverrideGyroscopeUncalibrated(ToVec3(raw.gyroscope_uncalibrated()));
        }
        if (raw.has_magnetometer_uncalibrated()) {
            physical_model_.OverrideMagnetometerUncalibrated(
                    ToVec3(raw.magnetometer_uncalibrated()));
        }
    }

    return absl::OkStatus();
}

::grpc::Status SensorsServiceImpl::GetSensorsState(::grpc::ServerContext* /*context*/,
                                                   const GetSensorsStateRequest* /*request*/,
                                                   SensorsState* response) {
    *response = BuildSensorsState();
    return ::grpc::Status::OK;
}

::grpc::ServerWriteReactor<SensorsState>* SensorsServiceImpl::StreamSensorsState(
        ::grpc::CallbackServerContext* /*context*/, const StreamSensorsStateRequest* /*request*/) {
    return new ::android::emulation::control::StateStreamWriter<SensorsState,
                                                                PhysicalModelChangeEvent>(
            &physical_model_, [this](SensorsState* state) { *state = BuildSensorsState(); },
            IsNonChangingStateEvent);
}

::grpc::Status SensorsServiceImpl::UpdateSensorsState(::grpc::ServerContext* /*context*/,
                                                      const UpdateSensorsStateRequest* request,
                                                      SensorsState* response) {
    if (auto status = ApplySensorsUpdate(*request); !status.ok()) {
        return ::android::emulation::control::AbslStatusToGrpcStatus(status);
    }
    *response = BuildSensorsState();
    return ::grpc::Status::OK;
}

::grpc::ServerReadReactor<UpdateSensorsStateRequest>* SensorsServiceImpl::StreamSensorsUpdates(
        ::grpc::CallbackServerContext* /*context*/, StreamSensorsUpdatesResponse* /*response*/) {
    return new ::SimpleServerLambdaReader<UpdateSensorsStateRequest>(
            [this](const UpdateSensorsStateRequest* request) {
                if (auto status = ApplySensorsUpdate(*request); !status.ok()) {
                    return ::android::emulation::control::AbslStatusToGrpcStatus(status);
                }
                return ::grpc::Status::OK;
            });
}

::grpc::Status SensorsServiceImpl::GetPostureState(::grpc::ServerContext* /*context*/,
                                                   const GetPostureStateRequest* /*request*/,
                                                   PostureState* response) {
    if (!physical_model_.HasFoldableModel()) {
        return FoldableUnsupportedStatus();
    }
    *response = BuildPostureState();
    return ::grpc::Status::OK;
}

::grpc::ServerWriteReactor<PostureState>* SensorsServiceImpl::StreamPostureState(
        ::grpc::CallbackServerContext* /*context*/, const StreamPostureStateRequest* /*request*/) {
    if (!physical_model_.HasFoldableModel()) {
        return new ::ErrorServerWriter<PostureState>(FoldableUnsupportedStatus());
    }
    return new ::android::emulation::control::StateStreamWriter<PostureState,
                                                                PhysicalModelChangeEvent>(
            &physical_model_, [this](PostureState* state) { *state = BuildPostureState(); },
            IsNonChangingStateEvent);
}

::grpc::Status SensorsServiceImpl::UpdatePostureState(::grpc::ServerContext* /*context*/,
                                                      const UpdatePostureStateRequest* request,
                                                      PostureState* response) {
    if (!physical_model_.HasFoldableModel()) {
        return FoldableUnsupportedStatus();
    }

    const auto* config = physical_model_.GetFoldableConfig();
    const unsigned num_hinges = config != nullptr ? config->num_hinges : 0;
    const unsigned num_rolls = config != nullptr ? config->num_rolls : 0;

    // Validate posture, hinges, and rollables before mutating PhysicalModel.
    std::optional<::goldfish::sensors::FoldablePostures> target_posture;
    if (request->has_posture()) {
        auto posture_or = ToFoldablePosture(request->posture());
        if (!posture_or.ok()) {
            return ::android::emulation::control::AbslStatusToGrpcStatus(posture_or.status());
        }
        target_posture = *posture_or;
    }

    for (const auto& hinge : request->hinges()) {
        if (!IsFiniteInRange(hinge.angle_degrees(), 0.0F, 360.0F)) {
            return {::grpc::StatusCode::INVALID_ARGUMENT,
                    "Hinge angle_degrees must be finite and within [0.0, 360.0]."};
        }
        if (hinge.hinge_id() >= num_hinges || hinge.hinge_id() >= kHingeSetters.size()) {
            return {::grpc::StatusCode::INVALID_ARGUMENT, "Unsupported hinge_id."};
        }
    }

    for (const auto& rollable : request->rollables()) {
        if (!IsFiniteInRange(rollable.extension_percent(), 0.0F, 100.0F)) {
            return {::grpc::StatusCode::INVALID_ARGUMENT,
                    "Rollable extension_percent must be finite and within [0.0, 100.0]."};
        }
        if (rollable.rollable_id() >= num_rolls ||
            rollable.rollable_id() >= kRollableSetters.size()) {
            return {::grpc::StatusCode::INVALID_ARGUMENT, "Unsupported rollable_id."};
        }
    }

    const ::PhysicalInterpolation mode = ToPhysicalInterpolation(request->transition());

    if (target_posture.has_value()) {
        physical_model_.SetTargetPosture(static_cast<float>(*target_posture), mode);
    }

    for (const auto& hinge : request->hinges()) {
        (physical_model_.*kHingeSetters[hinge.hinge_id()])(hinge.angle_degrees(), mode);
    }

    for (const auto& rollable : request->rollables()) {
        (physical_model_.*kRollableSetters[rollable.rollable_id()])(rollable.extension_percent(),
                                                                    mode);
    }

    *response = BuildPostureState();
    return ::grpc::Status::OK;
}

}  // namespace goldfish::grpc::v2
