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

#include "absl/status/status.h"

#include "goldfish/sensors/physical_model.h"
#include "goldfish/system/steady_clock.h"
#include "sensors_service.grpc.pb.h"

namespace goldfish::grpc::v2 {

using SensorsServiceBase =
        ::android::emulation::v2::SensorsService::WithCallbackMethod_StreamSensorsState<
                ::android::emulation::v2::SensorsService::WithCallbackMethod_StreamSensorsUpdates<
                        ::android::emulation::v2::SensorsService::
                                WithCallbackMethod_StreamPostureState<
                                        ::android::emulation::v2::SensorsService::Service>>>;

/**
 * Implements the AEMU v2 SensorsService gRPC contract over the Goldfish
 * rigid-body physics and virtual sensor HAL model.
 *
 * Concept & Data Flow:
 *   - Physical Simulation Layer: Mutations to `DevicePose` and
 *     `AmbientEnvironment` drive `goldfish::sensors::PhysicalModel` target
 *     parameters (`SetTargetRotation`, `SetTargetPosition`, `SetTargetLight`,
 *     etc.), which continuously synthesize frame-consistent guest IMU readings
 *     (`accelerometer`, `gyroscope`, `magnetometer`, `orientation_degrees`).
 *   - Raw Sensor Override Layer: Pinning channels via `RawSensorOverrides`
 *     calls `PhysicalModel::Override*` and reports active channels via
 *     `PhysicalModel::IsSensorOverridden` in `ImuTelemetry::active_overrides`
 *     until cleared via `clear_raw_overrides = true` (`PhysicalModel::ClearOverrides`).
 *   - Posture Layer: Queries and mutates foldable postures, multi-hinge angles,
 *     and rollable display extensions via `PhysicalModel::HasFoldableModel()`.
 *
 * Invariants:
 *   - `ImuTelemetry::active_overrides` directly reflects `PhysicalModel`'s
 *     authoritative override bitmask across gRPC and WebRTC callers.
 *
 * Thread Safety:
 *   - Thread-safe across concurrent gRPC threads and WebRTC DataChannel
 *     dispatchers by delegating all state access to the synchronized
 *     `PhysicalModel`.
 *
 * Ownership & Lifetimes:
 *   - Holds a non-owning reference to `goldfish::sensors::PhysicalModel`,
 *     which must outlive this service instance.
 */
class SensorsServiceImpl final : public SensorsServiceBase {
  public:
    explicit SensorsServiceImpl(::goldfish::sensors::PhysicalModel& physical_model,
                                const ::goldfish::system::SteadyClock& clock =
                                        ::goldfish::system::SteadyClock::GetRealClock());
    ~SensorsServiceImpl() override = default;

    ::grpc::Status GetSensorsState(::grpc::ServerContext* context,
                                   const ::android::emulation::v2::GetSensorsStateRequest* request,
                                   ::android::emulation::v2::SensorsState* response) override;

    ::grpc::ServerWriteReactor<::android::emulation::v2::SensorsState>* StreamSensorsState(
            ::grpc::CallbackServerContext* context,
            const ::android::emulation::v2::StreamSensorsStateRequest* request) override;

    ::grpc::Status UpdateSensorsState(
            ::grpc::ServerContext* context,
            const ::android::emulation::v2::UpdateSensorsStateRequest* request,
            ::android::emulation::v2::SensorsState* response) override;

    ::grpc::ServerReadReactor<::android::emulation::v2::UpdateSensorsStateRequest>*
    StreamSensorsUpdates(::grpc::CallbackServerContext* context,
                         ::android::emulation::v2::StreamSensorsUpdatesResponse* response) override;

    ::grpc::Status GetPostureState(::grpc::ServerContext* context,
                                   const ::android::emulation::v2::GetPostureStateRequest* request,
                                   ::android::emulation::v2::PostureState* response) override;

    ::grpc::ServerWriteReactor<::android::emulation::v2::PostureState>* StreamPostureState(
            ::grpc::CallbackServerContext* context,
            const ::android::emulation::v2::StreamPostureStateRequest* request) override;

    ::grpc::Status UpdatePostureState(
            ::grpc::ServerContext* context,
            const ::android::emulation::v2::UpdatePostureStateRequest* request,
            ::android::emulation::v2::PostureState* response) override;

    /**
     * Applies an `UpdateSensorsStateRequest` directly to the underlying
     * `PhysicalModel`. Used both by gRPC handlers and by the low-latency
     * WebRTC `"sensors_v2"` SCTP DataChannel forwarder.
     *
     * @param request Partial sensor/pose mutation request.
     * @return `absl::OkStatus()` on success or `INVALID_ARGUMENT` if out-of-range.
     */
    absl::Status ApplySensorsUpdate(
            const ::android::emulation::v2::UpdateSensorsStateRequest& request);

  private:
    ::android::emulation::v2::SensorsState BuildSensorsState() const;
    ::android::emulation::v2::PostureState BuildPostureState() const;

    ::goldfish::sensors::PhysicalModel& physical_model_;
    const ::goldfish::system::SteadyClock& clock_;
};

}  // namespace goldfish::grpc::v2
