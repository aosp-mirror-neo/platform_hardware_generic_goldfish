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
#include "sensor_service.h"

#include <glm/trigonometric.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <vector>

#include "android/goldfish/fake_hardware_config.h"
#include "emulator_controller.grpc.pb.h"
#include "goldfish/system/testing/fake_clocks.h"
#include "notification_store.h"
#include "test/grpc_service_test.h"

namespace android::emulation::control {

using ::goldfish::avd_universe::grpc::GrpcNotificationEventSource;
using ::goldfish::sensors::PhysicalModel;
using ::google::protobuf::Empty;
using ::grpc::ServerContext;
using ::grpc::Status;

class SensorServiceWrapper : public EmulatorController::Service {
  public:
    explicit SensorServiceWrapper(SensorServiceImpl& sensor_service)
            : sensor_service_(sensor_service) {}

    Status setSensor(ServerContext* /*context*/, const SensorValue* request,
                     Empty* /*reply*/) override {
        return sensor_service_.setSensor(*request);
    }
    Status getSensor(ServerContext* /*context*/, const SensorValue* request,
                     SensorValue* reply) override {
        return sensor_service_.getSensor(*request, reply);
    }
    Status setPhysicalModel(ServerContext* /*context*/, const PhysicalModelValue* request,
                            Empty* /*reply*/) override {
        return sensor_service_.setPhysicalModel(*request);
    }
    Status getPhysicalModel(ServerContext* /*context*/, const PhysicalModelValue* request,
                            PhysicalModelValue* reply) override {
        return sensor_service_.getPhysicalModel(*request, reply);
    }
    Status rotateVirtualSceneCamera(ServerContext* /*context*/, const RotationRadian* request,
                                    Empty* /*reply*/) override {
        return sensor_service_.rotateVirtualSceneCamera(*request);
    }
    Status setVirtualSceneCameraVelocity(ServerContext* /*context*/, const Velocity* request,
                                         Empty* /*reply*/) override {
        return sensor_service_.setVirtualSceneCameraVelocity(*request);
    }

  private:
    SensorServiceImpl& sensor_service_;
};

class SensorServiceTest : public GrcpServiceTest {
  protected:
    void SetUp() override {
        hw_ = android::goldfish::FakeHardwareConfig::GetHwConfig();
        physical_model_ = std::make_unique<PhysicalModel>(hw_, virtual_clock_);
        notification_store_ = std::make_unique<NotificationStore>(&notification_channel_);
        sensor_service_ =
                std::make_unique<SensorServiceImpl>(*physical_model_, &notification_channel_);
        service_wrapper_ = std::make_unique<SensorServiceWrapper>(*sensor_service_);

        GrcpServiceTest::SetUp();
    }

    EmulatorController::Service* getService() override { return service_wrapper_.get(); }

  protected:
    android::goldfish::HardwareConfig hw_;
    ::goldfish::system::testing::FakeVirtualClock virtual_clock_;
    GrpcNotificationEventSource notification_channel_;
    std::unique_ptr<NotificationStore> notification_store_;
    std::unique_ptr<PhysicalModel> physical_model_;
    std::unique_ptr<SensorServiceImpl> sensor_service_;
    std::unique_ptr<SensorServiceWrapper> service_wrapper_;
};

TEST_F(SensorServiceTest, SetGetPhysicalModelRotation) {
    PhysicalModelValue request;
    request.set_target(PhysicalModelValue::ROTATION);
    request.mutable_value()->add_data(10.0F);
    request.mutable_value()->add_data(20.0F);
    request.mutable_value()->add_data(30.0F);

    Empty reply;
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->setPhysicalModel(context.get(), request, &reply));

    PhysicalModelValue get_request;
    get_request.set_target(PhysicalModelValue::ROTATION);
    PhysicalModelValue get_reply;
    auto get_context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getPhysicalModel(get_context.get(), get_request, &get_reply));

    EXPECT_EQ(get_reply.target(), PhysicalModelValue::ROTATION);
    EXPECT_EQ(get_reply.status(), PhysicalModelValue::OK);
    ASSERT_EQ(get_reply.value().data_size(), 3);
    EXPECT_NEAR(get_reply.value().data(0), 10.0F, 0.1F);
    EXPECT_NEAR(get_reply.value().data(1), 20.0F, 0.1F);
    EXPECT_NEAR(get_reply.value().data(2), 30.0F, 0.1F);
}

TEST_F(SensorServiceTest, SetGetPhysicalModelPosition) {
    PhysicalModelValue request;
    request.set_target(PhysicalModelValue::POSITION);
    request.mutable_value()->add_data(1.0F);
    request.mutable_value()->add_data(2.0F);
    request.mutable_value()->add_data(3.0F);

    Empty reply;
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->setPhysicalModel(context.get(), request, &reply));

    PhysicalModelValue get_request;
    get_request.set_target(PhysicalModelValue::POSITION);
    PhysicalModelValue get_reply;
    auto get_context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getPhysicalModel(get_context.get(), get_request, &get_reply));

    EXPECT_EQ(get_reply.target(), PhysicalModelValue::POSITION);
    EXPECT_EQ(get_reply.status(), PhysicalModelValue::OK);
    ASSERT_EQ(get_reply.value().data_size(), 3);
    EXPECT_NEAR(get_reply.value().data(0), 1.0F, 0.1F);
    EXPECT_NEAR(get_reply.value().data(1), 2.0F, 0.1F);
    EXPECT_NEAR(get_reply.value().data(2), 3.0F, 0.1F);
}

