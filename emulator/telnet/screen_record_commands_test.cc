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
#include "screen_record_commands.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>

#include "android/base/testing/test_temp_dir.h"
#include "emulator_controller_mock.grpc.pb.h"
#include "legacy_console_bridge.h"
#include "screen_recording_service.grpc.pb.h"

namespace goldfish::telnet {
namespace {

using android::emulation::control::incubating::RecordingInfo;
using android::emulation::control::incubating::ScreenRecording;
using testing::_;
using testing::HasSubstr;

class MockScreenRecordingStub : public ScreenRecording::StubInterface {
  public:
    MOCK_METHOD(grpc::Status, StartRecording,
                (grpc::ClientContext * context, const RecordingInfo& request,
                 RecordingInfo* response),
                (override));
    MOCK_METHOD(grpc::Status, StopRecording,
                (grpc::ClientContext * context, const RecordingInfo& request,
                 RecordingInfo* response),
                (override));
    MOCK_METHOD(grpc::Status, ListRecordings,
                (grpc::ClientContext * context, const RecordingInfo& request,
                 android::emulation::control::incubating::RecordingInfoList* response),
                (override));
    MOCK_METHOD((grpc::ClientReaderInterface<RecordingInfo>*), ReceiveRecordingEventsRaw,
                (grpc::ClientContext * context, const google::protobuf::Empty& request),
                (override));

