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

#pragma once

#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/emulation/control/grpc_event_stream_support.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/avd_universe/gps/location.h"
#include "goldfish/eventing/event_sources.h"
#include "goldfish/system/steady_clock.h"
#include "location_service.grpc.pb.h"

namespace goldfish::grpc::v2 {

using LocationServiceBase =
        ::android::emulation::v2::LocationService::WithCallbackMethod_StreamLocationState<
                ::android::emulation::v2::LocationService::Service>;

/**
 * @brief Implements the AEMU v2 LocationService gRPC interface.
 *
 * Observes and injects WGS84 geographic coordinates, signal quality metrics,
 * and server-paced multi-waypoint routes over the shared `ObservableLocation`
 * state.
 *
 * ### Concept & Data Flow:
 * - `GetLocationState` returns the latest cached `LocationState` snapshot.
 * - `StreamLocationState` emits the current `LocationState` immediately upon
 *   subscription and pushes subsequent updates whenever `ObservableLocation`
 *   changes, coalescing rapid bursts to the newest fix for slow consumers.
 * - `UpdateLocationState` validates only the fields present on the wire,
 *   cancels any active `PlayRoute` sequence, and atomically merges the partial
 *   update into `ObservableLocation`.
 * - `PlayRoute` validates all waypoints upfront, cancels any prior route, and
 *   paces waypoint updates on `Options::event_loop` scaled by
 *   `playback_speed_multiplier` (defaulting `0` delays after the initial fix to
 *   1000ms / 1 Hz).
 *
 * ### Invariants:
 * - Outside `RouteState::mutex`, `RouteState::timer != nullptr` if and only if
 *   `RouteState::active_route.has_value()` and `!RouteState::shutting_down`.
 * - `current_location_` always reflects a valid `LocationState` stamped with a
 *   monotonic `timestamp_nanos` sample whenever `options_.has_gps` is true.
 *
 * ### Thread Safety:
 * - Thread-safe across concurrent gRPC threads, `Options::event_loop` timer
 *   callbacks, and external `ObservableLocation` publishers.
 * - Strict lock acquisition order: `RouteState::mutex` ->
 *   `ObservableLocation` internal mutex -> `state_mutex_` (leaf lock). Stream
 *   notifications and `EventLoop::Timer` cancellations always execute outside
 *   service mutexes.
 *
 * ### Ownership & Lifetimes:
 * - Holds non-owning references/pointers to `ObservableLocation` and
 *   `Options::event_loop`, both of which must outlive this service instance.
 *   Cancels active route timers, detaches shared `RouteState` pointers under
 *   `RouteState::mutex` so late timer callbacks become safe no-ops, and
 *   deregisters its `ObservableLocation` listener on destruction.
 */
class LocationServiceImpl final : public LocationServiceBase {
  public:
    using ObservableLocation = ::goldfish::avd_universe::gps::ObservableLocation;
    using Location = ::goldfish::avd_universe::gps::Location;
    using LocationEventSource = ::android::base::eventing::CallbackEventSource<Location>;
    using EventLoop = ::goldfish::async::EventLoop;
    using TimerPtr = std::shared_ptr<EventLoop::Timer>;

    using LocationState = ::android::emulation::v2::LocationState;
    using GetLocationStateRequest = ::android::emulation::v2::GetLocationStateRequest;
    using StreamLocationStateRequest = ::android::emulation::v2::StreamLocationStateRequest;
    using UpdateLocationStateRequest = ::android::emulation::v2::UpdateLocationStateRequest;
    using PlayRouteRequest = ::android::emulation::v2::PlayRouteRequest;
    using PlayRouteResponse = ::android::emulation::v2::PlayRouteResponse;

    static constexpr size_t kMaxPendingStreamUpdates = 1;
    static constexpr absl::Duration kDefaultWaypointDelay = absl::Seconds(1);
    static constexpr absl::Duration kMinScheduledWaypointDelay = absl::Milliseconds(1);
    static constexpr absl::Duration kMaxScheduledWaypointDelay = absl::Hours(24);

    struct Options {
        bool has_gps{true};
        bool read_only{false};
        EventLoop* event_loop{nullptr};
        std::reference_wrapper<const ::goldfish::system::SteadyClock> clock{
                ::goldfish::system::SteadyClock::GetRealClock()};
    };

