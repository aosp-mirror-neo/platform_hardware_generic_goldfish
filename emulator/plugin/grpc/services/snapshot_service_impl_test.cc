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

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "android/goldfish/vm_interface.h"
#include "fake_vm_operations.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "snapshot_service.grpc.pb.h"

namespace android::emulation::control {
namespace {

using ::android::goldfish::FakeVmOperations;
using ::android::goldfish::VmOperations;
using ::goldfish::async::testing::TestEventLoop;

std::map<std::string, SnapshotDetails> IndexById(const SnapshotList& list) {
    std::map<std::string, SnapshotDetails> by_id;
    for (const SnapshotDetails& details : list.snapshots()) {
        by_id[details.snapshot_id()] = details;
    }
    return by_id;
}

class SnapshotServiceImplTest : public ::testing::Test {
  protected:
    const std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    FakeVmOperations vm_operations_{loop_.get()};
    SnapshotServiceImpl service_{vm_operations_};
};

TEST_F(SnapshotServiceImplTest, ListSnapshotsDefaultsToCompatibleOnlyAndReportsLoaded) {
    constexpr int64_t kDefaultBootCreatedSec = 1700000100;
    constexpr int64_t kCheckpointCreatedSec = 1700000200;
    vm_operations_.entries["default_boot"] = VmOperations::SnapshotEntry{
        .name = "default_boot",
        .timestamp = absl::FromUnixSeconds(kDefaultBootCreatedSec),
        .compatible = true,
        .loaded = true,
    };
    vm_operations_.entries["checkpoint"] = VmOperations::SnapshotEntry{
        .name = "checkpoint",
        .timestamp = absl::FromUnixSeconds(kCheckpointCreatedSec),
        .compatible = true,
        .loaded = false,
    };
    vm_operations_.entries["stale"] = VmOperations::SnapshotEntry{
        .name = "stale",
        .timestamp = absl::FromUnixSeconds(1600000000),
        .compatible = false,
        .loaded = false,
    };

    SnapshotFilter filter;
    SnapshotList reply;
    grpc::ServerContext context;
    ASSERT_TRUE(service_.ListSnapshots(&context, &filter, &reply).ok());

    const auto by_id = IndexById(reply);
    ASSERT_EQ(by_id.size(), 2U);
    ASSERT_TRUE(by_id.contains("default_boot"));
    EXPECT_EQ(by_id.at("default_boot").status(), SnapshotDetails::Loaded);
    EXPECT_EQ(by_id.at("default_boot").details().logical_name(), "default_boot");
    EXPECT_EQ(by_id.at("default_boot").details().creation_time(), kDefaultBootCreatedSec);

    ASSERT_TRUE(by_id.contains("checkpoint"));
    EXPECT_EQ(by_id.at("checkpoint").status(), SnapshotDetails::Compatible);
    EXPECT_EQ(by_id.at("checkpoint").details().logical_name(), "checkpoint");
    EXPECT_EQ(by_id.at("checkpoint").details().creation_time(), kCheckpointCreatedSec);

    SnapshotList null_request_reply;
    grpc::ServerContext null_context;
    ASSERT_TRUE(service_.ListSnapshots(&null_context, nullptr, &null_request_reply).ok());
    EXPECT_EQ(null_request_reply.snapshots_size(), 2);
}

TEST_F(SnapshotServiceImplTest, ListSnapshotsWithAllFilterIncludesIncompatibleSnapshots) {
    vm_operations_.entries["default_boot"] = VmOperations::SnapshotEntry{
        .name = "default_boot",
        .timestamp = absl::FromUnixSeconds(1700000100),
        .compatible = true,
        .loaded = true,
    };
    vm_operations_.entries["compatible_idle"] = VmOperations::SnapshotEntry{
        .name = "compatible_idle",
        .timestamp = absl::FromUnixSeconds(1700000200),
        .compatible = true,
        .loaded = false,
    };
    // Even if loaded were set on an incompatible entry, the proto invariant
    // requires that only compatible snapshots be reported as Loaded.
    vm_operations_.entries["incompatible_stale"] = VmOperations::SnapshotEntry{
        .name = "incompatible_stale",
        .timestamp = absl::FromUnixSeconds(1600000000),
        .compatible = false,
        .loaded = true,
    };

    SnapshotFilter filter;
    filter.set_statusfilter(SnapshotFilter::All);
    SnapshotList reply;
    grpc::ServerContext context;
    ASSERT_TRUE(service_.ListSnapshots(&context, &filter, &reply).ok());

    const auto by_id = IndexById(reply);
    ASSERT_EQ(by_id.size(), 3U);
    EXPECT_EQ(by_id.at("default_boot").status(), SnapshotDetails::Loaded);
    EXPECT_EQ(by_id.at("compatible_idle").status(), SnapshotDetails::Compatible);
    EXPECT_EQ(by_id.at("incompatible_stale").status(), SnapshotDetails::Incompatible);
}

TEST_F(SnapshotServiceImplTest, StartupLoadProgressAndRuntimeCrudUpdateLoadedStatus) {
    for (const char* snapshot_id : {"default_boot", "snap_日本語"}) {
        SnapshotPackage save_req;
        save_req.set_snapshot_id(snapshot_id);
        SnapshotPackage save_reply;
        grpc::ServerContext save_ctx;
        ASSERT_TRUE(service_.SaveSnapshot(&save_ctx, &save_req, &save_reply).ok());
        EXPECT_TRUE(save_reply.success());
        EXPECT_EQ(save_reply.snapshot_id(), snapshot_id);
    }

    // Simulate startup -loadvm default_boot completing via QEMU progress hooks.
    vm_operations_.EmitSnapshotProgress(VmOperations::SnapshotProgress{
        .operation = VmOperations::SnapshotOperationType::Load,
        .phase = VmOperations::SnapshotPhase::Preparing,
        .snapshot_id = "default_boot",
    });
    vm_operations_.EmitSnapshotProgress(VmOperations::SnapshotProgress{
        .operation = VmOperations::SnapshotOperationType::Load,
        .phase = VmOperations::SnapshotPhase::Completed,
        .snapshot_id = "default_boot",
    });

    SnapshotFilter filter;
    SnapshotList initial_list;
    grpc::ServerContext list_ctx1;
    ASSERT_TRUE(service_.ListSnapshots(&list_ctx1, &filter, &initial_list).ok());
    const auto after_boot = IndexById(initial_list);
    ASSERT_EQ(after_boot.size(), 2U);
    EXPECT_EQ(after_boot.at("default_boot").status(), SnapshotDetails::Loaded);
    EXPECT_EQ(after_boot.at("snap_日本語").status(), SnapshotDetails::Compatible);

    // Loading another snapshot moves Loaded to that snapshot.
    SnapshotPackage load_req;
    load_req.set_snapshot_id("snap_日本語");
    SnapshotPackage load_reply;
    grpc::ServerContext load_ctx;
    ASSERT_TRUE(service_.LoadSnapshot(&load_ctx, &load_req, &load_reply).ok());
    EXPECT_TRUE(load_reply.success());
    EXPECT_EQ(load_reply.snapshot_id(), "snap_日本語");

    // Saving a new snapshot does not change which snapshot was loaded.
    SnapshotPackage save_after_req;
    save_after_req.set_snapshot_id("after_load");
    SnapshotPackage save_after_reply;
    grpc::ServerContext save_after_ctx;
    ASSERT_TRUE(service_.SaveSnapshot(&save_after_ctx, &save_after_req, &save_after_reply).ok());
    EXPECT_TRUE(save_after_reply.success());

    SnapshotList mid_list;
    grpc::ServerContext list_ctx2;
    ASSERT_TRUE(service_.ListSnapshots(&list_ctx2, &filter, &mid_list).ok());
    const auto after_load = IndexById(mid_list);
    ASSERT_EQ(after_load.size(), 3U);
    EXPECT_EQ(after_load.at("default_boot").status(), SnapshotDetails::Compatible);
    EXPECT_EQ(after_load.at("snap_日本語").status(), SnapshotDetails::Loaded);
    EXPECT_EQ(after_load.at("after_load").status(), SnapshotDetails::Compatible);

    // Deleting the loaded snapshot leaves no snapshot in the Loaded state.
    SnapshotPackage delete_req;
    delete_req.set_snapshot_id("snap_日本語");
    SnapshotPackage delete_reply;
    grpc::ServerContext delete_ctx;
    ASSERT_TRUE(service_.DeleteSnapshot(&delete_ctx, &delete_req, &delete_reply).ok());
    EXPECT_TRUE(delete_reply.success());
    EXPECT_EQ(delete_reply.snapshot_id(), "snap_日本語");

    SnapshotList final_list;
    grpc::ServerContext list_ctx3;
    ASSERT_TRUE(service_.ListSnapshots(&list_ctx3, &filter, &final_list).ok());
    const auto after_delete = IndexById(final_list);
    ASSERT_EQ(after_delete.size(), 2U);
    EXPECT_EQ(after_delete.at("default_boot").status(), SnapshotDetails::Compatible);
    EXPECT_EQ(after_delete.at("after_load").status(), SnapshotDetails::Compatible);
}

TEST_F(SnapshotServiceImplTest, PropagatesVmOperationFailures) {
    SnapshotPackage missing_req;
    missing_req.set_snapshot_id("missing");
    SnapshotPackage missing_reply;
    grpc::ServerContext missing_ctx;
    EXPECT_EQ(service_.LoadSnapshot(&missing_ctx, &missing_req, &missing_reply).error_code(),
              grpc::StatusCode::NOT_FOUND);
    EXPECT_FALSE(missing_reply.success());
    EXPECT_EQ(missing_reply.snapshot_id(), "missing");

    vm_operations_.fail_save = true;
    SnapshotPackage save_req;
    save_req.set_snapshot_id("failed_save");
    SnapshotPackage save_reply;
    grpc::ServerContext save_ctx;
    EXPECT_EQ(service_.SaveSnapshot(&save_ctx, &save_req, &save_reply).error_code(),
              grpc::StatusCode::INTERNAL);
    EXPECT_FALSE(save_reply.success());
    EXPECT_EQ(save_reply.snapshot_id(), "failed_save");
    vm_operations_.fail_save = false;

    SnapshotPackage delete_req;
    delete_req.set_snapshot_id("missing");
    SnapshotPackage delete_reply;
    grpc::ServerContext delete_ctx;
    EXPECT_EQ(service_.DeleteSnapshot(&delete_ctx, &delete_req, &delete_reply).error_code(),
              grpc::StatusCode::NOT_FOUND);
    EXPECT_FALSE(delete_reply.success());

    vm_operations_.list_status =
            absl::FailedPreconditionError("Cannot list snapshots: no block device");
    SnapshotFilter filter;
    SnapshotList list_reply;
    grpc::ServerContext list_ctx;
    EXPECT_EQ(service_.ListSnapshots(&list_ctx, &filter, &list_reply).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
}

}  // namespace
}  // namespace android::emulation::control
