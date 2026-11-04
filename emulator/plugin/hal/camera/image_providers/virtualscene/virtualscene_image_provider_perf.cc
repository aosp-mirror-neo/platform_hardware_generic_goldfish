/* Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstddef>
#include <cstdint>
#include <memory>

#include "benchmark/benchmark.h"

#include "goldfish/imaging/query_info.h"
#include "virtualscene_image_provider.h"

namespace goldfish::camera_image_providers::virtualscene {
namespace {

struct MultiStreamProfile {
    CameraImageProviderStreamConfig streams[4];
    size_t per_stream_scaled_bytes;
};

MultiStreamProfile BuildMultiStreamProfile(uint16_t max_width, uint16_t max_height) {
    const uint16_t scaled_width = (max_width > 1280) ? 1920 : 1280;
    const uint16_t scaled_height = (max_width > 1280) ? 1080 : 720;
    const uint16_t analysis_width = 640;
    const uint16_t analysis_height = 480;

    MultiStreamProfile profile = {
        .streams =
                {
                    {
                        .size = {max_width, max_height},
                        .id = 1,
                        .format = imaging::ImageFormat::kRgba8888,
                    },
                    {
                        .size = {scaled_width, scaled_height},
                        .id = 2,
                        .format = imaging::ImageFormat::kRgba8888,
                    },
                    {
                        .size = {scaled_width, scaled_height},
                        .id = 3,
                        .format = imaging::ImageFormat::kYuV420NV12,
                    },
                    {
                        .size = {analysis_width, analysis_height},
                        .id = 4,
                        .format = imaging::ImageFormat::kYuV4203P,
                    },
                },
        .per_stream_scaled_bytes = (static_cast<size_t>(scaled_width) * scaled_height * 4u) * 2u +
                                   (static_cast<size_t>(analysis_width) * analysis_height * 4u),
    };
    return profile;
}

std::unique_ptr<VirtualsceneImageProvider> CreateColorProvider() {
    ver_initialize({});
    VirtualsceneImageProvider::CreateArgs create_args{
        .sceneConfig = VerSceneConfig(VerSceneConfig::Mode::Color, "#336699"),
        .physicalModel = nullptr,
    };
    const CameraImageProviderInfo info = {
        .createArg = &create_args,
        .isBackFacing = 1,
    };
    return std::unique_ptr<VirtualsceneImageProvider>(
            static_cast<VirtualsceneImageProvider*>(VirtualsceneImageProvider::create(info)));
}

void BmVirtualsceneMultiStreamStart(benchmark::State& state) {
    const auto max_width = static_cast<uint16_t>(state.range(0));
    const auto max_height = static_cast<uint16_t>(state.range(1));
    const MultiStreamProfile profile = BuildMultiStreamProfile(max_width, max_height);

    std::unique_ptr<VirtualsceneImageProvider> provider = CreateColorProvider();
    if (!provider) {
        state.SkipWithError("Failed to create VirtualsceneImageProvider");
        return;
    }

    size_t scaled_fb_bytes = 0;
    for (auto _ : state) {
        if (provider->start(profile.streams, std::size(profile.streams)) != 0) {
            state.SkipWithError("Failed to start VirtualsceneImageProvider");
            break;
        }
        scaled_fb_bytes = provider->getScaledFramebufferSizeBytes();
        benchmark::DoNotOptimize(scaled_fb_bytes);
        provider->stop();
    }

    state.counters["scaled_fb_bytes"] = static_cast<double>(scaled_fb_bytes);
    state.counters["per_stream_scaled_bytes"] =
            static_cast<double>(profile.per_stream_scaled_bytes);

    provider.reset();
    ver_cleanup();
}

void BmVirtualsceneMultiStreamCapture(benchmark::State& state) {
    const auto max_width = static_cast<uint16_t>(state.range(0));
    const auto max_height = static_cast<uint16_t>(state.range(1));
    const MultiStreamProfile profile = BuildMultiStreamProfile(max_width, max_height);

    std::unique_ptr<VirtualsceneImageProvider> provider = CreateColorProvider();
    if (!provider || provider->start(profile.streams, std::size(profile.streams)) != 0) {
        state.SkipWithError("Failed to initialize VirtualsceneImageProvider");
        ver_cleanup();
        return;
    }

    const CameraImageProviderStreamCaptureInfo capture_infos[] = {
        {.cfg = &profile.streams[0]},
        {.cfg = &profile.streams[1]},
        {.cfg = &profile.streams[2]},
        {.cfg = &profile.streams[3]},
    };
    const CameraImageProviderCaptureOpts opts = {};
    size_t total_sink_bytes = 0;
    auto sink = [](void* opaque, const CameraImageProviderStreamCaptureInfo*, const void* bits,
                   size_t byte_size) -> int {
        benchmark::DoNotOptimize(bits);
        *static_cast<size_t*>(opaque) += byte_size;
        return 0;
    };

    for (auto _ : state) {
        total_sink_bytes = 0;
        if (provider->capture(opts, sink, &total_sink_bytes, capture_infos,
                              std::size(capture_infos)) != 0) {
            state.SkipWithError("Failed to capture VirtualsceneImageProvider streams");
            break;
        }
        benchmark::DoNotOptimize(total_sink_bytes);
    }

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(total_sink_bytes));
    state.counters["scaled_fb_bytes"] =
            static_cast<double>(provider->getScaledFramebufferSizeBytes());
    state.counters["per_stream_scaled_bytes"] =
            static_cast<double>(profile.per_stream_scaled_bytes);

    provider->stop();
    provider.reset();
    ver_cleanup();
}

BENCHMARK(BmVirtualsceneMultiStreamStart)
        ->Args({1280, 960})
        ->Args({1920, 1440})
        ->Args({3840, 2160})
        ->Unit(benchmark::kMicrosecond);

BENCHMARK(BmVirtualsceneMultiStreamCapture)
        ->Args({1280, 960})
        ->Args({1920, 1440})
        ->Args({3840, 2160})
        ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace goldfish::camera_image_providers::virtualscene
