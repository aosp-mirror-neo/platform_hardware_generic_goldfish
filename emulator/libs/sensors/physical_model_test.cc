// Copyright (C) 2017 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "goldfish/sensors/physical_model.h"

#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/vec3.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "absl/time/time.h"

#include "android/goldfish/fake_hardware_config.h"
#include "android/goldfish/hardware_config.h"
#include "goldfish/archive/deque_archive.h"
#include "goldfish/eventing/event_sources.h"
#include "goldfish/eventing/with_callbacks.h"
#include "goldfish/physics/glm_helpers.h"
#include "goldfish/system/testing/fake_clocks.h"

namespace goldfish::sensors {

using goldfish::physics::kMinStateChangeTimeSeconds;
using goldfish::physics::NsToSeconds;
using goldfish::physics::SecondsToNs;

static constexpr vec3 kDefaultAccelerometer = {0.f, 9.81f, 0.f};

#define EXPECT_VEC3_NEAR(e, a, d) \
    EXPECT_NEAR(e.x, a.x, d);     \
    EXPECT_NEAR(e.y, a.y, d);     \
    EXPECT_NEAR(e.z, a.z, d);

class PhysicalModelTest : public ::testing::Test {
  protected:
    void SetUp() override {
        model = std::make_unique<PhysicalModel>(
                android::goldfish::FakeHardwareConfig::GetHwConfig(), virtual_clock_);
    }

    ::goldfish::system::testing::FakeVirtualClock virtual_clock_;
    std::unique_ptr<PhysicalModel> model;
};

TEST_F(PhysicalModelTest, DefaultInertialSensorValues) {
    model->SetCurrentTime(1000000000L);
    size_t measurement_id;
    vec3 accelerometer = model->GetAccelerometer(&measurement_id);
    EXPECT_VEC3_NEAR((vec3{0.f, 9.81f, 0.f}), accelerometer, 0.001f);

    vec3 gyro = model->GetGyroscope(&measurement_id);
    EXPECT_VEC3_NEAR((vec3{0.f, 0.f, 0.f}), gyro, 0.001f);
}

TEST_F(PhysicalModelTest, ConstantMeasurementId) {
    model->SetCurrentTime(1000000000L);
    size_t measurement_id0;
    model->GetAccelerometer(&measurement_id0);

    model->SetCurrentTime(2000000000L);

    size_t measurement_id1;
    model->GetAccelerometer(&measurement_id1);

    EXPECT_EQ(measurement_id0, measurement_id1);
}

TEST_F(PhysicalModelTest, NewMeasurementId) {
    model->SetCurrentTime(1000000000L);
    size_t measurement_id0;
    model->GetAccelerometer(&measurement_id0);

    model->SetCurrentTime(2000000000L);

    vec3 targetPosition;
    targetPosition.x = 2.0f;
    targetPosition.y = 3.0f;
    targetPosition.z = 4.0f;
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kSmooth);

    size_t measurement_id1;
    model->GetAccelerometer(&measurement_id1);

    EXPECT_NE(measurement_id0, measurement_id1);
}

TEST_F(PhysicalModelTest, SetTargetPosition) {
    model->SetCurrentTime(0L);
    vec3 targetPosition;
    targetPosition.x = 2.0f;
    targetPosition.y = 3.0f;
    targetPosition.z = 4.0f;
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kStep);

    model->SetCurrentTime(500000000L);

    vec3 currentPosition = model->GetParameterPosition(ParameterValueType::kCurrent);

    EXPECT_VEC3_NEAR(targetPosition, currentPosition, 0.0001f);
}

TEST_F(PhysicalModelTest, SetTargetRotation) {
    model->SetCurrentTime(0L);
    vec3 targetRotation;
    targetRotation.x = 45.0f;
    targetRotation.y = 10.0f;
    targetRotation.z = 4.0f;
    model->SetTargetRotation(targetRotation, PhysicalInterpolation::kStep);

    model->SetCurrentTime(500000000L);
    vec3 currentRotation = model->GetParameterRotation(ParameterValueType::kCurrent);

    EXPECT_VEC3_NEAR(targetRotation, currentRotation, 0.0001f);
}

struct GravityTestCase {
    glm::vec3 target_rotation;
    glm::vec3 expected_acceleration;
};

const GravityTestCase gravityTestCases[] = {
    {{0.0f, 0.0f, 0.0f}, {0.0f, 9.81f, 0.0f}},    {{90.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -9.81f}},
    {{-90.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 9.81f}},  {{0.0f, 90.0f, 0.0f}, {0.0f, 9.81f, 0.0f}},
    {{0.0f, 0.0f, 90.0f}, {9.81f, 0.0f, 0.0f}},   {{0.0f, 0.0f, -90.0f}, {-9.81f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 180.0f}, {0.0f, -9.81f, 0.0f}},
};

