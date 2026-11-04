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

#include "legacy_console_bridge.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/base/system.h"
#include "android/base/testing/test_system.h"
#include "android/base/testing/test_temp_dir.h"
#include "emulator_controller.grpc.pb.h"
#include "emulator_controller_mock.grpc.pb.h"
#include "goldfish/discovery/emulator_advertisement.h"
#include "goldfish/file/file.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/testing/fake_metrics_writer.h"
#include "snapshot_service.grpc.pb.h"
#include "snapshot_service_mock.grpc.pb.h"
#include "telnet_auth.h"

namespace goldfish::telnet {
namespace {

using ::goldfish::metrics::testing::FakeMetricsWriter;
using testing::_;

struct MockConsoleContext : public LegacyConsoleBridge::ConsoleContext {
    using ConsoleContext::ConsoleContext;

    absl::StatusOr<std::unique_ptr<android::emulation::control::EmulatorController::StubInterface>>
    EmulatorControllerStub() override {
        if (mock_stub) {
            return std::move(mock_stub);
        }
        return ConsoleContext::EmulatorControllerStub();
    }

    absl::StatusOr<std::unique_ptr<android::emulation::control::SnapshotService::StubInterface>>
    SnapshotStub() override {
        if (mock_snapshot_stub) {
            return std::move(mock_snapshot_stub);
        }
        return ConsoleContext::SnapshotStub();
    }

    absl::StatusOr<std::unique_ptr<grpc::ClientContext>> NewContext(
            std::chrono::time_point<std::chrono::system_clock> deadline =
                    std::chrono::system_clock::now() + std::chrono::milliseconds(500)) override {
        ++new_context_count;
        auto ctx = std::make_unique<grpc::ClientContext>();
        ctx->set_deadline(deadline);
        return ctx;
    }

    absl::StatusOr<std::vector<std::filesystem::path>> DiscoverRunningEmulators() override {
        if (mock_discovered_emulators) {
            return *mock_discovered_emulators;
        }
        return ConsoleContext::DiscoverRunningEmulators();
    }

    absl::StatusOr<LegacyConsoleBridge::DiscoveredEmulator> DiscoverEmulatorWithProperties(
            const absl::flat_hash_map<std::string, std::string>& props) override {
        if (mock_discovery) {
            return *mock_discovery;
        }
        return ConsoleContext::DiscoverEmulatorWithProperties(props);
    }

    void TriggerCrash() override { crash_triggered = true; }

    std::unique_ptr<android::emulation::control::EmulatorController::StubInterface> mock_stub;
    std::unique_ptr<android::emulation::control::SnapshotService::StubInterface> mock_snapshot_stub;
    std::optional<absl::StatusOr<LegacyConsoleBridge::DiscoveredEmulator>> mock_discovery;
    std::optional<absl::StatusOr<std::vector<std::filesystem::path>>> mock_discovered_emulators;
    int new_context_count = 0;
    bool crash_triggered = false;
};

class LegacyConsoleBridgeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_home_ = tmpdir_.Path() / "test_home_auth";
        auto discovery_dir = test_home_ / "Library/Caches/TemporaryItems/avd/running";
        auto status = android::base::file::mkdir_recursive(discovery_dir, 0700);
        ASSERT_TRUE(status.ok()) << "Failed to create test home directory: " << status.message();
        test_system_.SetHomeDirectory(test_home_);
        token_path_ = (test_home_ / ".emulator_console_auth_token").string();

        WriteToken("valid_token_123");

        auto writer = std::make_unique<FakeMetricsWriter>();
        metrics_writer_ = writer.get();
        metrics_reporter_.SetWriter(std::move(writer));
        bridge_ = std::make_unique<LegacyConsoleBridge>(5554, token_path_, metrics_reporter_);
    }

    std::unique_ptr<MockConsoleContext> CreateContext(int port = 5554) {
        return std::make_unique<MockConsoleContext>(port);
    }

    void WriteToken(const std::string& token) {
        std::ofstream ofs(token_path_);
        ofs << token;
    }

    android::base::TestTempDir tmpdir_{"LegacyConsoleBridgeTest"};
    android::base::TestSystem test_system_{"/foo/bar"};
    std::filesystem::path test_home_;
    std::string token_path_;
    goldfish::metrics::MetricsReporter metrics_reporter_;
    FakeMetricsWriter* metrics_writer_ = nullptr;
    std::unique_ptr<LegacyConsoleBridge> bridge_;
};

TEST_F(LegacyConsoleBridgeTest, AuthSucceedsWithValidToken) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    EXPECT_FALSE(ctx.authenticated);

    auto result = (*bridge_)("auth valid_token_123", ctx);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "Android Console: type 'help' for a list of commands");
    EXPECT_TRUE(ctx.authenticated);
}

TEST_F(LegacyConsoleBridgeTest, AuthFailsWithInvalidToken) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    EXPECT_FALSE(ctx.authenticated);

    auto result = (*bridge_)("auth wrong_token", ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("authentication token does not match") !=
                std::string::npos);
    EXPECT_FALSE(ctx.authenticated);
}

TEST_F(LegacyConsoleBridgeTest, HelpReturnsDifferentListAfterAuth) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);

    auto result_before = (*bridge_)("help", ctx);
    ASSERT_TRUE(result_before.ok());

    auto auth_result = (*bridge_)("auth valid_token_123", ctx);
    ASSERT_TRUE(auth_result.ok());

    auto result_after = (*bridge_)("help", ctx);
    ASSERT_TRUE(result_after.ok());

    EXPECT_NE(*result_before, *result_after);
}

