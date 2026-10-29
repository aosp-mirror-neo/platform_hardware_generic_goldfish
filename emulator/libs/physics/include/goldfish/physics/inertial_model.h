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

#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <numbers>

#include "absl/status/status.h"

#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/physics/physics.h"

namespace goldfish::physics {

constexpr uint64_t SecondsToNs(float seconds) {
    return static_cast<uint64_t>(seconds * 1000000000.0);
}

constexpr float NsToSeconds(uint64_t nano_seconds) {
    return static_cast<float>(static_cast<float>(nano_seconds) / 1000000000.0);
}

constexpr float kMaxStateChangeTimeSeconds = 0.5F;
constexpr float kMinStateChangeTimeSeconds = 0.05F;

// Ambient motion frequency of 0.5Hz.  This is applied directly in the x axis
// and scaled by 1 / sqrt(2) and 1 / sqrt(3) in y and z axis respectively.
const float kAmbientFrequency = 0.5F;
const glm::vec3 kAmbientFrequencyVec =
        glm::vec3(1.F, 1.F / std::numbers::sqrt2_v<float>, std::numbers::inv_sqrt3_v<float>) *
        kAmbientFrequency * 2.F * std::numbers::pi_v<float>;

enum class InertialState : std::uint8_t {
    kChanging = 0,
    kStable = 1,
};

/*
 * Implements a model of inertial motion of a rigid body such that smooth
 * movement occurs bringing the body to the target rotation and position with
 * dependent sensor data constantly available and up to date.
 *
 * The inertial model should be used by sending it target positions and
 * then polling the current actual rotation and position, acceleration and
 * velocity values in order to find the current state of the rigid body.
 */
class InertialModel {
  public:
    InertialModel() = default;

    /*
     * Sets the current time of the InertialModel simulation.  This time is
     * used as the current time in calculating current position, velocity and
     * acceleration, along with the time when target position/rotation change
     * requests are recorded as taking place.  Time values must be
     * non-decreasing.
     */
    InertialState SetCurrentTime(uint64_t time_ns);

    /*
     * Sets the position that the modeled object should move toward.
     */
    void SetTargetPosition(glm::vec3 position, PhysicalInterpolation mode);

    /*
     * Sets the velocity at which the modeled object should start moving.
     */
    void SetTargetVelocity(glm::vec3 velocity, PhysicalInterpolation mode);

    /*
     * Sets the rotation that the modeled object should move toward.
     */
    void SetTargetRotation(glm::quat rotation, PhysicalInterpolation mode);

    /*
     * Set the half-width of the bounding box for ambient motion.  Setting this
     * to zero disables ambient motion.
     */
    void SetTarGetAmbientMotion(float bounds, PhysicalInterpolation mode);

    /*
     * Set the value reported by WRIST_TILT_GESTURE sensor. 1 means GAZE and
     * 0 means UNGAZE.
     */
    void SetWristTilt(float value, PhysicalInterpolation mode);