TEST_F(PhysicalModelTest, GravityAcceleration) {
    for (const auto& testCase : gravityTestCases) {
        model->SetCurrentTime(1000000000L);

        vec3 targetRotation;
        targetRotation.x = testCase.target_rotation.x;
        targetRotation.y = testCase.target_rotation.y;
        targetRotation.z = testCase.target_rotation.z;

        model->SetTargetRotation(targetRotation, PhysicalInterpolation::kSmooth);

        model->SetCurrentTime(2000000000L);

        size_t measurement_id;
        vec3 accelerometer = model->GetAccelerometer(&measurement_id);

        EXPECT_VEC3_NEAR(testCase.expected_acceleration, accelerometer, 0.01f);
    }
}

TEST_F(PhysicalModelTest, GravityOnlyAcceleration) {
    model->SetCurrentTime(1000000000L);

    vec3 targetPosition;
    targetPosition.x = 2.0f;
    targetPosition.y = 3.0f;
    targetPosition.z = 4.0f;
    // at 1 second we move the target to (2, 3, 4)
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kSmooth);

    model->SetCurrentTime(2000000000L);
    // at 2 seconds the target is still at (2, 3, 4);
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kStep);

    size_t measurement_id;
    // the acceleration is expected to be close to zero at this point.
    vec3 currentAcceleration = model->GetAccelerometer(&measurement_id);
    EXPECT_VEC3_NEAR(kDefaultAccelerometer, currentAcceleration, 0.01f);
}

TEST_F(PhysicalModelTest, NonInstantaneousRotation) {
    model->SetCurrentTime(0L);

    vec3 startRotation;
    startRotation.x = 0.f;
    startRotation.y = 0.f;
    startRotation.z = 0.f;
    model->SetTargetRotation(startRotation, PhysicalInterpolation::kStep);

    model->SetCurrentTime(1000000000L);
    vec3 newRotation;
    newRotation.x = -0.5f;
    newRotation.y = 0.0f;
    newRotation.z = 0.0f;
    model->SetTargetRotation(newRotation, PhysicalInterpolation::kSmooth);

    model->SetCurrentTime(1000000000L + SecondsToNs(kMinStateChangeTimeSeconds / 2.f));

    size_t measurement_id;
    vec3 currentGyro = model->GetGyroscope(&measurement_id);
    EXPECT_LE(currentGyro.x, -0.01f);
    EXPECT_NEAR(currentGyro.y, 0.0, 0.000001f);
    EXPECT_NEAR(currentGyro.z, 0.0, 0.000001f);
}

TEST_F(PhysicalModelTest, InstantaneousRotation) {
    model->SetCurrentTime(0L);

    vec3 startRotation;
    startRotation.x = 0.f;
    startRotation.y = 0.f;
    startRotation.z = 0.f;
    model->SetTargetRotation(startRotation, PhysicalInterpolation::kStep);

    model->SetCurrentTime(1000000000L);
    vec3 newRotation;
    newRotation.x = 180.0f;
    newRotation.y = 0.0f;
    newRotation.z = 0.0f;
    model->SetTargetRotation(newRotation, PhysicalInterpolation::kStep);

    size_t measurement_id;
    vec3 currentGyro = model->GetGyroscope(&measurement_id);
    EXPECT_VEC3_NEAR((vec3{0.f, 0.f, 0.f}), currentGyro, 0.000001f);
}

TEST_F(PhysicalModelTest, OverrideAccelerometer) {
    model->SetCurrentTime(0L);

    size_t initial_measurement_id;
    model->GetAccelerometer(&initial_measurement_id);

    vec3 overrideValue;
    overrideValue.x = 1.f;
    overrideValue.y = 2.f;
    overrideValue.z = 3.f;
    model->OverrideAccelerometer(overrideValue);

    size_t override_measurement_id;
    vec3 sensorOverriddenValue = model->GetAccelerometer(&override_measurement_id);
    EXPECT_VEC3_NEAR(overrideValue, sensorOverriddenValue, 0.000001f);

    EXPECT_NE(initial_measurement_id, override_measurement_id);

    vec3 targetPosition;
    targetPosition.x = 0.f;
    targetPosition.y = 0.f;
    targetPosition.z = 0.f;
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kStep);

    size_t physical_measurement_id;
    vec3 sensorPhysicalValue = model->GetAccelerometer(&physical_measurement_id);
    EXPECT_VEC3_NEAR(kDefaultAccelerometer, sensorPhysicalValue, 0.000001f);

    EXPECT_NE(physical_measurement_id, override_measurement_id);
    EXPECT_NE(physical_measurement_id, initial_measurement_id);
}

