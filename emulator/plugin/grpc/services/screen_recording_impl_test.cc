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

#include "android/emulation/control/incubating/screen_recording_impl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/display/test/display_test_peer.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "goldfish/file/file.h"

// Undefine close macro on Windows to avoid conflict with std::ofstream::close()
#ifdef _WIN32
#undef close
#endif

namespace android::emulation::control::incubating {

using ::goldfish::async::LibuvEventLoop;
using ::goldfish::async::ThreadedEventLoop;
using ::goldfish::display::test::FakeMultiDisplay;
using ::testing::AllOf;
using ::testing::HasSubstr;

namespace {

class ExpiredDisplayMultiDisplay : public FakeMultiDisplay {
  public:
    explicit ExpiredDisplayMultiDisplay(::goldfish::async::EventLoop* event_loop)
            : FakeMultiDisplay(event_loop) {}

    absl::StatusOr<::goldfish::display::DisplayPtr> GetDisplay(
            ::goldfish::display::DisplayId /*display_id*/) const override {
        return ::goldfish::display::DisplayPtr{};
    }
};

class TestRgbSlot final : public ::goldfish::display::Slot {
  public:
    TestRgbSlot(::goldfish::display::Dimensions size, uint8_t fill_byte, int* read_count = nullptr,
                absl::Status status = absl::OkStatus())
            : size_(size)
            , fill_byte_(fill_byte)
            , read_count_(read_count)
            , status_(std::move(status)) {}

    ::goldfish::display::Dimensions size() const override { return size_; }

  protected:
    absl::Status DoReadInto(const ::goldfish::display::ReadRequest& read,
                            const ::goldfish::display::ReadTarget& dst) const override {
        if (read_count_ != nullptr) {
            ++(*read_count_);
        }
        if (!status_.ok()) {
            return status_;
        }
        const auto& rgb = std::get<::goldfish::display::Rgb888Target>(dst);
        const size_t row_bytes = static_cast<size_t>(read.output.width) * 3;
        const size_t stride = rgb.plane.stride == 0 ? row_bytes : rgb.plane.stride;
        for (uint32_t row = 0; row < read.output.height; ++row) {
            std::memset(rgb.plane.data.data() + row * stride, fill_byte_, row_bytes);
        }
        return absl::OkStatus();
    }

  private:
    ::goldfish::display::Dimensions size_;
    uint8_t fill_byte_;
    int* read_count_;
    absl::Status status_;
};

}  // namespace

class ScreenRecordingServiceImplTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // The display posts demand changes and captures to its loop, so the
        // loop must run for the recorder to receive frames.
        loop = ThreadedEventLoop::Create(LibuvEventLoop::Create());
        fake_display = std::make_unique<FakeMultiDisplay>(loop.get());
        // Create default display 0
        fake_display->CreateDisplay(0, 1080, 2400, 440, 0).IgnoreError();
        service = std::make_unique<ScreenRecordingServiceImpl>(fake_display.get());

        // Setup platform-independent temp path
        temp_dir_ = std::filesystem::temp_directory_path();
        if (const char* bazel_tmp = std::getenv("TEST_TMPDIR")) {
            temp_dir_ = bazel_tmp;
        }
    }

    std::unique_ptr<::goldfish::async::EventLoop> loop;
    std::unique_ptr<FakeMultiDisplay> fake_display;
    std::unique_ptr<ScreenRecordingServiceImpl> service;
    std::filesystem::path temp_dir_;
};