    // Async methods stubs to satisfy pure virtual requirements
    ::grpc::ClientAsyncResponseReaderInterface<
            ::android::emulation::control::incubating::RecordingInfo>*
    AsyncStartRecordingRaw(::grpc::ClientContext* context,
                           const ::android::emulation::control::incubating::RecordingInfo& request,
                           ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
    ::grpc::ClientAsyncResponseReaderInterface<
            ::android::emulation::control::incubating::RecordingInfo>*
    PrepareAsyncStartRecordingRaw(
            ::grpc::ClientContext* context,
            const ::android::emulation::control::incubating::RecordingInfo& request,
            ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
    ::grpc::ClientAsyncResponseReaderInterface<
            ::android::emulation::control::incubating::RecordingInfo>*
    AsyncStopRecordingRaw(::grpc::ClientContext* context,
                          const ::android::emulation::control::incubating::RecordingInfo& request,
                          ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
    ::grpc::ClientAsyncResponseReaderInterface<
            ::android::emulation::control::incubating::RecordingInfo>*
    PrepareAsyncStopRecordingRaw(
            ::grpc::ClientContext* context,
            const ::android::emulation::control::incubating::RecordingInfo& request,
            ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
    ::grpc::ClientAsyncResponseReaderInterface<
            ::android::emulation::control::incubating::RecordingInfoList>*
    AsyncListRecordingsRaw(::grpc::ClientContext* context,
                           const ::android::emulation::control::incubating::RecordingInfo& request,
                           ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
    ::grpc::ClientAsyncResponseReaderInterface<
            ::android::emulation::control::incubating::RecordingInfoList>*
    PrepareAsyncListRecordingsRaw(
            ::grpc::ClientContext* context,
            const ::android::emulation::control::incubating::RecordingInfo& request,
            ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
    ::grpc::ClientAsyncReaderInterface<::android::emulation::control::incubating::RecordingInfo>*
    AsyncReceiveRecordingEventsRaw(::grpc::ClientContext* context,
                                   const ::google::protobuf::Empty& request,
                                   ::grpc::CompletionQueue* cq, void* tag) override {
        return nullptr;
    }
    ::grpc::ClientAsyncReaderInterface<::android::emulation::control::incubating::RecordingInfo>*
    PrepareAsyncReceiveRecordingEventsRaw(::grpc::ClientContext* context,
                                          const ::google::protobuf::Empty& request,
                                          ::grpc::CompletionQueue* cq) override {
        return nullptr;
    }
};

struct MockConsoleContext : public LegacyConsoleBridge::ConsoleContext {
    explicit MockConsoleContext(int port) : ConsoleContext(port) {}

    absl::StatusOr<std::unique_ptr<
            android::emulation::control::incubating::ScreenRecording::StubInterface>>
    ScreenRecordingStub() override {
        if (mock_stub) {
            return std::move(mock_stub);
        }
        return absl::NotFoundError("No mock stub set");
    }

    absl::StatusOr<std::unique_ptr<android::emulation::control::EmulatorController::StubInterface>>
    EmulatorControllerStub() override {
        if (mock_emu_stub) {
            return std::move(mock_emu_stub);
        }
        return absl::NotFoundError("No mock emu stub set");
    }

    absl::StatusOr<std::unique_ptr<grpc::ClientContext>> NewContext(
            std::chrono::time_point<std::chrono::system_clock> deadline =
                    std::chrono::system_clock::now() + std::chrono::milliseconds(500)) override {
        auto ctx = std::make_unique<grpc::ClientContext>();
        ctx->set_deadline(deadline);
        return ctx;
    }

    std::unique_ptr<MockScreenRecordingStub> mock_stub;
    std::unique_ptr<android::emulation::control::MockEmulatorControllerStub> mock_emu_stub;
};

TEST(ScreenRecordCommandsTest, ParseStartCommand) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;
    auto mock_stub = std::make_unique<MockScreenRecordingStub>();

    EXPECT_CALL(*mock_stub, StartRecording(_, _, _))
            .WillOnce([](grpc::ClientContext*, const RecordingInfo& request, RecordingInfo*) {
                EXPECT_EQ(request.file_name(), "test.webm");
                EXPECT_EQ(request.width(), 1280);
                EXPECT_EQ(request.height(), 720);
                EXPECT_EQ(request.bit_rate(), 4000000);
                EXPECT_EQ(request.time_limit(), 180);
                EXPECT_EQ(request.fps(), 24);
                EXPECT_EQ(request.display(), 0);
                return grpc::Status::OK;
            });

    ctx.mock_stub = std::move(mock_stub);

    auto result = (*registry)(
            "screenrecord start --size 1280x720 --bit-rate 4M --time-limit 180 "
            "--fps 24 --display 0 test.webm",
            ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST(ScreenRecordCommandsTest, ParseStopCommand) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;
    auto mock_stub = std::make_unique<MockScreenRecordingStub>();

    // Mock ListRecordings to return one active and one stopped recording
    EXPECT_CALL(*mock_stub, ListRecordings(_, _, _))
            .WillOnce([](grpc::ClientContext*, const RecordingInfo&,
                         android::emulation::control::incubating::RecordingInfoList* response) {
                auto* rec1 = response->add_recordings();
                rec1->set_file_name("active.webm");
                rec1->set_state(android::emulation::control::incubating::RecordingInfo::
                                        RECORDER_STATE_RECORDING);

                auto* rec2 = response->add_recordings();
                rec2->set_file_name("stopped.webm");
                rec2->set_state(android::emulation::control::incubating::RecordingInfo::
                                        RECORDER_STATE_STOPPED);

                return grpc::Status::OK;
            });

    // Expect StopRecording to be called ONLY for the active recording
    EXPECT_CALL(*mock_stub, StopRecording(_, _, _))
            .WillOnce([](grpc::ClientContext*, const RecordingInfo& request, RecordingInfo*) {
                EXPECT_EQ(request.file_name(), "active.webm");
                return grpc::Status::OK;
            });

    ctx.mock_stub = std::move(mock_stub);

    auto result = (*registry)("screenrecord stop", ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST(ScreenRecordCommandsTest, ParseScreenshotCommand) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;
    auto mock_emu_stub =
            std::make_unique<android::emulation::control::MockEmulatorControllerStub>();

    EXPECT_CALL(*mock_emu_stub, getScreenshot(_, _, _))
            .WillOnce([](grpc::ClientContext*,
                         const android::emulation::control::ImageFormat& request,
                         android::emulation::control::Image* response) {
                EXPECT_EQ(request.format(), android::emulation::control::ImageFormat::PNG);
                EXPECT_EQ(request.display(), 1);
                response->set_image("fake png data");
                return grpc::Status::OK;
            });

    ctx.mock_emu_stub = std::move(mock_emu_stub);

    android::base::TestTempDir tmp_dir("screenrecord_test");
    auto screenshot_path = tmp_dir.Path() / "my_screenshot.png";

    auto result = (*registry)(
            std::string("screenrecord screenshot --display 1 ") + screenshot_path.generic_string(),
            ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");

    // Verify file was created
    EXPECT_TRUE(std::filesystem::exists(screenshot_path));
}

TEST(ScreenRecordCommandsTest, ParseStartCommandExtendedTimeLimit) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;
    auto mock_stub = std::make_unique<MockScreenRecordingStub>();

    EXPECT_CALL(*mock_stub, StartRecording(_, _, _))
            .WillOnce([](grpc::ClientContext*, const RecordingInfo& request, RecordingInfo*) {
                EXPECT_EQ(request.time_limit(), 14400);
                return grpc::Status::OK;
            });

    ctx.mock_stub = std::move(mock_stub);

    auto result = (*registry)("screenrecord start --time-limit 14400 test.webm", ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST(ScreenRecordCommandsTest, ParseStartCommandExceedsTimeLimitFails) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;

    auto result = (*registry)("screenrecord start --time-limit 14401 test.webm", ctx);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().message(), "Time limit 14401s outside acceptable range [1,14400]");
}

TEST(ScreenRecordCommandsTest, ParseStartCommandPreservesWindowsPaths) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;
    auto mock_stub = std::make_unique<MockScreenRecordingStub>();

    const std::string windows_path = R"(C:\Users\user\.android\avd\Medium_Phone.avd\tmp.webm)";

    EXPECT_CALL(*mock_stub, StartRecording(_, _, _))
            .WillOnce([&windows_path](grpc::ClientContext*, const RecordingInfo& request,
                                      RecordingInfo*) {
                EXPECT_EQ(request.file_name(), windows_path);
                return grpc::Status::OK;
            });

    ctx.mock_stub = std::move(mock_stub);

    auto result = (*registry)("screenrecord start " + windows_path, ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST(ScreenRecordCommandsTest, ParseStartCommandPreservesPathsWithSpaces) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;
    auto mock_stub = std::make_unique<MockScreenRecordingStub>();

    const std::string path_with_spaces = R"(C:\Users\user name\Android Studio Projects\tmp.webm)";

    EXPECT_CALL(*mock_stub, StartRecording(_, _, _))
            .WillOnce([&path_with_spaces](grpc::ClientContext*, const RecordingInfo& request,
                                          RecordingInfo*) {
                EXPECT_EQ(request.file_name(), path_with_spaces);
                return grpc::Status::OK;
            });

    ctx.mock_stub = std::move(mock_stub);

    auto result = (*registry)("screenrecord start --time-limit 60 " + path_with_spaces, ctx);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(*result, "");
}

TEST(ScreenRecordCommandsTest, ParseStartCommandRejectsInvalidOptionsAndValues) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;

    const auto bad_size = (*registry)("screenrecord start --size 1280 test.webm", ctx);
    ASSERT_FALSE(bad_size.ok());
    EXPECT_EQ(bad_size.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(bad_size.status().message(), HasSubstr("1280"));
    EXPECT_THAT(bad_size.status().message(), HasSubstr("--size 1280x720"));

    const auto zero_size = (*registry)("screenrecord start --size 0x720 test.webm", ctx);
    ASSERT_FALSE(zero_size.ok());
    EXPECT_EQ(zero_size.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(zero_size.status().message(), HasSubstr("0x720"));
    EXPECT_THAT(zero_size.status().message(), HasSubstr("width and height may not be zero"));

    const auto bad_bitrate = (*registry)("screenrecord start --bit-rate invalid test.webm", ctx);
    ASSERT_FALSE(bad_bitrate.ok());
    EXPECT_EQ(bad_bitrate.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(bad_bitrate.status().message(), HasSubstr("invalid"));
    EXPECT_THAT(bad_bitrate.status().message(), HasSubstr("--bit-rate 4M"));

    const auto overflow_bitrate = (*registry)("screenrecord start --bit-rate 5000M test.webm", ctx);
    ASSERT_FALSE(overflow_bitrate.ok());
    EXPECT_EQ(overflow_bitrate.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(overflow_bitrate.status().message(), HasSubstr("5000000000bps"));

    const auto negative_time_limit =
            (*registry)("screenrecord start --time-limit -5 test.webm", ctx);
    ASSERT_FALSE(negative_time_limit.ok());
    EXPECT_EQ(negative_time_limit.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(negative_time_limit.status().message(),
              "Time limit -5s outside acceptable range [1,14400]");

    const auto bad_fps = (*registry)("screenrecord start --fps 999 test.webm", ctx);
    ASSERT_FALSE(bad_fps.ok());
    EXPECT_EQ(bad_fps.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(bad_fps.status().message(), HasSubstr("999fps outside acceptable range"));

    const auto negative_display = (*registry)("screenrecord start --display -1 test.webm", ctx);
    ASSERT_FALSE(negative_display.ok());
    EXPECT_EQ(negative_display.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(negative_display.status().message(), HasSubstr("-1"));
    EXPECT_THAT(negative_display.status().message(), HasSubstr("non-negative integer"));

    const auto unknown_option = (*registry)("screenrecord start --unknown-flag test.webm", ctx);
    ASSERT_FALSE(unknown_option.ok());
    EXPECT_EQ(unknown_option.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(unknown_option.status().message(), HasSubstr("--unknown-flag"));

    const auto missing_filename = (*registry)("screenrecord start --fps 24", ctx);
    ASSERT_FALSE(missing_filename.ok());
    EXPECT_EQ(missing_filename.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(missing_filename.status().message(), HasSubstr("Must provide an output filename"));
}

TEST(ScreenRecordCommandsTest, StartAndStopCommandsPreserveGrpcStatusAndDetails) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    {
        MockConsoleContext ctx(5554);
        ctx.authenticated = true;
        auto mock_stub = std::make_unique<MockScreenRecordingStub>();
        EXPECT_CALL(*mock_stub, StartRecording(_, _, _))
                .WillOnce([](grpc::ClientContext*, const RecordingInfo&, RecordingInfo*) {
                    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                        "output path is read-only");
                });
        ctx.mock_stub = std::move(mock_stub);

        const auto start_err = (*registry)("screenrecord start /readonly/out.webm", ctx);
        ASSERT_FALSE(start_err.ok());
        EXPECT_EQ(start_err.status().code(), absl::StatusCode::kPermissionDenied);
        EXPECT_THAT(start_err.status().message(), HasSubstr("/readonly/out.webm"));
        EXPECT_THAT(start_err.status().message(), HasSubstr("output path is read-only"));
    }

    {
        MockConsoleContext ctx(5554);
        ctx.authenticated = true;
        auto mock_stub = std::make_unique<MockScreenRecordingStub>();
        EXPECT_CALL(*mock_stub, ListRecordings(_, _, _))
                .WillOnce([](grpc::ClientContext*, const RecordingInfo&,
                             android::emulation::control::incubating::RecordingInfoList*) {
                    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                        "recording service disconnected");
                });
        ctx.mock_stub = std::move(mock_stub);

        const auto list_err = (*registry)("screenrecord stop", ctx);
        ASSERT_FALSE(list_err.ok());
        EXPECT_EQ(list_err.status().code(), absl::StatusCode::kUnavailable);
        EXPECT_THAT(list_err.status().message(), HasSubstr("recording service disconnected"));
    }

    {
        MockConsoleContext ctx(5554);
        ctx.authenticated = true;
        auto mock_stub = std::make_unique<MockScreenRecordingStub>();
        EXPECT_CALL(*mock_stub, ListRecordings(_, _, _))
                .WillOnce([](grpc::ClientContext*, const RecordingInfo&,
                             android::emulation::control::incubating::RecordingInfoList* response) {
                    auto* rec1 = response->add_recordings();
                    rec1->set_file_name("first.webm");
                    rec1->set_display(1);
                    rec1->set_state(RecordingInfo::RECORDER_STATE_RECORDING);

                    auto* rec2 = response->add_recordings();
                    rec2->set_file_name("second.webm");
                    rec2->set_display(2);
                    rec2->set_state(RecordingInfo::RECORDER_STATE_RECORDING);
                    return grpc::Status::OK;
                });
        EXPECT_CALL(*mock_stub, StopRecording(_, _, _))
                .WillOnce([](grpc::ClientContext*, const RecordingInfo& request, RecordingInfo*) {
                    EXPECT_EQ(request.file_name(), "first.webm");
                    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "encoder busy");
                })
                .WillOnce([](grpc::ClientContext*, const RecordingInfo& request, RecordingInfo*) {
                    EXPECT_EQ(request.file_name(), "second.webm");
                    return grpc::Status::OK;
                });
        ctx.mock_stub = std::move(mock_stub);

        const auto stop_err = (*registry)("screenrecord stop", ctx);
        ASSERT_FALSE(stop_err.ok());
        EXPECT_EQ(stop_err.status().code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_THAT(stop_err.status().message(), HasSubstr("first.webm"));
        EXPECT_THAT(stop_err.status().message(), HasSubstr("display 1"));
        EXPECT_THAT(stop_err.status().message(), HasSubstr("encoder busy"));
    }
}

TEST(ScreenRecordCommandsTest, ScreenshotCommandReportsSpecificErrorsAndSupportsUtf8Paths) {
    CommandRegistryBuilder builder("/dummy/token");
    auto screenrecord = builder.Command("screenrecord", "desc");
    RegisterScreenRecordCommands(screenrecord);
    auto registry = builder.Build();

    MockConsoleContext ctx(5554);
    ctx.authenticated = true;

    const auto bad_display = (*registry)("screenrecord screenshot --display -2 out.png", ctx);
    ASSERT_FALSE(bad_display.ok());
    EXPECT_EQ(bad_display.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(bad_display.status().message(), HasSubstr("-2"));

    const auto unknown_opt = (*registry)("screenrecord screenshot --bad-opt out.png", ctx);
    ASSERT_FALSE(unknown_opt.ok());
    EXPECT_EQ(unknown_opt.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(unknown_opt.status().message(), HasSubstr("--bad-opt"));

    auto mock_emu_stub =
            std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
    EXPECT_CALL(*mock_emu_stub, getScreenshot(_, _, _))
            .WillOnce([](grpc::ClientContext*, const android::emulation::control::ImageFormat&,
                         android::emulation::control::Image* response) {
                response->set_image("fake png data");
                return grpc::Status::OK;
            });
    ctx.mock_emu_stub = std::move(mock_emu_stub);

    android::base::TestTempDir tmp_dir("screenrecord_utf8_test");
    const std::filesystem::path missing_utf8_path =
            tmp_dir.Path() / "täst_中文_missing" / "캡처.png";
    const auto open_err = (*registry)(
            std::string("screenrecord screenshot ") + missing_utf8_path.generic_string(), ctx);
    ASSERT_FALSE(open_err.ok());
    EXPECT_EQ(open_err.status().code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(open_err.status().message(), HasSubstr(missing_utf8_path.generic_string()));
    EXPECT_THAT(open_err.status().message(), HasSubstr("target directory exists and is writable"));
}

}  // namespace
}  // namespace goldfish::telnet
