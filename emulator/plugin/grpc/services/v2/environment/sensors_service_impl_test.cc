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

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <limits>
#include <memory>

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/goldfish/hardware_config.h"
#include "goldfish/sensors/physical_model.h"
#include "goldfish/system/testing/fake_clocks.h"

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::GetPostureStateRequest;
using ::android::emulation::v2::GetSensorsStateRequest;
using ::android::emulation::v2::PostureState;
using ::android::emulation::v2::PostureType;
using ::android::emulation::v2::SensorsService;
using ::android::emulation::v2::SensorsState;
using ::android::emulation::v2::SensorType;
using ::android::emulation::v2::StreamPostureStateRequest;
using ::android::emulation::v2::StreamSensorsStateRequest;
using ::android::emulation::v2::StreamSensorsUpdatesResponse;
using ::android::emulation::v2::TransitionMode;
using ::android::emulation::v2::UpdatePostureStateRequest;
using ::android::emulation::v2::UpdateSensorsStateRequest;

class SensorsServiceImplTest : public ::testing::Test {
  protected:
    void SetUp() override { StartServer(hw_); }

    void TearDown() override { StopServer(); }

    void StartServer(const ::android::goldfish::HardwareConfig& hw) {
        StopServer();
        physical_model_ =
                std::make_unique<::goldfish::sensors::PhysicalModel>(hw, virtual_clock_);
        service_ = std::make_unique<SensorsServiceImpl>(*physical_model_, clock_);
        ::grpc::ServerBuilder builder;
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        channel_ = server_->InProcessChannel(::grpc::ChannelArguments());
        stub_ = SensorsService::NewStub(channel_);
    }

    void StopServer() {
        stub_.reset();
        channel_.reset();
        if (server_) {
            server_->Shutdown(absl::ToChronoTime(absl::Now() + absl::Seconds(1)));
            server_->Wait();
            server_.reset();
        }
        service_.reset();
        physical_model_.reset();
    }

    ::android::goldfish::HardwareConfig hw_{};
    ::goldfish::system::testing::FakeVirtualClock virtual_clock_;
    ::goldfish::system::testing::FakeSteadyClock clock_{absl::Seconds(1)};
    std::unique_ptr<::goldfish::sensors::PhysicalModel> physical_model_;
    std::unique_ptr<SensorsServiceImpl> service_;
    std::unique_ptr<::grpc::Server> server_;
    std::shared_ptr<::grpc::Channel> channel_;
    std::unique_ptr<SensorsService::Stub> stub_;
};

