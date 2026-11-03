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

#include "goldfish/display/write_png.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "absl/time/time.h"

#include "image_writer_test_utils.h"
#include "png.h"

namespace goldfish::display {
namespace {

using test_utils::CurrentThreadCpuTime;
using test_utils::MakeSyntheticScreenshot;

struct PngReadState {
    std::span<const uint8_t> data;
    size_t offset = 0;
};

bool DecodePng(std::span<const uint8_t> png_bytes, unsigned int* out_width,
               unsigned int* out_height, unsigned int* out_channels,
               std::vector<uint8_t>* out_pixels) {
    if (png_bytes.size() < 8 || png_sig_cmp(png_bytes.data(), 0, 8) != 0) {
        return false;
    }
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return false;
    }
    PngReadState state{.data = png_bytes, .offset = 0};
    png_set_read_fn(png_ptr, &state, [](png_structp p, png_bytep out, png_size_t len) {
        auto* s = reinterpret_cast<PngReadState*>(png_get_io_ptr(p));
        if (s->offset + len > s->data.size()) {
            png_error(p, "unexpected EOF");
        }
        std::memcpy(out, s->data.data() + s->offset, len);
        s->offset += len;
    });

    png_read_info(png_ptr, info_ptr);
    const unsigned int width = png_get_image_width(png_ptr, info_ptr);
    const unsigned int height = png_get_image_height(png_ptr, info_ptr);
    const unsigned int channels = png_get_channels(png_ptr, info_ptr);
    *out_width = width;
    *out_height = height;
    *out_channels = channels;