TEST_F(LegacyConsoleBridgeTest, PingFailsWhenNoEmulatorFound) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    ctx.authenticated = true;  // Safe to call commands

    auto result = (*bridge_)("ping", ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LegacyConsoleBridgeTest, PingSucceedsAndReturnsAlive) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("ping", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "I am alive!");
}

TEST_F(LegacyConsoleBridgeTest, GeoFixFailsWhenNoEmulatorFound) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    ctx.authenticated = true;  // Safe to call commands

    auto result = (*bridge_)("geo fix 12.3 45.6", ctx);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LegacyConsoleBridgeTest, GeoFixFailsWithInvalidCoordinates) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    ctx.authenticated = true;  // Safe to call commands

    auto result = (*bridge_)("geo fix 200.0 45.6", ctx);  // Invalid longitude

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Longitude must be between -180 and 180") !=
                std::string::npos);

    result = (*bridge_)("geo fix 12.3 100.0", ctx);  // Invalid latitude

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Latitude must be between -90 and 90") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, GeoFixSucceedsWithValidCoordinates) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setGps(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::GpsState& request,
                         google::protobuf::Empty* response) {
                EXPECT_DOUBLE_EQ(request.longitude(), 12.3);
                EXPECT_DOUBLE_EQ(request.latitude(), 45.6);
                EXPECT_EQ(request.satellites(), 4);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("geo fix 12.3 45.6", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, GeoFixForwardsExplicitSatelliteCountIncludingZero) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setGps(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::GpsState& request,
                         google::protobuf::Empty* response) {
                EXPECT_DOUBLE_EQ(request.longitude(), 12.3);
                EXPECT_DOUBLE_EQ(request.latitude(), 45.6);
                EXPECT_DOUBLE_EQ(request.altitude(), 10.0);
                EXPECT_EQ(request.satellites(), 0);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("geo fix 12.3 45.6 10.0 0", *ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();

    auto invalid = (*bridge_)("geo fix 12.3 45.6 10.0 13", *ctx);
    EXPECT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(LegacyConsoleBridgeTest, FingerTouchSendsGrpcRequest) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, sendFingerprint(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::Fingerprint& request,
                         google::protobuf::Empty* response) {
                EXPECT_TRUE(request.istouching());
                EXPECT_EQ(request.touchid(), 1);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("finger touch 1", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, FingerRemoveSendsGrpcRequest) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, sendFingerprint(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::Fingerprint& request,
                         google::protobuf::Empty* response) {
                EXPECT_FALSE(request.istouching());
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("finger remove", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, HeartbeatFailsWhenNotAuthenticated) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    EXPECT_FALSE(ctx.authenticated);

    auto result = (*bridge_)("avd heartbeat", ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
    EXPECT_TRUE(result.status().message().find("unknown command") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, HeartbeatFailsWhenNoEmulatorFound) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    ctx.authenticated = true;

    auto result = (*bridge_)("avd heartbeat", ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LegacyConsoleBridgeTest, HeartbeatSucceedsAndReturnsStatus) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) {
                response->set_heartbeat(42);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd heartbeat", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "heartbeat: 42");
}

TEST_F(LegacyConsoleBridgeTest, NameFailsWhenNoEmulatorFound) {
    LegacyConsoleBridge::ConsoleContext ctx(5554);
    ctx.authenticated = true;

    auto result = (*bridge_)("avd name", ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LegacyConsoleBridgeTest, NameSucceedsAndReturnsAvdName) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) {
                auto* config = response->mutable_platformconfig();
                (*config)["avd.name"] = "Pixel_9_Pro";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd name", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "Pixel_9_Pro");
}

TEST_F(LegacyConsoleBridgeTest, IdSucceedsAndReturnsAvdId) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) {
                auto* config = response->mutable_platformconfig();
                (*config)["avd.id"] = "avd_id_123";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd id", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "avd_id_123");
}

TEST_F(LegacyConsoleBridgeTest, NameReturnsUnknownIfAvdNameMissingInPlatformConfig) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) {
                auto* config = response->mutable_platformconfig();
                (*config)["other.property"] = "some_value";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd name", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
}

TEST_F(LegacyConsoleBridgeTest, PathSucceedsAndReturnsAvdPath) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) {
                auto* config = response->mutable_platformconfig();
                (*config)["avd.content_path"] = "/path/to/avd";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd path", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "/path/to/avd");
}

TEST_F(LegacyConsoleBridgeTest, PathReturnsErrorIfAvdPathMissingInPlatformConfig) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) {
                auto* config = response->mutable_platformconfig();
                (*config)["other.property"] = "some_value";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd path", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
}

TEST_F(LegacyConsoleBridgeTest, AvdGrpcSucceedsWhenEmulatorIsActive) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    LegacyConsoleBridge::DiscoveredEmulator mock_res;
    mock_res.discovery_file = "/path/to/dummy.ini";
    mock_res.properties["port.serial"] = "5554";
    mock_res.properties["grpc.port"] = "8554";
    ctx->mock_discovery = mock_res;

    auto result = (*bridge_)("avd grpc", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "8554");
}

