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

#include "goldfish/display/write_jpeg.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "absl/base/config.h"
#include "absl/time/time.h"

#include "image_writer_test_utils.h"

extern "C" {
#include <jpeglib.h>
}

namespace goldfish::display {
namespace {

using test_utils::CurrentThreadCpuTime;
using test_utils::MakeSyntheticScreenshot;

struct JpegTestErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

void OnJpegTestErrorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegTestErrorMgr*>(cinfo->err);
    longjmp(err->setjmp_buffer, 1);
}

bool DecodeJpegToRgb(std::span<const uint8_t> jpeg_bytes, unsigned int* out_width,
                     unsigned int* out_height, std::vector<uint8_t>* out_rgb_pixels) {
    if (jpeg_bytes.size() < 4U || jpeg_bytes[0] != 0xFFU || jpeg_bytes[1] != 0xD8U) {
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegTestErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = OnJpegTestErrorExit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_bytes.data(), static_cast<unsigned long>(jpeg_bytes.size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const unsigned int width = cinfo.output_width;
    const unsigned int height = cinfo.output_height;
    const auto channels = static_cast<unsigned int>(cinfo.output_components);
    if (channels != 3U) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    *out_width = width;
    *out_height = height;
    const size_t stride = static_cast<size_t>(width) * 3U;
    out_rgb_pixels->resize(stride * height);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row =
                out_rgb_pixels->data() + (static_cast<size_t>(cinfo.output_scanline) * stride);
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

// Exact unoptimized RGBA path prior to optimization (per-call compressor init,
// jpeg_mem_dest reallocations, default JDCT_ISLOW, and #ifdef JCS_EXT_RGBA checking an
// enumerator rather than a macro and falling back to per-row RGB stripping).
bool WriteJpegLegacyUnoptimized(unsigned int width, unsigned int height, const void* pixels,
                                std::string* out_data, int quality) {
    const auto* byte_pixels = static_cast<const uint8_t*>(pixels);
    jpeg_compress_struct cinfo{};
    JpegTestErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = OnJpegTestErrorExit;

    unsigned char* mem_buffer = nullptr;
    unsigned long mem_size = 0;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        if (mem_buffer != nullptr) {
            std::free(mem_buffer);
        }
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem_buffer, &mem_size);

    cinfo.image_width = width;
    cinfo.image_height = height;

    std::vector<uint8_t> rgb_row(static_cast<size_t>(width) * 3U);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    const size_t rgba_stride = static_cast<size_t>(width) * 4U;
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* src = byte_pixels + (static_cast<size_t>(cinfo.next_scanline) * rgba_stride);
        for (unsigned int x = 0; x < width; ++x) {
            rgb_row[static_cast<size_t>(x) * 3U + 0U] = src[static_cast<size_t>(x) * 4U + 0U];
            rgb_row[static_cast<size_t>(x) * 3U + 1U] = src[static_cast<size_t>(x) * 4U + 1U];
            rgb_row[static_cast<size_t>(x) * 3U + 2U] = src[static_cast<size_t>(x) * 4U + 2U];
        }
        JSAMPROW row_pointer = rgb_row.data();
        jpeg_write_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    out_data->assign(reinterpret_cast<const char*>(mem_buffer), static_cast<size_t>(mem_size));
    std::free(mem_buffer);
    return true;
}

// Intermediate reference with JCS_EXT_RGBA SIMD enabled, but still using per-call
// compressor init, jpeg_mem_dest, JDCT_ISLOW, and 1-row jpeg_write_scanlines.
bool WriteJpegSimdMemDestUnbatched(unsigned int width, unsigned int height, const void* pixels,
                                   std::string* out_data, int quality) {
    const auto* byte_pixels = static_cast<const uint8_t*>(pixels);
    jpeg_compress_struct cinfo{};
    JpegTestErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = OnJpegTestErrorExit;

    unsigned char* mem_buffer = nullptr;
    unsigned long mem_size = 0;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        if (mem_buffer != nullptr) {
            std::free(mem_buffer);
        }
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem_buffer, &mem_size);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    const size_t stride = static_cast<size_t>(width) * 4U;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer = const_cast<JSAMPROW>(
                byte_pixels + (static_cast<size_t>(cinfo.next_scanline) * stride));
        jpeg_write_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    out_data->assign(reinterpret_cast<const char*>(mem_buffer), static_cast<size_t>(mem_size));
    std::free(mem_buffer);
    return true;
}

absl::Duration MedianDuration(std::vector<absl::Duration> samples) {
    const size_t mid = samples.size() / 2U;
    std::nth_element(samples.begin(), samples.begin() + static_cast<ptrdiff_t>(mid), samples.end());
    return samples[mid];
}

std::vector<uint8_t> MakeSolidImage(unsigned int width, unsigned int height, unsigned int channels,
                                    uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> buf(static_cast<size_t>(width) * height * channels);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        buf[i * channels + 0U] = r;
        buf[i * channels + 1U] = g;
        buf[i * channels + 2U] = b;
        if (channels == 4U) {
            buf[i * channels + 3U] = 0xFFU;
        }
    }
    return buf;
}

std::vector<uint8_t> MakeDetailedPattern(unsigned int width, unsigned int height,
                                         unsigned int channels) {
    std::vector<uint8_t> buf(static_cast<size_t>(width) * height * channels);
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            uint8_t* px = buf.data() + ((static_cast<size_t>(y) * width + x) * channels);
            px[0] = static_cast<uint8_t>((x * 13U + y * 7U) & 0xFFU);
            px[1] = static_cast<uint8_t>((x * 5U ^ y * 19U) & 0xFFU);
            px[2] = static_cast<uint8_t>((x * 23U + y * 3U) & 0xFFU);
            if (channels == 4U) {
                px[3] = 0xFFU;
            }
        }
    }
    return buf;
}

