// Copyright 2025 The Android Open Source Project
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
#include "goldfish/display/cpu_read_pipeline.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/base/config.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/barrier.h"

#include "goldfish/display/test/read_pipeline_conformance.h"

#ifdef ABSL_HAVE_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

namespace goldfish::display {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::goldfish::display::conformance::DescribeWorstDifference;
using ::goldfish::display::conformance::Destination;
using ::goldfish::display::conformance::kNv12Tolerance;
using ::goldfish::display::conformance::kScaledTolerance;
using ::goldfish::display::conformance::MakeCheckerboardFrame;
using ::goldfish::display::conformance::MakeNoiseFrame;
using ::goldfish::display::conformance::MakeSmoothFrame;
using ::goldfish::display::conformance::MaxAbsDiff;
using ::goldfish::display::conformance::ReadOutput;
using ::goldfish::display::conformance::ReferenceRead;
using ::goldfish::display::conformance::TestFrame;
using ::goldfish::display::internal::GetScratchPoolStatsForTesting;
using ::goldfish::display::internal::ScratchPoolStats;
using ::testing::AllOf;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::Field;

constexpr ImageRotation kRotations[] = {ImageRotation::kRotation0, ImageRotation::kRotation90,
                                        ImageRotation::kRotation180, ImageRotation::kRotation270};
constexpr ReadFormat kFormats[] = {ReadFormat::kRgba8888, ReadFormat::kRgb888, ReadFormat::kNv12};

bool IsQuarterTurn(ImageRotation rotation) {
    return rotation == ImageRotation::kRotation90 || rotation == ImageRotation::kRotation270;
}

std::string Describe(const ReadRequest& read, ReadFormat format) {
    const Rect crop = read.crop.value_or(Rect{});
    return absl::StrFormat("crop %dx%d@(%d,%d) rotation %d output %dx%d format %d", crop.width,
                           crop.height, crop.x, crop.y, static_cast<int>(read.rotation),
                           read.output.width, read.output.height, static_cast<int>(format));
}

class PixelsSlot final : public Slot {
  public:
    explicit PixelsSlot(SlotPixels pixels) : pixels_(pixels) {}
    Dimensions size() const override { return pixels_.dimensions; }
    const std::optional<ReadRequest>& LastRead() const { return last_read_; }

  protected:
    absl::Status DoReadInto(const ReadRequest& read, const ReadTarget& dst) const override {
        last_read_ = read;
        return CpuReadPipeline(pixels_, read, dst);
    }