TEST_F(LegacyConsoleBridgeTest, AvdGrpcFailsWhenNoActiveGrpcService) {
    auto ctx = CreateContext();
    ctx->authenticated = true;
    ctx->mock_discovery = absl::NotFoundError("No matching emulator found");

    auto result = (*bridge_)("avd grpc", *ctx);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LegacyConsoleBridgeTest, AvdGrpcFailsWhenMultipleEmulatorsMatch) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto file1 = tmpdir_.Path() / "pid_1.ini";
    auto file2 = tmpdir_.Path() / "pid_2.ini";

    std::ofstream ofs1(file1);
    ofs1 << "port.serial=5554\n";
    ofs1 << "grpc.port=8554\n";
    ofs1.close();

    std::ofstream ofs2(file2);
    ofs2 << "port.serial=5554\n";
    ofs2 << "grpc.port=8555\n";
    ofs2.close();

    ctx->mock_discovered_emulators = std::vector<std::filesystem::path>{file1, file2};

    auto result = (*bridge_)("avd grpc", *ctx);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(result.status().message().find("Multiple matching emulators found") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, KillSucceedsAndCallsSetVmState) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::VmRunState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.state(), android::emulation::control::VmRunState::SHUTDOWN);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("kill", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, CrashSucceedsAndTriggersCrash) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("crash", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "crashing emulator, bye bye");
    EXPECT_TRUE(ctx->crash_triggered);
}

TEST_F(LegacyConsoleBridgeTest, CrashOnExitSucceedsAndTriggersCrash) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("crash-on-exit", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "crashing emulator on exit, bye bye");
    EXPECT_TRUE(ctx->crash_triggered);
}

TEST_F(LegacyConsoleBridgeTest, DebugReturnsWarningForTags) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("debug init,sensors", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "warning: debug tags are deprecated and have no effect");
}

TEST_F(LegacyConsoleBridgeTest, DebugReturnsWarningWithoutArgs) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("debug", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "warning: debug tags are deprecated and have no effect");
}

TEST_F(LegacyConsoleBridgeTest, GrpcStartReturnsPort) {
    auto ctx = CreateContext(8554);
    ctx->authenticated = true;

    auto result = (*bridge_)("grpc start 8554", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "gRPC endpoint available at port 8554");
}

TEST_F(LegacyConsoleBridgeTest, GrpcStartReportsAlreadyActivatedPort) {
    auto ctx = CreateContext(8554);
    ctx->authenticated = true;

    auto result = (*bridge_)("grpc start 8558", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "Port has already been activated at port: 8554");
}

TEST_F(LegacyConsoleBridgeTest, GrpcStartFailsOnInvalidPort) {
    auto ctx = CreateContext(8554);
    ctx->authenticated = true;

    auto result = (*bridge_)("grpc start -1", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(LegacyConsoleBridgeTest, ResumeSucceedsAndCallsSetVmState) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::VmRunState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.state(), android::emulation::control::VmRunState::RUNNING);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd resume", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, CreatesTokenFileIfMissingOnConstruction) {
    std::filesystem::remove(token_path_);
    EXPECT_FALSE(std::filesystem::exists(token_path_));

    auto new_bridge = std::make_unique<LegacyConsoleBridge>(5554, token_path_, metrics_reporter_);

    EXPECT_TRUE(std::filesystem::exists(token_path_));
    EXPECT_EQ(TelnetAuth::GetStatus(token_path_), AuthStatus::kRequired);

    auto read_result = TelnetAuth::ReadToken(token_path_);
    ASSERT_TRUE(read_result.ok()) << read_result.status();
    EXPECT_FALSE(read_result->AsStringView().empty());
}

TEST_F(LegacyConsoleBridgeTest, CreateContextRespectsDisabledAuth) {
    WriteToken("");
    EXPECT_EQ(TelnetAuth::GetStatus(token_path_), AuthStatus::kDisabled);

    auto ctx = bridge_->CreateContext();
    EXPECT_TRUE(ctx->authenticated);
}

TEST_F(LegacyConsoleBridgeTest, WelcomeMessageDoesNotRequireAuthWhenDisabled) {
    WriteToken("");
    auto ctx = bridge_->CreateContext();
    auto welcome = bridge_->WelcomeMessage(*ctx);

    EXPECT_EQ(welcome, "Android Console: type 'help' for a list of commands\r\n");
}