TEST_F(PhysicalModelTest, SetRotatedIMUResults) {
    model->SetCurrentTime(0L);

    const vec3 initialRotation{45.0f, 10.0f, 4.0f};

    model->SetTargetRotation(initialRotation, PhysicalInterpolation::kStep);

    const vec3 initialPosition{2.0f, 3.0f, 4.0f};
    model->SetTargetPosition(initialPosition, PhysicalInterpolation::kStep);

    const glm::quat quaternionRotation = glm::toQuat(
            glm::eulerAngleXYZ(glm::radians(initialRotation.x), glm::radians(initialRotation.y),
                               glm::radians(initialRotation.z)));

    uint64_t time = 500000000UL;
    const uint64_t stepNs = 1000UL;
    const uint64_t maxConvergenceTimeNs = time + SecondsToNs(5.0f);

    const vec3 targetPosition{1.0f, 2.0f, 3.0f};

    static bool targetStateChanged = false;
    static bool physicalStateChanging = false;

    auto scoped = android::base::eventing::MakeScopedCallback(
            *model, [&](PhysicalModelChangeEvent event) {
                switch (event.type) {
                case PhysicalModelChangeEvent::Type::kPhysicalStateChanging:
                    physicalStateChanging = true;
                    break;
                case PhysicalModelChangeEvent::Type::kPhysicalStateStabilized:
                    physicalStateChanging = false;
                    break;
                case PhysicalModelChangeEvent::Type::kTargetStateChanged:
                    targetStateChanged = true;
                    break;
                default:
                    break;
                }
            });

    model->SetCurrentTime(time);
    EXPECT_FALSE(physicalStateChanging);
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kSmooth);
    EXPECT_TRUE(targetStateChanged);
    EXPECT_TRUE(physicalStateChanging);
    targetStateChanged = false;

    const glm::vec3 gravity(0.f, 9.81f, 0.f);

    glm::vec3 velocity(0.f);
    glm::vec3 position(initialPosition.x, initialPosition.y, initialPosition.z);
    const float stepSeconds = NsToSeconds(stepNs);
    size_t prevMeasurementId = -1;
    time += stepNs / 2;

    size_t iteration = 0;
    while (physicalStateChanging) {
        SCOPED_TRACE(testing::Message() << "Iteration " << iteration);
        ++iteration;

        ASSERT_LT(time, maxConvergenceTimeNs) << "Physical state did not stabilize";
        model->SetCurrentTime(time);
        size_t measurementId;
        const vec3 measuredAcceleration = model->GetAccelerometer(&measurementId);
        ASSERT_NE(prevMeasurementId, measurementId);
        prevMeasurementId = measurementId;
        const glm::vec3 acceleration(measuredAcceleration.x, measuredAcceleration.y,
                                     measuredAcceleration.z);
        velocity += (quaternionRotation * acceleration - gravity) * stepSeconds;
        position += velocity * stepSeconds;
        time += stepNs;
    }

    const vec3 integratedPosition{position.x, position.y, position.z};

    EXPECT_VEC3_NEAR(targetPosition, integratedPosition, 0.01f);

    EXPECT_FALSE(targetStateChanged);
}

TEST_F(PhysicalModelTest, SetRotationIMUResults) {
    model->SetCurrentTime(0L);

    vec3 initialRotation{45.0f, 10.0f, 4.0f};

    model->SetTargetRotation(initialRotation, PhysicalInterpolation::kStep);

    uint64_t time = 0UL;
    const uint64_t stepNs = 5000UL;
    const uint64_t maxConvergenceTimeNs = time + SecondsToNs(5.0f);

    vec3 targetRotation{-10.0f, 20.0f, 45.0f};

    static bool targetStateChanged = false;
    static bool physicalStateChanging = false;
    auto scoped = android::base::eventing::MakeScopedCallback(
            *model, [&](PhysicalModelChangeEvent event) {
                switch (event.type) {
                case PhysicalModelChangeEvent::Type::kPhysicalStateChanging:
                    physicalStateChanging = true;
                    break;
                case PhysicalModelChangeEvent::Type::kPhysicalStateStabilized:
                    physicalStateChanging = false;
                    break;
                case PhysicalModelChangeEvent::Type::kTargetStateChanged:
                    targetStateChanged = true;
                    break;
                default:
                    break;
                }
            });

    model->SetTargetRotation(targetRotation, PhysicalInterpolation::kSmooth);
    EXPECT_TRUE(targetStateChanged);
    EXPECT_TRUE(physicalStateChanging);
    targetStateChanged = false;

    glm::quat rotation = glm::toQuat(glm::eulerAngleXYZ(glm::radians(initialRotation.x),
                                                        glm::radians(initialRotation.y),
                                                        glm::radians(initialRotation.z)));
    const float stepSeconds = NsToSeconds(stepNs);
    size_t prevMeasurementId = -1;
    time += stepNs / 2;
    while (physicalStateChanging) {
        ASSERT_LT(time, maxConvergenceTimeNs) << "Physical state did not stabilize";
        model->SetCurrentTime(time);
        size_t measurementId;
        const vec3 measuredGyroscope = model->GetGyroscope(&measurementId);
        ASSERT_NE(prevMeasurementId, measurementId);
        prevMeasurementId = measurementId;
        const glm::vec3 deviceSpaceRotationalVelocity(measuredGyroscope.x, measuredGyroscope.y,
                                                      measuredGyroscope.z);
        const glm::vec3 rotationalVelocity = rotation * deviceSpaceRotationalVelocity;
        const glm::mat4 deltaRotationMatrix = glm::eulerAngleXYZ(
                rotationalVelocity.x * stepSeconds, rotationalVelocity.y * stepSeconds,
                rotationalVelocity.z * stepSeconds);

        rotation = glm::quat_cast(deltaRotationMatrix) * rotation;
        time += stepNs;
    }

    const glm::quat targetRotationQuat = glm::toQuat(
            glm::eulerAngleXYZ(glm::radians(targetRotation.x), glm::radians(targetRotation.y),
                               glm::radians(targetRotation.z)));

    EXPECT_NEAR(targetRotationQuat.x, rotation.x, 0.0001f);
    EXPECT_NEAR(targetRotationQuat.y, rotation.y, 0.0001f);
    EXPECT_NEAR(targetRotationQuat.z, rotation.z, 0.0001f);
    EXPECT_NEAR(targetRotationQuat.w, rotation.w, 0.0001f);

    EXPECT_FALSE(targetStateChanged);
}

