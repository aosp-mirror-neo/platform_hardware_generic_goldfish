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

#include "goldfish/physics/inertial_model.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "goldfish/archive/glm.h"

namespace goldfish::physics {

constexpr float kEpsilon = 0.0000000001F;

namespace {

/// Powers of elapsed time from t^1 through t^7 and packed polynomial vectors
/// for evaluating 7th-degree (heptic) trajectories.
struct HepticTimePowers {
    float t1;
    float t2;
    float t3;
    float t4;
    float t5;
    float t6;
    float t7;
    glm::vec4 heptic;
    glm::vec4 cubic;
};

/// Returns the cubic time-power vector `[t^3, t^2, t, 1]`.
glm::vec4 MakeCubicTimeVector(float time1) {
    const float time2 = time1 * time1;
    const float time3 = time2 * time1;
    return glm::vec4(time3, time2, time1, 1.F);
}

/// Returns the higher-order quintic time-power vector `[t^5, t^4]`.
glm::vec2 MakeQuinticTimeVector(float time1) {
    const float time2 = time1 * time1;
    const float time3 = time2 * time1;
    const float time4 = time2 * time2;
    return glm::vec2(time3 * time2, time4);
}

/// Computes scalar powers `t^1..t^7` and packed `[t^7..t^4]` and `[t^3..1]`
/// vectors for heptic polynomial evaluation.
HepticTimePowers MakeHepticTimePowers(float t1) {
    const float t2 = t1 * t1;
    const float t3 = t1 * t2;
    const float t4 = t2 * t2;
    const float t5 = t2 * t3;
    const float t6 = t3 * t3;
    const float t7 = t3 * t4;
    return {
        .t1 = t1,
        .t2 = t2,
        .t3 = t3,
        .t4 = t4,
        .t5 = t5,
        .t6 = t6,
        .t7 = t7,
        .heptic = glm::vec4(t7, t6, t5, t4),
        .cubic = glm::vec4(t3, t2, t1, 1.F),
    };
}

/// Computes the clamped interpolation duration in `[kMinStateChangeTimeSeconds,
/// kMaxStateChangeTimeSeconds]` proportional to `time_scale / max_time_scale`.
float ComputeTransitionTime(float time_scale, float max_time_scale) {
    return kMinStateChangeTimeSeconds +
           (std::min(time_scale, max_time_scale) / max_time_scale) *
                   (kMaxStateChangeTimeSeconds - kMinStateChangeTimeSeconds);
}

/// Polynomial coefficients for 5th-degree (quintic) interpolation.
template <typename T>
struct QuinticCoefficients {
    T quintic;
    T quartic;
    T cubic;
    T quadratic;
    T linear;
    T constant;
};

/// Solves for the 5th-degree polynomial coefficients that smoothly interpolate
/// from initial state `(x_init, v_init, a_init)` at `t = 0` to `x_target` with
/// zero first and second derivatives at `t = transition_time`.
template <typename T>
QuinticCoefficients<T> SolveQuinticCoefficients(const T& x_init, const T& v_init, const T& a_init,
                                                const T& x_target, float transition_time) {
    const float time2 = transition_time * transition_time;
    const float time3 = transition_time * time2;
    const float time4 = time2 * time2;
    const float time5 = time2 * time3;
    return {
        .quintic =
                (1.F / (2.0F * time5)) * (-1.F * time2 * a_init + -6.F * transition_time * v_init +
                                          -12.F * x_init + 12.F * x_target),
        .quartic =
                (1.F / (2.0F * time4)) * (3.F * time2 * a_init + 16.F * transition_time * v_init +
                                          30.F * x_init + -30.F * x_target),
        .cubic =
                (1.F / (2.0F * time3)) * (-3.F * time2 * a_init + -12.F * transition_time * v_init +
                                          -20.F * x_init + 20.F * x_target),
        .quadratic = (1.F / 2.F) * a_init,
        .linear = v_init,
        .constant = x_init,
    };
}

/// Evaluates the per-axis sinusoidal ambient motion wave at `time` (in seconds).
glm::vec3 AmbientWaveValue(double time) {
    return glm::vec3(static_cast<float>(std::sin(kAmbientFrequencyVec.x * time)),
                     static_cast<float>(std::sin(kAmbientFrequencyVec.y * time)),
                     static_cast<float>(std::sin(kAmbientFrequencyVec.z * time)));
}

/// Evaluates the first time derivative of the sinusoidal ambient motion wave at
/// `time` (in seconds).
glm::vec3 AmbientWaveFirstDeriv(double time) {
    return glm::vec3(static_cast<float>(std::cos(kAmbientFrequencyVec.x * time)),
                     static_cast<float>(std::cos(kAmbientFrequencyVec.y * time)),
                     static_cast<float>(std::cos(kAmbientFrequencyVec.z * time))) *
           kAmbientFrequencyVec;
}

}  // namespace

InertialState InertialModel::SetCurrentTime(uint64_t time_ns) {
    if (time_ns < model_time_ns_) {
        // If time goes backwards, set the position and rotation immediately
        // to their targets.
        const glm::vec3 target_position = GetPosition(ParameterValueType::kTarget);
        const glm::quat target_rotation = GetRotation(ParameterValueType::kTarget);
        model_time_ns_ = time_ns;
        SetTargetPosition(target_position, PhysicalInterpolation::kStep);
        SetTargetRotation(target_rotation, PhysicalInterpolation::kStep);
    } else {
        model_time_ns_ = time_ns;
    }

    return (zero_velocity_after_end_time_ && model_time_ns_ >= position_change_end_time_ &&
            model_time_ns_ >= rotation_change_end_time_ &&
            GetAmbientMotionBoundsValue(ParameterValueType::kCurrent) < kEpsilon)
                   ? InertialState::kStable
                   : InertialState::kChanging;
}

void InertialModel::SetTargetPosition(glm::vec3 position, PhysicalInterpolation mode) {
    float transition_time = kMinStateChangeTimeSeconds;
    if (mode == PhysicalInterpolation::kStep) {
        transition_time = 0.F;
        const HepticTimePowers powers = MakeHepticTimePowers(transition_time);

        // For Step changes, we simply set the transform to immediately take the
        // user to the given position and not reflect any movement-based
        // acceleration or velocity.
        SetInertialTransforms(glm::vec3(), glm::vec3(), glm::vec3(), glm::vec3(), glm::vec3(),
                              glm::vec3(), glm::vec3(), position, powers.heptic, powers.cubic);
    } else {
        // We ensure that velocity, acceleration, jerk, and position are
        // continuously interpolating from the current state.  Here, and
        // throughout, x is the position, v is the velocity, a is the
        // acceleration and j is the jerk.
        const glm::vec3 x_init = GetPosition(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 v_init = GetVelocity(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 a_init = GetAcceleration(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 j_init = GetJerk(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 x_target = position;

        // Use the square root of distance as the basis for the transition time
        // in order to have a roughly consistent magnitude acceleration.
        const float time_scale = std::sqrt(glm::distance(x_target, x_init));
        // Use the max time for distances above 10cm.
        const float max_time_scale = std::sqrt(0.1F);

        transition_time = ComputeTransitionTime(time_scale, max_time_scale);
        const HepticTimePowers powers = MakeHepticTimePowers(transition_time);

        // Computed by solving for heptic movement in
        // state_change_timeSeconds. Position, Velocity, Acceleration and Jerk
        // are computed here by solving the system of linear equations created
        // by setting the initial position, velocity, acceleration and jerk to
        // the current values, and the final state to the target position, with
        // no velocity, acceleration or jerk.
        //
        // Equation of motion is:
        //
        // f(t) == At^7 + Bt^6 + Ct^5 + Dt^4 + Et^3 + Ft^2 + Gt + H
        //
        // Where:
        //     A == heptic_term
        //     B == hexic_term
        //     C == quintic_term
        //     D == quartic_term
        //     E == cubic_term
        //     F == quadratic_term
        //     G == linear_term
        //     H == constant_term
        // t_end == state_change_timeSeconds
        //
        // And this system of equations is solved:
        //
        // Initial State:
        //                 f(0) == x_init
        //            df/d_t(0) == v_init
        //           d2f/d2t(0) == a_init
        //           d3f/d3t(0) == j_init
        // Final State:
        //             f(t_end) == x_target
        //         df/dt(t_end) == 0
        //       d2f/d2t(t_end) == 0
        //       d3f/d3t(t_end) == 0
        //
        // These can be solved via the following Mathematica command:
        // RowReduce[{{0,0,0,0,0,0,0,1,x},
        //            {0,0,0,0,0,0,1,0,v},
        //            {0,0,0,0,0,2,0,0,a},
        //            {0,0,0,0,6,0,0,0,j},
        //            {t^7,t^6,t^5,t^4,t^3,t^2,t,1,y},
        //            {7t^6,6t^5,5t^4,4t^3,3t^2,2t,1,0,0},
        //            {42t^5,30t^4,20t^3,12t^2,6t,2,0,0,0},
        //            {210t^4,120t^3,60t^2,24t,6,0,0,0,0}}]
        //
        // Where:
        //     x = x_init
        //     v = v_init
        //     a = a_init
        //     j = j_init
        //     t = state_change_timeSeconds
        //     y = x_target

        const glm::vec3 heptic_term =
                (1.F / (6.F * powers.t7)) *
                (1.F * powers.t3 * j_init + 12.F * powers.t2 * a_init + 60.F * powers.t1 * v_init +
                 120.F * x_init + -120.F * x_target);

        const glm::vec3 hexic_term =
                (1.F / (6.F * powers.t6)) *
                (-4.F * powers.t3 * j_init + -45.F * powers.t2 * a_init +
                 -216.F * powers.t1 * v_init + -420.F * x_init + 420.F * x_target);

        const glm::vec3 quintic_term =
                (1.F / (1.F * powers.t5)) *
                (1.F * powers.t3 * j_init + 10.F * powers.t2 * a_init + 45.F * powers.t1 * v_init +
                 84.F * x_init + -84.F * x_target);

        const glm::vec3 quartic_term =
                (1.F / (3.F * powers.t4)) *
                (-2.F * powers.t3 * j_init + -15.F * powers.t2 * a_init +
                 -60.F * powers.t1 * v_init + -105.F * x_init + 105.F * x_target);

        const glm::vec3 cubic_term = (1.F / 6.F) * j_init;

        const glm::vec3 quadratic_term = (1.F / 2.F) * a_init;

        const glm::vec3 linear_term = v_init;

        const glm::vec3 constant_term = x_init;

        SetInertialTransforms(heptic_term, hexic_term, quintic_term, quartic_term, cubic_term,
                              quadratic_term, linear_term, constant_term, powers.heptic,
                              powers.cubic);
    }
    position_change_start_time_ = model_time_ns_;
    position_change_end_time_ = model_time_ns_ + SecondsToNs(transition_time);
    zero_velocity_after_end_time_ = true;
}

void InertialModel::SetTargetVelocity(glm::vec3 velocity, PhysicalInterpolation mode) {
    float transition_time = kMinStateChangeTimeSeconds;
    if (mode == PhysicalInterpolation::kStep) {
        transition_time = 0.F;
        const HepticTimePowers powers = MakeHepticTimePowers(transition_time);

        // For Step changes, we simply set the transform to immediately move the
        // user at a given velocity starting from the current position.
        SetInertialTransforms(glm::vec3(), glm::vec3(), glm::vec3(), glm::vec3(), glm::vec3(),
                              glm::vec3(), velocity,
                              GetPosition(ParameterValueType::kCurrentNoAmbientMotion),
                              powers.heptic, powers.cubic);
    } else {
        // We ensure that velocity, acceleration, jerk, and position are
        // continuously interpolating from the current state.  Here, and
        // throughout, x is the position, v is the velocity, a is the
        // acceleration and j is the jerk.
        const glm::vec3 x_init = GetPosition(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 v_init = GetVelocity(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 a_init = GetAcceleration(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 j_init = GetJerk(ParameterValueType::kCurrentNoAmbientMotion);
        const glm::vec3 v_target = velocity;

        // Use the velocity difference as the basis for the transition time
        // in order to have a roughly consistent magnitude acceleration.
        const float time_scale = glm::distance(v_init, v_target);
        // Use the max time for velocity differences above 1m/s.
        constexpr float kMaxTimeScale = 1.F;

        transition_time = ComputeTransitionTime(time_scale, kMaxTimeScale);
        const HepticTimePowers powers = MakeHepticTimePowers(transition_time);

        // Computed by solving for hexic movement in
        // state_change_timeSeconds. Position, Velocity, Acceleration, and Jerk
        // are computed here by solving the system of linear equations created
        // by setting the initial position, velocity, acceleration and jerk to
        // the current values, and the final state to the target velocity, with
        // no acceleration or jerk.  Note that there is no target position
        // specified.
        //
        // Equation of motion is:
        //
        // f(t) == At^6 + Bt^5 + Ct^4 + Dt^3 + Et^2 + Ft + G
        //
        // Where:
        //     A == hexic_term
        //     B == quintic_term
        //     C == quartic_term
        //     D == cubic_term
        //     E == quadratic_term
        //     F == linear_term
        //     G == constant_term
        // t_end == state_change_timeSeconds
        //
        // And this system of equations is solved:
        //
        // Initial State:
        //                 f(0) == x_init
        //            df/d_t(0) == v_init
        //           d2f/d2t(0) == a_init
        //           d3f/d3t(0) == j_init
        // Final State:
        //         df/dt(t_end) == v_target
        //       d2f/d2t(t_end) == 0
        //       d3f/d3t(t_end) == 0
        //
        // These can be solved via the following Mathematica command:
        // RowReduce[{{0,0,0,0,0,0,1,x},
        //            {0,0,0,0,0,1,0,v},
        //            {0,0,0,0,2,0,0,a},
        //            {0,0,0,6,0,0,0,j},
        //            {6t^5,5t^4,4t^3,3t^2,2t,1,0,w},
        //            {30t^4,20t^3,12t^2,6t,2,0,0,0},
        //            {120t^3,60t^2,24t,6,0,0,0,0}}]
        //
        // Where:
        //     x = x_init
        //     v = v_init
        //     a = a_init
        //     j = j_init
        //     t = state_change_timeSeconds
        //     w = v_target

        const glm::vec3 hexic_term = (1.F / (12.F * powers.t5)) *
                                     (-1.F * powers.t2 * j_init + -6.F * powers.t1 * a_init +
                                      -12.F * v_init + 12.F * v_target);
        const glm::vec3 quintic_term =
                (1.F / (10.F * powers.t4)) * (3.F * powers.t2 * j_init + 16.F * powers.t1 * a_init +
                                              30.F * v_init + -30.F * v_target);
        const glm::vec3 quartic_term = (1.F / (8.F * powers.t3)) *
                                       (-3.F * powers.t2 * j_init + -12.F * powers.t1 * a_init +
                                        -20.F * v_init + 20.F * v_target);
        const glm::vec3 cubic_term = (1.F / 6.F) * j_init;
        const glm::vec3 quadratic_term = (1.F / 2.F) * a_init;
        const glm::vec3 linear_term = v_init;
        const glm::vec3 constant_term = x_init;

        SetInertialTransforms(glm::vec3(0.0F), hexic_term, quintic_term, quartic_term, cubic_term,
                              quadratic_term, linear_term, constant_term, powers.heptic,
                              powers.cubic);
    }
    position_change_start_time_ = model_time_ns_;
    position_change_end_time_ = model_time_ns_ + SecondsToNs(transition_time);
    zero_velocity_after_end_time_ = glm::length(velocity) <= kEpsilon;
}

void InertialModel::SetTargetRotation(glm::quat rotation, PhysicalInterpolation mode) {
    float transition_time = kMinStateChangeTimeSeconds;
    if (mode == PhysicalInterpolation::kStep) {
        transition_time = 0.F;
        // For Step changes, we simply set the transform to immediately set the
        // rotation to the target, with zero rotational velocity.
        SetStepRotation(rotation);
    } else {
        // Computed by solving for cubic movement in 4d space. Position and
        // Velocity in 4d space are computed here by solving the system of
        // linear equations created by setting the initial 4d position (i.e.
        // rotation), and velocity (i.e. rotational velocity) to the current
        // normalized values, and the final state to the target 4d position,
        // with zero velocity.
        //
        // Equation of motion is:
        //
        // f(t) == At^3 + Bt^2 + Ct + D
        //
        // Where:
        //     A == cubic_term
        //     B == quadratic_term
        //     C == linear_term
        //     D == constant_term
        // t_end == kRotationStateChangeTimeSeconds
        //
        // And this system of equations is solved:
        //
        // Initial State:
        //                 f(0) == x_init
        //            df/d_t(0) == v_init
        //            df/d_t(0) == a_init
        // Final State:
        //             f(t_end) == x_target
        //         df/dt(t_end) == 0
        //        df/d_t(t_end) == 0
        //
        // These can be solved via the following Mathematica command:
        // RowReduce[{{0,0,0,0,0,1,x},
        //            {0,0,0,0,1,0,v},
        //            {0,0,0,2,0,0,a},
        //            {t^5,t^4,t^3,t^2,t,1,y},
        //            {5t^4,4t^3,3t^2,2t,1,0,0},
        //            {20t^3,12t^2,6t,2,0,0,0}}]
        //
        // Where:
        //     x = x_init
        //     v = v_init
        //     a = a_init
        //     t = state_change_timeSeconds
        //     y = x_target

        const glm::vec4 current_rotation = CalculateRotationalState(
                rotation_quintic_, rotation_cubic_, rotation_after_end_cubic_,
                ParameterValueType::kCurrentNoAmbientMotion);

        const glm::vec4 current_rotational_velocity = CalculateRotationalState(
                rotational_velocity_quintic_, rotational_velocity_cubic_, glm::mat4x4(0.F),
                ParameterValueType::kCurrentNoAmbientMotion);

        const glm::vec4 current_rotational_acceleration = CalculateRotationalState(
                rotational_acceleration_quintic_, rotational_acceleration_cubic_, glm::mat4x4(0.F),
                ParameterValueType::kCurrentNoAmbientMotion);

        const float rotation_length = glm::length(current_rotation);

        // Rotation length should not be zero, but it may be possible by driving
        // the inertial model in an extreme way (i.e. well timed oscilations) to
        // hit this case.  In this case, we will simply do a step to the target.
        if (rotation_length == 0.F) {
            SetStepRotation(rotation);
            return;
        }

        // Scale rotation and rotational velocity such that the rotation is a unit
        // quaternion.
        glm::vec4 x_init = (1.F / rotation_length) * current_rotation;

        // Component of 4d velocity that is orthogonal to the current 4d normalized
        // rotation.
        const glm::vec4 scaled_rotational_velocity =
                (1.F / rotation_length) * current_rotational_velocity;
        glm::vec4 v_init =
                scaled_rotational_velocity - glm::dot(scaled_rotational_velocity, x_init) * x_init;

        // Component of 4d acceleration that is orthogonal to the current 4d normalized
        // rotation.
        const glm::vec4 scaled_rotational_acceleration =
                (1.F / rotation_length) * current_rotational_acceleration;
        const glm::vec4 a_init = scaled_rotational_acceleration -
                                 glm::dot(scaled_rotational_acceleration, x_init) * x_init;

        const glm::vec4 x_target = glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w);

        if (glm::distance(-x_init, x_target) < glm::distance(x_init, x_target)) {
            // If x_target is closer to the negation of x_init than x_init, then
            // negate x_init.
            x_init = -x_init;
            v_init = -v_init;
        }

        // Use the square root of the rotation distance as the basis for the
        // transition time in order to have a roughly consistent magnitude of
        // angular acceleration.
        const float time_scale = std::sqrt(glm::distance(x_init, x_target));
        // Use the max time for transitions of 180 degrees (i.e. distance 2 in
        // quaternion space).
        const float max_time_scale = std::numbers::sqrt2_v<float>;

        transition_time = ComputeTransitionTime(time_scale, max_time_scale);
        const QuinticCoefficients<glm::vec4> coeffs =
                SolveQuinticCoefficients(x_init, v_init, a_init, x_target, transition_time);

        rotation_quintic_ = glm::mat2x4(coeffs.quintic, coeffs.quartic);
        rotation_cubic_ =
                glm::mat4x4(coeffs.cubic, coeffs.quadratic, coeffs.linear, coeffs.constant);
        rotation_after_end_cubic_ =
                glm::mat4x4(glm::vec4(0.F), glm::vec4(0.F), glm::vec4(0.F), x_target);
        rotational_velocity_quintic_ = glm::mat2x4(glm::vec4(), 5.F * coeffs.quintic);
        rotational_velocity_cubic_ = glm::mat4x4(4.F * coeffs.quartic, 3.F * coeffs.cubic,
                                                 2.F * coeffs.quadratic, coeffs.linear);
        rotational_acceleration_quintic_ = glm::mat2x4(glm::vec4(), glm::vec4());
        rotational_acceleration_cubic_ = glm::mat4x4(20.F * coeffs.quintic, 12.F * coeffs.quartic,
                                                     6.F * coeffs.cubic, 2.F * coeffs.quadratic);
    }

    rotation_change_start_time_ = model_time_ns_;
    rotation_change_end_time_ = model_time_ns_ + SecondsToNs(transition_time);
}

void InertialModel::SetTarGetAmbientMotion(float bounds, PhysicalInterpolation mode) {
    if (mode == PhysicalInterpolation::kStep) {
        ambient_motion_value_quintic_ = glm::vec2(0.F);
        ambient_motion_value_cubic_ = glm::vec4(0.F, 0.F, 0.F, bounds);
        ambient_motion_first_deriv_quintic_ = glm::vec2(0.F);
        ambient_motion_first_deriv_cubic_ = glm::vec4(0.F);
        ambient_motion_second_deriv_quintic_ = glm::vec2(0.F);
        ambient_motion_second_deriv_cubic_ = glm::vec4(0.F);
        ambient_motion_change_start_time_ = model_time_ns_;
        ambient_motion_change_end_time_ = model_time_ns_;
    } else {
        // Ambient motion bounds expansion needs to be differentiable so we can
        // always compute the acceleration of the ambient motion.  This does the
        // same polynomial computation as the quintic rotation above with the
        // same coefficients, but in one dimension instead of 4.
        const float x_init = GetAmbientMotionBoundsValue(ParameterValueType::kCurrent);
        const float v_init = GetAmbientMotionBoundsDeriv(ParameterValueType::kCurrent);
        const float a_init = GetAmbientMotionBoundsSecondDeriv(ParameterValueType::kCurrent);
        const QuinticCoefficients<float> coeffs = SolveQuinticCoefficients(
                x_init, v_init, a_init, bounds, kMaxStateChangeTimeSeconds);

        ambient_motion_end_value_ = bounds;
        ambient_motion_value_quintic_ = glm::vec2(coeffs.quintic, coeffs.quartic);
        ambient_motion_value_cubic_ =
                glm::vec4(coeffs.cubic, coeffs.quadratic, coeffs.linear, coeffs.constant);
        ambient_motion_first_deriv_quintic_ = glm::vec2(0.F, 5.F * coeffs.quintic);
        ambient_motion_first_deriv_cubic_ = glm::vec4(4.F * coeffs.quartic, 3.F * coeffs.cubic,
                                                      2.F * coeffs.quadratic, coeffs.linear);
        ambient_motion_second_deriv_quintic_ = glm::vec2(0.F, 0.F);
        ambient_motion_second_deriv_cubic_ = glm::vec4(20.F * coeffs.quintic, 12.F * coeffs.quartic,
                                                       6.F * coeffs.cubic, 2.F * coeffs.quadratic);

        ambient_motion_change_start_time_ = model_time_ns_;
        ambient_motion_change_end_time_ = model_time_ns_ + SecondsToNs(kMaxStateChangeTimeSeconds);
    }
}

void InertialModel::SetWristTilt(float value, PhysicalInterpolation /*mode*/) {
    wrist_tilt_ = value;
}

glm::vec3 InertialModel::GetPosition(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return {};
    }

    glm::vec3 position = CalculateInertialState(position_heptic_, position_cubic_,
                                                position_after_end_cubic_, parameter_value_type);
    if (parameter_value_type == ParameterValueType::kCurrent) {
        const double time = static_cast<double>(model_time_ns_) /
                            1000000000.0;  // No narrowing conversion here.
        const glm::vec3 f = AmbientWaveValue(time);
        const glm::vec3 g =
                glm::vec3(1.F) * GetAmbientMotionBoundsValue(ParameterValueType::kCurrent);
        position += f * g;
    }
    return position;
}

glm::vec3 InertialModel::GetVelocity(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return {};
    }

    glm::vec3 velocity = CalculateInertialState(
            velocity_heptic_, velocity_cubic_,
            zero_velocity_after_end_time_ ? glm::mat4x3(0.F) : velocity_after_end_cubic_,
            parameter_value_type);
    if (parameter_value_type == ParameterValueType::kCurrent) {
        const double time = static_cast<double>(model_time_ns_) / 1000000000.0;
        const glm::vec3 f = AmbientWaveValue(time);
        // Apply the chain rule.
        const glm::vec3 df = AmbientWaveFirstDeriv(time);
        const glm::vec3 g =
                glm::vec3(1.F) * GetAmbientMotionBoundsValue(ParameterValueType::kCurrent);
        const glm::vec3 dg =
                glm::vec3(1.F) * GetAmbientMotionBoundsDeriv(ParameterValueType::kCurrent);
        // Apply the product rule.
        velocity += f * dg + df * g;
    }
    return velocity;
}

glm::vec3 InertialModel::GetAcceleration(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return {};
    }

    glm::vec3 acceleration = CalculateInertialState(acceleration_heptic_, acceleration_cubic_,
                                                    glm::mat4x3(0.F), parameter_value_type);
    if (parameter_value_type == ParameterValueType::kCurrent) {
        const double time = static_cast<double>(model_time_ns_) / 1000000000.0;
        const glm::vec3 f = AmbientWaveValue(time);
        // Apply the chain rule.
        const glm::vec3 df = AmbientWaveFirstDeriv(time);
        // Apply the chain rule twice.
        const glm::vec3 d2f = -f * kAmbientFrequencyVec * kAmbientFrequencyVec;
        const glm::vec3 g =
                glm::vec3(1.F) * GetAmbientMotionBoundsValue(ParameterValueType::kCurrent);
        const glm::vec3 dg =
                glm::vec3(1.F) * GetAmbientMotionBoundsDeriv(ParameterValueType::kCurrent);
        const glm::vec3 d2g =
                glm::vec3(1.F) * GetAmbientMotionBoundsSecondDeriv(ParameterValueType::kCurrent);
        // Apply the product rule twice.
        acceleration += f * d2g + 2.F * df * dg + d2f * g;
    }
    return acceleration;
}

glm::vec3 InertialModel::GetJerk(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return {};
    }

    return CalculateInertialState(jerk_heptic_, jerk_cubic_, glm::mat4x3(0.F),
                                  parameter_value_type);
}

glm::quat InertialModel::GetRotation(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return {};
    }

    const glm::vec4 rotation_vector = CalculateRotationalState(
            rotation_quintic_, rotation_cubic_, rotation_after_end_cubic_, parameter_value_type);
    if (glm::length(rotation_vector) <= std::numeric_limits<float>::epsilon()) {
        return glm::quat(1.F, 0.F, 0.F, 0.F);
    }

    const glm::quat rotation(rotation_vector.w, rotation_vector.x, rotation_vector.y,
                             rotation_vector.z);

    return glm::normalize(rotation);
}

glm::vec3 InertialModel::GetRotationalVelocity(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return {};
    }

    const glm::vec4 rotation_vector = CalculateRotationalState(
            rotation_quintic_, rotation_cubic_, rotation_after_end_cubic_, parameter_value_type);
    const float rotation_vector_length = glm::length(rotation_vector);

    // Rotation length should not be zero, but it may be possible by driving
    // the inertial model in an extreme way (i.e. well timed oscilations) to
    // hit this case.  In this case, we will simply throw away this target
    // state.
    if (rotation_vector_length == 0.F) {
        return glm::vec3(0.F);
    }

    const glm::vec4 rotation_normalized = (1.F / rotation_vector_length) * rotation_vector;
    const glm::quat rotation = glm::quat(rotation_normalized.w, rotation_normalized.x,
                                         rotation_normalized.y, rotation_normalized.z);

    const glm::vec4 scaled_derivative =
            (1.F / rotation_vector_length) *
            CalculateRotationalState(rotational_velocity_quintic_, rotational_velocity_cubic_,
                                     glm::mat4x4(0.F), parameter_value_type);

    const glm::vec4 rotation_derivative =
            scaled_derivative -
            glm::dot(scaled_derivative, rotation_normalized) * rotation_normalized;

    const glm::quat rotation_derivative_quat =
            glm::quat(rotation_derivative.w, rotation_derivative.x, rotation_derivative.y,
                      rotation_derivative.z);

    const glm::quat rotation_conjugate = glm::conjugate(rotation);

    const glm::quat angular_velocity = 2.F * (rotation_derivative_quat * rotation_conjugate);

    return {angular_velocity.x, angular_velocity.y, angular_velocity.z};
}

float InertialModel::GetAmbientMotion(ParameterValueType parameter_value_type) const {
    return GetAmbientMotionBoundsValue(parameter_value_type);
}

float InertialModel::GetWristTilt(ParameterValueType /*parameter_value_type*/) const {
    return wrist_tilt_;
}

void InertialModel::SetInertialTransforms(
        const glm::vec3& heptic_coefficient, const glm::vec3& hexic_coefficient,
        const glm::vec3& quintic_coefficient, const glm::vec3& quartic_coefficient,
        const glm::vec3& cubic_coefficient, const glm::vec3& quadratic_coefficient,
        const glm::vec3& linear_coefficient, const glm::vec3& constant_coefficient,
        const glm::vec4& heptic_time_vector, const glm::vec4& cubic_time_vector) {
    position_heptic_ = glm::mat4x3(heptic_coefficient, hexic_coefficient, quintic_coefficient,
                                   quartic_coefficient);
    position_cubic_ = glm::mat4x3(cubic_coefficient, quadratic_coefficient, linear_coefficient,
                                  constant_coefficient);

    velocity_heptic_ = glm::mat4x3(glm::vec3(0.0F), 7.F * heptic_coefficient,
                                   6.F * hexic_coefficient, 5.F * quintic_coefficient);
    velocity_cubic_ = glm::mat4x3(4.F * quartic_coefficient, 3.F * cubic_coefficient,
                                  2.F * quadratic_coefficient, linear_coefficient);

    acceleration_heptic_ = glm::mat4x3(glm::vec3(0.0F), glm::vec3(0.0F), 42.F * heptic_coefficient,
                                       30.F * hexic_coefficient);
    acceleration_cubic_ = glm::mat4x3(20.F * quintic_coefficient, 12.F * quartic_coefficient,
                                      6.F * cubic_coefficient, 2.F * quadratic_coefficient);

    jerk_heptic_ = glm::mat4x3(glm::vec3(0.0F), glm::vec3(0.0F), glm::vec3(0.0F),
                               210.F * heptic_coefficient);
    jerk_cubic_ = glm::mat4x3(120.F * hexic_coefficient, 60.F * quintic_coefficient,
                              24.F * quartic_coefficient, 6.F * cubic_coefficient);

    const glm::vec3 end_position =
            position_cubic_ * cubic_time_vector + position_heptic_ * heptic_time_vector;
    const glm::vec3 end_velocity =
            velocity_cubic_ * cubic_time_vector + velocity_heptic_ * heptic_time_vector;

    position_after_end_cubic_ = glm::mat4x3(glm::vec3(0.0F), glm::vec3(0.0F), end_velocity,
                                            end_position - cubic_time_vector.z * end_velocity);
    velocity_after_end_cubic_ =
            glm::mat4x3(glm::vec3(0.0F), glm::vec3(0.0F), glm::vec3(0.0F), end_velocity);
}

glm::vec3 InertialModel::CalculateInertialState(const glm::mat4x3& heptic_transform,
                                                const glm::mat4x3& cubic_transform,
                                                const glm::mat4x3& after_end_cubic_transform,
                                                ParameterValueType parameter_value_type) const {
    const uint64_t requested_time_ns = parameter_value_type == ParameterValueType::kTarget
                                               ? position_change_end_time_
                                               : model_time_ns_;

    const float time1 = NsToSeconds(requested_time_ns - position_change_start_time_);
    if (requested_time_ns < position_change_end_time_) {
        const HepticTimePowers powers = MakeHepticTimePowers(time1);
        return cubic_transform * powers.cubic + heptic_transform * powers.heptic;
    }
    return after_end_cubic_transform * MakeCubicTimeVector(time1);
}

glm::vec4 InertialModel::CalculateRotationalState(const glm::mat2x4& quintic_transform,
                                                  const glm::mat4x4& cubic_transform,
                                                  const glm::mat4x4& after_end_cubic_transform,
                                                  ParameterValueType parameter_value_type) const {
    const uint64_t requested_time_ns = parameter_value_type == ParameterValueType::kTarget
                                               ? rotation_change_end_time_
                                               : model_time_ns_;

    const float time1 = NsToSeconds(requested_time_ns - rotation_change_start_time_);
    const glm::vec4 cubic_time_vector = MakeCubicTimeVector(time1);
    if (requested_time_ns < rotation_change_end_time_) {
        return quintic_transform * MakeQuinticTimeVector(time1) +
               cubic_transform * cubic_time_vector;
    }
    return after_end_cubic_transform * cubic_time_vector;
}

float InertialModel::GetAmbientMotionBoundsValue(ParameterValueType parameter_value_type) const {
    if (parameter_value_type == ParameterValueType::kDefault) {
        return 0.F;
    }
    return EvaluateAmbientMotionBounds(ambient_motion_value_quintic_, ambient_motion_value_cubic_,
                                       ambient_motion_end_value_, parameter_value_type);
}

float InertialModel::GetAmbientMotionBoundsDeriv(ParameterValueType parameter_value_type) const {
    return EvaluateAmbientMotionBounds(ambient_motion_first_deriv_quintic_,
                                       ambient_motion_first_deriv_cubic_, 0.F,
                                       parameter_value_type);
}

float InertialModel::GetAmbientMotionBoundsSecondDeriv(
        ParameterValueType parameter_value_type) const {
    return EvaluateAmbientMotionBounds(ambient_motion_second_deriv_quintic_,
                                       ambient_motion_second_deriv_cubic_, 0.F,
                                       parameter_value_type);
}

void InertialModel::SetStepRotation(glm::quat rotation) {
    rotation_quintic_ = glm::mat2x4(glm::vec4(0.F), glm::vec4(0.F));
    rotation_cubic_ = glm::mat4x4(glm::vec4(0.F), glm::vec4(0.F), glm::vec4(0.F),
                                  glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
    rotation_after_end_cubic_ = rotation_cubic_;
    rotational_velocity_quintic_ = glm::mat2x4(0.F);
    rotational_velocity_cubic_ = glm::mat4x4(0.F);
    rotational_acceleration_quintic_ = glm::mat2x4(0.F);
    rotational_acceleration_cubic_ = glm::mat4x4(0.F);
}

float InertialModel::EvaluateAmbientMotionBounds(const glm::vec2& quintic, const glm::vec4& cubic,
                                                 float end_value,
                                                 ParameterValueType parameter_value_type) const {
    if (parameter_value_type != ParameterValueType::kTarget &&
        model_time_ns_ < ambient_motion_change_end_time_) {
        const float time1 = NsToSeconds(model_time_ns_ - ambient_motion_change_start_time_);
        return glm::dot(quintic, MakeQuinticTimeVector(time1)) +
               glm::dot(cubic, MakeCubicTimeVector(time1));
    }
    return end_value;
}

archive::IWriter& operator<<(archive::IWriter& w, const InertialModel& im) {
    w << im.position_change_start_time_ << im.position_heptic_ << im.position_cubic_
      << im.velocity_heptic_ << im.velocity_cubic_ << im.acceleration_heptic_
      << im.acceleration_cubic_ << im.jerk_heptic_ << im.jerk_cubic_ << im.position_change_end_time_
      << im.zero_velocity_after_end_time_ << im.position_after_end_cubic_
      << im.velocity_after_end_cubic_ << im.rotation_change_start_time_ << im.rotation_quintic_
      << im.rotation_cubic_ << im.rotation_after_end_cubic_ << im.rotational_velocity_quintic_
      << im.rotational_velocity_cubic_ << im.rotational_acceleration_quintic_
      << im.rotational_acceleration_cubic_ << im.rotation_change_end_time_
      << im.ambient_motion_change_start_time_ << im.ambient_motion_end_value_
      << im.ambient_motion_value_quintic_ << im.ambient_motion_value_cubic_
      << im.ambient_motion_first_deriv_quintic_ << im.ambient_motion_first_deriv_cubic_
      << im.ambient_motion_second_deriv_quintic_ << im.ambient_motion_second_deriv_cubic_
      << im.ambient_motion_change_end_time_ << im.wrist_tilt_ << im.model_time_ns_;

    return w;
}

absl::Status ReadValue(archive::IReader& r, InertialModel& im) {
    return ReadValue(r, im.position_change_start_time_, im.position_heptic_, im.position_cubic_,
                     im.velocity_heptic_, im.velocity_cubic_, im.acceleration_heptic_,
                     im.acceleration_cubic_, im.jerk_heptic_, im.jerk_cubic_,
                     im.position_change_end_time_, im.zero_velocity_after_end_time_,
                     im.position_after_end_cubic_, im.velocity_after_end_cubic_,
                     im.rotation_change_start_time_, im.rotation_quintic_, im.rotation_cubic_,
                     im.rotation_after_end_cubic_, im.rotational_velocity_quintic_,
                     im.rotational_velocity_cubic_, im.rotational_acceleration_quintic_,
                     im.rotational_acceleration_cubic_, im.rotation_change_end_time_,
                     im.ambient_motion_change_start_time_, im.ambient_motion_end_value_,
                     im.ambient_motion_value_quintic_, im.ambient_motion_value_cubic_,
                     im.ambient_motion_first_deriv_quintic_, im.ambient_motion_first_deriv_cubic_,
                     im.ambient_motion_second_deriv_quintic_, im.ambient_motion_second_deriv_cubic_,
                     im.ambient_motion_change_end_time_, im.wrist_tilt_, im.model_time_ns_);
}

}  // namespace goldfish::physics