TEST_F(LegacyConsoleBridgeTest, RotateAdvancesClockwise) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getPhysicalModel(_, _, _))
            .WillOnce(
                    [](grpc::ClientContext* context,
                       const android::emulation::control::PhysicalModelValue& request,
                       android::emulation::control::PhysicalModelValue* response) -> grpc::Status {
                        EXPECT_EQ(request.target(),
                                  android::emulation::control::PhysicalModelValue::ROTATION);
                        response->set_target(
                                android::emulation::control::PhysicalModelValue::ROTATION);
                        response->mutable_value()->add_data(0.0f);
                        response->mutable_value()->add_data(0.0f);
                        response->mutable_value()->add_data(0.0f);  // Current is portrait (0 deg)
                        return grpc::Status::OK;
                    });
    EXPECT_CALL(*mock_stub, setPhysicalModel(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::PhysicalModelValue& request,
                         google::protobuf::Empty* response) -> grpc::Status {
                EXPECT_EQ(request.target(),
                          android::emulation::control::PhysicalModelValue::ROTATION);
                EXPECT_EQ(request.value().data_size(), 3);
                EXPECT_FLOAT_EQ(request.value().data(2), -90.0f);  // Clockwise is -90 deg
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("rotate", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, FoldSetsClosedPosture) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setPhysicalModel(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::PhysicalModelValue& request,
                         google::protobuf::Empty* response) -> grpc::Status {
                EXPECT_EQ(request.target(),
                          android::emulation::control::PhysicalModelValue::POSTURE);
                EXPECT_EQ(request.value().data_size(), 1);
                EXPECT_FLOAT_EQ(request.value().data(0), 1.0f);  // Closed posture
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("fold", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, UnfoldSetsOpenedPosture) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setPhysicalModel(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::PhysicalModelValue& request,
                         google::protobuf::Empty* response) -> grpc::Status {
                EXPECT_EQ(request.target(),
                          android::emulation::control::PhysicalModelValue::POSTURE);
                EXPECT_EQ(request.value().data_size(), 1);
                EXPECT_FLOAT_EQ(request.value().data(0), 3.0f);  // Opened posture
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("unfold", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PostureSetsSpecificPosture) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setPhysicalModel(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::PhysicalModelValue& request,
                         google::protobuf::Empty* response) -> grpc::Status {
                EXPECT_EQ(request.target(),
                          android::emulation::control::PhysicalModelValue::POSTURE);
                EXPECT_EQ(request.value().data_size(), 1);
                EXPECT_FLOAT_EQ(request.value().data(0), 2.0f);  // Half-opened posture
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("posture 2", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PostureFailsOnMissingArgs) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("posture", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Usage: \"posture <posture_id>\"") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, PostureFailsOnInvalidPosture) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("posture 99", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Usage: \"posture <posture_id>\"") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotsPathQueriesPath) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) -> grpc::Status {
                auto* config = response->mutable_platformconfig();
                (*config)["avd.content_path"] = "/path/to/avd";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshotspath", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, (std::filesystem::path("/path/to/avd") / "snapshots").string());
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotPathQueriesSpecificPath) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getStatus(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::EmulatorStatus* response) -> grpc::Status {
                auto* config = response->mutable_platformconfig();
                (*config)["avd.content_path"] = "/path/to/avd";
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshotpath snap1", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, (std::filesystem::path("/path/to/avd") / "snapshots" / "snap1").string());
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotListReturnsFormattedList) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockSnapshotServiceStub>();
    EXPECT_CALL(*mock_stub, ListSnapshots(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SnapshotFilter& request,
                         android::emulation::control::SnapshotList* response) -> grpc::Status {
                EXPECT_EQ(request.statusfilter(), android::emulation::control::SnapshotFilter::All);
                auto* snap1 = response->add_snapshots();
                snap1->set_snapshot_id("1");
                snap1->mutable_details()->set_logical_name("default_boot");
                auto* snap2 = response->add_snapshots();
                snap2->set_snapshot_id("checkpoint1");
                return grpc::Status::OK;
            });
    ctx->mock_snapshot_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshot list", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result,
              "List of snapshots present on all disks:\r\ndefault_boot\r\ncheckpoint1\r\n");
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotListReturnsNoSnapshotAvailableWhenEmpty) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockSnapshotServiceStub>();
    EXPECT_CALL(*mock_stub, ListSnapshots(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SnapshotFilter& request,
                         android::emulation::control::SnapshotList* response) -> grpc::Status {
                return grpc::Status::OK;
            });
    ctx->mock_snapshot_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshot list", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "There is no snapshot available.");
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotSaveCallsSnapshotService) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockSnapshotServiceStub>();
    EXPECT_CALL(*mock_stub, SaveSnapshot(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SnapshotPackage& request,
                         android::emulation::control::SnapshotPackage* response) -> grpc::Status {
                EXPECT_EQ(request.snapshot_id(), "my_snap");
                response->set_success(true);
                return grpc::Status::OK;
            });
    ctx->mock_snapshot_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshot save my_snap", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotLoadCallsSnapshotService) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockSnapshotServiceStub>();
    EXPECT_CALL(*mock_stub, LoadSnapshot(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SnapshotPackage& request,
                         android::emulation::control::SnapshotPackage* response) -> grpc::Status {
                EXPECT_EQ(request.snapshot_id(), "my_snap");
                response->set_success(true);
                return grpc::Status::OK;
            });
    ctx->mock_snapshot_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshot load my_snap", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, AvdSnapshotDeleteCallsSnapshotService) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockSnapshotServiceStub>();
    EXPECT_CALL(*mock_stub, DeleteSnapshot(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SnapshotPackage& request,
                         android::emulation::control::SnapshotPackage* response) -> grpc::Status {
                EXPECT_EQ(request.snapshot_id(), "my_snap");
                response->set_success(true);
                return grpc::Status::OK;
            });
    ctx->mock_snapshot_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd snapshot del my_snap", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, SensorStatusReturnsFormattedList) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    int get_sensor_calls = 0;
    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getSensor(_, _, _))
            .Times(testing::AtLeast(1))
            .WillRepeatedly(
                    [&ctx, &get_sensor_calls](
                            grpc::ClientContext* context,
                            const android::emulation::control::SensorValue& request,
                            android::emulation::control::SensorValue* response) -> grpc::Status {
                        ++get_sensor_calls;
                        EXPECT_NE(context, nullptr);
                        EXPECT_EQ(ctx->new_context_count, get_sensor_calls)
                                << "Each getSensor RPC must use a new grpc::ClientContext";
                        response->set_target(request.target());
                        response->set_status(android::emulation::control::SensorValue::OK);
                        return grpc::Status::OK;
                    });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("sensor status", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(get_sensor_calls, 18);
    EXPECT_EQ(ctx->new_context_count, 18);
    EXPECT_TRUE(result->find("acceleration: enabled.\r\n") != std::string::npos);
    EXPECT_TRUE(result->find("gyroscope: enabled.\r\n") != std::string::npos);
    EXPECT_TRUE(result->find("light: enabled.\r\n") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SensorGetReturnsFormattedValues) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getSensor(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SensorValue& request,
                         android::emulation::control::SensorValue* response) -> grpc::Status {
                EXPECT_EQ(request.target(), android::emulation::control::SensorValue::ACCELERATION);
                response->set_target(request.target());
                response->mutable_value()->add_data(0.0f);
                response->mutable_value()->add_data(9.81f);
                response->mutable_value()->add_data(0.0f);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("sensor get acceleration", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "acceleration = 0:9.81:0");
}

TEST_F(LegacyConsoleBridgeTest, SensorGetSingleValue) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getSensor(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SensorValue& request,
                         android::emulation::control::SensorValue* response) -> grpc::Status {
                EXPECT_EQ(request.target(), android::emulation::control::SensorValue::LIGHT);
                response->set_target(request.target());
                response->mutable_value()->add_data(100.0f);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("sensor get light", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "light = 100");
}

TEST_F(LegacyConsoleBridgeTest, SensorGetFailsOnMissingArgs) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sensor get", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Usage: \"get <sensorname>\"") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SensorGetFailsOnUnknownSensor) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sensor get nonexistent_sensor", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
    EXPECT_TRUE(result.status().message().find("unknown sensor name: nonexistent_sensor") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SensorSetSendsValues) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setSensor(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SensorValue& request,
                         google::protobuf::Empty* response) -> grpc::Status {
                EXPECT_EQ(request.target(), android::emulation::control::SensorValue::ACCELERATION);
                EXPECT_EQ(request.value().data_size(), 3);
                EXPECT_FLOAT_EQ(request.value().data(0), 0.0f);
                EXPECT_FLOAT_EQ(request.value().data(1), 9.81f);
                EXPECT_FLOAT_EQ(request.value().data(2), 0.0f);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("sensor set acceleration 0 9.81 0", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, SensorSetColonSeparatedValues) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setSensor(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::SensorValue& request,
                         google::protobuf::Empty* response) -> grpc::Status {
                EXPECT_EQ(request.target(), android::emulation::control::SensorValue::ACCELERATION);
                EXPECT_EQ(request.value().data_size(), 3);
                EXPECT_FLOAT_EQ(request.value().data(0), 1.0f);
                EXPECT_FLOAT_EQ(request.value().data(1), 2.0f);
                EXPECT_FLOAT_EQ(request.value().data(2), 3.0f);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("sensor set acceleration 1:2:3", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, SensorSetFailsOnMissingArgs) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sensor set", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Usage: \"set <sensorname>") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SensorSetFailsOnMissingValues) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sensor set acceleration", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Usage: \"set <sensorname>") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SensorSetFailsOnUnknownSensor) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sensor set nonexistent_sensor 1 2 3", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
    EXPECT_TRUE(result.status().message().find("unknown sensor name: nonexistent_sensor") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, AllSensorsGetAndSetRoundTrip) {
    const std::vector<std::pair<std::string, std::vector<float>>> kTestSensors = {
        {"acceleration", {1.0f, 2.0f, 3.0f}},
        {"gyroscope", {0.1f, 0.2f, 0.3f}},
        {"magnetic-field", {10.0f, 20.0f, 30.0f}},
        {"orientation", {45.0f, 90.0f, 180.0f}},
        {"temperature", {25.5f}},
        {"proximity", {5.0f}},
        {"light", {300.0f}},
        {"pressure", {1013.25f}},
        {"humidity", {55.0f}},
        {"magnetic-field-uncalibrated", {1.0f, 2.0f, 3.0f}},
        {"gyroscope-uncalibrated", {0.4f, 0.5f, 0.6f}},
        {"hinge-angle0", {90.0f}},
        {"hinge-angle1", {120.0f}},
        {"hinge-angle2", {150.0f}},
        {"heart-rate", {72.0f}},
        {"rgbc-light", {100.0f, 150.0f, 200.0f, 250.0f}},
        {"wrist-tilt", {1.0f}},
        {"acceleration-uncalibrated", {4.0f, 5.0f, 6.0f}},
    };

    for (const auto& [name, values] : kTestSensors) {
        // Test SET
        {
            auto ctx = CreateContext();
            ctx->authenticated = true;
            auto mock_stub =
                    std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
            EXPECT_CALL(*mock_stub, setSensor(_, _, _))
                    .WillOnce([&values](grpc::ClientContext* context,
                                        const android::emulation::control::SensorValue& request,
                                        google::protobuf::Empty* response) -> grpc::Status {
                        EXPECT_EQ(request.value().data_size(), values.size());
                        for (size_t i = 0; i < values.size(); ++i) {
                            EXPECT_FLOAT_EQ(request.value().data(i), values[i]);
                        }
                        return grpc::Status::OK;
                    });
            ctx->mock_stub = std::move(mock_stub);

            std::string set_cmd = absl::StrCat("sensor set ", name, " ");
            for (size_t i = 0; i < values.size(); ++i) {
                absl::StrAppend(&set_cmd, values[i], (i == values.size() - 1) ? "" : ":");
            }
            auto set_res = (*bridge_)(set_cmd, *ctx);
            ASSERT_TRUE(set_res.ok()) << set_res.status().message();
        }

        // Test GET
        {
            auto ctx = CreateContext();
            ctx->authenticated = true;
            auto mock_stub =
                    std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
            EXPECT_CALL(*mock_stub, getSensor(_, _, _))
                    .WillOnce([&values](grpc::ClientContext* context,
                                        const android::emulation::control::SensorValue& request,
                                        android::emulation::control::SensorValue* response)
                                      -> grpc::Status {
                        response->set_target(request.target());
                        for (float v : values) {
                            response->mutable_value()->add_data(v);
                        }
                        return grpc::Status::OK;
                    });
            ctx->mock_stub = std::move(mock_stub);

            auto get_res = (*bridge_)(absl::StrCat("sensor get ", name), *ctx);
            ASSERT_TRUE(get_res.ok()) << get_res.status().message();
        }
    }
}

