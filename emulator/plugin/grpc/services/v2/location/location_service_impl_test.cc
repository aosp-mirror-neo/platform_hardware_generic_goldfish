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

#include "location/location_service_impl.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/avd_universe/gps/location.h"
#include "goldfish/system/testing/fake_clocks.h"
#include "test/grpc_v2_service_test.h"

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::GetLocationStateRequest;
using ::android::emulation::v2::LocationService;
using ::android::emulation::v2::LocationState;
using ::android::emulation::v2::PlayRouteRequest;
using ::android::emulation::v2::PlayRouteResponse;
using ::android::emulation::v2::StreamLocationStateRequest;
using ::android::emulation::v2::UpdateLocationStateRequest;
using ::goldfish::async::LibuvEventLoop;
using ::goldfish::async::ThreadedEventLoop;
using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::avd_universe::gps::Location;
using ::goldfish::avd_universe::gps::ObservableLocation;
using ::grpc::ClientContext;
using ::grpc::Server;
using ::grpc::ServerBuilder;
using ::grpc::ServerContext;
using ::grpc::Status;
using ::grpc::StatusCode;

class LocationServiceTest : public ::testing::Test {
  protected:
    void SetUp() override { RecreateService(LocationServiceImpl::Options{}); }

    void RecreateService(LocationServiceImpl::Options options) {
        if (!options.event_loop) {
            options.event_loop = event_loop_.get();
        }
        options.clock = clock_;
        service_ = std::make_unique<LocationServiceImpl>(observable_location_, std::move(options));
    }