  private:
    const SlotPixels pixels_;
    mutable std::optional<ReadRequest> last_read_;
};

// Resolves `request` against `frame` via Slot::ReadInto, runs CpuReadPipeline
// into a padded destination, and checks the padding.
std::optional<std::pair<ReadRequest, ReadOutput>> ReadWithPipeline(const TestFrame& frame,
                                                                   const ReadRequest& request,
                                                                   ReadFormat format) {
    const ReadLayout layout = Slot::LayoutFor(frame.size(), request, format);
    if (layout.total_bytes == 0) return std::nullopt;
    Destination dst(layout.dimensions, format, /*row_padding=*/5);
    PixelsSlot slot(frame.pixels());
    EXPECT_THAT(slot.ReadInto(request, dst.target()), IsOk());
    if (!slot.LastRead().has_value()) return std::nullopt;
    EXPECT_TRUE(dst.PaddingIntact()) << Describe(*slot.LastRead(), format);
    return std::make_pair(*slot.LastRead(), dst.Pixels());
}

// Crops of a 37x23 frame that touch each edge, a corner, and nothing.
std::vector<std::optional<Rect>> EdgeCrops() {
    return {std::nullopt,       Rect{0, 4, 9, 11},  Rect{6, 0, 13, 7}, Rect{28, 5, 9, 13},
            Rect{7, 16, 15, 7}, Rect{30, 18, 7, 5}, Rect{3, 3, 30, 17}};
}

constexpr Dimensions kOddFrame{37, 23};

TEST(CpuReadPipelineConformanceTest, CropAndRotationAreBitExactForRgb) {
    const TestFrame frame = MakeNoiseFrame(kOddFrame, /*seed=*/1, /*row_padding=*/12);
    for (const std::optional<Rect>& crop : EdgeCrops()) {
        for (ImageRotation rotation : kRotations) {
            for (ReadFormat format : {ReadFormat::kRgba8888, ReadFormat::kRgb888}) {
                auto result = ReadWithPipeline(frame, {.crop = crop, .rotation = rotation}, format);
                ASSERT_TRUE(result.has_value());
                const auto& [read, output] = *result;
                EXPECT_EQ(MaxAbsDiff(output, ReferenceRead(frame.pixels(), read, format)), 0)
                        << Describe(read, format);
            }
        }
    }
}

TEST(CpuReadPipelineConformanceTest, Nv12MatchesReferenceWithinTolerance) {
    for (const TestFrame& frame :
         {MakeNoiseFrame(kOddFrame, /*seed=*/2), MakeSmoothFrame(kOddFrame, /*row_padding=*/4)}) {
        for (const std::optional<Rect>& crop : EdgeCrops()) {
            for (ImageRotation rotation : kRotations) {
                auto result = ReadWithPipeline(frame, {.crop = crop, .rotation = rotation},
                                               ReadFormat::kNv12);
                ASSERT_TRUE(result.has_value());
                const auto& [read, output] = *result;
                EXPECT_LE(
                        MaxAbsDiff(output, ReferenceRead(frame.pixels(), read, ReadFormat::kNv12)),
                        kNv12Tolerance)
                        << Describe(read, ReadFormat::kNv12);
            }
        }
    }
}

TEST(CpuReadPipelineConformanceTest, DownscalingMatchesReferenceWithinTolerance) {
    constexpr Dimensions kFrame{150, 97};
    const std::pair<std::string_view, TestFrame> frames[] = {
        {"smooth", MakeSmoothFrame(kFrame, /*row_padding=*/8)},
        {"noise", MakeNoiseFrame(kFrame, /*seed=*/10, /*row_padding=*/8)},
        {"checkerboard 1", MakeCheckerboardFrame(kFrame, /*cell=*/1, /*row_padding=*/8)},
        {"checkerboard 3", MakeCheckerboardFrame(kFrame, /*cell=*/3)},
    };
    const std::vector<std::optional<Rect>> crops = {std::nullopt, Rect{0, 0, 71, 97},
                                                    Rect{79, 40, 71, 57}, Rect{10, 3, 131, 90}};
    for (const auto& [content, frame] : frames) {
        for (const std::optional<Rect>& crop : crops) {
            const Rect c = crop.value_or(Rect{0, 0, kFrame.width, kFrame.height});
            for (ImageRotation rotation : kRotations) {
                const bool quarter = rotation == ImageRotation::kRotation90 ||
                                     rotation == ImageRotation::kRotation270;
                const Dimensions rotated =
                        quarter ? Dimensions{c.height, c.width} : Dimensions{c.width, c.height};
                const Dimensions outputs[] = {
                    {rotated.width / 2, rotated.height / 2},          // Exactly 2x.
                    {rotated.width * 2 / 3, rotated.height * 3 / 5},  // Fractional.
                    {rotated.width / 5 + 1, rotated.height / 7 + 1},  // Large ratio.
                    {rotated.width - 1, rotated.height},              // One axis only.
                };
                for (Dimensions output : outputs) {
                    for (ReadFormat format : kFormats) {
                        auto result = ReadWithPipeline(
                                frame, {.crop = crop, .rotation = rotation, .output = output},
                                format);
                        ASSERT_TRUE(result.has_value());
                        const auto& [read, pixels] = *result;
                        const ReadOutput expected = ReferenceRead(frame.pixels(), read, format);
                        EXPECT_LE(MaxAbsDiff(pixels, expected), kScaledTolerance)
                                << content << ": " << Describe(read, format) << ", "
                                << DescribeWorstDifference(pixels, expected);
                    }
                }
            }
        }
    }
}

TEST(CpuReadPipelineConformanceTest, TinyAndDegenerateFrames) {
    for (Dimensions size : {Dimensions{1, 1}, Dimensions{2, 2}, Dimensions{5, 1}, Dimensions{1, 7},
                            Dimensions{3, 3}}) {
        const TestFrame frame = MakeNoiseFrame(size, /*seed=*/3);
        for (ImageRotation rotation : kRotations) {
            for (ReadFormat format : kFormats) {
                auto result = ReadWithPipeline(frame, {.rotation = rotation}, format);
                if (!result.has_value()) {
                    // Only NV12 rejects frames narrower or shorter than 2.
                    EXPECT_EQ(format, ReadFormat::kNv12);
                    EXPECT_TRUE(size.width < 2 || size.height < 2);
                    continue;
                }
                const auto& [read, output] = *result;
                const int tolerance = format == ReadFormat::kNv12 ? kNv12Tolerance : 0;
                EXPECT_LE(MaxAbsDiff(output, ReferenceRead(frame.pixels(), read, format)),
                          tolerance)
                        << Describe(read, format);
            }
        }
    }
}

// Frames one or two pixels thin, downscaled along their length, across it
// where possible, and to a single pixel, in every rotation.
TEST(CpuReadPipelineConformanceTest, ScalesDegenerateStrips) {
    for (Dimensions size :
         {Dimensions{1, 37}, Dimensions{37, 1}, Dimensions{2, 29}, Dimensions{29, 2}}) {
        const std::pair<std::string_view, TestFrame> frames[] = {
            {"smooth", MakeSmoothFrame(size, /*row_padding=*/4)},
            {"noise", MakeNoiseFrame(size, /*seed=*/12, /*row_padding=*/4)},
        };
        for (const auto& [content, frame] : frames) {
            for (ImageRotation rotation : kRotations) {
                const Dimensions rotated =
                        IsQuarterTurn(rotation) ? Dimensions{size.height, size.width} : size;
                const Dimensions outputs[] = {
                    {std::max(rotated.width / 3, 1U), std::max(rotated.height / 3, 1U)},
                    {std::max(rotated.width - 1, 1U), rotated.height},
                    {rotated.width, std::max(rotated.height - 1, 1U)},
                    {1, 1},
                };
                for (Dimensions output : outputs) {
                    if (output == rotated) continue;  // Not scaled.
                    for (ReadFormat format : {ReadFormat::kRgba8888, ReadFormat::kRgb888}) {
                        auto result = ReadWithPipeline(
                                frame, {.rotation = rotation, .output = output}, format);
                        ASSERT_TRUE(result.has_value());
                        const auto& [read, pixels] = *result;
                        const ReadOutput expected = ReferenceRead(frame.pixels(), read, format);
                        EXPECT_LE(MaxAbsDiff(pixels, expected), kScaledTolerance)
                                << content << " " << size.width << "x" << size.height << ": "
                                << Describe(read, format) << ", "
                                << DescribeWorstDifference(pixels, expected);
                    }
                }
            }
        }
    }
}

// A 3x2 frame of distinct pixels A..F. Each rotation has a hand-written
// expectation, so a reversed rotation fails even if the reference were wrong.
class RotationMarkerTest : public ::testing::Test {
  protected:
    RotationMarkerTest() : frame_({3, 2}, /*row_padding=*/0) {
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 3; ++x) {
                frame_.SetPixel(x, y, {Id(x, y), 0, 0, 0});
            }
        }
    }

    // A=1 B=2 C=3 on the top row, D=4 E=5 F=6 below.
    static uint8_t Id(uint32_t x, uint32_t y) { return static_cast<uint8_t>(y * 3 + x + 1); }

    std::vector<uint8_t> ReadIds(ImageRotation rotation) {
        auto result = ReadWithPipeline(frame_, {.rotation = rotation}, ReadFormat::kRgb888);
        std::vector<uint8_t> ids;
        if (!result.has_value()) return ids;
        const std::vector<uint8_t>& rgb = result->second.planes[0];
        for (size_t i = 0; i < rgb.size(); i += 3) ids.push_back(rgb[i]);
        return ids;
    }

    TestFrame frame_;
};