TEST_F(LegacyConsoleBridgeTest, AvdStatusReturnsRunning) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::VmRunState* response) {
                response->set_state(android::emulation::control::VmRunState::RUNNING);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd status", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "virtual device is running");
}

TEST_F(LegacyConsoleBridgeTest, AvdStatusReturnsStopped) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::VmRunState* response) {
                response->set_state(android::emulation::control::VmRunState::PAUSED);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd status", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "virtual device is stopped");
}

TEST_F(LegacyConsoleBridgeTest, AvdPauseCallsSetVmStatePaused) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::VmRunState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.state(), android::emulation::control::VmRunState::PAUSED);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd pause", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, AvdStopCallsSetVmStateStopWhenRunning) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::VmRunState* response) {
                response->set_state(android::emulation::control::VmRunState::RUNNING);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::VmRunState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.state(), android::emulation::control::VmRunState::STOP);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd stop", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, AvdStopFailsWhenAlreadyStopped) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::VmRunState* response) {
                response->set_state(android::emulation::control::VmRunState::PAUSED);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd stop", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(result.status().message().find("virtual device already stopped") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, AvdStartCallsSetVmStateStartWhenStopped) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::VmRunState* response) {
                response->set_state(android::emulation::control::VmRunState::PAUSED);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::VmRunState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.state(), android::emulation::control::VmRunState::START);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd start", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, AvdStartFailsWhenAlreadyRunning) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::VmRunState* response) {
                response->set_state(android::emulation::control::VmRunState::RUNNING);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("avd start", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(result.status().message().find("virtual device already running") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, RestartCallsSetVmStateReset) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setVmState(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::VmRunState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.state(), android::emulation::control::VmRunState::RESET);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("restart", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "restarting emulator, bye bye");
}

