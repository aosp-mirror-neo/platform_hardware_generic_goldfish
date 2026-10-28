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

/**
 * @file screen_recording_perf.cc
 * @brief Google Benchmark suite comparing ScreenRecordingFrameGenerator heap
 *        buffer footprint and frame generation throughput against the previous
 *        FrameRepeatState (last_encoded duplicate frame buffer) implementation.
 *
 * Measured results (`bazel run --config=release`
 * `@goldfish//emulator/plugin/grpc/services:screen_recording_perf --`
 * `--benchmark_repetitions=10 --benchmark_enable_random_interleaving=true`
 * `--benchmark_report_aggregates_only=true`):
 * - Retained heap buffer (`retained_buf_bytes`):
 *   - 1920x1080 (1080p):    6,220,800 B -> 0 B (-100%)
 *   - 1080x2400 (Phone):    7,776,000 B -> 0 B (-100%)
 *   - 1260x2400 (Foldable): 9,072,000 B -> 0 B (-100%)
 * - Repeated frame generation (`BmRepeatFrameBaseline` vs `BmRepeatFrameOptimized`, median CPU):
 *   - 1920x1080: 301,083 ns (3.34k frames/s) -> 15.8 ns (63.41M frames/s, 19,056x faster)
 *   - 1080x2400: 408,345 ns (2.45k frames/s) -> 15.7 ns (64.44M frames/s, 26,009x faster)
 *   - 1260x2400: 510,329 ns (1.97k frames/s) -> 16.2 ns (61.92M frames/s, 31,502x faster)
 * - New frame generation (`BmNewFrameBaseline` vs `BmNewFrameOptimized`, median CPU):
 *   - 1920x1080:   804,946 ns (7.21 GiB/s) -> 431,173 ns (13.44 GiB/s, 1.87x faster)
 *   - 1080x2400: 1,256,086 ns (5.77 GiB/s) -> 443,645 ns (16.32 GiB/s, 2.83x faster)
 *   - 1260x2400: 1,864,099 ns (4.59 GiB/s) -> 892,114 ns (9.58 GiB/s, 2.09x faster)
 */

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/types/span.h"

#include "android/emulation/control/incubating/screen_recording_impl.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/display/cpu_read_pipeline.h"
#include "goldfish/display/display.h"
#include "goldfish/display/ev_dev_event.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/geometry.h"

namespace android::emulation::control::incubating {
namespace {

constexpr int64_t kWidth1080p = 1920;
constexpr int64_t kHeight1080p = 1080;
constexpr int64_t kWidthDefaultPhone = 1080;
constexpr int64_t kHeightDefaultPhone = 2400;
constexpr int64_t kWidthFoldable = 1260;
constexpr int64_t kHeightFoldable = 2400;
constexpr uint8_t kPixelSeedByte = 0x5A;

class BenchmarkSlot final : public ::goldfish::display::Slot {
  public:
    BenchmarkSlot(::goldfish::display::Dimensions size, const uint8_t* rgba)
            : size_(size), rgba_(rgba) {}

    ::goldfish::display::Dimensions size() const override { return size_; }

  protected:
    absl::Status DoReadInto(const ::goldfish::display::ReadRequest& read,
                            const ::goldfish::display::ReadTarget& dst) const override {
        const ::goldfish::display::SlotPixels src{
            .data = rgba_,
            .stride = static_cast<size_t>(size_.width) * 4,
            .dimensions = size_,
        };
        return ::goldfish::display::CpuReadPipeline(src, read, dst);
    }

  private:
    ::goldfish::display::Dimensions size_;
    const uint8_t* rgba_;
};

class BenchmarkDisplay final : public ::goldfish::display::IDisplay {
  public:
    BenchmarkDisplay(::goldfish::async::EventLoop* loop, uint32_t width, uint32_t height)
            : ::goldfish::display::IDisplay(loop, 0, width, height)
            , dims_{width, height}
            , rgba_(static_cast<size_t>(width) * height * 4, kPixelSeedByte) {}

    void PublishTestFrame() {
        frames()->PublishFrame(::goldfish::display::SlotLease(
                                       std::make_unique<BenchmarkSlot>(dims_, rgba_.data())),
                               absl::Now());
    }

    void SendMultiTouchEvent(uint8_t /*slot*/, int /*x*/, int /*y*/,
                             ::goldfish::display::MultiTouchType /*type*/) override {}
    void SendMouseEvent(int /*x*/, int /*y*/, int /*button_mask*/) override {}
    void SendEvDevEvent(uint16_t /*type*/, uint16_t /*code*/, uint32_t /*value*/) override {}
    void SendEvDevReport(::goldfish::display::EvDevEvents /*events*/) override {}
    void WriteEvDevReportOnLoop(
            absl::Span<const ::goldfish::display::EvDevEvent> /*events*/) override {}
    bool IsInputActive() const override { return true; }