std::vector<uint8_t> MakeHighEntropyNoise(unsigned int width, unsigned int height,
                                          unsigned int channels) {
    std::vector<uint8_t> buf(static_cast<size_t>(width) * height * channels);
    uint32_t state = 0x9E3779B9U;
    for (size_t i = 0; i < buf.size(); ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        buf[i] = static_cast<uint8_t>(state & 0xFFU);
    }
    return buf;
}

TEST(WriteJpegTest, RejectsInvalidArguments) {
    const std::vector<uint8_t> pixel(64U, 128U);
    std::string out_str = "stale_payload";
    std::vector<uint8_t> out_vec = {1U, 2U, 3U};

    // Invalid channel counts must fail and clear pre-populated output buffers.
    EXPECT_FALSE(WriteJpeg(0, 1, 1, pixel.data(), &out_str));
    EXPECT_TRUE(out_str.empty());
    EXPECT_FALSE(WriteJpeg(0, 1, 1, pixel.data(), out_vec));
    EXPECT_TRUE(out_vec.empty());
    EXPECT_FALSE(WriteJpeg(1, 1, 1, pixel.data(), &out_str));
    EXPECT_FALSE(WriteJpeg(2, 1, 1, pixel.data(), &out_str));
    EXPECT_FALSE(WriteJpeg(5, 1, 1, pixel.data(), &out_str));

    // Invalid dimensions or null pointers.
    EXPECT_FALSE(WriteJpeg(4, 0, 1, pixel.data(), &out_str));
    EXPECT_FALSE(WriteJpeg(4, 1, 0, pixel.data(), &out_str));
    EXPECT_FALSE(WriteJpeg(4, 1, 1, nullptr, &out_str));
    EXPECT_FALSE(WriteJpeg(4, 1, 1, pixel.data(), nullptr));
    EXPECT_FALSE(EncodeRgbaToJpeg(0, 1, pixel.data(), &out_str));
    EXPECT_FALSE(EncodeRgbaToJpeg(1, 0, pixel.data(), &out_str));
    EXPECT_FALSE(EncodeRgbaToJpeg(1, 1, nullptr, &out_str));
    EXPECT_FALSE(EncodeRgbaToJpeg(1, 1, pixel.data(), nullptr));

    // Out-of-range quality values.
    EXPECT_FALSE(WriteJpeg(4, 1, 1, pixel.data(), &out_str, 0));
    EXPECT_FALSE(WriteJpeg(4, 1, 1, pixel.data(), &out_str, -10));
    EXPECT_FALSE(WriteJpeg(4, 1, 1, pixel.data(), &out_str, 101));
    EXPECT_FALSE(WriteJpeg(4, 1, 1, pixel.data(), out_vec, 0));
    EXPECT_FALSE(WriteJpeg(4, 1, 1, pixel.data(), out_vec, 105));

    // Non-zero row_stride smaller than width * n_channels.
    EXPECT_FALSE(WriteJpeg(4, 4, 2, pixel.data(), &out_str, kDefaultJpegQuality, /*row_stride=*/1));
    EXPECT_FALSE(
            WriteJpeg(4, 4, 2, pixel.data(), &out_str, kDefaultJpegQuality, /*row_stride=*/15));
    EXPECT_FALSE(WriteJpeg(3, 4, 2, pixel.data(), out_vec, kDefaultJpegQuality, /*row_stride=*/11));
    EXPECT_FALSE(
            EncodeRgbaToJpeg(4, 2, pixel.data(), &out_str, kDefaultJpegQuality, /*row_stride=*/15));

    // Overflow across (height - 1) * actual_stride + min_stride (where the start of the final
    // row fits in size_t, but reading its width * n_channels bytes wraps around SIZE_MAX).
    constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
    EXPECT_FALSE(WriteJpeg(4, 16, 2, pixel.data(), &out_str, kDefaultJpegQuality,
                           /*row_stride=*/kMaxSize - 32U));
    EXPECT_FALSE(WriteJpeg(4, 16, 2, pixel.data(), out_vec, kDefaultJpegQuality,
                           /*row_stride=*/kMaxSize - 32U));
    EXPECT_FALSE(WriteJpeg(4, 16, 3, pixel.data(), &out_str, kDefaultJpegQuality,
                           /*row_stride=*/kMaxSize / 2U));
    EXPECT_FALSE(WriteJpeg(4, std::numeric_limits<unsigned int>::max(), 2, pixel.data(), &out_str));
    EXPECT_FALSE(WriteJpeg(4, 2, std::numeric_limits<unsigned int>::max(), pixel.data(), &out_str));

    EXPECT_TRUE(out_str.empty());
    EXPECT_TRUE(out_vec.empty());

    // Oversized JPEG dimension (> JPEG_MAX_DIMENSION = 65500) fails fast without allocating
    // output buffers and leaves the thread-local compressor reusable for subsequent calls.
    const std::vector<uint8_t> wide_row(static_cast<size_t>(70000U) * 4U, 0x80U);
    out_str = "stale_payload";
    out_vec = {9U, 8U, 7U};
    EXPECT_FALSE(WriteJpeg(4, 70000U, 1, wide_row.data(), &out_str));
    EXPECT_FALSE(WriteJpeg(4, 1, 70000U, wide_row.data(), out_vec));
    EXPECT_TRUE(out_str.empty());
    EXPECT_TRUE(out_vec.empty());
    ASSERT_TRUE(WriteJpeg(4, 8, 8, wide_row.data(), &out_str));
    EXPECT_FALSE(out_str.empty());

    // Malformed JPEG bytes exercise libjpeg setjmp/longjmp error_exit (including Windows
    // /GUARD:CF longjmp target validation).
    const std::vector<uint8_t> corrupt_jpeg = {0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x02U};
    unsigned int bad_w = 0;
    unsigned int bad_h = 0;
    std::vector<uint8_t> bad_rgb;
    EXPECT_FALSE(DecodeJpegToRgb(corrupt_jpeg, &bad_w, &bad_h, &bad_rgb));
}