    const size_t stride = static_cast<size_t>(width) * channels;
    out_pixels->resize(stride * height);
    for (unsigned int y = 0; y < height; ++y) {
        png_read_row(png_ptr, out_pixels->data() + (static_cast<size_t>(y) * stride), nullptr);
    }
    png_read_end(png_ptr, nullptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    return true;
}

bool WritePngWithDefaultAdaptiveFilters(unsigned int n_channels, unsigned int width,
                                        unsigned int height, const void* pixels,
                                        std::vector<uint8_t>& png_data) {
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!p) {
        return false;
    }
    png_infop pi = png_create_info_struct(p);
    if (!pi) {
        png_destroy_write_struct(&p, nullptr);
        return false;
    }
    png_set_write_fn(
            p, &png_data,
            [](png_structp png_ptr, png_bytep data, png_size_t length) {
                auto* vec = reinterpret_cast<std::vector<uint8_t>*>(png_get_io_ptr(png_ptr));
                vec->insert(vec->end(), &data[0], &data[length]);
            },
            [](png_structp /*png_ptr*/) {});
    if (setjmp(png_jmpbuf(p))) {
        png_destroy_write_struct(&p, &pi);
        return false;
    }
    constexpr int kZBestSpeed = 1;
    png_set_compression_level(p, kZBestSpeed);
    png_set_IHDR(p, pi, width, height, 8,
                 n_channels == 3 ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(p, pi);
    const auto* upixels = reinterpret_cast<const uint8_t*>(pixels);
    const size_t stride = static_cast<size_t>(n_channels) * width;
    for (unsigned int i = 0; i < height; ++i) {
        png_write_row(p, upixels + (static_cast<size_t>(i) * stride));
    }
    png_write_end(p, nullptr);
    png_destroy_write_struct(&p, &pi);
    return true;
}

TEST(WritePngTest, RejectsInvalidArguments) {
    const std::vector<uint8_t> pixel = {1, 2, 3, 4};
    std::vector<uint8_t> out;

    EXPECT_FALSE(write_png(0, 1, 1, pixel.data(), out));
    EXPECT_FALSE(write_png(2, 1, 1, pixel.data(), out));
    EXPECT_FALSE(write_png(5, 1, 1, pixel.data(), out));
    EXPECT_FALSE(write_png(4, 0, 1, pixel.data(), out));
    EXPECT_FALSE(write_png(4, 1, 0, pixel.data(), out));
    EXPECT_FALSE(write_png(4, 1, 1, nullptr, out));
    EXPECT_TRUE(out.empty());
}

TEST(WritePngTest, RoundTripsRgbAndRgbaLosslessly) {
    for (unsigned int channels : {3U, 4U}) {
        constexpr unsigned int kWidth = 67;
        constexpr unsigned int kHeight = 43;
        const std::vector<uint8_t> src = MakeSyntheticScreenshot(kWidth, kHeight, channels);
        std::vector<uint8_t> png_bytes;

        ASSERT_TRUE(write_png(channels, kWidth, kHeight, src.data(), png_bytes));
        ASSERT_GT(png_bytes.size(), 8U);

        unsigned int decoded_w = 0;
        unsigned int decoded_h = 0;
        unsigned int decoded_channels = 0;
        std::vector<uint8_t> decoded;
        ASSERT_TRUE(DecodePng(png_bytes, &decoded_w, &decoded_h, &decoded_channels, &decoded));
        EXPECT_EQ(decoded_w, kWidth);
        EXPECT_EQ(decoded_h, kHeight);
        EXPECT_EQ(decoded_channels, channels);
        EXPECT_EQ(decoded, src);
    }
}

TEST(WritePngTest, FasterThanAdaptiveRowFilteringOn1080pFrame) {
    constexpr unsigned int kWidth = 1080;
    constexpr unsigned int kHeight = 1920;
    constexpr unsigned int kChannels = 4;
    constexpr int kIterations = 5;

    const std::vector<uint8_t> frame = MakeSyntheticScreenshot(kWidth, kHeight, kChannels);

    // Warm up both paths.
    {
        std::vector<uint8_t> warmup_baseline;
        std::vector<uint8_t> warmup_opt;
        ASSERT_TRUE(WritePngWithDefaultAdaptiveFilters(kChannels, kWidth, kHeight, frame.data(),
                                                       warmup_baseline));
        ASSERT_TRUE(write_png(kChannels, kWidth, kHeight, frame.data(), warmup_opt));

        unsigned int w = 0;
        unsigned int h = 0;
        unsigned int c = 0;
        std::vector<uint8_t> decoded;
        ASSERT_TRUE(DecodePng(warmup_opt, &w, &h, &c, &decoded));
        EXPECT_EQ(decoded, frame);
    }

    absl::Duration baseline_total = absl::ZeroDuration();
    absl::Duration opt_total = absl::ZeroDuration();
    for (int i = 0; i < kIterations; ++i) {
        std::vector<uint8_t> out_baseline;
        const absl::Duration b_start = CurrentThreadCpuTime();
        ASSERT_TRUE(WritePngWithDefaultAdaptiveFilters(kChannels, kWidth, kHeight, frame.data(),
                                                       out_baseline));
        baseline_total += CurrentThreadCpuTime() - b_start;

        std::vector<uint8_t> out_opt;
        const absl::Duration o_start = CurrentThreadCpuTime();
        ASSERT_TRUE(write_png(kChannels, kWidth, kHeight, frame.data(), out_opt));
        opt_total += CurrentThreadCpuTime() - o_start;
    }

    const double baseline_ms = absl::ToDoubleMilliseconds(baseline_total) / kIterations;
    const double opt_ms = absl::ToDoubleMilliseconds(opt_total) / kIterations;
    const double speedup = baseline_ms / opt_ms;

    RecordProperty("baseline_ms_per_frame", std::to_string(baseline_ms));
    RecordProperty("optimized_ms_per_frame", std::to_string(opt_ms));
    RecordProperty("speedup_x", std::to_string(speedup));
    GTEST_LOG_(INFO) << "1080x1920 RGBA write_png: baseline=" << baseline_ms
                     << " ms/frame, optimized=" << opt_ms << " ms/frame (" << speedup
                     << "x speedup)";

    EXPECT_LT(opt_total, baseline_total);
}

}  // namespace
}  // namespace goldfish::display
