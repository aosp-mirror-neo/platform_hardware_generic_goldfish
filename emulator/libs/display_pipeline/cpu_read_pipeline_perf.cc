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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"

#include "goldfish/display/cpu_read_pipeline.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/geometry.h"
#include "libyuv/rotate_argb.h"
#include "libyuv/scale_argb.h"

namespace goldfish::display {
namespace {

constexpr uint32_t kFrameWidth = 1080;
constexpr uint32_t kFrameHeight = 2424;
constexpr uint32_t kHdFrameWidth = 720;
constexpr uint32_t kHdFrameHeight = 1600;
constexpr size_t kBytesPerPixel = 4;
constexpr size_t kTightRowBytes = size_t{kFrameWidth} * kBytesPerPixel;
// Matches CpuSlotStorage's 64-byte row alignment (4352 bytes for width 1080).
constexpr size_t kPaddedRowBytes = (kTightRowBytes + 63U) & ~size_t{63};
constexpr size_t kHdTightRowBytes = size_t{kHdFrameWidth} * kBytesPerPixel;
constexpr size_t kHdPaddedRowBytes = (kHdTightRowBytes + 63U) & ~size_t{63};

std::vector<uint8_t> MakeSourceBuffer(size_t stride, uint32_t height) {
    std::vector<uint8_t> buffer(stride * height);
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>((i * 131U + 17U) & 0xFFU);
    }
    return buffer;
}

ABSL_ATTRIBUTE_NOINLINE void LegacyCopyOpaque(const uint8_t* src, size_t src_stride, uint8_t* dst,
                                              size_t dst_stride, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* __restrict in = src + size_t{y} * src_stride;
        uint8_t* __restrict out = dst + size_t{y} * dst_stride;
        for (size_t x = 0; x < width; ++x) {
            uint32_t px = 0;
            std::memcpy(&px, in + x * kBytesPerPixel, sizeof(px));
            px |= 0xFF000000U;
            std::memcpy(out + x * kBytesPerPixel, &px, sizeof(px));
        }
    }
}

ABSL_ATTRIBUTE_NOINLINE void LegacyMakeOpaque(uint8_t* data, size_t stride, uint32_t width,
                                              uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* __restrict row = data + size_t{y} * stride;
        for (size_t x = 0; x < width; ++x) {
            uint32_t px = 0;
            std::memcpy(&px, row + x * kBytesPerPixel, sizeof(px));
            px |= 0xFF000000U;
            std::memcpy(row + x * kBytesPerPixel, &px, sizeof(px));
        }
    }
}

void BmReadRgba720pPaddedStrideBaseline(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kHdPaddedRowBytes, kHdFrameHeight);
    std::vector<uint8_t> dst_bytes(kHdTightRowBytes * kHdFrameHeight);

    for (auto _ : state) {
        LegacyCopyOpaque(src_bytes.data(), kHdPaddedRowBytes, dst_bytes.data(), kHdTightRowBytes,
                         kHdFrameWidth, kHdFrameHeight);
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgba720pPaddedStrideBaseline);

void BmReadRgba720pPaddedStride(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kHdPaddedRowBytes, kHdFrameHeight);
    std::vector<uint8_t> dst_bytes(kHdTightRowBytes * kHdFrameHeight);
    const SlotPixels src{
        .data = src_bytes.data(),
        .stride = kHdPaddedRowBytes,
        .dimensions = {kHdFrameWidth, kHdFrameHeight},
    };
    const ReadRequest read{.output = {kHdFrameWidth, kHdFrameHeight}};
    const ReadTarget dst = Rgba8888Target{Plane{absl::MakeSpan(dst_bytes), 0}};

    for (auto _ : state) {
        CHECK_OK(CpuReadPipeline(src, read, dst));
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgba720pPaddedStride);

void BmReadRgbaPaddedStrideBaseline(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kPaddedRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kTightRowBytes * kFrameHeight);

    for (auto _ : state) {
        LegacyCopyOpaque(src_bytes.data(), kPaddedRowBytes, dst_bytes.data(), kTightRowBytes,
                         kFrameWidth, kFrameHeight);
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaPaddedStrideBaseline);

void BmReadRgbaPaddedStride(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kPaddedRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kTightRowBytes * kFrameHeight);
    const SlotPixels src{
        .data = src_bytes.data(),
        .stride = kPaddedRowBytes,
        .dimensions = {kFrameWidth, kFrameHeight},
    };
    const ReadRequest read{.output = {kFrameWidth, kFrameHeight}};
    const ReadTarget dst = Rgba8888Target{Plane{absl::MakeSpan(dst_bytes), 0}};

    for (auto _ : state) {
        CHECK_OK(CpuReadPipeline(src, read, dst));
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaPaddedStride);

