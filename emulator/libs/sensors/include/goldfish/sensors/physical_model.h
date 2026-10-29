/*
 * Copyright (C) 2024 The Android Open Source Project
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
#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "absl/status/status.h"

#include "android/goldfish/hardware_config.h"
#include "goldfish/archive/glm.h"
#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/eventing/event_sources.h"
#include "goldfish/physics/ambient_environment.h"
#include "goldfish/physics/body_model.h"
#include "goldfish/physics/inertial_model.h"
#include "goldfish/physics/physics.h"
#include "goldfish/physics/rotation.h"
#include "goldfish/sensors/android_sensor.h"
#include "goldfish/sensors/foldable_model.h"
#include "goldfish/sensors/physical_parameter.h"
#include "goldfish/sensors/sensor_data.h"
#include "goldfish/sensors/virtual_scene_camera.h"
#include "goldfish/system/virtual_clock.h"

namespace goldfish::sensors {

using android::base::eventing::CallbackEventSource;

using ::goldfish::physics::AmbientEnvironment;
using ::goldfish::physics::BodyModel;
using ::goldfish::physics::InertialModel;
using ::goldfish::physics::Rotation;

using glm::vec3;
using glm::vec4;

class PhysicalModel;

/**
 * @brief Event structure for physical model state changes.
 */
struct PhysicalModelChangeEvent {
    /**
     * @brief Types of physical model state changes.
     */
    enum class Type : std::uint8_t {
        /// Notify the agent that physical target states have changed
        kTargetStateChanged,
        /// Notify the agent that physical state changes are beginning
        kPhysicalStateChanging,
        /// Notify the agent that physical state changes are complete and model is stable
        kPhysicalStateStabilized,
    };

    Type type;             ///< Type of state change event
    PhysicalModel* model;  ///< Pointer to the model generating the event
};

/**
 * @brief Simulates an ambient environment with a rigid body and generates sensor data.
 *
 * This class provides a comprehensive physical simulation environment for Android
 * device emulation. It models various physical aspects including:
 * - Position and orientation in 3D space
 * - Ambient environmental conditions (temperature, pressure, etc.)
 * - Device sensors (accelerometer, gyroscope, etc.)
 * - Foldable device states and transformations
 *
 * The model supports both immediate and interpolated state changes, sensor value
 * overrides, and provides event notifications for state changes.
 *
 * @note All public methods are thread-safe unless otherwise specified.
 */
class PhysicalModel : public CallbackEventSource<PhysicalModelChangeEvent> {
  public:
    static constexpr size_t kNumSensors = static_cast<size_t>(AndroidSensor::MAX_SENSORS);

    PhysicalModel(const android::goldfish::HardwareConfig& hw,
                  const ::goldfish::system::VirtualClock& virtual_clock);
    virtual ~PhysicalModel() = default;

    SensorData GetSensorData(AndroidSensor) const;
    void SetSensorValue(AndroidSensor, const SensorValue&);

    static size_t GetPhysicalParameterSize(PhysicalParameter parameter);
    void GetPhysicalParameterValue(PhysicalParameter parameter, float* out, size_t count,
                                   ParameterValueType parameter_value_type) const;
    void SetPhysicalParameterValue(PhysicalParameter parameter, const float* val, size_t count,
                                   PhysicalInterpolation interpolation_mode);

    /**
     * @brief Sets the current simulation time.
     *
     * This time is used for calculating sensor values and recording when target
     * parameter changes occur. Time values should be monotonic (non-decreasing).
     *
     * @param time_ns The current time in nanoseconds.
     */
    void SetCurrentTime(int64_t time_ns);

    /**
     * @brief Sets the gravity vector for the simulation.
     * @param x X component of gravity vector
     * @param y Y component of gravity vector
     * @param z Z component of gravity vector
     */
    void SetGravity(float x, float y, float z);

    /*
     * Target state setters.
     * @note Foldable/Rollable target setters (HingeAngle*, Posture, Rollable*)
     *       require that HasFoldableModel() is true. Calling them on a non-foldable
     *       target will be safely ignored.
     */
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(x, y, z, w) \
    void SetTarget##z(w value, PhysicalInterpolation mode);

    GOLDFISH_PHYSICAL_PARAMETERS_LIST
#undef GOLDFISH_PHYSICAL_PARAMETER_DEF

    /*
     * Gets current target state of the modeled object.
     * @note Foldable/Rollable parameter getters (HingeAngle*, Posture, Rollable*)
     *       require that HasFoldableModel() is true. Calling them on a non-foldable
     *       target will return default 0.0f.
     */
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(x, y, z, w) \
    w GetParameter##z(ParameterValueType parameter_value_type) const;

    GOLDFISH_PHYSICAL_PARAMETERS_LIST
#undef GOLDFISH_PHYSICAL_PARAMETER_DEF