    ::goldfish::system::testing::FakeSteadyClock clock_{absl::Nanoseconds(1'000'000'000LL)};
    std::unique_ptr<TestEventLoop> event_loop_ = TestEventLoop::Create("LocationTestLoop");
    ObservableLocation observable_location_;
    std::unique_ptr<LocationServiceImpl> service_;
};

TEST_F(LocationServiceTest, GetLocationStateReturnsDefaultGoogleplexFix) {
    GetLocationStateRequest req;
    LocationState reply;
    ServerContext ctx;

    const Status status = service_->GetLocationState(&ctx, &req, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_DOUBLE_EQ(reply.latitude(), Location::kDefaultLatitude);
    EXPECT_DOUBLE_EQ(reply.longitude(), Location::kDefaultLongitude);
    EXPECT_DOUBLE_EQ(reply.altitude_meters(), Location::kDefaultAltitudeMeters);
    EXPECT_FLOAT_EQ(reply.speed_meters_per_second(), 0.0F);
    EXPECT_FLOAT_EQ(reply.bearing_degrees(), 0.0F);
    EXPECT_FLOAT_EQ(reply.accuracy_meters(), 0.0F);
    EXPECT_EQ(reply.satellite_count(), Location::kDefaultSatellites);
    EXPECT_EQ(reply.timestamp_nanos(), 1'000'000'000LL);
}

TEST_F(LocationServiceTest, UpdateLocationStateAppliesPartialAndZeroValueMutations) {
    clock_.Set(absl::Nanoseconds(2'500'000'000LL));
    UpdateLocationStateRequest update_req;
    update_req.set_latitude(52.3676);
    update_req.set_longitude(4.9041);
    update_req.set_speed_meters_per_second(12.5F);
    update_req.set_bearing_degrees(270.0F);
    update_req.set_accuracy_meters(4.25F);

    LocationState update_reply;
    ServerContext update_ctx;
    ASSERT_TRUE(service_->UpdateLocationState(&update_ctx, &update_req, &update_reply).ok());

    EXPECT_DOUBLE_EQ(update_reply.latitude(), 52.3676);
    EXPECT_DOUBLE_EQ(update_reply.longitude(), 4.9041);
    // Omitted fields preserve prior values (altitude=10.0, satellites=8).
    EXPECT_DOUBLE_EQ(update_reply.altitude_meters(), Location::kDefaultAltitudeMeters);
    EXPECT_FLOAT_EQ(update_reply.speed_meters_per_second(), 12.5F);
    EXPECT_FLOAT_EQ(update_reply.bearing_degrees(), 270.0F);
    EXPECT_FLOAT_EQ(update_reply.accuracy_meters(), 4.25F);
    EXPECT_EQ(update_reply.satellite_count(), Location::kDefaultSatellites);
    EXPECT_EQ(update_reply.timestamp_nanos(), 2'500'000'000LL);

    // Explicit zero values (e.g., satellite_count = 0 for tunnel fix loss) and negative altitude.
    clock_.Set(absl::Nanoseconds(3'000'000'000LL));
    UpdateLocationStateRequest tunnel_req;
    tunnel_req.set_satellite_count(0);
    tunnel_req.set_speed_meters_per_second(0.0F);
    tunnel_req.set_altitude_meters(-430.5);

    LocationState tunnel_reply;
    ServerContext tunnel_ctx;
    ASSERT_TRUE(service_->UpdateLocationState(&tunnel_ctx, &tunnel_req, &tunnel_reply).ok());

    EXPECT_DOUBLE_EQ(tunnel_reply.latitude(), 52.3676);
    EXPECT_DOUBLE_EQ(tunnel_reply.longitude(), 4.9041);
    EXPECT_DOUBLE_EQ(tunnel_reply.altitude_meters(), -430.5);
    EXPECT_FLOAT_EQ(tunnel_reply.speed_meters_per_second(), 0.0F);
    EXPECT_EQ(tunnel_reply.satellite_count(), 0);
    EXPECT_EQ(tunnel_reply.timestamp_nanos(), 3'000'000'000LL);
    EXPECT_EQ(observable_location_.GetValue().satellites, 0);
}

TEST_F(LocationServiceTest, UpdateLocationStateRejectsOutOfBoundsAndNonFiniteInputs) {
    const auto expect_invalid = [this](const UpdateLocationStateRequest& req) {
        LocationState reply;
        ServerContext ctx;
        const Status status = service_->UpdateLocationState(&ctx, &req, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT) << status.error_message();
    };

    const double nan_d = std::numeric_limits<double>::quiet_NaN();
    const double inf_d = std::numeric_limits<double>::infinity();
    const float nan_f = std::numeric_limits<float>::quiet_NaN();
    const float inf_f = std::numeric_limits<float>::infinity();

    for (const double bad_lat : {-90.001, 90.001, nan_d, inf_d, -inf_d}) {
        UpdateLocationStateRequest req;
        req.set_latitude(bad_lat);
        expect_invalid(req);
    }

    for (const double bad_lon : {-180.001, 180.001, nan_d, inf_d, -inf_d}) {
        UpdateLocationStateRequest req;
        req.set_longitude(bad_lon);
        expect_invalid(req);
    }

    for (const double bad_alt : {nan_d, inf_d, -inf_d}) {
        UpdateLocationStateRequest req;
        req.set_altitude_meters(bad_alt);
        expect_invalid(req);
    }

    for (const float bad_speed : {-0.1F, nan_f, inf_f}) {
        UpdateLocationStateRequest req;
        req.set_speed_meters_per_second(bad_speed);
        expect_invalid(req);
    }

    for (const float bad_bearing : {-0.1F, 360.0F, 720.0F, nan_f, inf_f}) {
        UpdateLocationStateRequest req;
        req.set_bearing_degrees(bad_bearing);
        expect_invalid(req);
    }

    for (const float bad_acc : {-1.0F, nan_f, inf_f}) {
        UpdateLocationStateRequest req;
        req.set_accuracy_meters(bad_acc);
        expect_invalid(req);
    }

    {
        UpdateLocationStateRequest req;
        req.set_satellite_count(-1);
        expect_invalid(req);
    }

    // Boundary values [-90, 90], [-180, 180], and bearing [0, 359.999] must succeed.
    UpdateLocationStateRequest valid_bounds;
    valid_bounds.set_latitude(-90.0);
    valid_bounds.set_longitude(180.0);
    valid_bounds.set_bearing_degrees(359.9F);
    LocationState reply;
    ServerContext ctx;
    EXPECT_TRUE(service_->UpdateLocationState(&ctx, &valid_bounds, &reply).ok());
}

TEST_F(LocationServiceTest, PopulateLocationProtoClampsOutOfRangeAndNonFiniteDoubles) {
    // External publishers (e.g., V1 EmulatorController::setGps) can inject arbitrary doubles into
    // ObservableLocation; narrowing to float in LocationState must saturate safely without UB.
    const double huge = 1e100;
    const double nan_d = std::numeric_limits<double>::quiet_NaN();
    observable_location_.SetValue(Location{
        .latitude = 37.0,
        .longitude = -122.0,
        .speed = huge,
        .bearing = -huge,
        .altitude = 10.0,
        .accuracy = nan_d,
        .satellites = 5,
    });

    GetLocationStateRequest req;
    LocationState reply;
    ServerContext ctx;
    ASSERT_TRUE(service_->GetLocationState(&ctx, &req, &reply).ok());
    EXPECT_FLOAT_EQ(reply.speed_meters_per_second(), std::numeric_limits<float>::max());
    EXPECT_FLOAT_EQ(reply.bearing_degrees(), -std::numeric_limits<float>::max());
    EXPECT_FLOAT_EQ(reply.accuracy_meters(), 0.0F);
}

TEST_F(LocationServiceTest, DisabledGpsRejectsAllRpcsWithFailedPrecondition) {
    service_.reset();
    LocationServiceImpl disabled_service(observable_location_,
                                         LocationServiceImpl::Options{.has_gps = false});

    {
        GetLocationStateRequest req;
        LocationState reply;
        ServerContext ctx;
        EXPECT_EQ(disabled_service.GetLocationState(&ctx, &req, &reply).error_code(),
                  StatusCode::FAILED_PRECONDITION);
    }
    {
        UpdateLocationStateRequest req;
        req.set_latitude(37.0);
        LocationState reply;
        ServerContext ctx;
        EXPECT_EQ(disabled_service.UpdateLocationState(&ctx, &req, &reply).error_code(),
                  StatusCode::FAILED_PRECONDITION);
    }
    {
        PlayRouteRequest req;
        req.add_waypoints()->mutable_location()->set_latitude(37.0);
        PlayRouteResponse reply;
        ServerContext ctx;
        EXPECT_EQ(disabled_service.PlayRoute(&ctx, &req, &reply).error_code(),
                  StatusCode::FAILED_PRECONDITION);
    }

    // Verify StreamLocationState returns FAILED_PRECONDITION over a real in-process gRPC channel.
    ServerBuilder builder;
    builder.RegisterService(&disabled_service);
    std::unique_ptr<Server> server = builder.BuildAndStart();
    auto stub = LocationService::NewStub(server->InProcessChannel({}));

    ClientContext stream_ctx;
    StreamLocationStateRequest stream_req;
    auto reader = stub->StreamLocationState(&stream_ctx, stream_req);
    LocationState msg;
    EXPECT_FALSE(reader->Read(&msg));
    EXPECT_EQ(reader->Finish().error_code(), StatusCode::FAILED_PRECONDITION);
    server->Shutdown();
}

TEST_F(LocationServiceTest, ReadOnlyModeAllowsReadsAndBlocksMutations) {
    RecreateService(LocationServiceImpl::Options{.has_gps = true, .read_only = true});

    GetLocationStateRequest get_req;
    LocationState get_reply;
    ServerContext get_ctx;
    EXPECT_TRUE(service_->GetLocationState(&get_ctx, &get_req, &get_reply).ok());

    UpdateLocationStateRequest update_req;
    update_req.set_latitude(40.0);
    LocationState update_reply;
    ServerContext update_ctx;
    EXPECT_EQ(service_->UpdateLocationState(&update_ctx, &update_req, &update_reply).error_code(),
              StatusCode::PERMISSION_DENIED);

    PlayRouteRequest route_req;
    route_req.add_waypoints()->mutable_location()->set_latitude(40.0);
    PlayRouteResponse route_reply;
    ServerContext route_ctx;
    EXPECT_EQ(service_->PlayRoute(&route_ctx, &route_req, &route_reply).error_code(),
              StatusCode::PERMISSION_DENIED);
}

TEST_F(LocationServiceTest, StreamLocationStateEmitsInitialSnapshotAndCoalescesBursts) {
    auto* writer = service_->CreateLocationStateWriter<test::FakeWriteReactor<LocationState>>();
    ASSERT_EQ(writer->written_messages.size(), 1U);
    EXPECT_DOUBLE_EQ(writer->written_messages[0].latitude(), Location::kDefaultLatitude);

    // Keep the initial write in flight (do not call OnWriteDone yet) while pushing 3 rapid updates.
    for (const double lat : {37.5, 37.6, 37.7}) {
        UpdateLocationStateRequest req;
        req.set_latitude(lat);
        LocationState reply;
        ServerContext ctx;
        ASSERT_TRUE(service_->UpdateLocationState(&ctx, &req, &reply).ok());
    }

    // Because kMaxPendingStreamUpdates == 1, intermediate fixes are coalesced into the latest one.
    writer->OnWriteDone(true);
    ASSERT_EQ(writer->written_messages.size(), 2U);
    EXPECT_DOUBLE_EQ(writer->written_messages[1].latitude(), 37.7);
    writer->OnWriteDone(true);

    writer->OnCancel();
    EXPECT_TRUE(writer->finish_called);
    EXPECT_EQ(writer->finish_status.error_code(), StatusCode::CANCELLED);
    writer->OnDone();
}

TEST_F(LocationServiceTest, PlayRoutePacesWaypointsWithSpeedMultiplierAndStopsAtEnd) {
    auto* writer = service_->CreateLocationStateWriter<test::FakeWriteReactor<LocationState>>();
    ASSERT_EQ(writer->written_messages.size(), 1U);
    writer->OnWriteDone(true);

    PlayRouteRequest req;
    req.set_playback_speed_multiplier(2.0F);  // 2x speed
    {
        auto* wp0 = req.add_waypoints();
        wp0->mutable_location()->set_latitude(37.4220);
        wp0->mutable_location()->set_longitude(-122.0840);
        wp0->set_delay_ms(0);  // Immediate start
    }
    {
        auto* wp1 = req.add_waypoints();
        wp1->mutable_location()->set_latitude(37.4250);
        wp1->mutable_location()->set_longitude(-122.0820);
        wp1->set_delay_ms(0);  // Defaults to 1000ms -> 500ms at 2x speed
    }
    {
        auto* wp2 = req.add_waypoints();
        wp2->mutable_location()->set_latitude(37.4280);
        wp2->mutable_location()->set_longitude(-122.0800);
        wp2->set_delay_ms(4000);  // 4000ms -> 2000ms at 2x speed
    }

    PlayRouteResponse reply;
    ServerContext ctx;
    ASSERT_TRUE(service_->PlayRoute(&ctx, &req, &reply).ok());

    // Waypoint 0 applied immediately.
    ASSERT_EQ(writer->written_messages.size(), 2U);
    EXPECT_DOUBLE_EQ(writer->written_messages[1].latitude(), 37.4220);
    writer->OnWriteDone(true);

    // Not due yet at 499ms.
    event_loop_->AdvanceClock(absl::Milliseconds(499));
    EXPECT_EQ(writer->written_messages.size(), 2U);

    // Due at 500ms -> waypoint 1 fires and schedules waypoint 2 for +2000ms.
    event_loop_->AdvanceClock(absl::Milliseconds(1));
    ASSERT_EQ(writer->written_messages.size(), 3U);
    EXPECT_DOUBLE_EQ(writer->written_messages[2].latitude(), 37.4250);
    writer->OnWriteDone(true);

    // Advance 2000ms -> waypoint 2 fires and route finishes.
    event_loop_->AdvanceClock(absl::Milliseconds(2000));
    ASSERT_EQ(writer->written_messages.size(), 4U);
    EXPECT_DOUBLE_EQ(writer->written_messages[3].latitude(), 37.4280);
    writer->OnWriteDone(true);

    // Further clock advances emit nothing because loop == false.
    event_loop_->AdvanceClock(absl::Seconds(10));
    EXPECT_EQ(writer->written_messages.size(), 4U);

    writer->OnDone();
}

TEST_F(LocationServiceTest, PlayRouteLoopsUntilCancelledByUpdateLocationState) {
    PlayRouteRequest req;
    req.set_loop(true);
    {
        auto* wp0 = req.add_waypoints();
        wp0->mutable_location()->set_latitude(10.0);
        wp0->set_delay_ms(0);
    }
    {
        auto* wp1 = req.add_waypoints();
        wp1->mutable_location()->set_latitude(20.0);
        wp1->set_delay_ms(500);
    }

    PlayRouteResponse route_reply;
    ServerContext route_ctx;
    ASSERT_TRUE(service_->PlayRoute(&route_ctx, &req, &route_reply).ok());
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 10.0);

    // +500ms -> wp1 (20.0)
    event_loop_->AdvanceClock(absl::Milliseconds(500));
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 20.0);

    // Loop wrap-around to wp0 uses default 1000ms because wp0.delay_ms == 0.
    event_loop_->AdvanceClock(absl::Milliseconds(500));
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 20.0);
    event_loop_->AdvanceClock(absl::Milliseconds(500));
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 10.0);

    // Calling UpdateLocationState cancels the looping route and latches the static fix.
    UpdateLocationStateRequest static_req;
    static_req.set_latitude(45.0);
    LocationState static_reply;
    ServerContext static_ctx;
    ASSERT_TRUE(service_->UpdateLocationState(&static_ctx, &static_req, &static_reply).ok());
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 45.0);

    event_loop_->AdvanceClock(absl::Seconds(10));
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 45.0);
}

TEST_F(LocationServiceTest, PlayRouteReplacesActiveRouteAndValidatesUpfront) {
    PlayRouteRequest first_route;
    {
        auto* wp0 = first_route.add_waypoints();
        wp0->mutable_location()->set_latitude(1.0);
        wp0->set_delay_ms(1000);  // Delayed initial waypoint
    }
    PlayRouteResponse reply;
    ServerContext ctx1;
    ASSERT_TRUE(service_->PlayRoute(&ctx1, &first_route, &reply).ok());
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, Location::kDefaultLatitude);

    // Invalid second route (waypoint 1 out of bounds) must fail upfront without applying waypoint 0
    // or cancelling the valid first route.
    PlayRouteRequest invalid_route;
    invalid_route.add_waypoints()->mutable_location()->set_latitude(50.0);
    invalid_route.add_waypoints()->mutable_location()->set_latitude(95.0);  // Invalid!
    ServerContext bad_ctx;
    EXPECT_EQ(service_->PlayRoute(&bad_ctx, &invalid_route, &reply).error_code(),
              StatusCode::INVALID_ARGUMENT);
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, Location::kDefaultLatitude);

    // Valid replacement route supersedes first_route before first_route's 1000ms timer fires.
    PlayRouteRequest replacement_route;
    {
        auto* wp0 = replacement_route.add_waypoints();
        wp0->mutable_location()->set_latitude(2.0);
        wp0->set_delay_ms(0);
    }
    {
        auto* wp1 = replacement_route.add_waypoints();
        wp1->mutable_location()->set_latitude(3.0);
        wp1->set_delay_ms(2000);
    }
    ServerContext ctx2;
    ASSERT_TRUE(service_->PlayRoute(&ctx2, &replacement_route, &reply).ok());
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 2.0);

    // At +1000ms, cancelled first_route (1.0) must NOT fire!
    event_loop_->AdvanceClock(absl::Milliseconds(1000));
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 2.0);

