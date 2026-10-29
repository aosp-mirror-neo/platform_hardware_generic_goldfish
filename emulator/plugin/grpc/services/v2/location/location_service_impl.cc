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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/simple_async_grpc.h"

namespace goldfish::grpc::v2 {

namespace {

using ::android::emulation::control::AbslStatusToGrpcStatus;
using ::goldfish::avd_universe::gps::Location;
using ::grpc::CallbackServerContext;
using ::grpc::ServerContext;
using ::grpc::ServerWriteReactor;
using ::grpc::Status;
using ::grpc::StatusCode;

constexpr double kMinLatitudeDegrees = -90.0;
constexpr double kMaxLatitudeDegrees = 90.0;
constexpr double kMinLongitudeDegrees = -180.0;
constexpr double kMaxLongitudeDegrees = 180.0;
constexpr float kMinBearingDegrees = 0.0F;
constexpr float kMaxBearingExclusiveDegrees = 360.0F;

Status LocationUnsupportedStatus() {
    return Status(StatusCode::FAILED_PRECONDITION,
                  "Failed to access location state: GNSS hardware is disabled in the AVD "
                  "configuration (hw.gps=no). Enable hw.gps in the AVD hardware profile to use "
                  "LocationService.");
}

Status LocationReadOnlyStatus(absl::string_view operation) {
    return Status(StatusCode::PERMISSION_DENIED,
                  absl::StrCat("Failed to execute ", operation,
                               ": location mutations are disabled on this device "
                               "(ACCESS_MODE_READ_ONLY). Query GetLocationState or "
                               "StreamLocationState instead."));
}

absl::Status ValidateLocationUpdate(
        const ::android::emulation::v2::UpdateLocationStateRequest& req) {
    if (req.has_latitude()) {
        const double lat = req.latitude();
        if (!std::isfinite(lat) || lat < kMinLatitudeDegrees || lat > kMaxLatitudeDegrees) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to validate location update: latitude (%g) is out of bounds; expected "
                    "a finite value in [-90.0, 90.0] degrees.",
                    lat));
        }
    }
    if (req.has_longitude()) {
        const double lon = req.longitude();
        if (!std::isfinite(lon) || lon < kMinLongitudeDegrees || lon > kMaxLongitudeDegrees) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to validate location update: longitude (%g) is out of bounds; expected "
                    "a finite value in [-180.0, 180.0] degrees.",
                    lon));
        }
    }
    if (req.has_altitude_meters() && !std::isfinite(req.altitude_meters())) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate location update: altitude_meters (%g) is not finite; provide a "
                "finite altitude in meters.",
                req.altitude_meters()));
    }
    if (req.has_speed_meters_per_second()) {
        const float speed = req.speed_meters_per_second();
        if (!std::isfinite(speed) || speed < 0.0F) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to validate location update: speed_meters_per_second (%g) is invalid; "
                    "expected a finite value >= 0.0 m/s.",
                    speed));
        }
    }
    if (req.has_bearing_degrees()) {
        const float bearing = req.bearing_degrees();
        if (!std::isfinite(bearing) || bearing < kMinBearingDegrees ||
            bearing >= kMaxBearingExclusiveDegrees) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to validate location update: bearing_degrees (%g) is out of bounds; "
                    "expected a finite value in [0.0, 360.0) degrees.",
                    bearing));
        }
    }
    if (req.has_accuracy_meters()) {
        const float accuracy = req.accuracy_meters();
        if (!std::isfinite(accuracy) || accuracy < 0.0F) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to validate location update: accuracy_meters (%g) is invalid; expected "
                    "a finite value >= 0.0 meters.",
                    accuracy));
        }
    }
    if (req.has_satellite_count() && req.satellite_count() < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate location update: satellite_count (%d) is negative; expected >= "
                "0 (use 0 to simulate complete loss of GPS fix).",
                req.satellite_count()));
    }
    return absl::OkStatus();
}