TEST_F(SensorsServiceImplTest, GetSensorsStateReturnsInitialPhysicsSnapshot) {
    ::grpc::ServerContext ctx;
    GetSensorsStateRequest req;
    SensorsState resp;

    const auto status = service_->GetSensorsState(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.timestamp_nanos(), 1'000'000'000LL);
    EXPECT_TRUE(resp.has_pose());
    EXPECT_TRUE(resp.has_environment());
    EXPECT_TRUE(resp.has_imu());
    EXPECT_EQ(resp.imu().active_overrides_size(), 0);

    clock_.Advance(absl::Milliseconds(250));
    ::grpc::ServerContext next_ctx;
    SensorsState next_resp;
    ASSERT_TRUE(service_->GetSensorsState(&next_ctx, &req, &next_resp).ok());
    EXPECT_EQ(next_resp.timestamp_nanos(), 1'250'000'000LL);
}

TEST_F(SensorsServiceImplTest, UpdatePoseRotationStepUpdatesSynthesizedImu) {
    ::grpc::ServerContext ctx;
    UpdateSensorsStateRequest req;
    req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
    auto* rot = req.mutable_pose()->mutable_rotation_degrees();
    rot->set_x(0.0F);
    rot->set_y(0.0F);
    rot->set_z(90.0F);  // Roll 90 degrees into landscape

    SensorsState resp;
    const auto status = service_->UpdateSensorsState(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FLOAT_EQ(resp.pose().rotation_degrees().z(), 90.0F);
    EXPECT_FLOAT_EQ(resp.imu().orientation_degrees().z(), 90.0F);
    EXPECT_EQ(resp.imu().active_overrides_size(), 0);
}

TEST_F(SensorsServiceImplTest, UpdateAmbientEnvironmentAndValidateBounds) {
    ::grpc::ServerContext ctx;
    UpdateSensorsStateRequest req;
    req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
    auto* env = req.mutable_environment();
    env->set_light_lux(750.0F);
    env->set_ambient_temperature_celsius(24.5F);
    env->set_proximity_centimeters(0.0F);
    env->set_pressure_hpa(1015.0F);
    env->set_relative_humidity_percent(55.0F);

    SensorsState resp;
    ASSERT_TRUE(service_->UpdateSensorsState(&ctx, &req, &resp).ok());
    EXPECT_FLOAT_EQ(resp.environment().light_lux(), 750.0F);
    EXPECT_FLOAT_EQ(resp.environment().ambient_temperature_celsius(), 24.5F);
    EXPECT_FLOAT_EQ(resp.environment().proximity_centimeters(), 0.0F);
    EXPECT_FLOAT_EQ(resp.environment().pressure_hpa(), 1015.0F);
    EXPECT_FLOAT_EQ(resp.environment().relative_humidity_percent(), 55.0F);

    // Negative light_lux must fail with INVALID_ARGUMENT.
    UpdateSensorsStateRequest invalid_req;
    invalid_req.mutable_environment()->set_light_lux(-10.0F);
    ::grpc::ServerContext invalid_ctx;
    const auto invalid_status = service_->UpdateSensorsState(&invalid_ctx, &invalid_req, &resp);
    EXPECT_EQ(invalid_status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);

    // Negative rgbc_light channel must fail with INVALID_ARGUMENT.
    UpdateSensorsStateRequest invalid_rgbc_req;
    invalid_rgbc_req.mutable_environment()->mutable_rgbc_light()->set_red(-1.0F);
    ::grpc::ServerContext invalid_rgbc_ctx;
    EXPECT_EQ(
            service_->UpdateSensorsState(&invalid_rgbc_ctx, &invalid_rgbc_req, &resp).error_code(),
            ::grpc::StatusCode::INVALID_ARGUMENT);

    // Non-finite values (NaN / Inf) must fail with INVALID_ARGUMENT.
    UpdateSensorsStateRequest nan_req;
    nan_req.mutable_pose()->mutable_rotation_degrees()->set_z(
            std::numeric_limits<float>::quiet_NaN());
    ::grpc::ServerContext nan_ctx;
    EXPECT_EQ(service_->UpdateSensorsState(&nan_ctx, &nan_req, &resp).error_code(),
              ::grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(SensorsServiceImplTest, RawOverridesPinChannelAndClearRestoresPhysics) {
    ::grpc::ServerContext ctx;
    UpdateSensorsStateRequest req;
    auto* accel = req.mutable_raw_overrides()->mutable_accelerometer();
    accel->set_x(0.0F);
    accel->set_y(0.0F);
    accel->set_z(0.0F);  // 0g freefall override
    auto* orient = req.mutable_raw_overrides()->mutable_orientation_degrees();
    orient->set_x(15.0F);
    orient->set_y(30.0F);
    orient->set_z(45.0F);

    SensorsState resp;
    ASSERT_TRUE(service_->UpdateSensorsState(&ctx, &req, &resp).ok());
    EXPECT_FLOAT_EQ(resp.imu().accelerometer().x(), 0.0F);
    EXPECT_FLOAT_EQ(resp.imu().accelerometer().y(), 0.0F);
    EXPECT_FLOAT_EQ(resp.imu().accelerometer().z(), 0.0F);
    EXPECT_FLOAT_EQ(resp.imu().orientation_degrees().x(), 15.0F);
    EXPECT_FLOAT_EQ(resp.imu().orientation_degrees().y(), 30.0F);
    EXPECT_FLOAT_EQ(resp.imu().orientation_degrees().z(), 45.0F);
    ASSERT_EQ(resp.imu().active_overrides_size(), 2);
    EXPECT_EQ(resp.imu().active_overrides(0), SensorType::SENSOR_TYPE_ACCELEROMETER);
    EXPECT_EQ(resp.imu().active_overrides(1), SensorType::SENSOR_TYPE_ORIENTATION);

    // Verify a second SensorsServiceImpl instance wrapping the same PhysicalModel
    // observes the same active_overrides.
    SensorsServiceImpl peer_service(*physical_model_);
    GetSensorsStateRequest get_req;
    SensorsState peer_resp;
    ::grpc::ServerContext peer_ctx;
    ASSERT_TRUE(peer_service.GetSensorsState(&peer_ctx, &get_req, &peer_resp).ok());
    ASSERT_EQ(peer_resp.imu().active_overrides_size(), 2);

    // Clear raw overrides and verify active_overrides is emptied without clobbering
    // target rotation.
    UpdateSensorsStateRequest clear_req;
    clear_req.set_clear_raw_overrides(true);
    ::grpc::ServerContext clear_ctx;
    ASSERT_TRUE(service_->UpdateSensorsState(&clear_ctx, &clear_req, &resp).ok());
    EXPECT_EQ(resp.imu().active_overrides_size(), 0);
}

TEST_F(SensorsServiceImplTest, PostureReturnsFailedPreconditionWhenNonFoldable) {
    ::grpc::ServerContext ctx;
    GetPostureStateRequest req;
    PostureState resp;

    const auto status = service_->GetPostureState(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(SensorsServiceImplTest, PostureUpdatesHingeAngleAndPostureWhenFoldable) {
    ::android::goldfish::HardwareConfig foldable_hw{};
    foldable_hw.hw_sensor_hinge = true;
    foldable_hw.hw_sensor_hinge_count = 1;
    foldable_hw.hw_sensor_hinge_type = 1;
    foldable_hw.hw_sensor_hinge_ranges = "0-180";
    foldable_hw.hw_sensor_hinge_defaults = "180";
    foldable_hw.hw_sensor_hinge_areas = "540-0-0-2400";
    foldable_hw.hw_sensor_posture_list = "1,2,3";
    foldable_hw.hw_sensor_hinge_angles_posture_definitions = "0-30, 30-150, 150-180";

    StartServer(foldable_hw);

    ::grpc::ServerContext ctx;
    UpdatePostureStateRequest req;
    req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
    auto* h0 = req.add_hinges();
    h0->set_hinge_id(0);
    h0->set_angle_degrees(90.0F);

    PostureState resp;
    clock_.Set(absl::Milliseconds(4200));
    const auto status = service_->UpdatePostureState(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.timestamp_nanos(), 4'200'000'000LL);
    ASSERT_GE(resp.hinges_size(), 1);
    EXPECT_FLOAT_EQ(resp.hinges(0).angle_degrees(), 90.0F);
    EXPECT_EQ(resp.posture(), PostureType::POSTURE_TYPE_HALF_OPENED);

    // Request with a valid first hinge and an invalid second hinge (hinge_id 1 >= num_hinges 1)
    // must fail with INVALID_ARGUMENT without partially mutating hinge 0.
    UpdatePostureStateRequest partial_invalid_req;
    partial_invalid_req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
    auto* valid_h0 = partial_invalid_req.add_hinges();
    valid_h0->set_hinge_id(0);
    valid_h0->set_angle_degrees(10.0F);
    auto* invalid_h1 = partial_invalid_req.add_hinges();
    invalid_h1->set_hinge_id(1);
    invalid_h1->set_angle_degrees(10.0F);

    ::grpc::ServerContext invalid_ctx;
    EXPECT_EQ(service_->UpdatePostureState(&invalid_ctx, &partial_invalid_req, &resp).error_code(),
              ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_FLOAT_EQ(physical_model_->GetParameterHingeAngle0(::ParameterValueType::kCurrent),
                    90.0F);
}

TEST_F(SensorsServiceImplTest, StreamSensorsStatePushesInitialAndReactiveSnapshots) {
    ::grpc::ClientContext stream_ctx;
    stream_ctx.set_deadline(absl::ToChronoTime(absl::Now() + absl::Seconds(5)));
    StreamSensorsStateRequest stream_req;
    auto reader = stub_->StreamSensorsState(&stream_ctx, stream_req);

    SensorsState initial_state;
    ASSERT_TRUE(reader->Read(&initial_state));
    EXPECT_TRUE(initial_state.has_environment());

    UpdateSensorsStateRequest update_req;
    update_req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
    update_req.mutable_environment()->set_light_lux(1234.0F);
    SensorsState update_resp;
    ::grpc::ClientContext update_ctx;
    ASSERT_TRUE(stub_->UpdateSensorsState(&update_ctx, update_req, &update_resp).ok());

    SensorsState next_state;
    ASSERT_TRUE(reader->Read(&next_state));
    EXPECT_FLOAT_EQ(next_state.environment().light_lux(), 1234.0F);

    stream_ctx.TryCancel();
    const auto status = reader->Finish();
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::CANCELLED);
}

TEST_F(SensorsServiceImplTest, StreamSensorsUpdatesAppliesStreamAndRejectsInvalidValues) {
    {
        ::grpc::ClientContext ctx;
        StreamSensorsUpdatesResponse resp;
        auto writer = stub_->StreamSensorsUpdates(&ctx, &resp);

        UpdateSensorsStateRequest req;
        req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
        req.mutable_environment()->set_light_lux(888.0F);
        ASSERT_TRUE(writer->Write(req));
        ASSERT_TRUE(writer->WritesDone());
        ASSERT_TRUE(writer->Finish().ok());
        EXPECT_FLOAT_EQ(physical_model_->GetParameterLight(::ParameterValueType::kCurrent), 888.0F);
    }

    {
        ::grpc::ClientContext ctx;
        StreamSensorsUpdatesResponse resp;
        auto writer = stub_->StreamSensorsUpdates(&ctx, &resp);

        UpdateSensorsStateRequest invalid_req;
        invalid_req.mutable_environment()->set_light_lux(-5.0F);
        writer->Write(invalid_req);
        writer->WritesDone();
        const auto status = writer->Finish();
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    }
}

TEST_F(SensorsServiceImplTest, StreamPostureStateHandlesNonFoldableAndFoldableReactiveUpdates) {
    // Non-foldable should immediately finish with FAILED_PRECONDITION.
    {
        ::grpc::ClientContext ctx;
        StreamPostureStateRequest req;
        auto reader = stub_->StreamPostureState(&ctx, req);
        PostureState state;
        EXPECT_FALSE(reader->Read(&state));
        const auto status = reader->Finish();
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
    }

    // Foldable should push initial state and reactive updates.
    ::android::goldfish::HardwareConfig foldable_hw{};
    foldable_hw.hw_sensor_hinge = true;
    foldable_hw.hw_sensor_hinge_count = 1;
    foldable_hw.hw_sensor_hinge_type = 1;
    foldable_hw.hw_sensor_hinge_ranges = "0-180";
    foldable_hw.hw_sensor_hinge_defaults = "180";
    foldable_hw.hw_sensor_hinge_areas = "540-0-0-2400";
    foldable_hw.hw_sensor_posture_list = "1,2,3";
    foldable_hw.hw_sensor_hinge_angles_posture_definitions = "0-30, 30-150, 150-180";
    StartServer(foldable_hw);

    ::grpc::ClientContext stream_ctx;
    stream_ctx.set_deadline(absl::ToChronoTime(absl::Now() + absl::Seconds(5)));
    StreamPostureStateRequest stream_req;
    auto reader = stub_->StreamPostureState(&stream_ctx, stream_req);

    PostureState initial_state;
    ASSERT_TRUE(reader->Read(&initial_state));
    ASSERT_GE(initial_state.hinges_size(), 1);
    EXPECT_FLOAT_EQ(initial_state.hinges(0).angle_degrees(), 180.0F);

    UpdatePostureStateRequest update_req;
    update_req.set_transition(TransitionMode::TRANSITION_MODE_STEP);
    auto* h0 = update_req.add_hinges();
    h0->set_hinge_id(0);
    h0->set_angle_degrees(90.0F);
    PostureState update_resp;
    ::grpc::ClientContext update_ctx;
    ASSERT_TRUE(stub_->UpdatePostureState(&update_ctx, update_req, &update_resp).ok());

    PostureState next_state;
    ASSERT_TRUE(reader->Read(&next_state));
    ASSERT_GE(next_state.hinges_size(), 1);
    EXPECT_FLOAT_EQ(next_state.hinges(0).angle_degrees(), 90.0F);
    EXPECT_EQ(next_state.posture(), PostureType::POSTURE_TYPE_HALF_OPENED);

    stream_ctx.TryCancel();
    const auto status = reader->Finish();
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::CANCELLED);
}

}  // namespace
}  // namespace goldfish::grpc::v2