TEST_F(RotationMarkerTest, NoRotation) {
    EXPECT_THAT(ReadIds(ImageRotation::kRotation0), ElementsAreArray({1, 2, 3, 4, 5, 6}));
}

TEST_F(RotationMarkerTest, CounterClockwise90MovesTopRightToTopLeft) {
    // C F / B E / A D
    EXPECT_THAT(ReadIds(ImageRotation::kRotation90), ElementsAreArray({3, 6, 2, 5, 1, 4}));
}

TEST_F(RotationMarkerTest, Rotation180) {
    EXPECT_THAT(ReadIds(ImageRotation::kRotation180), ElementsAreArray({6, 5, 4, 3, 2, 1}));
}

TEST_F(RotationMarkerTest, CounterClockwise270MovesBottomLeftToTopLeft) {
    // D A / E B / F C
    EXPECT_THAT(ReadIds(ImageRotation::kRotation270), ElementsAreArray({4, 1, 5, 2, 6, 3}));
}

TEST(CpuReadPipelineTest, CropSelectsExactPixels) {
    const TestFrame frame = MakeNoiseFrame(kOddFrame, /*seed=*/4, /*row_padding=*/3);
    auto result = ReadWithPipeline(frame, {.crop = Rect{30, 20, 7, 3}}, ReadFormat::kRgba8888);
    ASSERT_TRUE(result.has_value());
    const std::vector<uint8_t>& rgba = result->second.planes[0];
    ASSERT_EQ(rgba.size(), size_t{7} * 3 * 4);
    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x < 7; ++x) {
            const std::array<uint8_t, 4> src = frame.Pixel(30 + x, 20 + y);
            const uint8_t* dst = &rgba[(static_cast<size_t>(y) * 7 + x) * 4];
            EXPECT_EQ(dst[0], src[0]);
            EXPECT_EQ(dst[1], src[1]);
            EXPECT_EQ(dst[2], src[2]);
        }
    }
}