TEST_F(LegacyConsoleBridgeTest, MultiDisplayAddCallsSetDisplayConfigurations) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getDisplayConfigurations(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::DisplayConfigurations* response) {
                auto* d0 = response->add_displays();
                d0->set_display(0);
                d0->set_width(1080);
                d0->set_height(1920);
                d0->set_dpi(420);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setDisplayConfigurations(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::DisplayConfigurations& request,
                         android::emulation::control::DisplayConfigurations* response) {
                EXPECT_EQ(request.displays_size(), 1);
                EXPECT_EQ(request.displays(0).display(), 1);
                EXPECT_EQ(request.displays(0).width(), 1200);
                EXPECT_EQ(request.displays(0).height(), 800);
                EXPECT_EQ(request.displays(0).dpi(), 240);
                EXPECT_EQ(request.displays(0).flags(), 0);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("multidisplay add 1 1200 800 240 0", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, MultiDisplayAddFailsOnNotEnoughArguments) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("multidisplay add 1 1200 800", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    // Checked InvalidArgument
}

TEST_F(LegacyConsoleBridgeTest, MultiDisplayAddFailsOnInvalidId) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("multidisplay add 9 1200 800 240 0", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("invalid display id") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, MultiDisplayDelCallsSetDisplayConfigurations) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getDisplayConfigurations(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::DisplayConfigurations* response) {
                auto* d0 = response->add_displays();
                d0->set_display(0);
                auto* d1 = response->add_displays();
                d1->set_display(1);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setDisplayConfigurations(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::DisplayConfigurations& request,
                         android::emulation::control::DisplayConfigurations* response) {
                EXPECT_EQ(request.displays_size(), 0);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("multidisplay del 1", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, MultiDisplayDelFailsOnEmptyArguments) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("multidisplay del", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    // Checked InvalidArgument
}

TEST_F(LegacyConsoleBridgeTest, MultiDisplayDelFailsOnInvalidId) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getDisplayConfigurations(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::DisplayConfigurations* response) {
                auto* d0 = response->add_displays();
                d0->set_display(0);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("multidisplay del 2", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("invalid display id") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, ResizeDisplayCallsSetDisplayMode) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, setDisplayMode(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::DisplayMode& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.value(), android::emulation::control::FOLDABLE);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("resize-display 1", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, ResizeDisplayFailsOnMissingIndex) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("resize-display", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("usage: \"resize-display <index>\"") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, ResizeDisplayFailsOnUnsupportedIndex) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("resize-display 3", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("size index 3 not supported") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SmsSendReturnsUnimplemented) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sms send 123456789 Hello World!", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnimplemented);
    EXPECT_EQ(result.status().message(), kSmsDeprecatedMessage);
}

TEST_F(LegacyConsoleBridgeTest, SmsSendFailsOnMissingArguments) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sms send", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("missing argument") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, SmsPduReturnsUnimplemented) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)(
            "sms pdu 07914151551512f2040b914151551512f200006021201112340205e8329bfd06", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnimplemented);
    EXPECT_EQ(result.status().message(), kSmsDeprecatedMessage);
}

TEST_F(LegacyConsoleBridgeTest, SmsPduFailsOnMissingArguments) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("sms pdu", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("missing argument") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, EventTextCallsSendKey) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, sendKey(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::KeyboardEvent& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.text(), "Hello emulator");
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("event text Hello emulator", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, EventTextFailsOnMissingMessage) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("event text", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("argument missing") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, EventMouseCallsSendMouse) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, sendMouse(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::MouseEvent& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.x(), 100);
                EXPECT_EQ(request.y(), 200);
                EXPECT_EQ(request.buttons(), 1);
                EXPECT_EQ(request.display(), 0);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("event mouse 100 200 0 1", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, EventMouseFailsOnInvalidArguments) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("event mouse 100 200", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    // Checked InvalidArgument
}

TEST_F(LegacyConsoleBridgeTest, EventTypesReturnsTypeList) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("event types", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_TRUE(result->find("EV_SYN") != std::string::npos);
    EXPECT_TRUE(result->find("EV_KEY") != std::string::npos);
    EXPECT_TRUE(result->find("EV_REL") != std::string::npos);
    EXPECT_TRUE(result->find("EV_ABS") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, EventCodesReturnsCodesForType) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("event codes EV_KEY", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_TRUE(result->find("KEY_ENTER") != std::string::npos);
    EXPECT_TRUE(result->find("KEY_SPACE") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, EventCodesFailsOnMissingType) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("event codes", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("argument missing") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, EventSendCallsSendKey) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, sendKey(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::KeyboardEvent& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.keycode(), 28);
                EXPECT_EQ(request.eventtype(), android::emulation::control::KeyboardEvent::keydown);
                return grpc::Status::OK;
            })
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::KeyboardEvent& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.keycode(), 28);
                EXPECT_EQ(request.eventtype(), android::emulation::control::KeyboardEvent::keyup);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("event send EV_KEY:KEY_ENTER:1 EV_KEY:KEY_ENTER:0", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PhoneNumberAcceptsValidNumber) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("phonenumber +15551234567", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PhoneNumberFailsOnInvalidCharacters) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("phonenumber invalid_num!", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Failed to set phone number") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, PhoneNumberFailsOnMissingArguments) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("phonenumber", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("usage: \"phonenumber") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, PowerDisplayFormatsBatteryState) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::BatteryState* response) {
                response->set_hasbattery(true);
                response->set_ispresent(true);
                response->set_charger(android::emulation::control::BatteryState::AC);
                response->set_chargelevel(85);
                response->set_health(android::emulation::control::BatteryState::GOOD);
                response->set_status(android::emulation::control::BatteryState::CHARGING);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("power display", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_TRUE(result->find("AC: online") != std::string::npos);
    EXPECT_TRUE(result->find("status: Charging") != std::string::npos);
    EXPECT_TRUE(result->find("health: Good") != std::string::npos);
    EXPECT_TRUE(result->find("present: true") != std::string::npos);
    EXPECT_TRUE(result->find("capacity: 85") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, PowerAcSetsChargingState) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::BatteryState* response) {
                response->set_charger(android::emulation::control::BatteryState::AC);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::BatteryState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.charger(), android::emulation::control::BatteryState::NONE);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("power ac off", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PowerAcFailsOnInvalidState) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("power ac maybe", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(result.status().message().find("Usage: \"ac on\" or \"ac off\"") !=
                std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, PowerStatusSetsStatusEnum) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::BatteryState* response) {
                response->set_status(android::emulation::control::BatteryState::CHARGING);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::BatteryState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.status(), android::emulation::control::BatteryState::FULL);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("power status full", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PowerStatusFailsOnInvalidStatus) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("power status invalid_status", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(LegacyConsoleBridgeTest, PowerPresentSetsPresence) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::BatteryState* response) {
                response->set_ispresent(true);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::BatteryState& request,
                         google::protobuf::Empty* response) {
                EXPECT_FALSE(request.ispresent());
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("power present false", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PowerHealthSetsHealthEnum) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::BatteryState* response) {
                response->set_health(android::emulation::control::BatteryState::GOOD);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::BatteryState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.health(), android::emulation::control::BatteryState::OVERHEATED);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("power health overheat", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PowerCapacitySetsPercentage) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto mock_stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_stub, getBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context, const google::protobuf::Empty& request,
                         android::emulation::control::BatteryState* response) {
                response->set_chargelevel(50);
                return grpc::Status::OK;
            });
    EXPECT_CALL(*mock_stub, setBattery(_, _, _))
            .WillOnce([](grpc::ClientContext* context,
                         const android::emulation::control::BatteryState& request,
                         google::protobuf::Empty* response) {
                EXPECT_EQ(request.chargelevel(), 42);
                return grpc::Status::OK;
            });
    ctx->mock_stub = std::move(mock_stub);

    auto result = (*bridge_)("power capacity 42", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, PowerCapacityFailsOnOutOfRangePercentage) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("power capacity 150", *ctx);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(LegacyConsoleBridgeTest, NetworkStatusReturnsStatus) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("network status", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_TRUE(result->find("Current network status:") != std::string::npos);
    EXPECT_TRUE(result->find("download speed:") != std::string::npos);
}