TEST(WriteJpegTest, RoundTripsRgbAndRgbaAndPreservesColors) {
    constexpr unsigned int kWidth = 48;
    constexpr unsigned int kHeight = 32;
    constexpr uint8_t kExpectedR = 200;
    constexpr uint8_t kExpectedG = 90;
    constexpr uint8_t kExpectedB = 40;

    for (unsigned int channels : {3U, 4U}) {
        for (int quality : {85, 98}) {
            const std::vector<uint8_t> src =
                    MakeSolidImage(kWidth, kHeight, channels, kExpectedR, kExpectedG, kExpectedB);
            std::vector<uint8_t> jpeg_bytes;
            ASSERT_TRUE(WriteJpeg(channels, kWidth, kHeight, src.data(), jpeg_bytes, quality));
            ASSERT_GE(jpeg_bytes.size(), 4U);
            // Verify SOI (FF D8) and EOI (FF D9) markers.
            EXPECT_EQ(jpeg_bytes[0], 0xFFU);
            EXPECT_EQ(jpeg_bytes[1], 0xD8U);
            EXPECT_EQ(jpeg_bytes[jpeg_bytes.size() - 2U], 0xFFU);
            EXPECT_EQ(jpeg_bytes[jpeg_bytes.size() - 1U], 0xD9U);

            unsigned int decoded_w = 0;
            unsigned int decoded_h = 0;
            std::vector<uint8_t> decoded_rgb;
            ASSERT_TRUE(DecodeJpegToRgb(jpeg_bytes, &decoded_w, &decoded_h, &decoded_rgb));
            EXPECT_EQ(decoded_w, kWidth);
            EXPECT_EQ(decoded_h, kHeight);
            ASSERT_EQ(decoded_rgb.size(), static_cast<size_t>(kWidth) * kHeight * 3U);

            for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
                EXPECT_NEAR(decoded_rgb[i * 3U + 0U], kExpectedR, 3);
                EXPECT_NEAR(decoded_rgb[i * 3U + 1U], kExpectedG, 3);
                EXPECT_NEAR(decoded_rgb[i * 3U + 2U], kExpectedB, 3);
            }
        }
    }
}