TEST(CpuReadPipelineTest, RgbaAlphaIsAlwaysOpaque) {
    const TestFrame frame = MakeNoiseFrame(kOddFrame, /*seed=*/5);
    for (ImageRotation rotation : kRotations) {
        for (Dimensions output : {Dimensions{0, 0}, Dimensions{11, 9}}) {
            auto result = ReadWithPipeline(frame, {.rotation = rotation, .output = output},
                                           ReadFormat::kRgba8888);
            ASSERT_TRUE(result.has_value());
            const std::vector<uint8_t>& rgba = result->second.planes[0];
            for (size_t i = 3; i < rgba.size(); i += 4) {
                ASSERT_EQ(rgba[i], 0xFF) << "byte " << i;
            }
        }
    }
}

TEST(CpuReadPipelineTest, RgbaVectorTailWidthsAndUnalignedStridesAreBitExact) {
    constexpr uint32_t kWidths[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                    13, 14, 15, 16, 17, 31, 32, 33, 63, 64, 65};
    for (uint32_t width : kWidths) {
        for (uint32_t height : {1U, 3U}) {
            for (size_t src_padding : {size_t{0}, size_t{3}}) {
                const TestFrame frame =
                        MakeNoiseFrame({width, height}, /*seed=*/width * 7 + height, src_padding);
                for (size_t dst_padding : {size_t{0}, size_t{5}}) {
                    for (ImageRotation rotation :
                         {ImageRotation::kRotation0, ImageRotation::kRotation180}) {
                        const ReadRequest read{
                            .crop = Rect{0, 0, width, height},
                            .rotation = rotation,
                            .output = {width, height},
                        };
                        Destination dst({width, height}, ReadFormat::kRgba8888, dst_padding);
                        ASSERT_THAT(CpuReadPipeline(frame.pixels(), read, dst.target()), IsOk())
                                << Describe(read, ReadFormat::kRgba8888);
                        EXPECT_TRUE(dst.PaddingIntact()) << Describe(read, ReadFormat::kRgba8888);
                        const ReadOutput actual = dst.Pixels();
                        EXPECT_EQ(MaxAbsDiff(actual, ReferenceRead(frame.pixels(), read,
                                                                   ReadFormat::kRgba8888)),
                                  0)
                                << Describe(read, ReadFormat::kRgba8888);
                    }
                }
            }
        }
    }
}

TEST(CpuReadPipelineTest, Rgb888KeepsMemoryOrder) {
    TestFrame frame({1, 1}, /*row_padding=*/0);
    frame.SetPixel(0, 0, {10, 20, 30, 40});
    auto result = ReadWithPipeline(frame, {}, ReadFormat::kRgb888);
    ASSERT_TRUE(result.has_value());
    EXPECT_THAT(result->second.planes[0], ElementsAreArray({10, 20, 30}));
}