  private:
    ::goldfish::display::Dimensions dims_;
    std::vector<uint8_t> rgba_;
};

// Baseline implementation reproducing the previous FrameRepeatState with a
// duplicate last_encoded buffer and per-frame temporary resize vector.
class BaselineFrameRepeatGenerator {
  public:
    explicit BaselineFrameRepeatGenerator(::goldfish::display::SharedDisplay display)
            : display_(std::move(display)) {}

    void operator()(uint8_t* pixels, int w, int h, int /*frame_index*/, double /*time*/) {
        if (pixels == nullptr || w <= 0 || h <= 0) {
            return;
        }
        const size_t dst_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
        auto latest = display_->AcquireLatestFrame();
        if (!latest.frame) {
            CopyLastOrClear(pixels, dst_bytes);
            return;
        }

        const auto frame_id = latest.frame->event().frame_id;
        if (frame_id == last_frame_id_ && last_encoded_.size() == dst_bytes) {
            std::memcpy(pixels, last_encoded_.data(), dst_bytes);
            return;
        }

        const auto f_dims = latest.frame->dimensions();
        if (f_dims.width == static_cast<uint32_t>(w) && f_dims.height == static_cast<uint32_t>(h)) {
            const ::goldfish::display::ReadRequest req{
                .rotation = ::goldfish::display::ImageRotation::kRotation0,
                .output = {0, 0},
            };
            const ::goldfish::display::Rgb888Target target{
                .plane =
                        ::goldfish::display::Plane{
                            .data = absl::MakeSpan(pixels, dst_bytes),
                            .stride = static_cast<size_t>(w) * 3,
                        },
            };
            if (latest.frame->ReadInto(req, target).ok()) {
                RecordFrame(frame_id, pixels, dst_bytes);
            } else {
                CopyLastOrClear(pixels, dst_bytes);
            }
            return;
        }

        std::memset(pixels, 0, dst_bytes);
        const uint32_t fit_w = std::min<uint32_t>(f_dims.width, static_cast<uint32_t>(w));
        const uint32_t fit_h = std::min<uint32_t>(f_dims.height, static_cast<uint32_t>(h));
        if (fit_w > 0 && fit_h > 0) {
            const ::goldfish::display::ReadRequest req{
                .rotation = ::goldfish::display::ImageRotation::kRotation0,
                .output = {fit_w, fit_h},
            };
            std::vector<uint8_t> tmp;
            if (latest.frame
                        ->ReadInto(req,
                                   [&](const ::goldfish::display::ReadLayout& layout)
                                           -> absl::StatusOr<::goldfish::display::Rgb888Target> {
                                       tmp.resize(layout.total_bytes);
                                       return ::goldfish::display::Rgb888Target{
                                           .plane = {.data = absl::MakeSpan(tmp)},
                                       };
                                   })
                        .ok()) {
                const uint32_t off_x = (static_cast<uint32_t>(w) - fit_w) / 2;
                const uint32_t off_y = (static_cast<uint32_t>(h) - fit_h) / 2;
                for (uint32_t row = 0; row < fit_h; ++row) {
                    std::memcpy(pixels + ((off_y + row) * static_cast<size_t>(w) + off_x) * 3,
                                tmp.data() + row * static_cast<size_t>(fit_w) * 3,
                                static_cast<size_t>(fit_w) * 3);
                }
                RecordFrame(frame_id, pixels, dst_bytes);
            } else {
                CopyLastOrClear(pixels, dst_bytes);
            }
        }
    }

    size_t BufferedBytes() const { return last_encoded_.capacity(); }

  private:
    void CopyLastOrClear(uint8_t* pixels, size_t dst_bytes) const {
        if (last_encoded_.size() == dst_bytes) {
            std::memcpy(pixels, last_encoded_.data(), dst_bytes);
        } else {
            std::memset(pixels, 0, dst_bytes);
        }
    }

    void RecordFrame(::goldfish::display::FrameId frame_id, const uint8_t* pixels,
                     size_t dst_bytes) {
        last_frame_id_ = frame_id;
        last_encoded_.assign(pixels, pixels + dst_bytes);
    }