TEST(WriteJpegTest, EncodesStridedRowsAndIgnoresPaddingAndFourthChannel) {
    constexpr unsigned int kWidth = 37;
    constexpr unsigned int kHeight = 35;  // Two 16-row batches plus a 3-row tail.

    for (unsigned int channels : {3U, 4U}) {
        const size_t tight_stride = static_cast<size_t>(kWidth) * channels;
        const size_t padded_stride = tight_stride + 29U;
        const std::vector<uint8_t> tight_src = MakeDetailedPattern(kWidth, kHeight, channels);

        std::vector<uint8_t> strided_src(padded_stride * kHeight, 0xA5U);
        for (unsigned int y = 0; y < kHeight; ++y) {
            const uint8_t* src_row = tight_src.data() + (static_cast<size_t>(y) * tight_stride);
            uint8_t* dst_row = strided_src.data() + (static_cast<size_t>(y) * padded_stride);
            for (unsigned int x = 0; x < kWidth; ++x) {
                dst_row[static_cast<size_t>(x) * channels + 0U] =
                        src_row[static_cast<size_t>(x) * channels + 0U];
                dst_row[static_cast<size_t>(x) * channels + 1U] =
                        src_row[static_cast<size_t>(x) * channels + 1U];
                dst_row[static_cast<size_t>(x) * channels + 2U] =
                        src_row[static_cast<size_t>(x) * channels + 2U];
                if (channels == 4U) {
                    // Populate the 4th byte with varying non-opaque values; RGBX must ignore it.
                    dst_row[static_cast<size_t>(x) * 4U + 3U] =
                            static_cast<uint8_t>((x * 31U + y * 17U) & 0xFFU);
                }
            }
        }

        std::string tight_jpeg;
        std::string strided_jpeg;
        std::vector<uint8_t> strided_vec;
        ASSERT_TRUE(WriteJpeg(channels, kWidth, kHeight, tight_src.data(), &tight_jpeg, 90));
        ASSERT_TRUE(WriteJpeg(channels, kWidth, kHeight, strided_src.data(), &strided_jpeg, 90,
                              padded_stride));
        ASSERT_TRUE(WriteJpeg(channels, kWidth, kHeight, strided_src.data(), strided_vec, 90,
                              padded_stride));

        EXPECT_EQ(strided_jpeg, tight_jpeg);
        EXPECT_EQ(
                std::string(reinterpret_cast<const char*>(strided_vec.data()), strided_vec.size()),
                tight_jpeg);

        if (channels == 4U) {
            std::string helper_jpeg;
            ASSERT_TRUE(EncodeRgbaToJpeg(kWidth, kHeight, strided_src.data(), &helper_jpeg, 90,
                                         padded_stride));
            EXPECT_EQ(helper_jpeg, tight_jpeg);
        }
    }
}