TEST_F(PhysicalModelTest, MoveWhileRotating) {
    model->SetCurrentTime(0L);

    const vec3 initialRotation{45.0f, 10.0f, 4.0f};

    model->SetTargetRotation(initialRotation, PhysicalInterpolation::kStep);

    const vec3 initialPosition{2.0f, 3.0f, 4.0f};
    model->SetTargetPosition(initialPosition, PhysicalInterpolation::kStep);

    uint64_t time = 0UL;
    const uint64_t stepNs = 5000UL;
    const uint64_t maxConvergenceTimeNs = time + SecondsToNs(5.0f);

    const vec3 targetPosition{1.0f, 2.0f, 3.0f};
    const vec3 targetRotation{-10.0f, 20.0f, 45.0f};

    static bool targetStateChanged = false;
    static bool physicalStateChanging = false;
    auto scoped = android::base::eventing::MakeScopedCallback(
            *model, [&](PhysicalModelChangeEvent event) {
                switch (event.type) {
                case PhysicalModelChangeEvent::Type::kPhysicalStateChanging:
                    physicalStateChanging = true;
                    break;
                case PhysicalModelChangeEvent::Type::kPhysicalStateStabilized:
                    physicalStateChanging = false;
                    break;
                case PhysicalModelChangeEvent::Type::kTargetStateChanged:
                    targetStateChanged = true;
                    break;
                default:
                    break;
                }
            });

    model->SetTargetRotation(targetRotation, PhysicalInterpolation::kSmooth);
    model->SetTargetPosition(targetPosition, PhysicalInterpolation::kSmooth);
    EXPECT_TRUE(targetStateChanged);
    EXPECT_TRUE(physicalStateChanging);
    targetStateChanged = false;

    glm::quat rotation = glm::toQuat(glm::eulerAngleXYZ(glm::radians(initialRotation.x),
                                                        glm::radians(initialRotation.y),
                                                        glm::radians(initialRotation.z)));
    const glm::vec3 gravity(0.f, 9.81f, 0.f);
    glm::vec3 velocity(0.f);
    glm::vec3 position(initialPosition.x, initialPosition.y, initialPosition.z);

    const float stepSeconds = NsToSeconds(stepNs);
    size_t prevGyroMeasurementId = -1;
    size_t prevAccelMeasurementId = -1;
    time += stepNs / 2;
    while (physicalStateChanging) {
        ASSERT_LT(time, maxConvergenceTimeNs) << "Physical state did not stabilize";
        model->SetCurrentTime(time);
        size_t gyroMeasurementId;
        const vec3 measuredGyroscope = model->GetGyroscope(&gyroMeasurementId);
        ASSERT_NE(prevGyroMeasurementId, gyroMeasurementId);
        prevGyroMeasurementId = gyroMeasurementId;
        const glm::vec3 deviceSpaceRotationalVelocity(measuredGyroscope.x, measuredGyroscope.y,
                                                      measuredGyroscope.z);
        const glm::vec3 rotationalVelocity = rotation * deviceSpaceRotationalVelocity;
        const glm::mat4 deltaRotationMatrix = glm::eulerAngleXYZ(
                rotationalVelocity.x * stepSeconds, rotationalVelocity.y * stepSeconds,
                rotationalVelocity.z * stepSeconds);

        rotation = glm::quat_cast(deltaRotationMatrix) * rotation;

        size_t accelMeasurementId;
        const vec3 measuredAcceleration = model->GetAccelerometer(&accelMeasurementId);
        EXPECT_NE(prevAccelMeasurementId, accelMeasurementId);
        prevAccelMeasurementId = accelMeasurementId;
        const glm::vec3 acceleration(measuredAcceleration.x, measuredAcceleration.y,
                                     measuredAcceleration.z);
        velocity += (rotation * acceleration - gravity) * stepSeconds;
        position += velocity * stepSeconds;

        time += stepNs;
    }

    const glm::quat targetRotationQuat = glm::toQuat(
            glm::eulerAngleXYZ(glm::radians(targetRotation.x), glm::radians(targetRotation.y),
                               glm::radians(targetRotation.z)));

    EXPECT_NEAR(targetRotationQuat.x, rotation.x, 0.0001f);
    EXPECT_NEAR(targetRotationQuat.y, rotation.y, 0.0001f);
    EXPECT_NEAR(targetRotationQuat.z, rotation.z, 0.0001f);
    EXPECT_NEAR(targetRotationQuat.w, rotation.w, 0.0001f);

    vec3 integratedPosition{position.x, position.y, position.z};

    EXPECT_VEC3_NEAR(targetPosition, integratedPosition, 0.001f);

    EXPECT_FALSE(targetStateChanged);
}