TEST(CpuReadPipelineTest, PaddingIsUntouchedForEveryPath) {
    const TestFrame frame = MakeSmoothFrame({40, 30});
    for (ImageRotation rotation : {ImageRotation::kRotation0, ImageRotation::kRotation90}) {
        for (Dimensions output : {Dimensions{0, 0}, Dimensions{20, 14}}) {
            for (ReadFormat format : kFormats) {
                // ReadWithPipeline checks the padding.
                EXPECT_TRUE(
                        ReadWithPipeline(frame, {.rotation = rotation, .output = output}, format)
                                .has_value());
            }
        }
    }
}

// Tight planes (stride 0), NV12's half-height UV plane included, in every
// rotation, with and without scaling.
TEST(CpuReadPipelineTest, TightStrideMatchesPaddedOutput) {
    const TestFrame frame = MakeNoiseFrame(kOddFrame, /*seed=*/11, /*row_padding=*/4);
    for (ImageRotation rotation : kRotations) {
        for (Dimensions output : {Dimensions{0, 0}, Dimensions{16, 12}, Dimensions{14, 10}}) {
            for (ReadFormat format : kFormats) {
                const ReadRequest request{.rotation = rotation, .output = output};
                auto padded = ReadWithPipeline(frame, request, format);
                ASSERT_TRUE(padded.has_value());
                const auto& [read, expected] = *padded;
                Destination tight(read.output, format, /*row_padding=*/0);
                ASSERT_THAT(CpuReadPipeline(frame.pixels(), read, tight.target()), IsOk())
                        << Describe(read, format);
                EXPECT_EQ(MaxAbsDiff(tight.Pixels(), expected), 0) << Describe(read, format);
            }
        }
    }
}

TEST(FillBlankTest, WritesTightPlanes) {
    const std::unique_ptr<const Slot> blank = Slot::Blank({6, 4});
    Destination rgb({6, 4}, ReadFormat::kRgb888, /*row_padding=*/0);
    ASSERT_THAT(blank->ReadInto({}, rgb.target()), IsOk());
    EXPECT_THAT(rgb.Pixels().planes[0], Each(0x00));
    Destination nv12({6, 4}, ReadFormat::kNv12, /*row_padding=*/0);
    ASSERT_THAT(blank->ReadInto({}, nv12.target()), IsOk());
    EXPECT_THAT(nv12.Pixels().planes[0], Each(16));
    EXPECT_THAT(nv12.Pixels().planes[1], Each(128));
}

TEST(CpuReadPipelineTest, ReadsAreRepeatable) {
    const TestFrame frame = MakeNoiseFrame(kOddFrame, /*seed=*/6);
    const ReadRequest request{.rotation = ImageRotation::kRotation90, .output = {16, 30}};
    auto first = ReadWithPipeline(frame, request, ReadFormat::kNv12);
    auto second = ReadWithPipeline(frame, request, ReadFormat::kNv12);
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(MaxAbsDiff(first->second, second->second), 0);
}