Location MergeLocationUpdate(const Location& base,
                             const ::android::emulation::v2::UpdateLocationStateRequest& req) {
    Location updated = base;
    if (req.has_latitude()) {
        updated.latitude = req.latitude();
    }
    if (req.has_longitude()) {
        updated.longitude = req.longitude();
    }
    if (req.has_altitude_meters()) {
        updated.altitude = req.altitude_meters();
    }
    if (req.has_speed_meters_per_second()) {
        updated.speed = static_cast<double>(req.speed_meters_per_second());
    }
    if (req.has_bearing_degrees()) {
        updated.bearing = static_cast<double>(req.bearing_degrees());
    }
    if (req.has_accuracy_meters()) {
        updated.accuracy = static_cast<double>(req.accuracy_meters());
    }
    if (req.has_satellite_count()) {
        updated.satellites = req.satellite_count();
    }
    return updated;
}

absl::StatusOr<absl::Duration> ComputeScaledWaypointDelay(uint64_t raw_delay_ms,
                                                          double speed_multiplier,
                                                          size_t waypoint_index) {
    const double scaled_ms = static_cast<double>(raw_delay_ms) / speed_multiplier;
    const double max_ms =
            absl::ToDoubleMilliseconds(LocationServiceImpl::kMaxScheduledWaypointDelay);
    if (!std::isfinite(scaled_ms) || scaled_ms > max_ms) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to start route playback at waypoints[%zu]: scaled delay (%g ms from "
                "delay_ms=%llu and playback_speed_multiplier=%g) exceeds maximum supported "
                "waypoint delay (%g ms); reduce delay_ms or increase playback_speed_multiplier.",
                waypoint_index, scaled_ms, static_cast<unsigned long long>(raw_delay_ms),
                speed_multiplier, max_ms));
    }
    const absl::Duration scaled = absl::Milliseconds(scaled_ms);
    return std::max(scaled, LocationServiceImpl::kMinScheduledWaypointDelay);
}

float ClampDoubleToFloat(double value) {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    constexpr double kMaxFloat = static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(std::clamp(value, -kMaxFloat, kMaxFloat));
}

// Cancels and resets the timer if non-null.
void CancelTimer(LocationServiceImpl::TimerPtr& timer) {
    if (timer != nullptr) {
        timer->Cancel();
        timer.reset();
    }
}

}  // namespace

LocationServiceImpl::LocationServiceImpl(ObservableLocation& observable_location)
        : LocationServiceImpl(observable_location, Options{}) {}

LocationServiceImpl::LocationServiceImpl(ObservableLocation& observable_location, Options options)
        : observable_location_(observable_location)
        , options_(std::move(options))
        , clock_(options_.clock.get())
        , route_state_(std::make_shared<RouteState>(&observable_location, options_.event_loop)) {
    if (options_.has_gps) {
        location_callback_id_ = observable_location_.AddCallback(
                [this](const Location& location) { OnLocationUpdated(location); });
        const Location initial = observable_location_.GetValue();
        const int64_t now_nanos = absl::ToInt64Nanoseconds(clock_.Now());
        absl::MutexLock state_lock(&state_mutex_);
        if (!has_location_snapshot_) {
            PopulateLocationProto(initial, now_nanos, &current_location_);
            has_location_snapshot_ = true;
        }
    }
}

LocationServiceImpl::~LocationServiceImpl() {
    TimerPtr timer_to_cancel;
    {
        absl::MutexLock route_lock(&route_state_->mutex);
        route_state_->shutting_down = true;
        route_state_->observable_location = nullptr;
        route_state_->event_loop = nullptr;
        ++route_state_->generation;
        route_state_->active_route.reset();
        timer_to_cancel = std::move(route_state_->timer);
    }
    CancelTimer(timer_to_cancel);

    if (location_callback_id_ != ObservableLocation::kInvalidCallbackId) {
        observable_location_.RemoveCallback(location_callback_id_);
    }
}

void LocationServiceImpl::PopulateLocationProto(const Location& in, int64_t timestamp_nanos,
                                                LocationState* out) {
    out->Clear();
    out->set_latitude(in.latitude);
    out->set_longitude(in.longitude);
    out->set_altitude_meters(in.altitude);
    out->set_speed_meters_per_second(ClampDoubleToFloat(in.speed));
    out->set_bearing_degrees(ClampDoubleToFloat(in.bearing));
    out->set_accuracy_meters(ClampDoubleToFloat(in.accuracy));
    out->set_satellite_count(in.satellites);
    out->set_timestamp_nanos(timestamp_nanos);
}