    // At +2000ms total, replacement_route's wp1 (3.0) fires.
    event_loop_->AdvanceClock(absl::Milliseconds(1000));
    EXPECT_DOUBLE_EQ(observable_location_.GetValue().latitude, 3.0);
}

TEST_F(LocationServiceTest, PlayRouteRejectsInvalidArgumentsAndMissingEventLoop) {
    PlayRouteResponse reply;

    // Empty waypoints.
    {
        PlayRouteRequest empty_req;
        ServerContext ctx;
        EXPECT_EQ(service_->PlayRoute(&ctx, &empty_req, &reply).error_code(),
                  StatusCode::INVALID_ARGUMENT);
    }

    // Negative / NaN / Inf playback_speed_multiplier.
    for (const float bad_mult :
         {-1.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        PlayRouteRequest req;
        req.set_playback_speed_multiplier(bad_mult);
        req.add_waypoints()->mutable_location()->set_latitude(37.0);
        ServerContext ctx;
        EXPECT_EQ(service_->PlayRoute(&ctx, &req, &reply).error_code(),
                  StatusCode::INVALID_ARGUMENT);
    }

    // Overflowing scaled delay.
    {
        PlayRouteRequest req;
        req.set_playback_speed_multiplier(1e-30F);
        auto* wp = req.add_waypoints();
        wp->mutable_location()->set_latitude(37.0);
        wp->set_delay_ms(10'000);
        ServerContext ctx;
        EXPECT_EQ(service_->PlayRoute(&ctx, &req, &reply).error_code(),
                  StatusCode::INVALID_ARGUMENT);
    }

    // Missing event_loop fails with FAILED_PRECONDITION when timer pacing is required.
    LocationServiceImpl loop_free_service(observable_location_,
                                          LocationServiceImpl::Options{.event_loop = nullptr});
    {
        PlayRouteRequest timed_req;
        timed_req.add_waypoints()->mutable_location()->set_latitude(37.0);
        timed_req.add_waypoints()->mutable_location()->set_latitude(38.0);
        ServerContext ctx;
        EXPECT_EQ(loop_free_service.PlayRoute(&ctx, &timed_req, &reply).error_code(),
                  StatusCode::FAILED_PRECONDITION);
    }
}

class LocationServiceConcurrencyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        threaded_loop_ = ThreadedEventLoop::Create(LibuvEventLoop::Create("LocationStressLoop"));
        service_ = std::make_unique<LocationServiceImpl>(
                observable_location_,
                LocationServiceImpl::Options{.event_loop = threaded_loop_.get()});
    }

    void TearDown() override {
        service_.reset();
        threaded_loop_->Shutdown().wait();
    }

    std::unique_ptr<ThreadedEventLoop> threaded_loop_;
    ObservableLocation observable_location_;
    std::unique_ptr<LocationServiceImpl> service_;
};