// Reads of different sizes, formats and paths race each other and a thread
// that leases random-sized scratch buffers, all released at once. Two holders
// sharing a scratch buffer, or any state kept between reads, would change an
// output from its single-threaded value or the scratch thread's fill.
TEST(CpuReadPipelineTest, ConcurrentReadsAgree) {
    constexpr int kIterations = 200;
    const TestFrame frame = MakeNoiseFrame({64, 48}, /*seed=*/7, /*row_padding=*/8);
    struct Job {
        ReadRequest request;
        ReadFormat format;
    };
    const std::vector<Job> jobs = {
        {{.output = {40, 30}}, ReadFormat::kRgba8888},  // Scaled straight into dst.
        {{.rotation = ImageRotation::kRotation90}, ReadFormat::kRgba8888},  // Rotated into dst.
        {{.crop = Rect{3, 5, 50, 40}}, ReadFormat::kRgba8888},              // Copied, no scratch.
        {{.rotation = ImageRotation::kRotation90, .output = {30, 50}},
         ReadFormat::kNv12},  // Two scratch buffers.
        {{.crop = Rect{3, 5, 50, 40}, .output = {48, 36}}, ReadFormat::kNv12},
        {{.rotation = ImageRotation::kRotation270, .output = {30, 50}}, ReadFormat::kRgb888},
    };
    std::vector<std::pair<ReadRequest, ReadOutput>> expected;
    for (const Job& job : jobs) {
        auto result = ReadWithPipeline(frame, job.request, job.format);
        ASSERT_TRUE(result.has_value());
        expected.push_back(*std::move(result));
    }

    absl::Barrier start(static_cast<int>(jobs.size()) + 1);
    std::vector<int> worst(jobs.size(), 0);
    std::vector<int> padding_damaged(jobs.size(), 0);
    std::atomic<int> scratch_corrupted{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < jobs.size(); ++t) {
        threads.emplace_back([&, t] {
            const auto& [read, output] = expected[t];
            start.Block();
            for (int i = 0; i < kIterations; ++i) {
                Destination dst(read.output, jobs[t].format, /*row_padding=*/3);
                if (!CpuReadPipeline(frame.pixels(), read, dst.target()).ok()) {
                    worst[t] = 256;
                    return;
                }
                worst[t] = std::max(worst[t], MaxAbsDiff(dst.Pixels(), output));
                if (!dst.PaddingIntact()) ++padding_damaged[t];
            }
        });
    }
    threads.emplace_back([&] {
        std::mt19937 random(7);
        std::uniform_int_distribution<size_t> sizes(0, size_t{64} * 48 * 4);
        start.Block();
        for (int i = 0; i < kIterations; ++i) {
            const size_t bytes = sizes(random);
            ScratchBuffer buffer = AcquireScratchBuffer(bytes);
            const auto fill = static_cast<uint8_t>(i);
            std::memset(buffer.data(), fill, bytes);
            std::this_thread::yield();
            if (!std::all_of(buffer.data(), buffer.data() + bytes,
                             [fill](uint8_t b) { return b == fill; })) {
                ++scratch_corrupted;
            }
        }
    });
    for (std::thread& thread : threads) thread.join();

    for (size_t t = 0; t < jobs.size(); ++t) {
        EXPECT_EQ(worst[t], 0) << Describe(expected[t].first, jobs[t].format);
        EXPECT_EQ(padding_damaged[t], 0) << Describe(expected[t].first, jobs[t].format);
    }
    EXPECT_EQ(scratch_corrupted.load(), 0);
    EXPECT_EQ(GetScratchPoolStatsForTesting().on_loan, 0U);
}