TEST_F(PhysicalModelTest, SetVelocityAndPositionWhileRotating) {
    model->SetCurrentTime(0L);

    const vec3 initialRotation{45.0f, 10.0f, 4.0f};

    model->SetTargetRotation(initialRotation, PhysicalInterpolation::kStep);

    const vec3 initialPosition{2.0f, 3.0f, 4.0f};
    model->SetTargetPosition(initialPosition, PhysicalInterpolation::kStep);

    vec3 intermediateVelocity{1.0f, 1.0f, 1.0f};

    uint64_t time = 0UL;
    const uint64_t stepNs = 5000UL;

    const vec3 targetPosition{1.0f, 2.0f, 3.0f};

    const vec3 intermediateRotation{100.0f, -30.0f, -10.0f};

    const vec3 targetRotation{-10.0f, 20.0f, 45.0f};

    bool targetStateChanged = false;
    bool physicalStateChanging = false;
    auto scoped = android::base::eventing::MakeScopedCallback(
            *model, [&](PhysicalModelChangeEvent event) {
                switch (event.type) {
                case PhysicalModelChangeEvent::Type::kPhysicalStateChanging:
                    physicalStateChanging = true;
                    break;
                case PhysicalModelChangeEvent::Type::kPhysicalStateStabilized:
                    physicalStateChanging = false;
                    break;
                case PhysicalModelChangeEvent::Type::kTargetStateChanged:
                    targetStateChanged = true;
                    break;
                default:
                    break;
                }
            });

    model->SetTargetRotation(intermediateRotation, PhysicalInterpolation::kSmooth);
    model->SetTargetVelocity(intermediateVelocity, PhysicalInterpolation::kSmooth);
    EXPECT_TRUE(targetStateChanged);
    EXPECT_TRUE(physicalStateChanging);
    targetStateChanged = false;

    glm::quat rotation = glm::toQuat(glm::eulerAngleXYZ(glm::radians(initialRotation.x),
                                                        glm::radians(initialRotation.y),
                                                        glm::radians(initialRotation.z)));
    const glm::vec3 gravity(0.f, 9.81f, 0.f);
    glm::vec3 velocity(0.f);
    glm::vec3 position(initialPosition.x, initialPosition.y, initialPosition.z);

    const float stepSeconds = NsToSeconds(stepNs);
    const float maxConvergenceTimeNs = time + SecondsToNs(5.0f);
    size_t prevGyroMeasurementId = -1;
    size_t prevAccelMeasurementId = -1;
    time += stepNs / 2;
    int stepsRemainingAfterStable = 10;
    while (physicalStateChanging || stepsRemainingAfterStable > 0) {
        ASSERT_LT(time, maxConvergenceTimeNs) << "Physical state did not stabilize";
        if (!physicalStateChanging) {
            stepsRemainingAfterStable--;
        }
        if (time < 500000000 && time + stepNs >= 500000000) {
            model->SetTargetRotation(targetRotation, PhysicalInterpolation::kSmooth);
            model->SetTargetPosition(targetPosition, PhysicalInterpolation::kSmooth);
            EXPECT_TRUE(targetStateChanged);
            targetStateChanged = false;
        }
        model->SetCurrentTime(time);
        size_t gyroMeasurementId;
        const vec3 measuredGyroscope = model->GetGyroscope(&gyroMeasurementId);
        if (physicalStateChanging) {
            ASSERT_NE(prevGyroMeasurementId, gyroMeasurementId);
        }
        prevGyroMeasurementId = gyroMeasurementId;
        const glm::vec3 deviceSpaceRotationalVelocity(measuredGyroscope.x, measuredGyroscope.y,
                                                      measuredGyroscope.z);
        const glm::vec3 rotationalVelocity = rotation * deviceSpaceRotationalVelocity;
        const glm::mat4 deltaRotationMatrix = glm::eulerAngleXYZ(
                rotationalVelocity.x * stepSeconds, rotationalVelocity.y * stepSeconds,
                rotationalVelocity.z * stepSeconds);

        rotation = glm::quat_cast(deltaRotationMatrix) * rotation;

        size_t accelMeasurementId;
        const vec3 measuredAcceleration = model->GetAccelerometer(&accelMeasurementId);
        if (physicalStateChanging) {
            ASSERT_NE(prevAccelMeasurementId, accelMeasurementId);
        }
        prevAccelMeasurementId = accelMeasurementId;
        const glm::vec3 acceleration(measuredAcceleration.x, measuredAcceleration.y,
                                     measuredAcceleration.z);
        velocity += (rotation * acceleration - gravity) * stepSeconds;
        position += velocity * stepSeconds;

        time += stepNs;
    }

    const glm::quat targetRotationQuat = glm::toQuat(
            glm::eulerAngleXYZ(glm::radians(targetRotation.x), glm::radians(targetRotation.y),
                               glm::radians(targetRotation.z)));

    EXPECT_NEAR(targetRotationQuat.x, rotation.x, 0.001f);
    EXPECT_NEAR(targetRotationQuat.y, rotation.y, 0.001f);
    EXPECT_NEAR(targetRotationQuat.z, rotation.z, 0.001f);
    EXPECT_NEAR(targetRotationQuat.w, rotation.w, 0.001f);

    const vec3 integratedPosition{position.x, position.y, position.z};

    EXPECT_VEC3_NEAR(targetPosition, integratedPosition, 0.01f);

    EXPECT_FALSE(targetStateChanged);
}