TEST_F(LocationServiceConcurrencyTest, ConcurrentDisjointPartialUpdatesAreAtomic) {
    std::atomic<bool> start{false};
    std::thread t_lat([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        UpdateLocationStateRequest req;
        req.set_latitude(48.8566);
        LocationState reply;
        ServerContext ctx;
        EXPECT_TRUE(service_->UpdateLocationState(&ctx, &req, &reply).ok());
    });
    std::thread t_lon([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        UpdateLocationStateRequest req;
        req.set_longitude(2.3522);
        LocationState reply;
        ServerContext ctx;
        EXPECT_TRUE(service_->UpdateLocationState(&ctx, &req, &reply).ok());
    });
    std::thread t_acc([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        UpdateLocationStateRequest req;
        req.set_accuracy_meters(7.5F);
        LocationState reply;
        ServerContext ctx;
        EXPECT_TRUE(service_->UpdateLocationState(&ctx, &req, &reply).ok());
    });
    std::thread t_sats([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        UpdateLocationStateRequest req;
        req.set_satellite_count(11);
        LocationState reply;
        ServerContext ctx;
        EXPECT_TRUE(service_->UpdateLocationState(&ctx, &req, &reply).ok());
    });

    start.store(true, std::memory_order_release);
    t_lat.join();
    t_lon.join();
    t_acc.join();
    t_sats.join();

    const Location merged = observable_location_.GetValue();
    EXPECT_DOUBLE_EQ(merged.latitude, 48.8566);
    EXPECT_DOUBLE_EQ(merged.longitude, 2.3522);
    EXPECT_FLOAT_EQ(static_cast<float>(merged.accuracy), 7.5F);
    EXPECT_EQ(merged.satellites, 11);
}