TEST(CpuReadPipelineTest, RejectsReadOutsideTheSlot) {
    const TestFrame frame = MakeNoiseFrame({8, 8}, /*seed=*/8);
    Destination dst({4, 4}, ReadFormat::kRgba8888);
    const ReadRequest outside{.crop = Rect{6, 0, 4, 4}, .output = {4, 4}};
    EXPECT_THAT(CpuReadPipeline(frame.pixels(), outside, dst.target()),
                StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(CpuReadPipelineTest, RejectsBadSourceView) {
    const TestFrame frame = MakeNoiseFrame({8, 8}, /*seed=*/9);
    Destination dst({8, 8}, ReadFormat::kRgba8888);
    const ReadRequest whole{.crop = Rect{0, 0, 8, 8}, .output = {8, 8}};

    SlotPixels null_data = frame.pixels();
    null_data.data = nullptr;
    EXPECT_THAT(CpuReadPipeline(null_data, whole, dst.target()),
                StatusIs(absl::StatusCode::kInvalidArgument));

    SlotPixels short_stride = frame.pixels();
    short_stride.stride = 8 * 4 - 1;
    EXPECT_THAT(CpuReadPipeline(short_stride, whole, dst.target()),
                StatusIs(absl::StatusCode::kInvalidArgument));
}

// A direct caller's request is checked like a resolved one: an empty,
// upscaled or odd NV12 output, or a stride libyuv can't take, is rejected
// before anything is written or leased.
TEST(CpuReadPipelineTest, RejectsZeroOutputAndHugeStride) {
    const TestFrame frame = MakeNoiseFrame({8, 6}, /*seed=*/13);
    const struct {
        ReadRequest read;
        ReadFormat format;
    } rejected[] = {
        {{.output = {0, 0}}, ReadFormat::kRgba8888},
        {{.output = {0, 4}}, ReadFormat::kRgb888},
        {{.output = {4, 0}}, ReadFormat::kNv12},
        {{.output = {9, 6}}, ReadFormat::kRgba8888},
        {{.output = {8, 7}}, ReadFormat::kRgb888},
        {{.crop = Rect{0, 0, 4, 4}, .output = {5, 4}}, ReadFormat::kRgba8888},
        // Fits the unrotated 8x6 frame, not the rotated 6x8 one.
        {{.rotation = ImageRotation::kRotation90, .output = {8, 6}}, ReadFormat::kRgba8888},
        {{.output = {5, 4}}, ReadFormat::kNv12},
        {{.output = {4, 3}}, ReadFormat::kNv12},
    };
    for (const auto& [read, format] : rejected) {
        Destination dst({9, 8}, format);
        EXPECT_THAT(CpuReadPipeline(frame.pixels(), read, dst.target()),
                    StatusIs(absl::StatusCode::kInvalidArgument))
                << Describe(read, format);
        for (const std::vector<uint8_t>& plane : dst.Pixels().planes) {
            EXPECT_THAT(plane, Each(Destination::kSentinel)) << Describe(read, format);
        }
    }

    SlotPixels huge_stride = frame.pixels();
    huge_stride.stride = size_t{std::numeric_limits<int>::max()} + 1;
    Destination dst({8, 6}, ReadFormat::kRgba8888);
    EXPECT_THAT(CpuReadPipeline(huge_stride, {.output = {8, 6}}, dst.target()),
                StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_EQ(GetScratchPoolStatsForTesting().on_loan, 0U);
}

::testing::Matcher<ScratchPoolStats> PoolIs(size_t on_loan, size_t free_count, size_t free_bytes) {
    return AllOf(Field("on_loan", &ScratchPoolStats::on_loan, on_loan),
                 Field("free_count", &ScratchPoolStats::free_count, free_count),
                 Field("free_bytes", &ScratchPoolStats::free_bytes, free_bytes));
}

// The pool is process-wide and idle between tests. When a one-byte lease
// returns, the pool drops every buffer larger than one byte and keeps at most
// one; a second one-byte lease then leaves exactly one one-byte buffer.
void ResetScratchPool() {
    ASSERT_EQ(GetScratchPoolStatsForTesting().on_loan, 0U);
    for (int i = 0; i < 2; ++i) (void)AcquireScratchBuffer(1);
    ASSERT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 1, 1));
}

TEST(ScratchPoolTest, ZeroByteLeaseDoesNotWedgeTrimming) {
    ASSERT_NO_FATAL_FAILURE(ResetScratchPool());
    {
        ScratchBuffer one = AcquireScratchBuffer(1);   // Takes the pooled buffer,
        ScratchBuffer zero = AcquireScratchBuffer(0);  // so this one is new.
        EXPECT_NE(zero.data(), nullptr);
        EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(2, 0, 0));
    }
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 2, 2));
    // Idle again, so trimming still runs: to the peak of one...
    (void)AcquireScratchBuffer(4096);
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 1, 4096));
    // ...and to no buffer larger than the largest request.
    (void)AcquireScratchBuffer(1);
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 0, 0));
}

TEST(ScratchPoolTest, SteadyStateReusesBuffer) {
    // Leaves a one-byte buffer, too small for the leases below.
    ASSERT_NO_FATAL_FAILURE(ResetScratchPool());
    const uint8_t* first = nullptr;
    {
        ScratchBuffer buffer = AcquireScratchBuffer(1000);
        first = buffer.data();
    }
    // The buffer that fits is kept, not the one that is merely smaller.
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 1, 1000));
    for (int i = 0; i < 3; ++i) {
        ScratchBuffer buffer = AcquireScratchBuffer(1000);
        EXPECT_EQ(buffer.data(), first);
    }
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 1, 1000));
}

TEST(ScratchPoolTest, RetainsAtMostFourAfterBurst) {
    ASSERT_NO_FATAL_FAILURE(ResetScratchPool());
    {
        std::vector<ScratchBuffer> burst;
        for (size_t i = 0; i < 6; ++i) burst.push_back(AcquireScratchBuffer(100 + i));
        EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(6, 1, 1));
    }
    // The four largest.
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 4, 102 + 103 + 104 + 105));
}

TEST(ScratchPoolTest, TrimsToPeakAndLargestRequestWhenIdle) {
    ASSERT_NO_FATAL_FAILURE(ResetScratchPool());
    {
        ScratchBuffer a = AcquireScratchBuffer(200);
        ScratchBuffer b = AcquireScratchBuffer(200);
        ScratchBuffer c = AcquireScratchBuffer(200);
    }
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 3, 600));
    (void)AcquireScratchBuffer(200);
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 1, 200));
    (void)AcquireScratchBuffer(50);  // Leases the 200-byte buffer.
    EXPECT_THAT(GetScratchPoolStatsForTesting(), PoolIs(0, 0, 0));
}