TEST_F(ScreenRecordingServiceImplTest, StartRecordingAppliesDefaults) {
    std::string test_file = (temp_dir_ / "test_defaults.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_fps(0);       // Request default
    request.set_bit_rate(0);  // Request default

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.fps(), kFPS);
    EXPECT_EQ(response.bit_rate(), kDefaultVideoBitrate);
    EXPECT_EQ(response.time_limit(), kDefaultTimeLimit);
    EXPECT_EQ(response.state(), RecordingInfo::RECORDER_STATE_RECORDING);

    service->StopRecording(&context, &request, &response);
    std::remove(test_file.c_str());
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingWithFoldableResolution) {
    // Create display 1 with resolution 1260x2400 (where 1260 / 2 = 630, which is odd)
    fake_display->CreateDisplay(1, 1260, 2400, 440, 0).IgnoreError();

    std::string test_file = (temp_dir_ / "test_foldable.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_display(1);
    request.set_fps(24);
    request.set_bit_rate(2000000);

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);

    EXPECT_TRUE(status.ok()) << status.error_message();

    // StopRecording drops the frames the encoder has not reached yet, and under
    // sanitizers one frame can take seconds to encode, so wait for encoded
    // frames rather than for a fixed time. Eleven frames of this static screen
    // encode to about 1.7 KB: two keyframes and nine small delta frames.
    constexpr uint64_t kMinEncodedFrames = 11;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (service->FramesEncodedForTesting() < kMinEncodedFrames &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(service->FramesEncodedForTesting(), kMinEncodedFrames);

    service->StopRecording(&context, &request, &response);
    EXPECT_GT(std::filesystem::file_size(test_file), 1000)
            << "Recording output should contain valid WebM video frames and header";
    std::remove(test_file.c_str());
}

TEST_F(ScreenRecordingServiceImplTest, DISABLED_StartRecording5Seconds) {
    std::string test_file = (temp_dir_ / "test_5s.webm").string();

    // Start the fake display generator so it produces varying frames (changing colors)
    auto display_ptr = fake_display->GetDisplay(0).value().lock();
    auto active_display =
            std::static_pointer_cast<::goldfish::display::test::ActiveFakePixmanDisplay>(
                    display_ptr);
    ASSERT_TRUE(active_display != nullptr);
    active_display->Start();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_fps(24);
    request.set_bit_rate(4000000);  // 4Mbps

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();

    // Record for 5 seconds
    std::this_thread::sleep_for(std::chrono::seconds(5));

    status = service->StopRecording(&context, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();

    active_display->Stop();

    EXPECT_TRUE(std::filesystem::exists(test_file));
    uint64_t size = std::filesystem::file_size(test_file);
    EXPECT_GT(size, 10000) << "File size is too small: " << size << " bytes";

    // We can also print the size for verification
    std::cout << "[Test] 5s Recording file size with dynamic content: " << size << " bytes"
              << std::endl;

    std::remove(test_file.c_str());
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsForInvalidFileName) {
    const std::string test_file = (temp_dir_ / "vidéo_中文.mp4").string();

    RecordingInfo request;
    request.set_file_name(test_file);

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("Failed to start screen recording"), HasSubstr("vidéo_中文.mp4"),
                      HasSubstr("must end with '.webm'"),
                      HasSubstr("Specify a destination path with a '.webm' extension.")));
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsForInvalidFps) {
    const std::string test_file = (temp_dir_ / "test_invalid_fps.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_fps(100);  // Max is 60

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("test_invalid_fps.webm"),
                      HasSubstr(absl::StrCat("FPS 100 is out of range [1, ", kMaxFPS, "]")),
                      HasSubstr("Specify an FPS between 1 and")));
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsForInvalidBitRate) {
    const std::string test_file = (temp_dir_ / "test_invalid_bitrate.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_bit_rate(kMinVideoBitrate - 1);

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_THAT(
            status.error_message(),
            AllOf(HasSubstr("test_invalid_bitrate.webm"),
                  HasSubstr(absl::StrCat("bitrate ", kMinVideoBitrate - 1, " bps is out of range [",
                                         kMinVideoBitrate, ", ", kMaxVideoBitrate, "]")),
                  HasSubstr("Specify a bitrate between")));
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsForExcessiveTimeLimit) {
    const std::string test_file = (temp_dir_ / "test_invalid_timelimit.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_time_limit(android::emulation::control::kMaxTimeLimit + 1);

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("test_invalid_timelimit.webm"),
                      HasSubstr(absl::StrCat("time limit ",
                                             android::emulation::control::kMaxTimeLimit + 1,
                                             "s is out of range [1, ",
                                             android::emulation::control::kMaxTimeLimit, "]")),
                      HasSubstr("Specify a time limit between 1 and")));
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsWhenAlreadyInProgress) {
    const std::string first_file = (temp_dir_ / "active_first.webm").string();
    const std::string second_file = (temp_dir_ / "second_attempt.webm").string();

    RecordingInfo first_request;
    first_request.set_file_name(first_file);
    RecordingInfo first_response;
    grpc::ServerContext context1;

    ASSERT_TRUE(service->StartRecording(&context1, &first_request, &first_response).ok());

    RecordingInfo second_request;
    second_request.set_file_name(second_file);
    RecordingInfo second_response;
    grpc::ServerContext context2;

    auto status = service->StartRecording(&context2, &second_request, &second_response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
    EXPECT_THAT(status.error_message(), AllOf(HasSubstr("second_attempt.webm"),
                                              HasSubstr("screen recording is already in progress"),
                                              HasSubstr("Call StopRecording")));

    grpc::ServerContext stop_context;
    EXPECT_TRUE(service->StopRecording(&stop_context, &first_request, &first_response).ok());
    android::base::file::rm(first_file).IgnoreError();
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsForMissingAndExpiredDisplay) {
    const std::string test_file = (temp_dir_ / "test_missing_display.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_display(99);

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("test_missing_display.webm"), HasSubstr("on display 99"),
                      HasSubstr("Display with id 99 not found"),
                      HasSubstr("Verify that display 99 exists and is active")));

    ExpiredDisplayMultiDisplay expired_display(loop.get());
    ScreenRecordingServiceImpl expired_service(&expired_display);
    request.set_display(7);
    grpc::ServerContext expired_context;
    status = expired_service.StartRecording(&expired_context, &request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("test_missing_display.webm"), HasSubstr("on display 7"),
                      HasSubstr("display 7 is no longer active"),
                      HasSubstr("Verify that display 7 has not been removed")));
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsWhenExistingPathCannotBeRemoved) {
    const std::filesystem::path conflict_dir = temp_dir_ / "locked_dir.webm";
    ASSERT_TRUE(android::base::file::mkdir_recursive(conflict_dir, 0700).ok());
    ASSERT_TRUE(android::base::file::touch(conflict_dir / "keep").ok());

    RecordingInfo request;
    request.set_file_name(conflict_dir.string());
    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("unable to remove existing recording file"),
                      HasSubstr("locked_dir.webm"), HasSubstr("write permissions")));

    android::base::file::rm_recursive(conflict_dir).IgnoreError();
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsForMissingParentDirWithUnicodePath) {
    const std::string test_file = (temp_dir_ / "täst_中文_missing" / "vidéo.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("unable to create recording file"), HasSubstr("täst_中文_missing"),
                      HasSubstr("vidéo.webm"), HasSubstr("Ensure the parent directory exists")));
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingFailsWhenEncoderInitFails) {
    ASSERT_TRUE(fake_display->CreateDisplay(2, 1, 1, 440, 0).ok());
    const std::string test_file = (temp_dir_ / "test_encoder_fail.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);
    request.set_display(2);
    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("test_encoder_fail.webm"), HasSubstr("on display 2"),
                      HasSubstr("unable to initialize WebM video encoder for 1x1"),
                      HasSubstr("check emulator logs")));

    android::base::file::rm(test_file).IgnoreError();
}

