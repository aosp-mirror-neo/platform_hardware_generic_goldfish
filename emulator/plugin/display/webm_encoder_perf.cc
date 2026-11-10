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
#include <filesystem>
#include <string>
#include <vector>

#include "benchmark/benchmark.h"

#include "goldfish/display/webm_encoder.h"
#include "goldfish/file/file.h"

namespace goldfish::display {
namespace {

constexpr int kDefaultFps = 24;
constexpr int kDefaultBitrate = 2000000;
constexpr int kRgbChannels = 3;
constexpr size_t kReferenceFrameBatch = 60;

std::filesystem::path GetBenchmarkOutputPath(int width, int height) {
    return std::filesystem::temp_directory_path() /
           ("webm_encoder_perf_" + std::to_string(width) + "x" + std::to_string(height) + ".webm");
}

std::vector<uint8_t> MakeBenchmarkRgbFrame(int width, int height) {
    const size_t total_bytes = static_cast<size_t>(width) * height * kRgbChannels;
    std::vector<uint8_t> frame(total_bytes);
    for (size_t i = 0; i < total_bytes; ++i) {
        frame[i] = static_cast<uint8_t>((i * 31U + 17U) & 0xFFU);
    }
    return frame;
}

void BmWebMEncoderAddFrame(benchmark::State& state) {
    const int width = static_cast<int>(state.range(0));
    const int height = static_cast<int>(state.range(1));
    const std::vector<uint8_t> rgb_frame = MakeBenchmarkRgbFrame(width, height);
    const std::filesystem::path output_path = GetBenchmarkOutputPath(width, height);
    android::base::file::rm(output_path).IgnoreError();

    WebMEncoder encoder(output_path.string(), width, height, kDefaultFps, kDefaultBitrate);
    if (!encoder.Init()) {
        state.SkipWithError("Failed to initialize WebMEncoder");
        return;
    }

    for (auto _ : state) {
        encoder.AddFrame(rgb_frame.data());
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(rgb_frame.size()));
    state.counters["frame_bytes"] = static_cast<double>(rgb_frame.size());
    state.counters["buffer_allocs"] = static_cast<double>(encoder.FrameBufferAllocations());
    state.counters["buffered_bytes"] = static_cast<double>(encoder.BufferedBytes());
    state.counters["unbounded_60f_bytes"] =
            static_cast<double>(kReferenceFrameBatch * rgb_frame.size());
    state.counters["yuv_frame_bytes"] = static_cast<double>(encoder.YuvFrameBufferBytes());
    state.counters["unscaled_yuv_bytes"] =
            static_cast<double>(static_cast<size_t>(width) * height * 3 / 2);

    encoder.Finish();
    android::base::file::rm(output_path).IgnoreError();
}

void BmWebMEncoderInitYuvFrame(benchmark::State& state) {
    const int width = static_cast<int>(state.range(0));
    const int height = static_cast<int>(state.range(1));
    const std::filesystem::path output_path = GetBenchmarkOutputPath(width, height);
    android::base::file::rm(output_path).IgnoreError();

    size_t yuv_frame_bytes = 0;
    for (auto _ : state) {
        WebMEncoder encoder(output_path.string(), width, height, kDefaultFps, kDefaultBitrate);
        if (!encoder.Init()) {
            state.SkipWithError("Failed to initialize WebMEncoder");
            return;
        }
        yuv_frame_bytes = encoder.YuvFrameBufferBytes();
        benchmark::DoNotOptimize(yuv_frame_bytes);
        encoder.Finish();
    }

    state.counters["yuv_frame_bytes"] = static_cast<double>(yuv_frame_bytes);
    state.counters["unscaled_yuv_bytes"] =
            static_cast<double>(static_cast<size_t>(width) * height * 3 / 2);

    android::base::file::rm(output_path).IgnoreError();
}

BENCHMARK(BmWebMEncoderAddFrame)
        ->Args({1080, 1920})
        ->Args({1080, 2400})
        ->Args({1260, 2400})
        ->Args({2160, 3840})
        ->Unit(benchmark::kMicrosecond);

BENCHMARK(BmWebMEncoderInitYuvFrame)
        ->Args({1080, 1920})
        ->Args({1080, 2400})
        ->Args({1260, 2400})
        ->Args({2160, 3840})
        ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace goldfish::display
