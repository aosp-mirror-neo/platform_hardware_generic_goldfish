/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "goldfish/sensors/physical_model.h"

#include <array>
#include <cmath>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include "android/goldfish/hardware_config.h"
#include "android/status/status_macros.h"
#include "goldfish/physics/ambient_environment.h"
#include "goldfish/physics/body_model.h"
#include "goldfish/physics/glm_helpers.h"
#include "goldfish/physics/inertial_model.h"
#include "goldfish/sensors/foldable_model.h"

using goldfish::physics::AmbientState;
using goldfish::physics::BodyState;
using goldfish::physics::InertialState;

namespace goldfish::sensors {
namespace {
void GetValues(const vec3& value, float* out, const size_t count) {
    if (count > 0) out[0] = value.x;
    if (count > 1) out[1] = value.y;
    if (count > 2) out[2] = value.z;
}

void GetValues(const vec4& value, float* out, const size_t count) {
    if (count > 0) out[0] = value.x;
    if (count > 1) out[1] = value.y;
    if (count > 2) out[2] = value.z;
    if (count > 3) out[3] = value.w;
}

void GetValues(const float value, float* out, const size_t count) {
    if (count > 0) out[0] = value;
}

vec3 Getvec3Value(const float* val, const size_t count) {
    return vec3{count > 0 ? val[0] : 0, count > 1 ? val[1] : 0, count > 2 ? val[2] : 0};
}

vec4 Getvec4Value(const float* val, const size_t count) {
    return vec4{count > 0 ? val[0] : 0, count > 1 ? val[1] : 0, count > 2 ? val[2] : 0,
                count > 3 ? val[3] : 0};
}

float GetfloatValue(const float* val, const size_t count) {
    return count > 0 ? val[0] : 0;
}

std::unique_ptr<FoldableModel> HandleFoldableModelError(
        absl::StatusOr<std::unique_ptr<FoldableModel>> fm) {
    if (fm.ok()) {
        return *std::move(fm);
    } else {
        LOG(ERROR) << "Could not create a foldable model: " << fm.status();
        return {};
    }
}

}  // namespace

bool PhysicalModel::HasFoldableModel() const {
    return static_cast<bool>(foldable_model_);
}

const FoldableConfig* PhysicalModel::GetFoldableConfig() const {
    return foldable_model_ ? &foldable_model_->GetFoldableConfig() : nullptr;
}

bool PhysicalModel::GetFoldedArea(int* x, int* y, int* w, int* h) const {
    return foldable_model_ && foldable_model_->GetFoldedArea(x, y, w, h);
}

FoldableModel::ObservablePosture* PhysicalModel::GetPostureListener() {
    return foldable_model_ ? &foldable_model_->GetPostureListener() : nullptr;
}

bool PhysicalModel::FoldableIsFolded() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foldable_model_ && foldable_model_->IsFolded();
}

FoldablePostures PhysicalModel::GetFoldablePosture() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foldable_model_ ? foldable_model_->GetFoldablePosture() : FoldablePostures::kUnknown;
}

PhysicalModel::PhysicalModel(const android::goldfish::HardwareConfig& hw,
                             const ::goldfish::system::VirtualClock& virtual_clock)
        : foldable_model_(HandleFoldableModelError(FoldableModel::Create(hw)))
        , virtual_scene_camera_(this, false, virtual_clock) {}

SensorData PhysicalModel::GetSensorData(const AndroidSensor sensor_id) const {
    const size_t sz = GetSensorValueSize(sensor_id);
    SensorData data;
    data.value.resize(sz);
    data.measurement_id = GetSensorDataImpl(sensor_id, data.value.data(), sz);
    return data;
}

void PhysicalModel::SetSensorValue(const AndroidSensor sensor_id, const SensorValue& val) {
    SetSensorValueImpl(sensor_id, val.data(), val.size());
}

size_t PhysicalModel::GetPhysicalParameterSize(PhysicalParameter parameter) {
#define VALUE_SIZE_float 1
#define VALUE_SIZE_vec3 3
#define VALUE_SIZE_vec4 4
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(X, Y, Z, W) \
    case PhysicalParameter::X:                      \
        return VALUE_SIZE_##W;

    switch (parameter) {
        GOLDFISH_PHYSICAL_PARAMETERS_LIST
    case PhysicalParameter::MAX_PHYSICAL_PARAMETERS:
        break;
    }

    LOG(FATAL) << "Unexpected parameter: " << static_cast<int>(parameter);

#undef GOLDFISH_PHYSICAL_PARAMETER_DEF
#undef VALUE_SIZE_vec4
#undef VALUE_SIZE_vec3
#undef VALUE_SIZE_float
}