TEST(WriteJpegTest, EncodeRgbaToJpegOverwritesExistingBufferAndRespectsQuality) {
    constexpr uint32_t kWidth = 128;
    constexpr uint32_t kHeight = 96;
    const std::vector<uint8_t> rgba = MakeDetailedPattern(kWidth, kHeight, 4U);

    std::string low_quality = "stale_prefix_that_must_be_overwritten";
    std::string high_quality = "another_stale_prefix";
    ASSERT_TRUE(EncodeRgbaToJpeg(kWidth, kHeight, rgba.data(), &low_quality, /*quality=*/20));
    ASSERT_TRUE(EncodeRgbaToJpeg(kWidth, kHeight, rgba.data(), &high_quality, /*quality=*/95));

    EXPECT_LT(low_quality.size(), high_quality.size());

    unsigned int decoded_w = 0;
    unsigned int decoded_h = 0;
    std::vector<uint8_t> decoded_rgb;
    ASSERT_TRUE(DecodeJpegToRgb(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(high_quality.data()),
                                     high_quality.size()),
            &decoded_w, &decoded_h, &decoded_rgb));
    EXPECT_EQ(decoded_w, kWidth);
    EXPECT_EQ(decoded_h, kHeight);
}

TEST(WriteJpegTest, ReusesPreallocatedOutputCapacityAndHandlesBufferGrowth) {
    constexpr unsigned int kSmallWidth = 128;
    constexpr unsigned int kSmallHeight = 96;
    constexpr size_t kReservedCapacity = static_cast<size_t>(256U) * 1024U;
    const std::vector<uint8_t> pattern = MakeDetailedPattern(kSmallWidth, kSmallHeight, 4U);

    std::string out_str;
    out_str.reserve(kReservedCapacity);
    const char* initial_str_data = out_str.data();

    std::vector<uint8_t> out_vec;
    out_vec.reserve(kReservedCapacity);
    const uint8_t* initial_vec_data = out_vec.data();

    ASSERT_TRUE(WriteJpeg(4U, kSmallWidth, kSmallHeight, pattern.data(), &out_str));
    ASSERT_TRUE(WriteJpeg(4U, kSmallWidth, kSmallHeight, pattern.data(), out_vec));

    EXPECT_GE(out_str.capacity(), kReservedCapacity);
    EXPECT_EQ(out_str.data(), initial_str_data);
    EXPECT_GE(out_vec.capacity(), kReservedCapacity);
    EXPECT_EQ(out_vec.data(), initial_vec_data);
    EXPECT_EQ(out_str, std::string(reinterpret_cast<const char*>(out_vec.data()), out_vec.size()));

    // High-entropy noise at quality=100 exceeds initial buffer estimates and 32 KiB chunks,
    // exercising dynamic destination buffer expansion for both string and vector outputs.
    constexpr unsigned int kNoisySize = 256;
    const std::vector<uint8_t> noise = MakeHighEntropyNoise(kNoisySize, kNoisySize, 4U);
    std::string noisy_str;
    std::vector<uint8_t> noisy_vec;
    ASSERT_TRUE(WriteJpeg(4U, kNoisySize, kNoisySize, noise.data(), &noisy_str, /*quality=*/100));
    ASSERT_TRUE(WriteJpeg(4U, kNoisySize, kNoisySize, noise.data(), noisy_vec, /*quality=*/100));
    EXPECT_GT(noisy_str.size(), static_cast<size_t>(64U) * 1024U);
    EXPECT_EQ(noisy_str,
              std::string(reinterpret_cast<const char*>(noisy_vec.data()), noisy_vec.size()));

    unsigned int decoded_w = 0;
    unsigned int decoded_h = 0;
    std::vector<uint8_t> decoded_rgb;
    ASSERT_TRUE(DecodeJpegToRgb(noisy_vec, &decoded_w, &decoded_h, &decoded_rgb));
    EXPECT_EQ(decoded_w, kNoisySize);
    EXPECT_EQ(decoded_h, kNoisySize);
}