TEST_F(LegacyConsoleBridgeTest, NetworkSpeedAcceptsValidSpeed) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("network speed lte", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, NetworkDelayAcceptsValidDelay) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto result = (*bridge_)("network delay edge", *ctx);

    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST_F(LegacyConsoleBridgeTest, NetworkCaptureStartAndStop) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto start_res = (*bridge_)("network capture start /tmp/test.pcap", *ctx);
    ASSERT_TRUE(start_res.ok()) << start_res.status().message();
    EXPECT_TRUE(start_res->find("capturing to /tmp/test.pcap") != std::string::npos);

    auto stop_res = (*bridge_)("network capture stop", *ctx);
    ASSERT_TRUE(stop_res.ok()) << stop_res.status().message();
    EXPECT_EQ(*stop_res, "");
}

TEST_F(LegacyConsoleBridgeTest, WifiAddBlockUnblock) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto add_res = (*bridge_)("wifi add AndroidTestWifi password123", *ctx);
    ASSERT_TRUE(add_res.ok()) << add_res.status().message();
    EXPECT_EQ(*add_res, "");

    auto block_res = (*bridge_)("wifi block AndroidTestWifi", *ctx);
    ASSERT_TRUE(block_res.ok()) << block_res.status().message();
    EXPECT_EQ(*block_res, "");

    auto unblock_res = (*bridge_)("wifi unblock AndroidTestWifi", *ctx);
    ASSERT_TRUE(unblock_res.ok()) << unblock_res.status().message();
    EXPECT_EQ(*unblock_res, "");
}

