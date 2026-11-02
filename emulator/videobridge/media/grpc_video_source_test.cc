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

#include "grpc_video_source.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "emulator_controller.grpc.pb.h"
#include "goldfish/memory/shared_memory.h"

namespace goldfish::videobridge {

using ::android::emulation::control::Image;
using ::android::emulation::control::ImageFormat;
using ::android::emulation::control::ImageTransport;

namespace {

namespace fs = std::filesystem;

class FakeImageReader : public ::grpc::ClientReaderInterface<Image> {
  public:
    static constexpr size_t kTestFrameBytes = 2ULL * 2ULL * 4ULL;  // 2x2 RGBA8888 = 16 bytes

    explicit FakeImageReader(const ImageFormat& format, bool force_payload_delivery = false,
                             size_t max_frames = 3)
            : format_(format)
            , force_payload_delivery_(force_payload_delivery)
            , max_frames_(max_frames) {
        if (format_.transport().channel() == ImageTransport::MMAP && !force_payload_delivery_) {
            std::string path_or_uri = format_.transport().handle();
            if (path_or_uri.starts_with("file://")) {
                path_or_uri = path_or_uri.substr(7);
            }
            shm_ = std::make_unique<::goldfish::memory::SharedMemory>(path_or_uri, kTestFrameBytes);
            shm_open_ok_ =
                    shm_->Open(::goldfish::memory::SharedMemory::AccessMode::kReadWrite).ok();
        }
    }

    bool Read(Image* frame) override {
        if (index_ >= max_frames_) {
            return false;
        }
        frame->mutable_format()->set_width(2);
        frame->mutable_format()->set_height(2);

        if (shm_ && shm_open_ok_) {
            const std::string frame_data(kTestFrameBytes, static_cast<char>('\x10' + index_));
            std::memcpy(shm_->Get(), frame_data.data(), frame_data.size());
        } else if (force_payload_delivery_) {
            const std::string frame_data(kTestFrameBytes, static_cast<char>('\x20' + index_));
            frame->set_image(frame_data);
        } else {
            frame->set_image(std::string(kTestFrameBytes, '\x7F'));
        }

        index_++;
        return true;
    }

    void WaitForInitialMetadata() override {}
    bool NextMessageSize(uint32_t* /*sz*/) override { return false; }
    ::grpc::Status Finish() override { return ::grpc::Status::OK; }

  private:
    ImageFormat format_;
    bool force_payload_delivery_;
    size_t max_frames_;
    size_t index_ = 0;
    std::unique_ptr<::goldfish::memory::SharedMemory> shm_;
    bool shm_open_ok_ = false;
};

class FakeEmulatorClient : public EmulatorClient {
  public:
    explicit FakeEmulatorClient(bool is_connected = true, size_t max_frames = 3)
            : is_connected_(is_connected), max_frames_(max_frames) {}

    bool IsConnected() const override { return is_connected_; }
    std::string TargetAddress() const override { return "fake_target_address"; }

    std::unique_ptr<::grpc::ClientReaderInterface<Image>> StreamScreenshot(
            ::grpc::ClientContext* /*context*/, const ImageFormat& format) override {
        screenshot_requested_ = true;
        requested_display_id_ = static_cast<int>(format.display());
        return std::make_unique<FakeImageReader>(format, force_payload_delivery_, max_frames_);
    }

    bool ScreenshotRequested() const { return screenshot_requested_; }
    int RequestedDisplayId() const { return requested_display_id_; }
    void SetForcePayloadDelivery(bool force) { force_payload_delivery_ = force; }

  private:
    bool is_connected_;
    size_t max_frames_;
    std::atomic<bool> screenshot_requested_{false};
    std::atomic<int> requested_display_id_{-1};
    bool force_payload_delivery_ = false;
};

class TestVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
  public:
    void OnFrame(const webrtc::VideoFrame& frame) override {
        const absl::MutexLock lock(&mutex_);
        if (frame.video_frame_buffer() != nullptr &&
            frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
            unique_y_planes_.insert(frame.video_frame_buffer()->GetNV12()->DataY());
        }
        last_frame_ = frame;
        frame_count_++;
    }