    template <typename Reactor = ::grpc::ServerWriteReactor<LocationState>>
    using LocationStateWriter = ::android::emulation::control::StateStreamWriter<
            LocationState, Location, LocationEventSource, kMaxPendingStreamUpdates, 0, Reactor>;

    LocationServiceImpl(ObservableLocation& observable_location, Options options);
    explicit LocationServiceImpl(ObservableLocation& observable_location);
    ~LocationServiceImpl() override;

    ::grpc::Status GetLocationState(::grpc::ServerContext* context,
                                    const GetLocationStateRequest* request,
                                    LocationState* reply) override
            ABSL_LOCKS_EXCLUDED(state_mutex_);

    /**
     * @brief Constructs a StateStreamWriter for streaming LocationState messages.
     */
    template <typename Reactor = ::grpc::ServerWriteReactor<LocationState>>
    auto* CreateLocationStateWriter() {
        return new LocationStateWriter<Reactor>(
                &location_events_, [this](LocationState* state) { PopulateLocationState(state); });
    }

    ::grpc::ServerWriteReactor<LocationState>* StreamLocationState(
            ::grpc::CallbackServerContext* context,
            const StreamLocationStateRequest* request) override;

    ::grpc::Status UpdateLocationState(::grpc::ServerContext* context,
                                       const UpdateLocationStateRequest* request,
                                       LocationState* reply) override
            ABSL_LOCKS_EXCLUDED(state_mutex_);

    ::grpc::Status PlayRoute(::grpc::ServerContext* context, const PlayRouteRequest* request,
                             PlayRouteResponse* reply) override ABSL_LOCKS_EXCLUDED(state_mutex_);

    static void PopulateLocationProto(const Location& in, int64_t timestamp_nanos,
                                      LocationState* out);

  private:
    struct PreparedWaypoint {
        UpdateLocationStateRequest location;
        absl::Duration initial_delay;
        absl::Duration step_delay;
    };

    struct ActiveRoute {
        std::vector<PreparedWaypoint> waypoints;
        size_t next_index{0};
        bool loop{false};
    };

    struct RouteState {
        RouteState(ObservableLocation* observable_location, EventLoop* event_loop)
                : observable_location(observable_location), event_loop(event_loop) {}

        absl::Mutex mutex;
        ObservableLocation* observable_location ABSL_GUARDED_BY(mutex);
        EventLoop* event_loop ABSL_GUARDED_BY(mutex);
        bool shutting_down ABSL_GUARDED_BY(mutex){false};
        uint64_t generation ABSL_GUARDED_BY(mutex){0};
        std::optional<ActiveRoute> active_route ABSL_GUARDED_BY(mutex);
        TimerPtr timer ABSL_GUARDED_BY(mutex);
    };

    static absl::StatusOr<std::vector<PreparedWaypoint>> PrepareWaypoints(
            const PlayRouteRequest& request);
    static void ApplyNextWaypointLocked(const std::shared_ptr<RouteState>& route_state,
                                        uint64_t generation)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(route_state->mutex);
    static TimerPtr ScheduleWaypointTimerLocked(const std::shared_ptr<RouteState>& route_state,
                                                uint64_t generation, absl::Duration delay)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(route_state->mutex);
    static void OnRouteTimerTick(const std::shared_ptr<RouteState>& route_state,
                                 uint64_t generation) ABSL_LOCKS_EXCLUDED(route_state->mutex);

    void PopulateLocationState(LocationState* state) const ABSL_LOCKS_EXCLUDED(state_mutex_);
    void OnLocationUpdated(const Location& location) ABSL_LOCKS_EXCLUDED(state_mutex_);
    // Returns OK if GPS is enabled and location state is writable.
    ::grpc::Status CheckMutationAllowed(absl::string_view operation) const;

    ObservableLocation& observable_location_;
    const Options options_;
    const ::goldfish::system::SteadyClock& clock_;
    std::shared_ptr<RouteState> route_state_;

    LocationEventSource location_events_;
    ObservableLocation::CallbackId location_callback_id_{ObservableLocation::kInvalidCallbackId};

    mutable absl::Mutex state_mutex_;
    LocationState current_location_ ABSL_GUARDED_BY(state_mutex_);
    bool has_location_snapshot_ ABSL_GUARDED_BY(state_mutex_){false};
};

}  // namespace goldfish::grpc::v2