    /*
     * Gets current simulated state and sensor values of the modeled object at
     * the most recently set current time (from setCurrentTime).
     */
    glm::vec3 GetPosition(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;
    glm::vec3 GetVelocity(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;
    glm::vec3 GetAcceleration(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;
    glm::vec3 GetJerk(ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;
    glm::quat GetRotation(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;
    // rotational velocity as rotation around (x, y, z) axis in rad/s
    glm::vec3 GetRotationalVelocity(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;

    /*
     * Gets half the width of the ambient motion bounding box.
     */
    float GetAmbientMotion(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;

    float GetWristTilt(
            ParameterValueType parameter_value_type = ParameterValueType::kCurrent) const;

    friend archive::IWriter& operator<<(archive::IWriter&, const InertialModel&);
    friend absl::Status ReadValue(archive::IReader&, InertialModel&);

  private:
    void UpdateRotations();

    // Helper for setting the transforms for position, velocity and acceleration
    // based on the coefficients for heptic motion.
    void SetInertialTransforms(
            const glm::vec3& heptic_coefficient, const glm::vec3& hexic_coefficient,
            const glm::vec3& quintic_coefficient, const glm::vec3& quartic_coefficient,
            const glm::vec3& cubic_coefficient, const glm::vec3& quadratic_coefficient,
            const glm::vec3& linear_coefficient, const glm::vec3& constant_coefficient,
            const glm::vec4& heptic_time_vector, const glm::vec4& cubic_time_vector);

    // Helper for calculating the current or target state given a transform
    // specifying either the acceleration, velocity, or position.
    glm::vec3 CalculateInertialState(const glm::mat4x3& heptic_transform,
                                     const glm::mat4x3& cubic_transform,
                                     const glm::mat4x3& after_end_cubic_transform,
                                     ParameterValueType parameter_value_type) const;

    // Helper for calculating the current or target rotational state given a
    // transform specifying either the rotational velocity, or rotation in
    // 4d vector space.
    glm::vec4 CalculateRotationalState(const glm::mat2x4& quintic_transform,
                                       const glm::mat4x4& cubic_transform,
                                       const glm::mat4x4& after_end_cubic_transform,
                                       ParameterValueType parameter_value_type) const;

    // Get the value and derivatives of the ambient motion bounding box size.
    float GetAmbientMotionBoundsValue(ParameterValueType parameter_value_type) const;
    float GetAmbientMotionBoundsDeriv(ParameterValueType parameter_value_type) const;
    float GetAmbientMotionBoundsSecondDeriv(ParameterValueType parameter_value_type) const;

    /// Immediately sets the rotation state to `rotation` with zero rotational
    /// velocity and acceleration.
    void SetStepRotation(glm::quat rotation);

    /// Evaluates the quintic ambient motion bounds polynomial (or its
    /// derivative) at the current model time, returning `end_value` once the
    /// transition has completed or when `kTarget` is requested.
    float EvaluateAmbientMotionBounds(const glm::vec2& quintic, const glm::vec4& cubic,
                                      float end_value,
                                      ParameterValueType parameter_value_type) const;

    // Note: Each target interpolation begins at a set time, accelerates at a
    //       for half of the target time, then decelerates for the second half
    //       with each half defined by a quartic polynomial such that the two
    //       halves are continuous up through the 3rd derivative.
    //
    //       Position/Velocity/Acceleration are calculated by multiplying a
    //       vector containing [t*t*t, t*t, t, 1] by the appropriate transform,
    //       and adding t*t*t*t multiplied by the quartic scale (if applicable -
    //       this is necessary because there is no glm::mat5x3 which would be
    //       necessary to do this calculation with a single matrix) where t is
    //       the amount of time in seconds since mAccelPhaseStartTime, and the
    //       AccelPhase transforms are used for the first half of the time
    //       between mAccelPhaseStartTime, and DecelPhaseTransforms for the
    //       second half.  At the end of the decel phase, the velocity and
    //       acceleration will be zero and the position will be as set in the
    //       target.
    uint64_t position_change_start_time_ = 0UL;
    glm::mat4x3 position_heptic_ = glm::mat4x3(0.F);
    glm::mat4x3 position_cubic_ = glm::mat4x3(0.F);
    glm::mat4x3 velocity_heptic_ = glm::mat4x3(0.F);
    glm::mat4x3 velocity_cubic_ = glm::mat4x3(0.F);
    glm::mat4x3 acceleration_heptic_ = glm::mat4x3(0.F);
    glm::mat4x3 acceleration_cubic_ = glm::mat4x3(0.F);
    glm::mat4x3 jerk_heptic_ = glm::mat4x3(0.F);
    glm::mat4x3 jerk_cubic_ = glm::mat4x3(0.F);
    uint64_t position_change_end_time_ = 0UL;
    bool zero_velocity_after_end_time_ = true;

    glm::mat4x3 position_after_end_cubic_ = glm::mat4x3(0.F);
    glm::mat4x3 velocity_after_end_cubic_ = glm::mat4x3(0.F);

    uint64_t rotation_change_start_time_ = 0UL;
    glm::mat2x4 rotation_quintic_ = glm::mat2x4(0.F);
    glm::mat4x4 rotation_cubic_ =
            glm::mat4x4(glm::vec4(), glm::vec4(), glm::vec4(), glm::vec4(0.F, 0.F, 0.F, 1.F));
    glm::mat4x4 rotation_after_end_cubic_ =
            glm::mat4x4(glm::vec4(), glm::vec4(), glm::vec4(), glm::vec4(0.F, 0.F, 0.F, 1.F));
    glm::mat2x4 rotational_velocity_quintic_ = glm::mat2x4(0.F);
    glm::mat4x4 rotational_velocity_cubic_ = glm::mat4x4(0.F);
    glm::mat2x4 rotational_acceleration_quintic_ = glm::mat2x4(0.F);
    glm::mat4x4 rotational_acceleration_cubic_ = glm::mat4x4(0.F);
    uint64_t rotation_change_end_time_ = 0UL;

    uint64_t ambient_motion_change_start_time_ = 0UL;
    float ambient_motion_end_value_ = 0.F;
    glm::vec2 ambient_motion_value_quintic_ = glm::vec2(0.F);
    glm::vec4 ambient_motion_value_cubic_ = glm::vec4(0.F);
    glm::vec2 ambient_motion_first_deriv_quintic_ = glm::vec2(0.F);
    glm::vec4 ambient_motion_first_deriv_cubic_ = glm::vec4(0.F);
    glm::vec2 ambient_motion_second_deriv_quintic_ = glm::vec2(0.F);
    glm::vec4 ambient_motion_second_deriv_cubic_ = glm::vec4(0.F);
    uint64_t ambient_motion_change_end_time_ = 0UL;

    float wrist_tilt_ = 0.F;

    /* The time to use as current in this model */
    uint64_t model_time_ns_ = 0UL;
};

}  // namespace goldfish::physics
