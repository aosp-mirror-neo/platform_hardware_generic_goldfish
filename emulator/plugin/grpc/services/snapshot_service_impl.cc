// Copyright 2026 The Android Open Source Project
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

#include "android/emulation/control/snapshot_service_impl.h"

#include <utility>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"

namespace android::emulation::control {
namespace {

grpc::Status FinishSnapshotPackageRpc(const SnapshotPackage* request, const absl::Status& status,
                                      SnapshotPackage* reply) {
    reply->set_success(status.ok());
    reply->set_snapshot_id(request->snapshot_id());
    return AbslStatusToGrpcStatus(status);
}

}  // namespace

grpc::Status SnapshotServiceImpl::ListSnapshots(grpc::ServerContext* context,
                                                const SnapshotFilter* request,
                                                SnapshotList* reply) {
    using SnapshotEntry = goldfish::VmOperations::SnapshotEntry;
    const bool compatible_only =
            request == nullptr || request->statusfilter() == SnapshotFilter::CompatibleOnly;

    return AbslStatusToGrpcStatus(
            vm_operations_.ListSnapshots([reply, compatible_only](SnapshotEntry se) {
                if (!se.compatible && compatible_only) {
                    return;
                }
                SnapshotDetails* details = reply->mutable_snapshots()->Add();
                details->set_snapshot_id(se.name);
                details->set_status(se.compatible ? (se.loaded ? SnapshotDetails::Loaded
                                                               : SnapshotDetails::Compatible)
                                                  : SnapshotDetails::Incompatible);

                emulator_snapshot::Snapshot* details2 = details->mutable_details();
                details2->set_logical_name(std::move(se.name));
                details2->set_creation_time(absl::ToUnixSeconds(se.timestamp));
            }));
}

grpc::Status SnapshotServiceImpl::LoadSnapshot(grpc::ServerContext* context,
                                               const SnapshotPackage* request,
                                               SnapshotPackage* reply) {
    return FinishSnapshotPackageRpc(
            request, vm_operations_.LoadSnapshot(request->snapshot_id(), /*and_resume=*/true),
            reply);
}

grpc::Status SnapshotServiceImpl::SaveSnapshot(grpc::ServerContext* context,
                                               const SnapshotPackage* request,
                                               SnapshotPackage* reply) {
    return FinishSnapshotPackageRpc(
            request, vm_operations_.SaveSnapshot(request->snapshot_id(), /*overwrite=*/true),
            reply);
}

grpc::Status SnapshotServiceImpl::DeleteSnapshot(grpc::ServerContext* context,
                                                 const SnapshotPackage* request,
                                                 SnapshotPackage* reply) {
    return FinishSnapshotPackageRpc(request, vm_operations_.DeleteSnapshot(request->snapshot_id()),
                                    reply);
}

grpc::Status SnapshotServiceImpl::UpdateSnapshot(grpc::ServerContext* context,
                                                 const SnapshotUpdateDescription* request,
                                                 SnapshotDetails* reply) {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "infeasible");
}

grpc::Status SnapshotServiceImpl::GetScreenshot(grpc::ServerContext* context,
                                                const SnapshotId* request,
                                                SnapshotScreenshotFile* reply) {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Not implemented yet");
}

}  // namespace android::emulation::control