TEST_F(PhysicalModelTest, FoldableInitialize) {
    // Foldable is not yet supported.
    auto hw = android::goldfish::FakeHardwareConfig::GetHwConfig();

    hw.hw_lcd_width = 1260;
    hw.hw_lcd_height = 2400;
    hw.hw_sensor_hinge = true;
    hw.hw_sensor_hinge_count = 2;
    hw.hw_sensor_hinge_type = 0;
    hw.hw_sensor_hinge_sub_type = 1;
    hw.hw_sensor_hinge_ranges = (char*)"0- 360, 0-180";
    hw.hw_sensor_hinge_defaults = (char*)"180,90";
    hw.hw_sensor_hinge_areas = (char*)"25-10, 50-10";
    hw.hw_sensor_posture_list = (char*)"1, 2,3 ,  4";
    hw.hw_sensor_hinge_angles_posture_definitions =
            (char*)"0-30&0-15,  30-150 & 15-75,150-330&75-165, 330-360&165-180";

    model = std::make_unique<PhysicalModel>(hw, virtual_clock_);

    model->SetCurrentTime(1000000000L);

    {
        const FoldableConfig* cfg = model->GetFoldableConfig();

        ASSERT_TRUE(cfg != nullptr);
        EXPECT_EQ(2, cfg->num_hinges);
        EXPECT_EQ(FoldableDisplayType::kHorizontalSplit, cfg->type);
        EXPECT_EQ(0, cfg->hinge_params[0].display_id);
        EXPECT_EQ(0, cfg->hinge_params[0].x);
        EXPECT_EQ(600, cfg->hinge_params[0].y);
        EXPECT_EQ(1260, cfg->hinge_params[0].width);
        EXPECT_EQ(10, cfg->hinge_params[0].height);
        EXPECT_EQ(0, cfg->hinge_params[0].min_degrees);
        EXPECT_EQ(360, cfg->hinge_params[0].max_degrees);
        EXPECT_EQ(0, cfg->hinge_params[1].display_id);
        EXPECT_EQ(0, cfg->hinge_params[1].x);
        EXPECT_EQ(1200, cfg->hinge_params[1].y);
        EXPECT_EQ(1260, cfg->hinge_params[1].width);
        EXPECT_EQ(10, cfg->hinge_params[1].height);
        EXPECT_EQ(0, cfg->hinge_params[1].min_degrees);
        EXPECT_EQ(180, cfg->hinge_params[1].max_degrees);
        EXPECT_EQ(180, cfg->hinge_params[0].default_degrees);
        EXPECT_EQ(90, cfg->hinge_params[1].default_degrees);
    }

    EXPECT_EQ(FoldablePostures::kOpened, model->GetFoldablePosture());
}

TEST_F(PhysicalModelTest, NonFoldableDevicePostureAndHingeAnglesDoNotCrash) {
    EXPECT_FALSE(model->HasFoldableModel());

    // Calling setters on non-foldable model must not crash.
    model->SetTargetPosture(1.0f, PhysicalInterpolation::kStep);
    model->SetTargetHingeAngle0(90.0f, PhysicalInterpolation::kStep);
    model->SetTargetHingeAngle1(90.0f, PhysicalInterpolation::kStep);
    model->SetTargetHingeAngle2(90.0f, PhysicalInterpolation::kStep);
    model->SetTargetRollable0(50.0f, PhysicalInterpolation::kStep);
    model->SetTargetRollable1(50.0f, PhysicalInterpolation::kStep);
    model->SetTargetRollable2(50.0f, PhysicalInterpolation::kStep);

    // Calling getters on non-foldable model must safely return default 0.0f.
    EXPECT_FLOAT_EQ(model->GetParameterPosture(ParameterValueType::kCurrent), 0.0f);
    EXPECT_FLOAT_EQ(model->GetParameterHingeAngle0(ParameterValueType::kCurrent), 0.0f);
    EXPECT_FLOAT_EQ(model->GetParameterHingeAngle1(ParameterValueType::kCurrent), 0.0f);
    EXPECT_FLOAT_EQ(model->GetParameterHingeAngle2(ParameterValueType::kCurrent), 0.0f);
    EXPECT_FLOAT_EQ(model->GetParameterRollable0(ParameterValueType::kCurrent), 0.0f);
    EXPECT_FLOAT_EQ(model->GetParameterRollable1(ParameterValueType::kCurrent), 0.0f);
    EXPECT_FLOAT_EQ(model->GetParameterRollable2(ParameterValueType::kCurrent), 0.0f);
    size_t measurement_id;
    EXPECT_FLOAT_EQ(model->GetHingeAngle0(&measurement_id), 0.0f);
    EXPECT_FLOAT_EQ(model->GetHingeAngle1(&measurement_id), 0.0f);
    EXPECT_FLOAT_EQ(model->GetHingeAngle2(&measurement_id), 0.0f);
}