TEST_F(LegacyConsoleBridgeTest, RedirListAddDel) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto list_res = (*bridge_)("redir list", *ctx);
    ASSERT_TRUE(list_res.ok()) << list_res.status().message();
    EXPECT_TRUE(list_res->find("no active redirections") != std::string::npos);

    auto add_res = (*bridge_)("redir add tcp:8080:80", *ctx);
    ASSERT_TRUE(add_res.ok()) << add_res.status().message();
    EXPECT_EQ(*add_res, "");

    auto del_res = (*bridge_)("redir del tcp:8080", *ctx);
    ASSERT_TRUE(del_res.ok()) << del_res.status().message();
    EXPECT_EQ(*del_res, "");
}

TEST_F(LegacyConsoleBridgeTest, CdmaSsourceAndPrlVersion) {
    auto ctx = CreateContext();
    ctx->authenticated = true;

    auto ssource_res = (*bridge_)("cdma ssource nv", *ctx);
    ASSERT_TRUE(ssource_res.ok()) << ssource_res.status().message();
    EXPECT_EQ(*ssource_res, "");

    auto prl_res = (*bridge_)("cdma prl_version 1", *ctx);
    ASSERT_TRUE(prl_res.ok()) << prl_res.status().message();
    EXPECT_EQ(*prl_res, "");
}

TEST_F(LegacyConsoleBridgeTest, ReportsTelnetCommandMetrics) {
    auto ctx = CreateContext();

    // Unauthenticated unsafe command should NOT be tracked.
    EXPECT_FALSE((*bridge_)("avd status", *ctx).ok());

    auto make_running_stub = []() {
        auto stub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
        EXPECT_CALL(*stub, getVmState(_, _, _))
                .WillRepeatedly([](grpc::ClientContext*, const google::protobuf::Empty&,
                                   android::emulation::control::VmRunState* reply) {
                    reply->set_state(android::emulation::control::VmRunState::RUNNING);
                    return grpc::Status::OK;
                });
        return stub;
    };

    // Safe commands work without authentication and are tracked by primary top-level name.
    ctx->mock_stub = make_running_stub();
    EXPECT_TRUE((*bridge_)("ping", *ctx).ok());
    EXPECT_TRUE((*bridge_)("help", *ctx).ok());
    EXPECT_TRUE((*bridge_)("h", *ctx).ok());
    EXPECT_TRUE((*bridge_)("?", *ctx).ok());
    EXPECT_TRUE((*bridge_)("auth valid_token_123", *ctx).ok());
    EXPECT_TRUE(ctx->authenticated);

    // Subcommands under non-avd/event groups are tracked by top-level name.
    EXPECT_TRUE((*bridge_)("cdma ssource nv", *ctx).ok());
    EXPECT_TRUE((*bridge_)("cdma prl_version 1", *ctx).ok());

    // Subcommands under avd and event are tracked by full primary path (including aliases ->
    // primary).
    ctx->mock_stub = make_running_stub();
    EXPECT_TRUE((*bridge_)("avd status", *ctx).ok());

    auto make_snap_stub = []() {
        auto stub = std::make_unique<android::emulation::control::MockSnapshotServiceStub>();
        EXPECT_CALL(*stub, DeleteSnapshot(_, _, _))
                .WillRepeatedly([](grpc::ClientContext*,
                                   const android::emulation::control::SnapshotPackage&,
                                   android::emulation::control::SnapshotPackage* reply) {
                    reply->set_success(true);
                    return grpc::Status::OK;
                });
        return stub;
    };
    // "delete" is an alias for "del" ("del|delete") -> should record "avd snapshot del"
    ctx->mock_snapshot_stub = make_snap_stub();
    EXPECT_TRUE((*bridge_)("avd snapshot delete snap1", *ctx).ok());
    ctx->mock_snapshot_stub = make_snap_stub();
    EXPECT_TRUE((*bridge_)("avd snapshot del snap2", *ctx).ok());

    EXPECT_TRUE((*bridge_)("event types", *ctx).ok());

    // Unknown or incomplete group commands are not tracked.
    EXPECT_FALSE((*bridge_)("unknown_cmd", *ctx).ok());
    EXPECT_FALSE((*bridge_)("avd", *ctx).ok());

    bridge_->ReportMetrics();
    ASSERT_TRUE(metrics_writer_->WaitForEvents(1, absl::Seconds(5)));

    const absl::MutexLock lock(metrics_writer_->mutex);
    ASSERT_EQ(metrics_writer_->events.size(), 1u);
    const auto& event = metrics_writer_->events[0];
    EXPECT_EQ(event.kind(), android_studio::AndroidStudioEvent::EMULATOR_CONSOLE_EVENTS);

    absl::flat_hash_map<std::string, int64_t> reported;
    for (const auto& console_event : event.emulator_console_events()) {
        EXPECT_EQ(console_event.source(), android_studio::EmulatorConsoleEvent::TELNET);
        reported[console_event.command()] = console_event.value();
    }

    EXPECT_EQ(reported["ping"], 1);
    EXPECT_EQ(reported["help"], 3);
    EXPECT_EQ(reported["auth"], 1);
    EXPECT_EQ(reported["cdma"], 2);
    EXPECT_EQ(reported["avd status"], 1);
    EXPECT_EQ(reported["avd snapshot del"], 2);
    EXPECT_EQ(reported["event types"], 1);
    EXPECT_FALSE(reported.contains("avd"));
    EXPECT_FALSE(reported.contains("unknown_cmd"));
}

}  // namespace
}  // namespace goldfish::telnet