#ifdef ABSL_HAVE_ADDRESS_SANITIZER
// A reused buffer is larger than the lease; an overrun into the rest must
// still be caught.
TEST(ScratchPoolTest, UnusedTailIsPoisonedUnderAsan) {
    ASSERT_NO_FATAL_FAILURE(ResetScratchPool());
    // Keeps the pool busy, so it doesn't trim between the leases below.
    ScratchBuffer guard = AcquireScratchBuffer(1);
    (void)AcquireScratchBuffer(4096);
    const uint8_t* reused = nullptr;
    {
        ScratchBuffer buffer = AcquireScratchBuffer(100);
        reused = buffer.data();
        EXPECT_FALSE(__asan_address_is_poisoned(buffer.data() + 99));
        EXPECT_TRUE(__asan_address_is_poisoned(buffer.data() + 100));
        EXPECT_TRUE(__asan_address_is_poisoned(buffer.data() + 4095));
    }
    ScratchBuffer buffer = AcquireScratchBuffer(4096);
    ASSERT_EQ(buffer.data(), reused);
    EXPECT_EQ(__asan_region_is_poisoned(buffer.data(), 4096), nullptr);
}
#endif

TEST(CpuReadPipelineTest, RejectsSourceTooTallToRotate) {
    // Rotated, each source column becomes a row of height * 4 scratch bytes,
    // which libyuv can't address as an int. Spans this large are never
    // allocated or touched; only their sizes are inspected.
    constexpr uint32_t kHeight = std::numeric_limits<int>::max() / 4 + 1;
    std::vector<uint8_t> one_pixel(4);
    const SlotPixels tall{.data = one_pixel.data(), .stride = 4, .dimensions = {1, kHeight}};
    const ReadRequest read{.crop = Rect{0, 0, 1, kHeight},
                           .rotation = ImageRotation::kRotation90,
                           .output = {kHeight, 1}};
    const Rgb888Target dst{Plane{absl::Span<uint8_t>(one_pixel.data(), size_t{kHeight} * 3)}};
    EXPECT_THAT(CpuReadPipeline(tall, read, dst), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FillBlankTest, WritesBlackInEveryFormat) {
    const std::unique_ptr<const Slot> blank = Slot::Blank({6, 4});

    Destination rgba({6, 4}, ReadFormat::kRgba8888, /*row_padding=*/3);
    ASSERT_THAT(blank->ReadInto({}, rgba.target()), IsOk());
    const ReadOutput rgba_output = rgba.Pixels();
    const std::vector<uint8_t>& rgba_bytes = rgba_output.planes[0];
    for (size_t i = 0; i < rgba_bytes.size(); ++i) {
        EXPECT_EQ(rgba_bytes[i], i % 4 == 3 ? 0xFF : 0x00) << "byte " << i;
    }
    EXPECT_TRUE(rgba.PaddingIntact());

    Destination rgb({6, 4}, ReadFormat::kRgb888, /*row_padding=*/3);
    ASSERT_THAT(blank->ReadInto({}, rgb.target()), IsOk());
    EXPECT_THAT(rgb.Pixels().planes[0], Each(0x00));
    EXPECT_TRUE(rgb.PaddingIntact());

    Destination nv12({6, 4}, ReadFormat::kNv12, /*row_padding=*/3);
    ASSERT_THAT(blank->ReadInto({}, nv12.target()), IsOk());
    const ReadOutput yuv = nv12.Pixels();
    EXPECT_THAT(yuv.planes[0], Each(16));
    EXPECT_THAT(yuv.planes[1], Each(128));
    EXPECT_EQ(yuv.planes[1].size(), 6U * 2);
    EXPECT_TRUE(nv12.PaddingIntact());
}

TEST(FillBlankTest, RejectsTooSmallPlane) {
    const std::unique_ptr<const Slot> blank = Slot::Blank({6, 4});
    std::vector<uint8_t> bytes(6 * 4 * 4 - 1);
    EXPECT_THAT(blank->ReadInto({}, Rgba8888Target{Plane{absl::MakeSpan(bytes)}}),
                StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace goldfish::display