TEST_F(SensorServiceTest, VirtualSceneCameraNotificationsAndControls) {
    // Initial state should be cached as inactive CameraNotification in NotificationStore.
    {
        const auto notifications = notification_store_->GetLatest();
        ASSERT_EQ(notifications.size(), 1U);
        ASSERT_TRUE(notifications[0].has_cameranotification());
        EXPECT_EQ(notifications[0].cameranotification().display(), 0U);
        EXPECT_FALSE(notifications[0].cameranotification().active());
    }

    // Activate virtual scene camera.
    physical_model_->GetVirtualSceneCamera().AddSceneUser();
    {
        const auto notifications = notification_store_->GetLatest();
        ASSERT_EQ(notifications.size(), 1U);
        ASSERT_TRUE(notifications[0].has_cameranotification());
        EXPECT_EQ(notifications[0].cameranotification().display(), 0U);
        EXPECT_TRUE(notifications[0].cameranotification().active());
    }

    // Rotate camera 90 degrees around Y via gRPC.
    RotationRadian rotate_req;
    rotate_req.set_x(0.0F);
    rotate_req.set_y(glm::radians(90.0F));
    rotate_req.set_z(0.0F);
    Empty reply;
    auto rotate_ctx = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->rotateVirtualSceneCamera(rotate_ctx.get(), rotate_req, &reply));

    const glm::vec3 target_rot =
            physical_model_->GetParameterRotation(::ParameterValueType::kTarget);
    EXPECT_NEAR(target_rot.y, 90.0F, 0.1F);

    // Set camera velocity along -Z via gRPC; with 90-degree Y yaw, world target velocity is -X.
    Velocity velocity_req;
    velocity_req.set_x(0.0F);
    velocity_req.set_y(0.0F);
    velocity_req.set_z(-1.5F);
    auto vel_ctx = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->setVirtualSceneCameraVelocity(vel_ctx.get(), velocity_req, &reply));

    const glm::vec3 target_vel =
            physical_model_->GetParameterVelocity(::ParameterValueType::kTarget);
    EXPECT_NEAR(target_vel.x, -1.5F, 0.01F);
    EXPECT_NEAR(target_vel.y, 0.0F, 0.01F);
    EXPECT_NEAR(target_vel.z, 0.0F, 0.01F);

    // Deactivate virtual scene camera.
    physical_model_->GetVirtualSceneCamera().RemoveSceneUser();
    {
        const auto notifications = notification_store_->GetLatest();
        ASSERT_EQ(notifications.size(), 1U);
        ASSERT_TRUE(notifications[0].has_cameranotification());
        EXPECT_FALSE(notifications[0].cameranotification().active());
    }
}

TEST_F(SensorServiceTest, VirtualSceneCameraRejectsNonFiniteCoordinates) {
    physical_model_->GetVirtualSceneCamera().AddSceneUser();

    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    Empty reply;

    for (const auto& [x, y, z] : std::vector<std::tuple<float, float, float>>{
             {kNaN, 0.0F, 0.0F}, {0.0F, kInf, 0.0F}, {0.0F, 0.0F, -kInf}}) {
        RotationRadian rotate_req;
        rotate_req.set_x(x);
        rotate_req.set_y(y);
        rotate_req.set_z(z);
        auto rotate_ctx = getContextWithTimeout();
        const Status rotate_status =
                mStub->rotateVirtualSceneCamera(rotate_ctx.get(), rotate_req, &reply);
        EXPECT_EQ(rotate_status.error_code(), StatusCode::INVALID_ARGUMENT);

        Velocity velocity_req;
        velocity_req.set_x(x);
        velocity_req.set_y(y);
        velocity_req.set_z(z);
        auto vel_ctx = getContextWithTimeout();
        const Status vel_status =
                mStub->setVirtualSceneCameraVelocity(vel_ctx.get(), velocity_req, &reply);
        EXPECT_EQ(vel_status.error_code(), StatusCode::INVALID_ARGUMENT);
    }

    // Subsequent valid rotation should still work cleanly.
    RotationRadian valid_rotate;
    valid_rotate.set_x(0.0F);
    valid_rotate.set_y(glm::radians(45.0F));
    valid_rotate.set_z(0.0F);
    auto valid_ctx = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->rotateVirtualSceneCamera(valid_ctx.get(), valid_rotate, &reply));

    const glm::vec3 target_rot =
            physical_model_->GetParameterRotation(::ParameterValueType::kTarget);
    EXPECT_NEAR(target_rot.y, 45.0F, 0.1F);

    physical_model_->GetVirtualSceneCamera().RemoveSceneUser();
}

}  // namespace android::emulation::control