void BmReadRgbaTightStrideBaseline(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kTightRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kTightRowBytes * kFrameHeight);

    for (auto _ : state) {
        LegacyCopyOpaque(src_bytes.data(), kTightRowBytes, dst_bytes.data(), kTightRowBytes,
                         kFrameWidth, kFrameHeight);
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaTightStrideBaseline);

void BmReadRgbaTightStride(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kTightRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kTightRowBytes * kFrameHeight);
    const SlotPixels src{
        .data = src_bytes.data(),
        .stride = kTightRowBytes,
        .dimensions = {kFrameWidth, kFrameHeight},
    };
    const ReadRequest read{.output = {kFrameWidth, kFrameHeight}};
    const ReadTarget dst = Rgba8888Target{Plane{absl::MakeSpan(dst_bytes), 0}};

    for (auto _ : state) {
        CHECK_OK(CpuReadPipeline(src, read, dst));
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaTightStride);

void BmReadRgbaRotated180Baseline(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kPaddedRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kTightRowBytes * kFrameHeight);

    for (auto _ : state) {
        CHECK_EQ(libyuv::ARGBRotate(src_bytes.data(), static_cast<int>(kPaddedRowBytes),
                                    dst_bytes.data(), static_cast<int>(kTightRowBytes),
                                    static_cast<int>(kFrameWidth), static_cast<int>(kFrameHeight),
                                    libyuv::kRotate180),
                 0);
        LegacyMakeOpaque(dst_bytes.data(), kTightRowBytes, kFrameWidth, kFrameHeight);
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaRotated180Baseline);

void BmReadRgbaRotated180(benchmark::State& state) {
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kPaddedRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kTightRowBytes * kFrameHeight);
    const SlotPixels src{
        .data = src_bytes.data(),
        .stride = kPaddedRowBytes,
        .dimensions = {kFrameWidth, kFrameHeight},
    };
    const ReadRequest read{
        .rotation = ImageRotation::kRotation180,
        .output = {kFrameWidth, kFrameHeight},
    };
    const ReadTarget dst = Rgba8888Target{Plane{absl::MakeSpan(dst_bytes), 0}};

    for (auto _ : state) {
        CHECK_OK(CpuReadPipeline(src, read, dst));
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaRotated180);

void BmReadRgbaHalfScaleBaseline(benchmark::State& state) {
    constexpr uint32_t kScaledWidth = kFrameWidth / 2;
    constexpr uint32_t kScaledHeight = kFrameHeight / 2;
    constexpr size_t kScaledRowBytes = size_t{kScaledWidth} * kBytesPerPixel;
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kPaddedRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kScaledRowBytes * kScaledHeight);

    for (auto _ : state) {
        CHECK_EQ(libyuv::ARGBScale(src_bytes.data(), static_cast<int>(kPaddedRowBytes),
                                   static_cast<int>(kFrameWidth), static_cast<int>(kFrameHeight),
                                   dst_bytes.data(), static_cast<int>(kScaledRowBytes),
                                   static_cast<int>(kScaledWidth), static_cast<int>(kScaledHeight),
                                   libyuv::kFilterBilinear),
                 0);
        LegacyMakeOpaque(dst_bytes.data(), kScaledRowBytes, kScaledWidth, kScaledHeight);
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaHalfScaleBaseline);

void BmReadRgbaHalfScale(benchmark::State& state) {
    constexpr uint32_t kScaledWidth = kFrameWidth / 2;
    constexpr uint32_t kScaledHeight = kFrameHeight / 2;
    constexpr size_t kScaledRowBytes = size_t{kScaledWidth} * kBytesPerPixel;
    const std::vector<uint8_t> src_bytes = MakeSourceBuffer(kPaddedRowBytes, kFrameHeight);
    std::vector<uint8_t> dst_bytes(kScaledRowBytes * kScaledHeight);
    const SlotPixels src{
        .data = src_bytes.data(),
        .stride = kPaddedRowBytes,
        .dimensions = {kFrameWidth, kFrameHeight},
    };
    const ReadRequest read{.output = {kScaledWidth, kScaledHeight}};
    const ReadTarget dst = Rgba8888Target{Plane{absl::MakeSpan(dst_bytes), 0}};

    for (auto _ : state) {
        CHECK_OK(CpuReadPipeline(src, read, dst));
        benchmark::DoNotOptimize(dst_bytes.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes.size()));
}
BENCHMARK(BmReadRgbaHalfScale);

}  // namespace
}  // namespace goldfish::display

BENCHMARK_MAIN();