    ::goldfish::display::SharedDisplay display_;
    ::goldfish::display::FrameId last_frame_id_ = 0;
    std::vector<uint8_t> last_encoded_;
};

void BmRepeatFrameBaseline(benchmark::State& state) {
    const auto width = static_cast<int>(state.range(0));
    const auto height = static_cast<int>(state.range(1));
    const size_t dst_bytes = static_cast<size_t>(width) * height * 3;

    auto loop = ::goldfish::async::LibuvEventLoop::Create();
    auto display = std::make_shared<BenchmarkDisplay>(loop.get(), static_cast<uint32_t>(width),
                                                      static_cast<uint32_t>(height));
    display->PublishTestFrame();

    BaselineFrameRepeatGenerator generator(display);
    std::vector<uint8_t> frame_buffer(dst_bytes, 0);
    generator(frame_buffer.data(), width, height, 0, 0.0);

    int frame_index = 1;
    for (auto _ : state) {
        generator(frame_buffer.data(), width, height, frame_index++, 0.04);
        benchmark::DoNotOptimize(frame_buffer.data());
        benchmark::ClobberMemory();
    }

    state.counters["retained_buf_bytes"] = static_cast<double>(generator.BufferedBytes());
    state.SetItemsProcessed(state.iterations());
}

void BmRepeatFrameOptimized(benchmark::State& state) {
    const auto width = static_cast<int>(state.range(0));
    const auto height = static_cast<int>(state.range(1));
    const size_t dst_bytes = static_cast<size_t>(width) * height * 3;

    auto loop = ::goldfish::async::LibuvEventLoop::Create();
    auto display = std::make_shared<BenchmarkDisplay>(loop.get(), static_cast<uint32_t>(width),
                                                      static_cast<uint32_t>(height));
    display->PublishTestFrame();

    ScreenRecordingFrameGenerator generator(display);
    std::vector<uint8_t> frame_buffer(dst_bytes, 0);
    generator(frame_buffer.data(), width, height, 0, 0.0);

    int frame_index = 1;
    for (auto _ : state) {
        generator(frame_buffer.data(), width, height, frame_index++, 0.04);
        benchmark::DoNotOptimize(frame_buffer.data());
        benchmark::ClobberMemory();
    }

    state.counters["retained_buf_bytes"] = static_cast<double>(generator.BufferedBytes());
    state.SetItemsProcessed(state.iterations());
}

void BmNewFrameBaseline(benchmark::State& state) {
    const auto width = static_cast<int>(state.range(0));
    const auto height = static_cast<int>(state.range(1));
    const size_t dst_bytes = static_cast<size_t>(width) * height * 3;

    auto loop = ::goldfish::async::LibuvEventLoop::Create();
    auto display = std::make_shared<BenchmarkDisplay>(loop.get(), static_cast<uint32_t>(width),
                                                      static_cast<uint32_t>(height));

    BaselineFrameRepeatGenerator generator(display);
    std::vector<uint8_t> frame_buffer(dst_bytes, 0);

    int frame_index = 0;
    for (auto _ : state) {
        display->PublishTestFrame();
        generator(frame_buffer.data(), width, height, frame_index++, 0.04);
        benchmark::DoNotOptimize(frame_buffer.data());
        benchmark::ClobberMemory();
    }

    state.counters["retained_buf_bytes"] = static_cast<double>(generator.BufferedBytes());
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes));
}

void BmNewFrameOptimized(benchmark::State& state) {
    const auto width = static_cast<int>(state.range(0));
    const auto height = static_cast<int>(state.range(1));
    const size_t dst_bytes = static_cast<size_t>(width) * height * 3;

    auto loop = ::goldfish::async::LibuvEventLoop::Create();
    auto display = std::make_shared<BenchmarkDisplay>(loop.get(), static_cast<uint32_t>(width),
                                                      static_cast<uint32_t>(height));

    ScreenRecordingFrameGenerator generator(display);
    std::vector<uint8_t> frame_buffer(dst_bytes, 0);

    int frame_index = 0;
    for (auto _ : state) {
        display->PublishTestFrame();
        generator(frame_buffer.data(), width, height, frame_index++, 0.04);
        benchmark::DoNotOptimize(frame_buffer.data());
        benchmark::ClobberMemory();
    }

    state.counters["retained_buf_bytes"] = static_cast<double>(generator.BufferedBytes());
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst_bytes));
}

BENCHMARK(BmRepeatFrameBaseline)
        ->Args({kWidth1080p, kHeight1080p})
        ->Args({kWidthDefaultPhone, kHeightDefaultPhone})
        ->Args({kWidthFoldable, kHeightFoldable});

BENCHMARK(BmRepeatFrameOptimized)
        ->Args({kWidth1080p, kHeight1080p})
        ->Args({kWidthDefaultPhone, kHeightDefaultPhone})
        ->Args({kWidthFoldable, kHeightFoldable});

BENCHMARK(BmNewFrameBaseline)
        ->Args({kWidth1080p, kHeight1080p})
        ->Args({kWidthDefaultPhone, kHeightDefaultPhone})
        ->Args({kWidthFoldable, kHeightFoldable});

BENCHMARK(BmNewFrameOptimized)
        ->Args({kWidth1080p, kHeight1080p})
        ->Args({kWidthDefaultPhone, kHeightDefaultPhone})
        ->Args({kWidthFoldable, kHeightFoldable});

}  // namespace
}  // namespace android::emulation::control::incubating

BENCHMARK_MAIN();