    int FrameCount() const {
        const absl::MutexLock lock(&mutex_);
        return frame_count_;
    }

    size_t UniqueYPlaneCount() const {
        const absl::MutexLock lock(&mutex_);
        return unique_y_planes_.size();
    }

    webrtc::VideoFrame LastFrame() const {
        const absl::MutexLock lock(&mutex_);
        if (!last_frame_.has_value()) {
            return webrtc::VideoFrame::Builder().build();
        }
        return *last_frame_;
    }

  private:
    mutable absl::Mutex mutex_;
    int frame_count_ ABSL_GUARDED_BY(mutex_) = 0;
    std::set<const uint8_t*> unique_y_planes_ ABSL_GUARDED_BY(mutex_);
    absl::optional<webrtc::VideoFrame> last_frame_ ABSL_GUARDED_BY(mutex_);
};

TEST(GrpcVideoSourceTest, GrpcVideoSourceStreamsFramesFromClient) {
    auto client = std::make_shared<FakeEmulatorClient>(/*is_connected=*/true);

    GrpcVideoSourceOptions options;
    options.display_id = 1;
    options.width = 2;
    options.height = 2;
    options.transport = GrpcVideoSourceOptions::Transport::kGrpcBytes;

    auto source = ::webrtc::make_ref_counted<GrpcVideoSource>(client, options);
    TestVideoSink sink;
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source_interface = source.get();
    source_interface->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink.FrameCount() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    source->Stop();
    source_interface->RemoveSink(&sink);

    EXPECT_GE(sink.FrameCount(), 3);
    EXPECT_TRUE(client->ScreenshotRequested());
    EXPECT_EQ(client->RequestedDisplayId(), 1);

    // Verify properties of the converted frame
    const webrtc::VideoFrame frame = sink.LastFrame();
    EXPECT_EQ(frame.width(), 2);
    EXPECT_EQ(frame.height(), 2);
    ASSERT_NE(frame.video_frame_buffer(), nullptr);
    EXPECT_EQ(frame.video_frame_buffer()->type(), webrtc::VideoFrameBuffer::Type::kNV12);
    const webrtc::NV12BufferInterface* nv12 = frame.video_frame_buffer()->GetNV12();
    ASSERT_NE(nv12, nullptr);
    EXPECT_NE(nv12->DataY(), nullptr);
    EXPECT_NE(nv12->DataUV(), nullptr);
    const ::webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
            frame.video_frame_buffer()->ToI420();
    ASSERT_NE(i420, nullptr);
    EXPECT_EQ(i420->width(), 2);
    EXPECT_EQ(i420->height(), 2);
}

TEST(GrpcVideoSourceTest, GrpcVideoSourceStreamsFramesViaSharedMemory) {
    auto client = std::make_shared<FakeEmulatorClient>(/*is_connected=*/true);

    // Setup Shared Memory option path
    const std::string shm_filename =
            absl::StrCat("grpc_video_source_test_shm_",
                         absl::Hex(absl::Uniform<uint64_t>(absl::BitGen()), absl::kSpacePad16));
    const fs::path shm_path = fs::temp_directory_path() / shm_filename;

    GrpcVideoSourceOptions options;
    options.display_id = 1;
    options.width = 2;
    options.height = 2;
    options.transport = GrpcVideoSourceOptions::Transport::kSharedMemory;
    options.shared_memory_path = shm_path;

    auto source = ::webrtc::make_ref_counted<GrpcVideoSource>(client, options);
    TestVideoSink sink;
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source_interface = source.get();
    source_interface->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink.FrameCount() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    source->Stop();
    source_interface->RemoveSink(&sink);

    EXPECT_GE(sink.FrameCount(), 3);
    EXPECT_TRUE(client->ScreenshotRequested());
    EXPECT_EQ(client->RequestedDisplayId(), 1);

    // Verify properties of the last converted frame read from shared memory.
    const webrtc::VideoFrame frame = sink.LastFrame();
    EXPECT_EQ(frame.width(), 2);
    EXPECT_EQ(frame.height(), 2);
    ASSERT_NE(frame.video_frame_buffer(), nullptr);
    EXPECT_EQ(frame.video_frame_buffer()->type(), webrtc::VideoFrameBuffer::Type::kNV12);
    const webrtc::NV12BufferInterface* nv12_shm = frame.video_frame_buffer()->GetNV12();
    ASSERT_NE(nv12_shm, nullptr);
    EXPECT_NE(nv12_shm->DataY(), nullptr);
    EXPECT_NE(nv12_shm->DataUV(), nullptr);
    const ::webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
            frame.video_frame_buffer()->ToI420();
    ASSERT_NE(i420, nullptr);
    EXPECT_EQ(i420->width(), 2);
    EXPECT_EQ(i420->height(), 2);

    // Since the fake wrote a constant value of 0x12 (18) to all channels (RGB),
    // the converted Y channel in limited-range YUV is: 16 + 0.859 * 18 = ~31.
    const uint8_t* y_data = i420->DataY();
    ASSERT_NE(y_data, nullptr);
    EXPECT_NEAR(y_data[0], 31, 2);
}

TEST(GrpcVideoSourceTest, GrpcVideoSourceFallsBackToBytesIfSharedMemoryNotMapped) {
    auto client = std::make_shared<FakeEmulatorClient>(/*is_connected=*/true);

    // Force the fake to ignore MMAP request and send the image in the proto payload
    client->SetForcePayloadDelivery(true);

    // We request kSharedMemory, but we provide an invalid/uncreatable path.
    // This causes shared_memory_->Create() to fail, so shared_memory_->IsMapped() is false,
    // forcing the client to fall back to reading from the proto payload.
    GrpcVideoSourceOptions options;
    options.display_id = 1;
    options.width = 2;
    options.height = 2;
    options.transport = GrpcVideoSourceOptions::Transport::kSharedMemory;
    // An empty path or a path in a non-existent directory will cause creation to fail.
    options.shared_memory_path = "/non_existent_directory/invalid_shm_path";

    auto source = ::webrtc::make_ref_counted<GrpcVideoSource>(client, options);
    TestVideoSink sink;
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source_interface = source.get();
    source_interface->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink.FrameCount() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    source->Stop();
    source_interface->RemoveSink(&sink);

    EXPECT_GE(sink.FrameCount(), 3);
    EXPECT_TRUE(client->ScreenshotRequested());
    EXPECT_EQ(client->RequestedDisplayId(), 1);

    // Verify properties of the last converted frame read from protobuf payload.
    const webrtc::VideoFrame frame = sink.LastFrame();
    EXPECT_EQ(frame.width(), 2);
    EXPECT_EQ(frame.height(), 2);
    ASSERT_NE(frame.video_frame_buffer(), nullptr);
    const ::webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
            frame.video_frame_buffer()->ToI420();
    ASSERT_NE(i420, nullptr);

    // Since the fake wrote a constant value of 0x22 (34) to all channels (RGB) for the last frame,
    // the converted Y channel in limited-range YUV is: 16 + 0.859 * 34 = ~45.
    const uint8_t* y_data = i420->DataY();
    ASSERT_NE(y_data, nullptr);
    EXPECT_NEAR(y_data[0], 45, 2);
}

TEST(GrpcVideoSourceTest, GrpcVideoSourceStreamsFramesWithI420Pipeline) {
    auto client = std::make_shared<FakeEmulatorClient>(/*is_connected=*/true);

    GrpcVideoSourceOptions options;
    options.display_id = 1;
    options.width = 2;
    options.height = 2;
    options.transport = GrpcVideoSourceOptions::Transport::kGrpcBytes;

    auto source = ::webrtc::make_ref_counted<GrpcVideoSource>(
            client, options, std::make_unique<RgbaToI420Pipeline>());
    TestVideoSink sink;
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source_interface = source.get();
    source_interface->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink.FrameCount() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    source->Stop();
    source_interface->RemoveSink(&sink);

    EXPECT_GE(sink.FrameCount(), 3);
    const webrtc::VideoFrame frame = sink.LastFrame();
    EXPECT_EQ(frame.width(), 2);
    EXPECT_EQ(frame.height(), 2);
    ASSERT_NE(frame.video_frame_buffer(), nullptr);
    EXPECT_EQ(frame.video_frame_buffer()->type(), webrtc::VideoFrameBuffer::Type::kI420);
}

TEST(GrpcVideoSourceTest, RgbaToNv12PipelinePoolsFrameBuffersAcrossFrames) {
    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
    constexpr size_t kRgbaBytes = static_cast<size_t>(kWidth) * kHeight * 4;
    constexpr size_t kNv12Bytes = static_cast<size_t>(kWidth) * kHeight * 3 / 2;
    constexpr size_t kFrameCount = 60;

    const std::vector<uint8_t> rgba_frame(kRgbaBytes, 0x42);
    RgbaToNv12Pipeline pipeline;

    // 1. Sequential consumption: when each frame is consumed before the next Convert() call,
    // all 60 frames reuse the single pooled NV12 buffer (345.6 KB allocated vs 20.74 MB unpooled).
    std::set<const uint8_t*> unique_y_planes;
    std::set<const uint8_t*> unique_uv_planes;
    for (size_t i = 0; i < kFrameCount; ++i) {
        auto frame = pipeline.Convert(rgba_frame.data(), rgba_frame.size(), kWidth, kHeight,
                                      static_cast<int64_t>(i) * 16667);
        ASSERT_TRUE(frame.has_value());
        const webrtc::NV12BufferInterface* nv12 = frame->video_frame_buffer()->GetNV12();
        ASSERT_NE(nv12, nullptr);
        unique_y_planes.insert(nv12->DataY());
        unique_uv_planes.insert(nv12->DataUV());
    }
    EXPECT_EQ(unique_y_planes.size(), 1U);
    EXPECT_EQ(unique_uv_planes.size(), 1U);
    const size_t unpooled_total_bytes = kFrameCount * kNv12Bytes;
    const size_t pooled_total_bytes = unique_y_planes.size() * kNv12Bytes;
    EXPECT_EQ(pooled_total_bytes, kNv12Bytes);
    EXPECT_LT(pooled_total_bytes, unpooled_total_bytes / 50);

    // 2. Double-buffered sink: when the sink holds the previous frame while the next frame is
    // converted, the pool alternates between at most 2 NV12 buffers across all 60 frames.
    std::set<const uint8_t*> double_buffered_y_planes;
    std::optional<webrtc::VideoFrame> held_frame;
    for (size_t i = 0; i < kFrameCount; ++i) {
        auto next_frame = pipeline.Convert(rgba_frame.data(), rgba_frame.size(), kWidth, kHeight,
                                           static_cast<int64_t>(i) * 16667);
        ASSERT_TRUE(next_frame.has_value());
        const webrtc::NV12BufferInterface* nv12 = next_frame->video_frame_buffer()->GetNV12();
        ASSERT_NE(nv12, nullptr);
        double_buffered_y_planes.insert(nv12->DataY());
        held_frame = std::move(next_frame);
    }
    EXPECT_EQ(double_buffered_y_planes.size(), 2U);

    // 3. Release() drops the pool's references without mutating an in-flight frame still held
    // by a downstream consumer.
    ASSERT_TRUE(held_frame.has_value());
    const uint8_t* in_flight_y = held_frame->video_frame_buffer()->GetNV12()->DataY();
    pipeline.Release();
    auto post_release_frame =
            pipeline.Convert(rgba_frame.data(), rgba_frame.size(), kWidth, kHeight, 1000000);
    ASSERT_TRUE(post_release_frame.has_value());
    EXPECT_NE(post_release_frame->video_frame_buffer()->GetNV12()->DataY(), in_flight_y);
}

TEST(GrpcVideoSourceTest, RgbaToI420PipelinePoolsFrameBuffersAcrossFrames) {
    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
    constexpr size_t kRgbaBytes = static_cast<size_t>(kWidth) * kHeight * 4;
    constexpr size_t kI420Bytes = static_cast<size_t>(kWidth) * kHeight * 3 / 2;
    constexpr size_t kFrameCount = 60;

    const std::vector<uint8_t> rgba_frame(kRgbaBytes, 0x36);
    RgbaToI420Pipeline pipeline;

    std::set<const uint8_t*> unique_y_planes;
    for (size_t i = 0; i < kFrameCount; ++i) {
        auto frame = pipeline.Convert(rgba_frame.data(), rgba_frame.size(), kWidth, kHeight,
                                      static_cast<int64_t>(i) * 16667);
        ASSERT_TRUE(frame.has_value());
        ASSERT_EQ(frame->video_frame_buffer()->type(), webrtc::VideoFrameBuffer::Type::kI420);
        const webrtc::I420BufferInterface* i420 = frame->video_frame_buffer()->GetI420();
        ASSERT_NE(i420, nullptr);
        unique_y_planes.insert(i420->DataY());
    }
    EXPECT_EQ(unique_y_planes.size(), 1U);
    EXPECT_EQ(unique_y_planes.size() * kI420Bytes, kI420Bytes);

    std::set<const uint8_t*> double_buffered_y_planes;
    std::optional<webrtc::VideoFrame> held_frame;
    for (size_t i = 0; i < kFrameCount; ++i) {
        auto next_frame = pipeline.Convert(rgba_frame.data(), rgba_frame.size(), kWidth, kHeight,
                                           static_cast<int64_t>(i) * 16667);
        ASSERT_TRUE(next_frame.has_value());
        double_buffered_y_planes.insert(next_frame->video_frame_buffer()->GetI420()->DataY());
        held_frame = std::move(next_frame);
    }
    EXPECT_EQ(double_buffered_y_planes.size(), 2U);

    ASSERT_TRUE(held_frame.has_value());
    const uint8_t* in_flight_y = held_frame->video_frame_buffer()->GetI420()->DataY();
    pipeline.Release();
    auto post_release_frame =
            pipeline.Convert(rgba_frame.data(), rgba_frame.size(), kWidth, kHeight, 1000000);
    ASSERT_TRUE(post_release_frame.has_value());
    EXPECT_NE(post_release_frame->video_frame_buffer()->GetI420()->DataY(), in_flight_y);
}

TEST(GrpcVideoSourceTest, GrpcVideoSourceReusesBuffersAcrossStreamedFrames) {
    constexpr size_t kStreamedFrames = 12;
    auto client = std::make_shared<FakeEmulatorClient>(/*is_connected=*/true, kStreamedFrames);

    GrpcVideoSourceOptions options;
    options.display_id = 1;
    options.width = 2;
    options.height = 2;
    options.transport = GrpcVideoSourceOptions::Transport::kGrpcBytes;

    auto source = ::webrtc::make_ref_counted<GrpcVideoSource>(client, options);
    TestVideoSink sink;
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source_interface = source.get();
    source_interface->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink.FrameCount() < static_cast<int>(kStreamedFrames) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    source->Stop();
    source_interface->RemoveSink(&sink);

    EXPECT_EQ(sink.FrameCount(), static_cast<int>(kStreamedFrames));
    // TestVideoSink retains only `last_frame_`, so at most 2 NV12 buffers are ever live
    // simultaneously across all 12 streamed frames.
    EXPECT_LE(sink.UniqueYPlaneCount(), 2U);
}

}  // namespace
}  // namespace goldfish::videobridge