void LocationServiceImpl::PopulateLocationState(LocationState* state) const {
    absl::MutexLock state_lock(&state_mutex_);
    *state = current_location_;
}

void LocationServiceImpl::OnLocationUpdated(const Location& location) {
    const int64_t now_nanos = absl::ToInt64Nanoseconds(clock_.Now());
    {
        absl::MutexLock state_lock(&state_mutex_);
        PopulateLocationProto(location, now_nanos, &current_location_);
        has_location_snapshot_ = true;
    }
    location_events_.FireEvent(location);
}

Status LocationServiceImpl::GetLocationState(ServerContext* /*context*/,
                                             const GetLocationStateRequest* /*request*/,
                                             LocationState* reply) {
    if (!options_.has_gps) {
        return LocationUnsupportedStatus();
    }
    PopulateLocationState(reply);
    return Status::OK;
}

ServerWriteReactor<LocationServiceImpl::LocationState>* LocationServiceImpl::StreamLocationState(
        CallbackServerContext* /*context*/, const StreamLocationStateRequest* /*request*/) {
    if (!options_.has_gps) {
        return new ::ErrorServerWriter<LocationState>(LocationUnsupportedStatus());
    }
    return CreateLocationStateWriter();
}

Status LocationServiceImpl::CheckMutationAllowed(absl::string_view operation) const {
    if (!options_.has_gps) {
        return LocationUnsupportedStatus();
    }
    if (options_.read_only) {
        return LocationReadOnlyStatus(operation);
    }
    return Status::OK;
}

Status LocationServiceImpl::UpdateLocationState(ServerContext* /*context*/,
                                                const UpdateLocationStateRequest* request,
                                                LocationState* reply) {
    if (const Status status = CheckMutationAllowed("UpdateLocationState"); !status.ok()) {
        return status;
    }
    if (const absl::Status status = ValidateLocationUpdate(*request); !status.ok()) {
        return AbslStatusToGrpcStatus(status);
    }

    TimerPtr timer_to_cancel;
    {
        absl::MutexLock route_lock(&route_state_->mutex);
        if (route_state_->shutting_down) {
            return Status(StatusCode::UNAVAILABLE,
                          "Failed to update location state: LocationService is shutting down.");
        }
        ++route_state_->generation;
        route_state_->active_route.reset();
        timer_to_cancel = std::move(route_state_->timer);

        const Location updated = MergeLocationUpdate(observable_location_.GetValue(), *request);
        observable_location_.SetValue(updated);
        PopulateLocationState(reply);
    }
    CancelTimer(timer_to_cancel);
    return Status::OK;
}

absl::StatusOr<std::vector<LocationServiceImpl::PreparedWaypoint>>
LocationServiceImpl::PrepareWaypoints(const PlayRouteRequest& request) {
    if (request.waypoints().empty()) {
        return absl::InvalidArgumentError(
                "Failed to start route playback: PlayRouteRequest.waypoints is empty; "
                "provide at least one RouteWaypoint.");
    }

    const float raw_speed_multiplier = request.playback_speed_multiplier();
    if (!std::isfinite(raw_speed_multiplier) || raw_speed_multiplier < 0.0F) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to start route playback: playback_speed_multiplier (%g) is invalid; "
                "expected a finite value > 0.0 (or 0.0 to use the default 1.0x real-time speed).",
                raw_speed_multiplier));
    }
    const double speed_multiplier =
            raw_speed_multiplier == 0.0F ? 1.0 : static_cast<double>(raw_speed_multiplier);
    const uint64_t default_delay_ms =
            static_cast<uint64_t>(absl::ToInt64Milliseconds(kDefaultWaypointDelay));

    std::vector<PreparedWaypoint> prepared;
    prepared.reserve(static_cast<size_t>(request.waypoints_size()));
    for (int i = 0; i < request.waypoints_size(); ++i) {
        const auto& wp = request.waypoints(i);
        if (const absl::Status status = ValidateLocationUpdate(wp.location()); !status.ok()) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to start route playback at waypoints[%d]: %s", i, status.message()));
        }

        const size_t idx = static_cast<size_t>(i);
        const uint64_t step_raw_ms = wp.delay_ms() > 0 ? wp.delay_ms() : default_delay_ms;
        absl::StatusOr<absl::Duration> step_delay =
                ComputeScaledWaypointDelay(step_raw_ms, speed_multiplier, idx);
        if (!step_delay.ok()) {
            return step_delay.status();
        }

        const absl::Duration initial_delay =
                (i == 0 && wp.delay_ms() > 0) ? *step_delay : absl::ZeroDuration();

        prepared.push_back(PreparedWaypoint{
            .location = wp.location(),
            .initial_delay = initial_delay,
            .step_delay = *step_delay,
        });
    }
    return prepared;
}