TEST_F(PhysicalModelTest, SnapshotSaveLoad) {
    model->SetCurrentTime(1000000000L);
    model->SetTargetRotation(vec3(15.0f, 30.0f, 45.0f), PhysicalInterpolation::kSmooth);
    model->SetTargetPosition(vec3(1.0f, 2.0f, 3.0f), PhysicalInterpolation::kSmooth);
    model->SetCurrentTime(2000000000L);

    size_t measurement_id = 0;
    const vec3 expected_accel = model->GetAccelerometer(&measurement_id);
    const vec3 expected_gyro = model->GetGyroscope(&measurement_id);

    archive::DequeArchive archive;
    archive << *model;

    auto loaded_model = std::make_unique<PhysicalModel>(
            android::goldfish::FakeHardwareConfig::GetHwConfig(), virtual_clock_);
    ASSERT_TRUE(ReadValue(archive, *loaded_model).ok());
    EXPECT_TRUE(archive.Empty());

    EXPECT_VEC3_NEAR(expected_accel, loaded_model->GetAccelerometer(&measurement_id), 1e-5f);
    EXPECT_VEC3_NEAR(expected_gyro, loaded_model->GetGyroscope(&measurement_id), 1e-5f);
}

TEST_F(PhysicalModelTest, SnapshotRejectsCorruptedState) {
    archive::DequeArchive valid_archive;
    valid_archive << *model;

    // 1. Truncated stream rejected
    {
        archive::DequeArchive truncated = valid_archive;
        ASSERT_FALSE(truncated.Empty());
        truncated.storage.pop_back();

        auto loaded_model = std::make_unique<PhysicalModel>(
                android::goldfish::FakeHardwareConfig::GetHwConfig(), virtual_clock_);
        EXPECT_FALSE(ReadValue(truncated, *loaded_model).ok());
    }

    // 2. Out-of-range use_override_bits rejected
    {
        physics::InertialModel im;
        physics::AmbientEnvironment ae;
        physics::BodyModel bm;
        archive::DequeArchive bad_archive;
        const unsigned long invalid_override_bits = 1UL << PhysicalModel::kNumSensors;
        bad_archive << im << ae << bm << uint64_t{0} << invalid_override_bits << false << false;

        auto loaded_model = std::make_unique<PhysicalModel>(
                android::goldfish::FakeHardwareConfig::GetHwConfig(), virtual_clock_);
        EXPECT_FALSE(ReadValue(bad_archive, *loaded_model).ok());
    }
}

TEST_F(PhysicalModelTest, IsSensorOverriddenAndClearOverridesPreservesSmoothTransition) {
    model->SetCurrentTime(0L);
    EXPECT_FALSE(model->IsSensorOverridden(AndroidSensor::ACCELERATION));
    EXPECT_FALSE(model->IsSensorOverridden(AndroidSensor::MAX_SENSORS));

    // Start an in-flight smooth rotation towards 90 degrees around Z.
    model->SetTargetRotation(vec3(0.0F, 0.0F, 90.0F), PhysicalInterpolation::kSmooth);
    model->SetCurrentTime(SecondsToNs(kMinStateChangeTimeSeconds / 2.0F));

    size_t mid_id = 0;
    model->GetAccelerometer(&mid_id);

    // Override accelerometer mid-transition.
    model->OverrideAccelerometer(vec3(0.0F, 0.0F, 0.0F));
    EXPECT_TRUE(model->IsSensorOverridden(AndroidSensor::ACCELERATION));
    size_t override_id = 0;
    EXPECT_VEC3_NEAR((vec3{0.0F, 0.0F, 0.0F}), model->GetAccelerometer(&override_id), 1e-5F);
    EXPECT_GT(override_id, mid_id);

    // Clear overrides without touching target rotation.
    model->ClearOverrides();
    EXPECT_FALSE(model->IsSensorOverridden(AndroidSensor::ACCELERATION));
    size_t cleared_id = 0;
    model->GetAccelerometer(&cleared_id);
    EXPECT_GT(cleared_id, override_id);
    EXPECT_VEC3_NEAR((vec3{0.0F, 0.0F, 90.0F}),
                     model->GetParameterRotation(ParameterValueType::kTarget), 0.01F);

    // Advance time past the end of the smooth transition and verify it still reaches 90 degrees.
    model->SetCurrentTime(SecondsToNs(2.0F));
    const vec3 final_rotation = model->GetParameterRotation(ParameterValueType::kCurrent);
    EXPECT_VEC3_NEAR((vec3{0.0F, 0.0F, 90.0F}), final_rotation, 0.01F);
}

