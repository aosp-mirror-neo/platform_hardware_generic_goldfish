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

#include "goldfish/display/webm_encoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "goldfish/file/file.h"

namespace goldfish::display {
namespace {

constexpr int kDefaultFps = 24;
constexpr int kDefaultBitrate = 400000;
constexpr int kRgbChannels = 3;

std::filesystem::path GetTempOutputPath(const std::string& filename) {
    return std::filesystem::temp_directory_path() / filename;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8_path = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8_path.data()), u8_path.size());
}

std::vector<uint8_t> MakeSyntheticRgbFrame(int width, int height, uint8_t seed) {
    const size_t total_bytes = static_cast<size_t>(width) * height * kRgbChannels;
    std::vector<uint8_t> frame(total_bytes);
    for (size_t i = 0; i < total_bytes; ++i) {
        frame[i] = static_cast<uint8_t>((i + seed) & 0xFFU);
    }
    return frame;
}

bool WaitForEncodedFrames(const WebMEncoder& encoder, uint64_t expected_frames,
                          absl::Duration timeout = absl::Seconds(5)) {
    const absl::Time deadline = absl::Now() + timeout;
    while (encoder.FramesEncoded() < expected_frames && absl::Now() < deadline) {
        absl::SleepFor(absl::Milliseconds(2));
    }
    return encoder.FramesEncoded() >= expected_frames;
}

TEST(WebMEncoderTest, ReusesFrameBuffersAcrossEncodedFrames) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr uint64_t kTotalFrames = 10;
    constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kHeight * kRgbChannels;

    const std::filesystem::path output_path = GetTempOutputPath("webm_encoder_reuse_test.webm");
    android::base::file::rm(output_path).IgnoreError();

    WebMEncoder encoder(output_path.string(), kWidth, kHeight, kDefaultFps, kDefaultBitrate);
    ASSERT_TRUE(encoder.Init());

    for (uint64_t i = 0; i < kTotalFrames; ++i) {
        const std::vector<uint8_t> frame =
                MakeSyntheticRgbFrame(kWidth, kHeight, static_cast<uint8_t>(i * 17U));
        encoder.AddFrame(frame.data());
        ASSERT_TRUE(WaitForEncodedFrames(encoder, i + 1));
    }

    EXPECT_EQ(encoder.FramesEncoded(), kTotalFrames);
    EXPECT_LE(encoder.FrameBufferAllocations(), 2U);
    EXPECT_LE(encoder.BufferedBytes(), 2U * kFrameBytes);

    encoder.Finish();
    EXPECT_EQ(encoder.QueuedFrames(), 0U);
    EXPECT_EQ(encoder.BufferedBytes(), 0U);

    ASSERT_TRUE(android::base::file::exists(output_path));
    const auto file_size = android::base::file::file_size(output_path);
    ASSERT_TRUE(file_size.ok()) << file_size.status();
    EXPECT_GT(file_size->Bytes(), 0U);

    android::base::file::rm(output_path).IgnoreError();
}

TEST(WebMEncoderTest, BoundsQueueDepthAndBufferMemoryUnderBackpressure) {
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    constexpr int kBurstFrames = 30;
    constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kHeight * kRgbChannels;
    constexpr size_t kMaxExpectedBuffers = WebMEncoder::kMaxQueuedFrames + 1;

    const std::filesystem::path output_path =
            GetTempOutputPath("webm_encoder_backpressure_test.webm");
    android::base::file::rm(output_path).IgnoreError();

    WebMEncoder encoder(output_path.string(), kWidth, kHeight, kDefaultFps, kDefaultBitrate);
    ASSERT_TRUE(encoder.Init());

    const std::vector<uint8_t> frame = MakeSyntheticRgbFrame(kWidth, kHeight, 42U);
    size_t max_queued_frames = 0;
    size_t max_buffered_bytes = 0;
    for (int i = 0; i < kBurstFrames; ++i) {
        encoder.AddFrame(frame.data());
        max_queued_frames = std::max(max_queued_frames, encoder.QueuedFrames());
        max_buffered_bytes = std::max(max_buffered_bytes, encoder.BufferedBytes());
    }

    EXPECT_LE(max_queued_frames, WebMEncoder::kMaxQueuedFrames);
    EXPECT_LE(encoder.FrameBufferAllocations(), kMaxExpectedBuffers);
    EXPECT_LE(max_buffered_bytes, kMaxExpectedBuffers * kFrameBytes);

    encoder.Finish();
    EXPECT_EQ(encoder.QueuedFrames(), 0U);
    EXPECT_EQ(encoder.BufferedBytes(), 0U);

    android::base::file::rm(output_path).IgnoreError();
}

TEST(WebMEncoderTest, IgnoresNullFrameAndFramesBeforeInitOrAfterFinish) {
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;

    const std::filesystem::path output_path = GetTempOutputPath("webm_encoder_lifecycle_test.webm");
    android::base::file::rm(output_path).IgnoreError();

    const std::vector<uint8_t> frame = MakeSyntheticRgbFrame(kWidth, kHeight, 7U);
    WebMEncoder encoder(output_path.string(), kWidth, kHeight, kDefaultFps, kDefaultBitrate);

    encoder.AddFrame(frame.data());
    EXPECT_EQ(encoder.FrameBufferAllocations(), 0U);
    EXPECT_EQ(encoder.QueuedFrames(), 0U);

    ASSERT_TRUE(encoder.Init());
    encoder.AddFrame(nullptr);
    EXPECT_EQ(encoder.FrameBufferAllocations(), 0U);
    EXPECT_EQ(encoder.QueuedFrames(), 0U);

    encoder.Finish();
    encoder.AddFrame(frame.data());
    EXPECT_EQ(encoder.FrameBufferAllocations(), 0U);
    EXPECT_EQ(encoder.BufferedBytes(), 0U);

    android::base::file::rm(output_path).IgnoreError();
}

TEST(WebMEncoderTest, EncodesToUnicodeOutputPath) {
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;

    const std::filesystem::path output_path =
            std::filesystem::temp_directory_path() /
            std::filesystem::path(std::u8string_view(u8"webm_测试_録画.webm"));
    android::base::file::rm(output_path).IgnoreError();

    WebMEncoder encoder(PathToUtf8(output_path), kWidth, kHeight, kDefaultFps, kDefaultBitrate);
    ASSERT_TRUE(encoder.Init());

    const std::vector<uint8_t> frame = MakeSyntheticRgbFrame(kWidth, kHeight, 99U);
    encoder.AddFrame(frame.data());
    ASSERT_TRUE(WaitForEncodedFrames(encoder, 1U));

    encoder.Finish();
    ASSERT_TRUE(android::base::file::exists(output_path));
    const auto file_size = android::base::file::file_size(output_path);
    ASSERT_TRUE(file_size.ok()) << file_size.status();
    EXPECT_GT(file_size->Bytes(), 0U);

    android::base::file::rm(output_path).IgnoreError();
}

}  // namespace
}  // namespace goldfish::display