Status LocationServiceImpl::PlayRoute(ServerContext* /*context*/, const PlayRouteRequest* request,
                                      PlayRouteResponse* /*reply*/) {
    if (const Status status = CheckMutationAllowed("PlayRoute"); !status.ok()) {
        return status;
    }
    absl::StatusOr<std::vector<PreparedWaypoint>> prepared = PrepareWaypoints(*request);
    if (!prepared.ok()) {
        return AbslStatusToGrpcStatus(prepared.status());
    }

    const bool needs_timer = (*prepared)[0].initial_delay > absl::ZeroDuration() ||
                             prepared->size() > 1 || request->loop();
    if (needs_timer && options_.event_loop == nullptr) {
        return Status(StatusCode::FAILED_PRECONDITION,
                      "Failed to start route playback: server event loop is not configured for "
                      "timed waypoint pacing; supply an EventLoop or use UpdateLocationState for "
                      "static fixes.");
    }

    TimerPtr timer_to_cancel;
    {
        absl::MutexLock route_lock(&route_state_->mutex);
        if (route_state_->shutting_down) {
            return Status(StatusCode::UNAVAILABLE,
                          "Failed to start route playback: LocationService is shutting down.");
        }
        const uint64_t generation = ++route_state_->generation;
        timer_to_cancel = std::move(route_state_->timer);
        route_state_->active_route = ActiveRoute{
            .waypoints = std::move(*prepared),
            .next_index = 0,
            .loop = request->loop(),
        };
        if (route_state_->active_route->waypoints[0].initial_delay == absl::ZeroDuration()) {
            ApplyNextWaypointLocked(route_state_, generation);
        } else {
            route_state_->timer = ScheduleWaypointTimerLocked(
                    route_state_, generation,
                    route_state_->active_route->waypoints[0].initial_delay);
        }
    }
    CancelTimer(timer_to_cancel);
    return Status::OK;
}

void LocationServiceImpl::ApplyNextWaypointLocked(const std::shared_ptr<RouteState>& route_state,
                                                  uint64_t generation) {
    DCHECK(route_state->active_route.has_value());
    DCHECK(route_state->observable_location != nullptr);

    ActiveRoute& active_route = *route_state->active_route;
    const size_t current_index = active_route.next_index;
    const Location updated = MergeLocationUpdate(route_state->observable_location->GetValue(),
                                                 active_route.waypoints[current_index].location);
    route_state->observable_location->SetValue(updated);

    if (current_index + 1 >= active_route.waypoints.size() && !active_route.loop) {
        route_state->active_route.reset();
        return;
    }

    const size_t next_index =
            (current_index + 1 < active_route.waypoints.size()) ? current_index + 1 : 0;
    active_route.next_index = next_index;
    route_state->timer = ScheduleWaypointTimerLocked(route_state, generation,
                                                     active_route.waypoints[next_index].step_delay);
}

LocationServiceImpl::TimerPtr LocationServiceImpl::ScheduleWaypointTimerLocked(
        const std::shared_ptr<RouteState>& route_state, uint64_t generation, absl::Duration delay) {
    DCHECK(route_state->event_loop != nullptr);
    return route_state->event_loop->ScheduleDelayed(
            [route_state, generation]() { OnRouteTimerTick(route_state, generation); }, delay);
}

void LocationServiceImpl::OnRouteTimerTick(const std::shared_ptr<RouteState>& route_state,
                                           uint64_t generation) {
    TimerPtr expired_timer;
    {
        absl::MutexLock route_lock(&route_state->mutex);
        if (route_state->shutting_down || generation != route_state->generation ||
            !route_state->active_route.has_value()) {
            return;
        }
        expired_timer = std::move(route_state->timer);
        ApplyNextWaypointLocked(route_state, generation);
    }
}

}  // namespace goldfish::grpc::v2