void PhysicalModel::GetPhysicalParameterValue(const PhysicalParameter parameter, float* out,
                                              const size_t count,
                                              const ParameterValueType parameter_value_type) const {
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(X, Y, Z, W)                   \
    case PhysicalParameter::X:                                        \
        GetValues(GetParameter##Z(parameter_value_type), out, count); \
        return;

    switch (parameter) {
        GOLDFISH_PHYSICAL_PARAMETERS_LIST
    case PhysicalParameter::MAX_PHYSICAL_PARAMETERS:
        break;
    }

    LOG(FATAL) << "Unexpected parameter: " << static_cast<int>(parameter);

#undef GOLDFISH_PHYSICAL_PARAMETER_DEF
}

size_t PhysicalModel::GetSensorValueSize(AndroidSensor sensor_id) {
#define VALUE_SIZE_float 1
#define VALUE_SIZE_vec3 3
#define VALUE_SIZE_vec4 4
#define GOLDFISH_SENSOR_DEF(X, Y, Z, V, W) \
    case AndroidSensor::X:                 \
        return VALUE_SIZE_##V;

    switch (sensor_id) {
        GOLDFISH_SENSORS_LIST
    case AndroidSensor::MAX_SENSORS:
        break;
    }

    LOG(FATAL) << "Unexpected sensor_id: " << static_cast<int>(sensor_id);

#undef GOLDFISH_SENSOR_DEF
#undef VALUE_SIZE_vec4
#undef VALUE_SIZE_vec3
#undef VALUE_SIZE_float
}

size_t PhysicalModel::GetSensorDataImpl(const AndroidSensor sensor_id, float* out,
                                        const size_t count) const {
#define GOLDFISH_SENSOR_DEF(X, Y, Z, V, W)              \
    case AndroidSensor::X:                              \
        GetValues(Get##Z(&measurement_id), out, count); \
        return measurement_id;

    size_t measurement_id = 0;
    switch (sensor_id) {
        GOLDFISH_SENSORS_LIST
    case AndroidSensor::MAX_SENSORS:
        break;
    }

    LOG(FATAL) << "Unexpected sensor_id: " << static_cast<int>(sensor_id);

#undef GOLDFISH_SENSOR_DEF
}

void PhysicalModel::SetSensorValueImpl(AndroidSensor sensor_id, const float* val,
                                       const size_t count) {
#define GOLDFISH_SENSOR_DEF(X, Y, Z, V, W)      \
    case AndroidSensor::X:                      \
        Override##Z(Get##V##Value(val, count)); \
        return;

    switch (sensor_id) {
        GOLDFISH_SENSORS_LIST
    case AndroidSensor::MAX_SENSORS:
        break;
    }

    LOG(FATAL) << "Unexpected sensor_id: " << static_cast<int>(sensor_id);

#undef GOLDFISH_SENSOR_DEF
}

void PhysicalModel::SetPhysicalParameterValue(const PhysicalParameter parameter, const float* val,
                                              const size_t count,
                                              const PhysicalInterpolation interpolation_mode) {
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(X, Y, Z, W)                  \
    case PhysicalParameter::X:                                       \
        SetTarget##Z(Get##W##Value(val, count), interpolation_mode); \
        return;

    switch (parameter) {
        GOLDFISH_PHYSICAL_PARAMETERS_LIST
    case PhysicalParameter::MAX_PHYSICAL_PARAMETERS:
        break;
    }

    LOG(FATAL) << "Unexpected parameter: " << static_cast<int>(parameter);

#undef GOLDFISH_PHYSICAL_PARAMETER_DEF
}

void PhysicalModel::SetCurrentTime(int64_t time_ns) {
    bool state_stabilized = false;
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        model_time_ns_ = time_ns;
        const bool is_inertial_model_stable =
                inertial_model_.SetCurrentTime(time_ns) == InertialState::kStable;
        state_stabilized = (is_inertial_model_stable && is_physical_state_changing_);
    }

    if (state_stabilized) {
        PhysicalStateStabilized();
    }
}

void PhysicalModel::SetGravity(float x, float y, float z) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetGravity(glm::vec3(x, y, z), PhysicalInterpolation::kStep);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalPosition(vec3 position, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        inertial_model_.SetTargetPosition(position, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalVelocity(vec3 velocity, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        inertial_model_.SetTargetVelocity(velocity, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalAmbientMotion(float bounds, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        inertial_model_.SetTarGetAmbientMotion(bounds, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalRotation(vec3 rotation, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        inertial_model_.SetTargetRotation(FromEulerAnglesXyz(glm::radians(rotation)), mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalMagneticField(vec3 field, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetMagneticField(field.x, field.y, field.z, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalTemperature(float celsius, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetTemperature(celsius, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalProximity(float centimeters, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetProximity(centimeters, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalLight(float lux, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetLight(lux, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalPressure(float h_pa, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetPressure(h_pa, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalHumidity(float percentage, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetHumidity(percentage, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalHingeAngle0(float degrees, PhysicalInterpolation mode) {
    if (!HasFoldableModel()) {
        LOG(WARNING) << "Device is not foldable, ignoring hinge-angle0 change to: " << degrees;
        return;
    }

    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        foldable_model_->SetHingeAngle(0, degrees, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalHingeAngle1(float degrees, PhysicalInterpolation mode) {
    if (!HasFoldableModel()) {
        LOG(WARNING) << "Device is not foldable, ignoring hinge-angle1 change to: " << degrees;
        return;
    }

    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        foldable_model_->SetHingeAngle(1, degrees, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalHingeAngle2(float degrees, PhysicalInterpolation mode) {
    if (!HasFoldableModel()) {
        LOG(WARNING) << "Device is not foldable, ignoring hinge-angle2 change to: " << degrees;
        return;
    }

    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        foldable_model_->SetHingeAngle(2, degrees, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalPosture(float posture, PhysicalInterpolation mode) {
    if (!HasFoldableModel()) {
        LOG(WARNING) << "Device is not foldable, ignoring posture change to: " << posture;
        return;
    }

    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        foldable_model_->SetPosture(posture, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalRollableImpl(unsigned index, float percentage,
                                                  PhysicalInterpolation mode) {
    if (!HasFoldableModel()) {
        LOG(WARNING) << "Device is not foldable, ignoring rollable" << index
                     << " change to: " << percentage;
        return;
    }

    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        foldable_model_->SetRollable(index, percentage, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalRollable0(float percentage, PhysicalInterpolation mode) {
    SetTargetInternalRollableImpl(0, percentage, mode);
}

void PhysicalModel::SetTargetInternalRollable1(float percentage, PhysicalInterpolation mode) {
    SetTargetInternalRollableImpl(1, percentage, mode);
}

void PhysicalModel::SetTargetInternalRollable2(float percentage, PhysicalInterpolation mode) {
    SetTargetInternalRollableImpl(2, percentage, mode);
}

void PhysicalModel::SetTargetInternalHeartRate(float bpm, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        body_model_.SetHeartRate(bpm, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalRgbcLight(vec4 light, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        ambient_environment_.SetRgbcLight(light, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalWristTilt(float value, PhysicalInterpolation mode) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        inertial_model_.SetWristTilt(value, mode);
    }
    TargetStateChanged();
}

void PhysicalModel::SetTargetInternalAccelerometerUncalibrated(vec3, PhysicalInterpolation) {
    PhysicalStateChanging();
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        // Not supported to set the uncalibrated accelerometer value.
    }
    TargetStateChanged();
}

vec3 PhysicalModel::GetParameterAccelerometerUncalibrated(ParameterValueType) const {
    return inertial_model_.GetAcceleration();
}

vec3 PhysicalModel::GetParameterPosition(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return inertial_model_.GetPosition(parameter_value_type);
}

vec3 PhysicalModel::GetParameterVelocity(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return inertial_model_.GetVelocity(parameter_value_type);
}

float PhysicalModel::GetParameterAmbientMotion(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return inertial_model_.GetAmbientMotion(parameter_value_type);
}

vec3 PhysicalModel::GetParameterRotation(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const glm::vec3 rotation_radians =
            ToEulerAnglesXyz(inertial_model_.GetRotation(parameter_value_type));
    return glm::degrees(rotation_radians);
}

vec3 PhysicalModel::GetParameterMagneticField(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetMagneticField(parameter_value_type);
}

float PhysicalModel::GetParameterTemperature(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetTemperature(parameter_value_type);
}

float PhysicalModel::GetParameterProximity(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetProximity(parameter_value_type);
}

float PhysicalModel::GetParameterLight(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetLight(parameter_value_type);
}

float PhysicalModel::GetParameterPressure(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetPressure(parameter_value_type);
}

float PhysicalModel::GetParameterHumidity(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetHumidity(parameter_value_type);
}

float PhysicalModel::GetParameterHingeAngle0(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetHingeAngle(0, parameter_value_type);
}

float PhysicalModel::GetParameterHingeAngle1(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetHingeAngle(1, parameter_value_type);
}

float PhysicalModel::GetParameterHingeAngle2(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetHingeAngle(2, parameter_value_type);
}

float PhysicalModel::GetParameterPosture(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetPosture(parameter_value_type);
}

float PhysicalModel::GetParameterRollable0(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetRollable(0, parameter_value_type);
}

float PhysicalModel::GetParameterRollable1(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetRollable(1, parameter_value_type);
}

float PhysicalModel::GetParameterRollable2(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetRollable(2, parameter_value_type);
}

float PhysicalModel::GetParameterHeartRate(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return body_model_.GetHeartRate(parameter_value_type);
}

vec4 PhysicalModel::GetParameterRgbcLight(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ambient_environment_.GetRgbcLight(parameter_value_type);
}

float PhysicalModel::GetParameterWristTilt(ParameterValueType parameter_value_type) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return inertial_model_.GetWristTilt(parameter_value_type);
}

template <class T, class GETTER>
T PhysicalModel::GetSensorValue(const AndroidSensor sensor, const T* override_member_pointer,
                                const GETTER& physical_getter, size_t* measurement_id) const {
    const auto sensor_index = static_cast<size_t>(sensor);

    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (use_override_[static_cast<size_t>(sensor)]) {
        *measurement_id = measurement_id_[sensor_index];
        return *override_member_pointer;
    }
    if (is_physical_state_changing_) {
        measurement_id_[sensor_index]++;
    }
    *measurement_id = measurement_id_[sensor_index];
    return physical_getter();
}

#define GET_FUNCTION_NAME(x) Get##x
#define OVERRIDE_FUNCTION_NAME(x) Override##x
#define OVERRIDE_NAME(x) m##x##Override
#define SENSOR_NAME(x) AndroidSensor::x
#define PHYSICAL_NAME(x) GetPhysical##x

// Implement sensor overrides.
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w)                              \
    void PhysicalModel::OVERRIDE_FUNCTION_NAME(z)(v override_value) {   \
        SetOverride(SENSOR_NAME(x), &OVERRIDE_NAME(z), override_value); \
    }

GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

// Implement getters that respect overrides.
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w)                                                        \
    v PhysicalModel::GET_FUNCTION_NAME(z)(size_t* measurement_id) const {                         \
        return GetSensorValue<v>(                                                                 \
                SENSOR_NAME(x), &OVERRIDE_NAME(z), [this]() { return this->PHYSICAL_NAME(z)(); }, \
                measurement_id);                                                                  \
    }

GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

#undef PHYSICAL_NAME
#undef SENSOR_NAME
#undef OVERRIDE_NAME
#undef OVERRIDE_FUNCTION_NAME
#undef GET_FUNCTION_NAME

bool PhysicalModel::IsSensorOverridden(const AndroidSensor sensor) const {
    const auto sensor_index = static_cast<size_t>(sensor);
    if (sensor_index >= kNumSensors) {
        return false;
    }
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return use_override_[sensor_index];
}

void PhysicalModel::ClearOverrides() {
    NotifyTargetState(PhysicalModelChangeEvent::Type::kPhysicalStateChanging);
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (size_t i = 0; i < kNumSensors; ++i) {
            if (use_override_[i]) {
                measurement_id_[i]++;
            }
        }
        use_override_.reset();
    }
    NotifyTargetState(PhysicalModelChangeEvent::Type::kTargetStateChanged);
}

vec3 PhysicalModel::GetPhysicalAccelerometer() const {
    // Implementation Note:
    // Gravity and magnetic vectors as observed by the device.
    // Note how we're applying the *inverse* of the transformation
    // represented by device_rotation_quat to the "absolute" coordinates
    // of the vectors.
    return glm::conjugate(inertial_model_.GetRotation()) *
           (inertial_model_.GetAcceleration() - ambient_environment_.GetGravity());
}

vec3 PhysicalModel::GetPhysicalAccelerometerUncalibrated() const {
    // Same values for the calibrated and uncalibrated accelerometer
    // Bias will be added to the values on the guest side.
    return GetPhysicalAccelerometer();
}

vec3 PhysicalModel::GetPhysicalGyroscope() const {
    return glm::conjugate(inertial_model_.GetRotation()) * inertial_model_.GetRotationalVelocity();
}

vec3 PhysicalModel::GetPhysicalMagnetometer() const {
    return glm::conjugate(inertial_model_.GetRotation()) * ambient_environment_.GetMagneticField();
}

/* (x, y, z) == (azimuth, pitch, roll) */
vec3 PhysicalModel::GetPhysicalOrientation() const {
    return ToEulerAnglesXyz(inertial_model_.GetRotation());
}

float PhysicalModel::GetPhysicalTemperature() const {
    return ambient_environment_.GetTemperature();
}

float PhysicalModel::GetPhysicalProximity() const {
    return ambient_environment_.GetProximity();
}

float PhysicalModel::GetPhysicalLight() const {
    return ambient_environment_.GetLight();
}

float PhysicalModel::GetPhysicalPressure() const {
    return ambient_environment_.GetPressure();
}

float PhysicalModel::GetPhysicalHumidity() const {
    return ambient_environment_.GetHumidity();
}

vec3 PhysicalModel::GetPhysicalMagnetometerUncalibrated() const {
    return glm::conjugate(inertial_model_.GetRotation()) * ambient_environment_.GetMagneticField();
}

vec3 PhysicalModel::GetPhysicalGyroscopeUncalibrated() const {
    return glm::conjugate(inertial_model_.GetRotation()) * inertial_model_.GetRotationalVelocity();
}

vec4 PhysicalModel::GetPhysicalRgbcLight() const {
    return ambient_environment_.GetRgbcLight();
}

void PhysicalModel::GetTransform(float* out_translation_x, float* out_translation_y,
                                 float* out_translation_z, float* out_rotation_x,
                                 float* out_rotation_y, float* out_rotation_z,
                                 int64_t* out_timestamp) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);

    const vec3 position = GetParameterPosition(ParameterValueType::kCurrent);
    *out_translation_x = position.x;
    *out_translation_y = position.y;
    *out_translation_z = position.z;
    const vec3 rotation = GetParameterRotation(ParameterValueType::kCurrent);
    *out_rotation_x = rotation.x;
    *out_rotation_y = rotation.y;
    *out_rotation_z = rotation.z;
    *out_timestamp = model_time_ns_;
}

Rotation PhysicalModel::GetDeviceRotation() const {
    using physics::SkinRotation;

    size_t measurement_id;
    const vec3 device_accelerometer = GetAccelerometer(&measurement_id);
    const glm::vec3 normalized_accelerometer = glm::normalize(device_accelerometer);

    static const std::array<std::pair<glm::vec3, SkinRotation>, 4> kDirections{
        std::make_pair(glm::vec3(0.0F, 1.0F, 0.0F), SkinRotation::kPortrait),
        std::make_pair(glm::vec3(1.0F, 0.0F, 0.0F), SkinRotation::kLandscape),
        std::make_pair(glm::vec3(0.0F, -1.0F, 0.0F), SkinRotation::kReversePortrait),
        std::make_pair(glm::vec3(-1.0F, 0.0F, 0.0F), SkinRotation::kReverseLandscape)};

    auto coarse_orientation = SkinRotation::kPortrait;
    for (const auto& v : kDirections) {
        if (fabs(glm::dot(normalized_accelerometer, v.first) - 1.F) < 0.1F) {
            coarse_orientation = v.second;
            break;
        }
    }

    return {
        .rotation = coarse_orientation,
        .x_axis = device_accelerometer.x,
        .y_axis = device_accelerometer.y,
        .z_axis = device_accelerometer.z,
    };
}

float PhysicalModel::GetPhysicalHingeAngle0() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetHingeAngle(0);
}

float PhysicalModel::GetPhysicalHingeAngle1() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetHingeAngle(1);
}

float PhysicalModel::GetPhysicalHingeAngle2() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!foldable_model_) {
        return 0.0F;
    }
    return foldable_model_->GetHingeAngle(2);
}

float PhysicalModel::GetPhysicalHeartRate() const {
    return body_model_.GetHeartRate();
}

float PhysicalModel::GetPhysicalWristTilt() const {
    return inertial_model_.GetWristTilt();
}

#define SET_TARGET_FUNCTION_NAME(x) SetTarget##x
#define SET_TARGET_INTERNAL_FUNCTION_NAME(x) SetTargetInternal##x
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(x, y, z, w)                                        \
    void PhysicalModel::SET_TARGET_FUNCTION_NAME(z)(w value, PhysicalInterpolation mode) { \
        SET_TARGET_INTERNAL_FUNCTION_NAME(z)(value, mode);                                 \
    }

GOLDFISH_PHYSICAL_PARAMETERS_LIST
#undef GOLDFISH_PHYSICAL_PARAMETER_DEF
#undef SET_TARGET_INTERNAL_FUNCTION_NAME
#undef SET_TARGET_FUNCTION_NAME

void PhysicalModel::PhysicalStateChanging() {
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        is_physical_state_changing_ = true;
    }

    NotifyTargetState(PhysicalModelChangeEvent::Type::kPhysicalStateChanging);
}

void PhysicalModel::PhysicalStateStabilized() {
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        DCHECK(is_physical_state_changing_);

        // Increment all of the measurement ids because the physical state has
        // stabilized.
        for (size_t& i : measurement_id_) {
            i++;
        }
        is_physical_state_changing_ = false;
    }

    NotifyTargetState(PhysicalModelChangeEvent::Type::kPhysicalStateStabilized);
}

void PhysicalModel::TargetStateChanged() {
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        use_override_.reset();  // When target state changes we reset all sensor overrides.
    }

    NotifyTargetState(PhysicalModelChangeEvent::Type::kTargetStateChanged);
}

void PhysicalModel::NotifyTargetState(PhysicalModelChangeEvent::Type type) {
    const PhysicalModelChangeEvent event{
        .type = type,
        .model = this,
    };
    FireEvent(event);
}

static_assert(PhysicalModel::kNumSensors <= sizeof(unsigned long) * CHAR_BIT);

archive::IWriter& operator<<(archive::IWriter& w, const PhysicalModel& pm) {
    w << pm.inertial_model_ << pm.ambient_environment_ << pm.body_model_ << pm.model_time_ns_
      << pm.use_override_.to_ulong() << pm.is_physical_state_changing_;

    if (pm.foldable_model_) {
        w << true << *pm.foldable_model_;
    } else {
        w << false;
    }

    for (const size_t measurement_id : pm.measurement_id_) {
        w << measurement_id;
    }

#define GOLDFISH_SENSOR_DEF(x, y, z, v, w0) w << pm.m##z##Override;
    GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

    return w;
}

absl::Status ReadValue(archive::IReader& r, PhysicalModel& pm) {
    unsigned long use_override_bits;
    bool has_foldable_model;

    RETURN_IF_ERROR(ReadValue(r, pm.inertial_model_, pm.ambient_environment_, pm.body_model_,
                              pm.model_time_ns_, use_override_bits, pm.is_physical_state_changing_,
                              has_foldable_model));

    if constexpr (PhysicalModel::kNumSensors < sizeof(unsigned long) * CHAR_BIT) {
        if ((use_override_bits >> PhysicalModel::kNumSensors) != 0) {
            return absl::InvalidArgumentError(absl::StrCat(
                    "Invalid sensor override bitmask 0x", absl::Hex(use_override_bits),
                    " in PhysicalModel (kNumSensors=", PhysicalModel::kNumSensors, ")"));
        }
    }
    pm.use_override_ = PhysicalModel::UseOverrideMask(use_override_bits);

    if (has_foldable_model != static_cast<bool>(pm.foldable_model_)) {
        return {absl::StatusCode::kInvalidArgument, "Mismatch on the foldable model configuration"};
    } else if (has_foldable_model) {
        RETURN_IF_ERROR(ReadValue(r, *pm.foldable_model_));
    }

    for (size_t& measurement_id : pm.measurement_id_) {
        RETURN_IF_ERROR(ReadValue(r, measurement_id));
    }

#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) RETURN_IF_ERROR(ReadValue(r, pm.m##z##Override));
    GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

    return absl::OkStatus();
}

}  // namespace goldfish::sensors