TEST_F(LocationServiceConcurrencyTest, ConcurrentRoutePlaybackMutationsAndStreamsAreRaceFree) {
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;

    workers.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 25; ++i) {
            PlayRouteRequest req;
            req.set_loop(true);
            req.set_playback_speed_multiplier(1000.0F);  // 1ms clamped timer ticks
            auto* wp0 = req.add_waypoints();
            wp0->mutable_location()->set_latitude(30.0 + (i % 10));
            wp0->set_delay_ms(1);
            auto* wp1 = req.add_waypoints();
            wp1->mutable_location()->set_latitude(40.0 + (i % 10));
            wp1->set_delay_ms(1);
            PlayRouteResponse reply;
            ServerContext ctx;
            EXPECT_TRUE(service_->PlayRoute(&ctx, &req, &reply).ok());
        }
    });

    workers.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 40; ++i) {
            UpdateLocationStateRequest req;
            req.set_longitude(-120.0 + (i % 20));
            LocationState reply;
            ServerContext ctx;
            EXPECT_TRUE(service_->UpdateLocationState(&ctx, &req, &reply).ok());
        }
    });

    workers.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 40; ++i) {
            auto* writer =
                    service_->CreateLocationStateWriter<test::FakeWriteReactor<LocationState>>();
            GetLocationStateRequest get_req;
            LocationState get_reply;
            ServerContext get_ctx;
            EXPECT_TRUE(service_->GetLocationState(&get_ctx, &get_req, &get_reply).ok());
            writer->OnCancel();
            writer->OnDone();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }
}

TEST_F(LocationServiceConcurrencyTest, DestructionDuringActiveRouteTimerTicksIsSafe) {
    PlayRouteRequest req;
    req.set_loop(true);
    req.set_playback_speed_multiplier(1000.0F);
    auto* wp = req.add_waypoints();
    wp->mutable_location()->set_latitude(37.422);
    wp->set_delay_ms(1);
    PlayRouteResponse reply;
    ServerContext ctx;
    ASSERT_TRUE(service_->PlayRoute(&ctx, &req, &reply).ok());
    absl::SleepFor(absl::Milliseconds(5));
    service_.reset();
}

}  // namespace
}  // namespace goldfish::grpc::v2