TEST_F(PhysicalModelTest, VirtualSceneCamera_Lifecycle) {
    auto& camera = model->GetVirtualSceneCamera();
    EXPECT_FALSE(camera.IsConnected());
    EXPECT_FLOAT_EQ(model->GetParameterAmbientMotion(ParameterValueType::kTarget), 0.0F);

    std::vector<bool> active_events;
    auto sub = android::base::eventing::MakeScopedCallback(
            camera.GetActiveListener(),
            [&active_events](const bool& active) { active_events.push_back(active); });

    camera.AddSceneUser();
    EXPECT_TRUE(camera.IsConnected());
    ASSERT_EQ(active_events.size(), 1U);
    EXPECT_TRUE(active_events[0]);

    // Adding a second user should not re-trigger the active event.
    camera.AddSceneUser();
    EXPECT_TRUE(camera.IsConnected());
    EXPECT_EQ(active_events.size(), 1U);

    // Removing the first of two users keeps the camera active.
    camera.RemoveSceneUser();
    EXPECT_TRUE(camera.IsConnected());
    EXPECT_EQ(active_events.size(), 1U);

    // Removing the last user disables the camera and resets ambient motion.
    camera.RemoveSceneUser();
    EXPECT_FALSE(camera.IsConnected());
    EXPECT_FLOAT_EQ(model->GetParameterAmbientMotion(ParameterValueType::kTarget), 0.0F);
    ASSERT_EQ(active_events.size(), 2U);
    EXPECT_FALSE(active_events[1]);
}

TEST_F(PhysicalModelTest, VirtualSceneCamera_RotateAndVelocity) {
    auto& camera = model->GetVirtualSceneCamera();

    // Ignored when inactive.
    camera.Rotate(vec3(glm::radians(10.0F), glm::radians(20.0F), 0.0F));
    camera.SetVelocity(vec3(1.0F, 0.0F, 0.0F));
    EXPECT_VEC3_NEAR((vec3{0.0F, 0.0F, 0.0F}),
                     model->GetParameterRotation(ParameterValueType::kTarget), 1e-4F);
    EXPECT_VEC3_NEAR((vec3{0.0F, 0.0F, 0.0F}),
                     model->GetParameterVelocity(ParameterValueType::kTarget), 1e-4F);

    camera.AddSceneUser();

    // Rotate 90 degrees around Y (yaw) and clamp X (pitch) to kMaxVerticalRotationDegrees (80).
    camera.Rotate(vec3(glm::radians(85.0F), glm::radians(90.0F), 0.0F));
    const vec3 expected_rot = glm::degrees(ToEulerAnglesXyz(
            FromEulerAnglesYxz(vec3(glm::radians(80.0F), glm::radians(90.0F), 0.0F))));
    EXPECT_VEC3_NEAR(expected_rot, model->GetParameterRotation(ParameterValueType::kTarget), 0.01F);

    // Forward velocity along -Z in camera look space should rotate by +90 deg around Y to -X.
    camera.SetVelocity(vec3(0.0F, 0.0F, -2.0F));
    EXPECT_VEC3_NEAR((vec3{-2.0F, 0.0F, 0.0F}),
                     model->GetParameterVelocity(ParameterValueType::kTarget), 0.01F);

    // Disabling resets velocity to zero.
    camera.RemoveSceneUser();
    EXPECT_VEC3_NEAR((vec3{0.0F, 0.0F, 0.0F}),
                     model->GetParameterVelocity(ParameterValueType::kTarget), 1e-4F);
}

TEST_F(PhysicalModelTest, VirtualSceneCamera_UsesInjectedVirtualClock) {
    ::goldfish::system::testing::FakeVirtualClock virtual_clock(absl::Seconds(5));
    PhysicalModel timed_model(android::goldfish::FakeHardwareConfig::GetHwConfig(), virtual_clock);
    auto& camera = timed_model.GetVirtualSceneCamera();

    auto read_timestamp = [&timed_model]() {
        float tx = 0.0F;
        float ty = 0.0F;
        float tz = 0.0F;
        float rx = 0.0F;
        float ry = 0.0F;
        float rz = 0.0F;
        int64_t ts = 0;
        timed_model.GetTransform(&tx, &ty, &tz, &rx, &ry, &rz, &ts);
        return ts;
    };

    camera.AddSceneUser();
    EXPECT_EQ(read_timestamp(), 5'000'000'000LL);

    virtual_clock.Advance(absl::Milliseconds(250));
    camera.Rotate(vec3(0.0F, glm::radians(15.0F), 0.0F));
    EXPECT_EQ(read_timestamp(), 5'250'000'000LL);

    virtual_clock.Advance(absl::Milliseconds(750));
    camera.RemoveSceneUser();
    EXPECT_EQ(read_timestamp(), 6'000'000'000LL);
}

}  // namespace goldfish::sensors