TEST_F(ScreenRecordingServiceImplTest, StopRecordingReportsActionableNotFoundErrors) {
    const std::string inactive_file = (temp_dir_ / "inactive_vidéo.webm").string();
    RecordingInfo stop_request;
    stop_request.set_file_name(inactive_file);
    RecordingInfo response;
    grpc::ServerContext context1;

    auto status = service->StopRecording(&context1, &stop_request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_THAT(status.error_message(), AllOf(HasSubstr("inactive_vidéo.webm"),
                                              HasSubstr("no screen recording is currently active"),
                                              HasSubstr("Call StartRecording")));

    const std::string active_file = (temp_dir_ / "active_recording.webm").string();
    RecordingInfo start_request;
    start_request.set_file_name(active_file);
    grpc::ServerContext start_context;
    ASSERT_TRUE(service->StartRecording(&start_context, &start_request, &response).ok());

    const std::string wrong_file = (temp_dir_ / "wrong_target.webm").string();
    stop_request.set_file_name(wrong_file);
    grpc::ServerContext wrong_context;
    status = service->StopRecording(&wrong_context, &stop_request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_THAT(status.error_message(),
                AllOf(HasSubstr("no recording found for"), HasSubstr("wrong_target.webm"),
                      HasSubstr("ListRecordings")));

    grpc::ServerContext valid_stop_context;
    EXPECT_TRUE(service->StopRecording(&valid_stop_context, &start_request, &response).ok());
    android::base::file::rm(active_file).IgnoreError();
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingOverwritesExistingFile) {
    std::string test_file = (temp_dir_ / "test_overwrite.webm").string();
    // Create pre-existing file on disk
    std::ofstream file(test_file);
    file << "dummy existing content";
    file.close();

    RecordingInfo request;
    request.set_file_name(test_file);

    RecordingInfo response;
    grpc::ServerContext context;

    auto status = service->StartRecording(&context, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();

    service->StopRecording(&context, &request, &response);
    // Clean up
    std::remove(test_file.c_str());
}

TEST_F(ScreenRecordingServiceImplTest, StartRecordingSucceedsOnSecondAttempt) {
    std::string test_file = (temp_dir_ / "test_second_attempt.webm").string();

    RecordingInfo request;
    request.set_file_name(test_file);

    RecordingInfo response;
    grpc::ServerContext context1;

    // First recording attempt
    auto status = service->StartRecording(&context1, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();

    status = service->StopRecording(&context1, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(std::filesystem::exists(test_file));

    // Second recording attempt to the same file path (e.g. Studio / Extended Controls staging)
    grpc::ServerContext context2;
    status = service->StartRecording(&context2, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();

    status = service->StopRecording(&context2, &request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();

    // Clean up
    std::remove(test_file.c_str());
}

TEST_F(ScreenRecordingServiceImplTest, FrameGeneratorAvoidsDuplicateFullFrameBuffer) {
    constexpr int kWidth = 1080;
    constexpr int kHeight = 2400;
    constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kHeight * 3;
    constexpr uint8_t kInitialGarbage = 0xEE;
    constexpr uint8_t kFrame1Fill = 0x5A;
    constexpr int kRepeatedTicks = 60;

    auto display = std::make_shared<::goldfish::display::test::FakePixmanDisplay>(
            loop.get(), 1, static_cast<::pixman_image_t*>(nullptr));
    const auto& frames = ::goldfish::display::DisplayTestPeer::frames(*display);
    ASSERT_NE(frames, nullptr);

    ScreenRecordingFrameGenerator generator(display);
    std::vector<uint8_t> frame_buffer(kFrameBytes, kInitialGarbage);

    // Before any frame is published, the first tick clears the buffer once without allocating.
    generator(frame_buffer.data(), kWidth, kHeight, 0, 0.0);
    EXPECT_EQ(generator.LastFrameId(), 0U);
    EXPECT_EQ(generator.BufferedBytes(), 0U);
    EXPECT_EQ(frame_buffer.front(), 0U);
    EXPECT_EQ(frame_buffer.back(), 0U);

    // Publish a 1080x2400 frame (7.78 MB RGB888) and verify 0 bytes are buffered in the generator.
    int read_count = 0;
    frames->PublishFrame(
            ::goldfish::display::SlotLease(std::make_unique<TestRgbSlot>(
                    ::goldfish::display::Dimensions{kWidth, kHeight}, kFrame1Fill, &read_count)),
            absl::Now());

    generator(frame_buffer.data(), kWidth, kHeight, 1, 0.04);
    const auto first_frame_id = generator.LastFrameId();
    EXPECT_GT(first_frame_id, 0U);
    EXPECT_EQ(read_count, 1);
    EXPECT_EQ(generator.BufferedBytes(), 0U);
    EXPECT_EQ(frame_buffer.front(), kFrame1Fill);
    EXPECT_EQ(frame_buffer.back(), kFrame1Fill);

    // Repeated ticks with the same frame_id reuse frame_buffer in place without re-reading or
    // copying a duplicate last_encoded buffer.
    for (int i = 0; i < kRepeatedTicks; ++i) {
        generator(frame_buffer.data(), kWidth, kHeight, 2 + i, 0.08 + i * 0.04);
    }
    EXPECT_EQ(read_count, 1);
    EXPECT_EQ(generator.BufferedBytes(), 0U);
    EXPECT_EQ(frame_buffer.front(), kFrame1Fill);
    EXPECT_EQ(frame_buffer.back(), kFrame1Fill);

    // If a subsequent frame read fails, the previous frame in frame_buffer is preserved intact.
    int failed_read_count = 0;
    frames->PublishFrame(::goldfish::display::SlotLease(std::make_unique<TestRgbSlot>(
                                 ::goldfish::display::Dimensions{kWidth, kHeight}, 0x99,
                                 &failed_read_count, absl::UnavailableError("Producer reset"))),
                         absl::Now());
    generator(frame_buffer.data(), kWidth, kHeight, 100, 4.0);
    EXPECT_EQ(failed_read_count, 1);
    EXPECT_EQ(generator.BufferedBytes(), 0U);
    EXPECT_EQ(frame_buffer.front(), kFrame1Fill);
    EXPECT_EQ(frame_buffer.back(), kFrame1Fill);
}

TEST_F(ScreenRecordingServiceImplTest,
       FrameGeneratorReusesResizeScratchAndReleasesWhenDimensionsMatch) {
    constexpr int kRecordWidth = 1080;
    constexpr int kRecordHeight = 2400;
    constexpr size_t kRecordBytes = static_cast<size_t>(kRecordWidth) * kRecordHeight * 3;
    constexpr uint32_t kSmallWidth = 640;
    constexpr uint32_t kSmallHeight = 480;
    constexpr size_t kSmallBytes = static_cast<size_t>(kSmallWidth) * kSmallHeight * 3;
    constexpr uint8_t kSmallFill = 0x3C;
    constexpr uint8_t kFullFill = 0xA5;

    auto display = std::make_shared<::goldfish::display::test::FakePixmanDisplay>(
            loop.get(), 1, static_cast<::pixman_image_t*>(nullptr));
    const auto& frames = ::goldfish::display::DisplayTestPeer::frames(*display);
    ASSERT_NE(frames, nullptr);

    ScreenRecordingFrameGenerator generator(display);
    std::vector<uint8_t> frame_buffer(kRecordBytes, 0xFF);

    int small_reads = 0;
    frames->PublishFrame(::goldfish::display::SlotLease(std::make_unique<TestRgbSlot>(
                                 ::goldfish::display::Dimensions{kSmallWidth, kSmallHeight},
                                 kSmallFill, &small_reads)),
                         absl::Now());

    generator(frame_buffer.data(), kRecordWidth, kRecordHeight, 0, 0.0);
    EXPECT_EQ(small_reads, 1);
    EXPECT_EQ(generator.BufferedBytes(), kSmallBytes);

    // Top-left border pixel is black (0x00), while the centered pixel is kSmallFill.
    EXPECT_EQ(frame_buffer[0], 0U);
    const size_t center_offset =
            ((static_cast<size_t>(kRecordHeight / 2) * kRecordWidth) + (kRecordWidth / 2)) * 3;
    EXPECT_EQ(frame_buffer[center_offset], kSmallFill);

    // A failed read on a mismatched-size frame preserves the previous frame in frame_buffer
    // instead of wiping it with memset.
    frames->PublishFrame(::goldfish::display::SlotLease(std::make_unique<TestRgbSlot>(
                                 ::goldfish::display::Dimensions{kSmallWidth, kSmallHeight}, 0x77,
                                 nullptr, absl::UnavailableError("Reset during resize"))),
                         absl::Now());
    generator(frame_buffer.data(), kRecordWidth, kRecordHeight, 1, 0.04);
    EXPECT_EQ(frame_buffer[center_offset], kSmallFill);

    // Once the display frame matches the recording dimensions again, resize_scratch_ is released.
    frames->PublishFrame(
            ::goldfish::display::SlotLease(std::make_unique<TestRgbSlot>(
                    ::goldfish::display::Dimensions{kRecordWidth, kRecordHeight}, kFullFill)),
            absl::Now());
    generator(frame_buffer.data(), kRecordWidth, kRecordHeight, 2, 0.08);
    EXPECT_EQ(generator.BufferedBytes(), 0U);
    EXPECT_EQ(frame_buffer[0], kFullFill);
    EXPECT_EQ(frame_buffer[center_offset], kFullFill);

    // Invalid arguments and zero-sized frames preserve the initialized buffer intact.
    generator(nullptr, kRecordWidth, kRecordHeight, 3, 0.12);
    generator(frame_buffer.data(), 0, kRecordHeight, 4, 0.16);
    generator(frame_buffer.data(), kRecordWidth, 0, 5, 0.20);
    frames->PublishFrame(::goldfish::display::SlotLease(std::make_unique<TestRgbSlot>(
                                 ::goldfish::display::Dimensions{0, kSmallHeight}, 0x11)),
                         absl::Now());
    generator(frame_buffer.data(), kRecordWidth, kRecordHeight, 6, 0.24);
    EXPECT_EQ(frame_buffer[0], kFullFill);

    // A generator with a null display clears an uninitialized buffer once and retains it on repeat.
    ScreenRecordingFrameGenerator null_display_generator(nullptr);
    std::vector<uint8_t> null_display_buffer(64, 0xCC);
    null_display_generator(null_display_buffer.data(), 4, 4, 0, 0.0);
    EXPECT_EQ(null_display_buffer.front(), 0U);
    null_display_buffer.front() = 0x42;
    null_display_generator(null_display_buffer.data(), 4, 4, 1, 0.04);
    EXPECT_EQ(null_display_buffer.front(), 0x42);
}

}  // namespace android::emulation::control::incubating