TEST(WriteJpegTest, ConcurrentThreadsEncodeIndependently) {
    constexpr unsigned int kWidth = 96;
    constexpr unsigned int kHeight = 72;
    const std::vector<uint8_t> rgb = MakeDetailedPattern(kWidth, kHeight, 3U);
    const std::vector<uint8_t> rgba = MakeDetailedPattern(kWidth, kHeight, 4U);

    std::string expected_rgb_fast;
    std::string expected_rgba_slow;
    ASSERT_TRUE(WriteJpeg(3U, kWidth, kHeight, rgb.data(), &expected_rgb_fast, /*quality=*/80));
    ASSERT_TRUE(WriteJpeg(4U, kWidth, kHeight, rgba.data(), &expected_rgba_slow, /*quality=*/98));

    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    constexpr int kThreads = 4;
    constexpr int kLoopsPerThread = 12;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::string out;
            for (int i = 0; i < kLoopsPerThread; ++i) {
                if (((t + i) & 1) == 0) {
                    if (!WriteJpeg(3U, kWidth, kHeight, rgb.data(), &out, /*quality=*/80) ||
                        out != expected_rgb_fast) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    if (!WriteJpeg(4U, kWidth, kHeight, rgba.data(), &out, /*quality=*/98) ||
                        out != expected_rgba_slow) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
}

template <typename EncodeFn>
bool RecordTimedSample(EncodeFn&& encode_fn, std::vector<absl::Duration>* samples) {
    const absl::Duration start = CurrentThreadCpuTime();
    if (!encode_fn()) {
        return false;
    }
    samples->push_back(CurrentThreadCpuTime() - start);
    return true;
}

void ReportBenchmarkMedians(absl::Duration legacy_median, absl::Duration simd_mem_dest_median,
                            absl::Duration opt_median) {
    const double baseline_ms = absl::ToDoubleMilliseconds(legacy_median);
    const double simd_mem_dest_ms = absl::ToDoubleMilliseconds(simd_mem_dest_median);
    const double opt_ms = absl::ToDoubleMilliseconds(opt_median);
    const double speedup_vs_legacy = baseline_ms / opt_ms;
    const double speedup_vs_simd_mem_dest = simd_mem_dest_ms / opt_ms;

    ::testing::Test::RecordProperty("baseline_ms_per_frame", std::to_string(baseline_ms));
    ::testing::Test::RecordProperty("simd_mem_dest_ms_per_frame", std::to_string(simd_mem_dest_ms));
    ::testing::Test::RecordProperty("optimized_ms_per_frame", std::to_string(opt_ms));
    ::testing::Test::RecordProperty("speedup_x", std::to_string(speedup_vs_legacy));
    ::testing::Test::RecordProperty("speedup_vs_simd_mem_dest_x",
                                    std::to_string(speedup_vs_simd_mem_dest));
    GTEST_LOG_(INFO) << "1080x1920 RGBA WriteJpeg median: legacy_baseline=" << baseline_ms
                     << " ms/frame, simd_mem_dest=" << simd_mem_dest_ms
                     << " ms/frame, optimized=" << opt_ms << " ms/frame (" << speedup_vs_legacy
                     << "x vs legacy, " << speedup_vs_simd_mem_dest << "x vs simd_mem_dest)";
}

TEST(WriteJpegTest, FasterThanUnoptimizedBaselineOn1080pFrame) {
    constexpr unsigned int kWidth = 1080;
    constexpr unsigned int kHeight = 1920;
    constexpr unsigned int kChannels = 4;
    constexpr size_t kIterations = 11;

    const std::vector<uint8_t> frame = MakeSyntheticScreenshot(kWidth, kHeight, kChannels);
    std::string out_opt;
    auto run_legacy = [&]() {
        std::string out;
        return WriteJpegLegacyUnoptimized(kWidth, kHeight, frame.data(), &out, kDefaultJpegQuality);
    };
    auto run_simd = [&]() {
        std::string out;
        return WriteJpegSimdMemDestUnbatched(kWidth, kHeight, frame.data(), &out,
                                             kDefaultJpegQuality);
    };
    auto run_opt = [&]() {
        return WriteJpeg(kChannels, kWidth, kHeight, frame.data(), &out_opt, kDefaultJpegQuality);
    };

    ASSERT_TRUE(run_legacy() && run_simd() && run_opt());
    unsigned int decoded_w = 0;
    unsigned int decoded_h = 0;
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(DecodeJpegToRgb(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(out_opt.data()),
                                     out_opt.size()),
            &decoded_w, &decoded_h, &decoded));
    EXPECT_EQ(decoded_w, kWidth);
    EXPECT_EQ(decoded_h, kHeight);

    std::vector<absl::Duration> legacy_samples;
    std::vector<absl::Duration> simd_mem_dest_samples;
    std::vector<absl::Duration> opt_samples;
    legacy_samples.reserve(kIterations);
    simd_mem_dest_samples.reserve(kIterations);
    opt_samples.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i) {
        if ((i & 1U) == 0U) {
            ASSERT_TRUE(RecordTimedSample(run_legacy, &legacy_samples));
            ASSERT_TRUE(RecordTimedSample(run_simd, &simd_mem_dest_samples));
            ASSERT_TRUE(RecordTimedSample(run_opt, &opt_samples));
        } else {
            ASSERT_TRUE(RecordTimedSample(run_opt, &opt_samples));
            ASSERT_TRUE(RecordTimedSample(run_simd, &simd_mem_dest_samples));
            ASSERT_TRUE(RecordTimedSample(run_legacy, &legacy_samples));
        }
    }

    const absl::Duration legacy_median = MedianDuration(legacy_samples);
    const absl::Duration simd_mem_dest_median = MedianDuration(simd_mem_dest_samples);
    const absl::Duration opt_median = MedianDuration(opt_samples);
    ReportBenchmarkMedians(legacy_median, simd_mem_dest_median, opt_median);

#if !defined(ABSL_HAVE_THREAD_SANITIZER) && !defined(ABSL_HAVE_ADDRESS_SANITIZER)
    EXPECT_LT(opt_median, legacy_median);
#endif
}

}  // namespace
}  // namespace goldfish::display