    /*
     * Sensor override methods.
     * @note Foldable/Rollable override methods (HingeAngle*, Posture, Rollable*)
     *       require that HasFoldableModel() is true.
     */
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) void Override##z(v override_value);
    GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

    bool IsSensorOverridden(AndroidSensor sensor) const;
    void ClearOverrides();

    /*
     * Getters for all sensor values.
     * Can be called from any thread.
     * @note Foldable/Rollable sensor getters (HingeAngle*, Posture, Rollable*)
     *       require that HasFoldableModel() is true.
     */
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) v Get##z(size_t* measurement_id) const;
    GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

    /**
     * @brief Gets the current physical transform of the device.
     * @param[out] out_translation_x X translation
     * @param[out] out_translation_y Y translation
     * @param[out] out_translation_z Z translation
     * @param[out] out_rotation_x X rotation in degrees
     * @param[out] out_rotation_y Y rotation in degrees
     * @param[out] out_rotation_z Z rotation in degrees
     * @param[out] out_timestamp Timestamp in nanoseconds
     */
    void GetTransform(float* out_translation_x, float* out_translation_y, float* out_translation_z,
                      float* out_rotation_x, float* out_rotation_y, float* out_rotation_z,
                      int64_t* out_timestamp) const;

    Rotation GetDeviceRotation() const;

    bool HasFoldableModel() const;
    const FoldableConfig* GetFoldableConfig() const;

    /**
     * @brief Gets the folded area dimensions.
     * @note Caller must ensure HasFoldableModel() is true before calling this API.
     *       Calling it on a non-foldable target will trigger a DCHECK assertion failure.
     * @param[out] x X coordinate of folded area
     * @param[out] y Y coordinate of folded area
     * @param[out] w Width of folded area
     * @param[out] h Height of folded area
     * @return true if area was retrieved successfully
     */
    bool GetFoldedArea(int* x, int* y, int* w, int* h) const;

    FoldableModel::ObservablePosture* GetPostureListener();

    bool FoldableIsFolded() const;
    FoldablePostures GetFoldablePosture() const;

    VirtualSceneCamera& GetVirtualSceneCamera() { return virtual_scene_camera_; }
    const VirtualSceneCamera& GetVirtualSceneCamera() const { return virtual_scene_camera_; }

    friend archive::IWriter& operator<<(archive::IWriter&, const PhysicalModel&);
    friend absl::Status ReadValue(archive::IReader&, PhysicalModel&);

  private:
    static size_t GetSensorValueSize(AndroidSensor);
    size_t GetSensorDataImpl(AndroidSensor, float* out, size_t count) const;
    void SetSensorValueImpl(AndroidSensor, const float* val, size_t count);
    void SetTargetInternalRollableImpl(unsigned index, float percentage,
                                       PhysicalInterpolation mode);

    /*
     * Sets the target value for the given physical parameter that the physical
     * model should move towards.
     * Can be called from any thread.
     */
#define GOLDFISH_PHYSICAL_PARAMETER_DEF(x, y, z, w) \
    void SetTargetInternal##z(w value, PhysicalInterpolation mode);

    GOLDFISH_PHYSICAL_PARAMETERS_LIST
#undef GOLDFISH_PHYSICAL_PARAMETER_DEF

    /*
     * Getters for non-overridden physical sensor values.
     */
#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) v GetPhysical##z() const;
    GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

    /*
     * Helper for setting overrides.
     */
    template <class T>
    void SetOverride(const AndroidSensor sensor, T* override_member_pointer, T override_value) {
        const auto sensor_index = static_cast<size_t>(sensor);

        NotifyTargetState(PhysicalModelChangeEvent::Type::kPhysicalStateChanging);
        {
            const std::lock_guard<std::recursive_mutex> lock(mutex_);
            use_override_[sensor_index] = true;
            measurement_id_[sensor_index]++;
            *override_member_pointer = override_value;
        }
        NotifyTargetState(PhysicalModelChangeEvent::Type::kTargetStateChanged);
    }

    /*
     * Helper for getting current sensor values.
     */
    template <class T, class GETTER>
    T GetSensorValue(AndroidSensor sensor, const T* override_member_pointer,
                     const GETTER& physical_getter, size_t* measurement_id) const;

    void PhysicalStateChanging();    ///< Called when physical state begins changing
    void PhysicalStateStabilized();  ///< Called when physical state stabilizes
    void TargetStateChanged();       ///< Called when target state changes
    void NotifyTargetState(PhysicalModelChangeEvent::Type);

    using UseOverrideMask = std::bitset<kNumSensors>;

    const std::unique_ptr<FoldableModel> foldable_model_;  ///< Models foldable device state
    VirtualSceneCamera virtual_scene_camera_;  ///< Manages virtual scene camera controls
    InertialModel inertial_model_;            ///< Models inertial motion
    AmbientEnvironment ambient_environment_;  ///< Models ambient conditions
    BodyModel body_model_;                    ///< Models body-related sensors
    int64_t model_time_ns_ = 0L;              ///< Current model time in nanoseconds
    UseOverrideMask use_override_;            ///< Sensor override flags
    mutable size_t measurement_id_[kNumSensors] = {0};  ///< Measurement IDs
    bool is_physical_state_changing_{false};  ///< True if physical state is changing

#define GOLDFISH_SENSOR_DEF(x, y, z, v, w) v m##z##Override{0.f};
    GOLDFISH_SENSORS_LIST
#undef GOLDFISH_SENSOR_DEF

    mutable std::recursive_mutex mutex_;  ///< Mutex for thread safety
};

}  // namespace goldfish::sensors
