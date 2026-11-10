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

#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_codec.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "android/status/status_matcher_macros.h"
#include "render-utils/stream.h"

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

void PutBe32(std::vector<uint8_t>* v, size_t off, uint32_t x) {
    ASSERT_GE(v->size(), off + 4) << "PutBe32 at offset " << off << " past the end of a "
                                  << v->size() << "-byte buffer";
    (*v)[off] = static_cast<uint8_t>(x >> 24);
    (*v)[off + 1] = static_cast<uint8_t>(x >> 16);
    (*v)[off + 2] = static_cast<uint8_t>(x >> 8);
    (*v)[off + 3] = static_cast<uint8_t>(x);
}

// Reference blob: one ASG entry (handle 7, payload 0xA5600001), one pipe
// (id 3, payload 0xCAFE1234), a 2x2 frame and non-default counters.
struct Fixture {
    std::vector<uint8_t> meta;
    static constexpr size_t kAsgCountOffset = 8;
    static constexpr size_t kPipeCountOffset = 8 + 4 + 8;
    static constexpr size_t kFrameWidthOffset = kPipeCountOffset + 4 + 8;
    static constexpr size_t kFrameSizeOffset = kFrameWidthOffset + 8;

    Fixture() {
        GpuSnapshotFrame frame;
        frame.width = 2;
        frame.height = 2;
        frame.bgra.assign(16, 0x44);
        GpuSnapshotCounters counters{.next_asg_handle = 8,
                                     .next_pipe_id = 4,
                                     .next_puid = 100,
                                     .next_timeline_id = 200};
        auto encoded = EncodeGpuSnapshot(
                {7}, [](uint32_t, gfxstream::Stream* s) { s->putBe32(0xA5600001); }, {3},
                [](uint32_t, gfxstream::Stream* s) { s->putBe32(0xCAFE1234); }, frame, counters);
        EXPECT_OK(encoded.status());
        meta = *encoded;
    }
};

// Decodes `bytes`, recording {id, payload} pairs for every entry callback.
absl::StatusOr<GpuSnapshotTail> DecodeCounting(const std::vector<uint8_t>& bytes,
                                               std::vector<uint32_t>* asg_handles,
                                               std::vector<uint32_t>* pipe_ids) {
    return DecodeGpuSnapshot(
            bytes.data(), bytes.size(),
            [&](uint32_t handle, gfxstream::Stream* s) {
                const uint32_t payload = s->getBe32();
                if (asg_handles != nullptr) {
                    asg_handles->push_back(handle);
                    asg_handles->push_back(payload);
                }
                return absl::OkStatus();
            },
            [&](uint32_t id, gfxstream::Stream* s) {
                const uint32_t payload = s->getBe32();
                if (pipe_ids != nullptr) {
                    pipe_ids->push_back(id);
                    pipe_ids->push_back(payload);
                }
                return absl::OkStatus();
            });
}

TEST(GpuSnapshotCodecTest, RoundTrip) {
    Fixture f;
    // magic, version, asg_count, {handle, marker}, pipe_count, {id, tag}, w, h,
    // size, 16 frame bytes, next_asg_handle, next_pipe_id, next_puid, next_timeline_id.
    ASSERT_EQ(f.meta.size(), 4U + 4U + 4U + 8U + 4U + 8U + 12U + 16U + 4U + 4U + 8U + 8U);
    EXPECT_EQ(f.meta.size(), MinGpuSnapshotBytes() + 8U + 8U + 16U);

    std::vector<uint32_t> asgs;
    std::vector<uint32_t> pipes;
    ASSERT_OK_AND_ASSIGN(const GpuSnapshotTail tail, DecodeCounting(f.meta, &asgs, &pipes));
    EXPECT_THAT(asgs, ElementsAre(7, 0xA5600001U));
    EXPECT_THAT(pipes, ElementsAre(3, 0xCAFE1234U));
    EXPECT_EQ(tail.frame.width, 2U);
    EXPECT_EQ(tail.frame.height, 2U);
    EXPECT_EQ(tail.frame.bgra, std::vector<uint8_t>(16, 0x44));
    EXPECT_EQ(tail.counters.next_asg_handle, 8U);
    EXPECT_EQ(tail.counters.next_pipe_id, 4U);
    EXPECT_EQ(tail.counters.next_puid, 100U);
    EXPECT_EQ(tail.counters.next_timeline_id, 200U);
}

TEST(GpuSnapshotCodecTest, EmptyFrameAndNoEntries) {
    ASSERT_OK_AND_ASSIGN(
            const std::vector<uint8_t> meta,
            EncodeGpuSnapshot({}, [](uint32_t, gfxstream::Stream*) {}, {},
                              [](uint32_t, gfxstream::Stream*) {}, GpuSnapshotFrame{},
                              GpuSnapshotCounters{}));
    EXPECT_EQ(meta.size(), MinGpuSnapshotBytes());
    std::vector<uint32_t> asgs;
    std::vector<uint32_t> pipes;
    ASSERT_OK_AND_ASSIGN(const GpuSnapshotTail tail, DecodeCounting(meta, &asgs, &pipes));
    EXPECT_TRUE(asgs.empty());
    EXPECT_TRUE(pipes.empty());
    EXPECT_TRUE(tail.frame.bgra.empty());
    EXPECT_EQ(tail.counters.next_asg_handle, 1U);
}

TEST(GpuSnapshotCodecTest, EveryTruncatedPrefixIsRejectedWithoutCreatingEntries) {
    Fixture f;
    for (size_t n = 0; n < f.meta.size(); ++n) {
        const std::vector<uint8_t> prefix(f.meta.begin(), f.meta.begin() + n);
        std::vector<uint32_t> asgs;
        std::vector<uint32_t> pipes;
        const auto result = DecodeCounting(prefix, &asgs, &pipes);
        EXPECT_THAT(result.status(), StatusIs(absl::StatusCode::kDataLoss))
                << "prefix of " << n << " bytes";
        // An entry callback may legitimately have run before the truncation
        // point, but never for an entry whose id was itself truncated.
        EXPECT_LE(asgs.size(), 2U) << n;
        EXPECT_LE(pipes.size(), 2U) << n;
    }
}

TEST(GpuSnapshotCodecTest, RejectsBadMagicAndUnsupportedVersion) {
    Fixture f;
    std::vector<uint8_t> bad = f.meta;
    PutBe32(&bad, 0, 0x12345678);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("magic")));

    bad = f.meta;
    PutBe32(&bad, 4, 99);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("version 99")));
}

TEST(GpuSnapshotCodecTest, RejectsCountsAboveTheBoundBeforeInvokingCallbacks) {
    Fixture f;
    std::vector<uint8_t> bad = f.meta;
    PutBe32(&bad, Fixture::kAsgCountOffset, 0xffffffff);
    std::vector<uint32_t> asgs;
    std::vector<uint32_t> pipes;
    EXPECT_THAT(DecodeCounting(bad, &asgs, &pipes).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("asg_count")));
    EXPECT_TRUE(asgs.empty());

    bad = f.meta;
    PutBe32(&bad, Fixture::kPipeCountOffset, 0xffffffff);
    asgs.clear();
    EXPECT_THAT(DecodeCounting(bad, &asgs, &pipes).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("pipe_count")));
    EXPECT_TRUE(pipes.empty());
}

TEST(GpuSnapshotCodecTest, RejectsInconsistentOrOversizedFrame) {
    Fixture f;
    std::vector<uint8_t> bad = f.meta;
    PutBe32(&bad, Fixture::kFrameSizeOffset, 0xffffffff);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("does not match 2x2x4")));

    // Consistent with w*h*4 (1 GiB) but far past the end of the blob: must be
    // rejected before any allocation.
    bad = f.meta;
    PutBe32(&bad, Fixture::kFrameWidthOffset, 0x4000);
    PutBe32(&bad, Fixture::kFrameWidthOffset + 4, 0x4000);
    PutBe32(&bad, Fixture::kFrameSizeOffset, 0x40000000);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("bytes left")));

    // w*h*4 wraps modulo 2^64 to exactly the stored size: must be caught by
    // the dimension cap, not by the (wrapped) size comparison.
    bad = f.meta;
    PutBe32(&bad, Fixture::kFrameWidthOffset, 2147437308U);
    PutBe32(&bad, Fixture::kFrameWidthOffset + 4, 2147529989U);
    PutBe32(&bad, Fixture::kFrameSizeOffset, 166832U);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("out of range")));

    // One edge just over the cap with a consistent size.
    bad = f.meta;
    PutBe32(&bad, Fixture::kFrameWidthOffset, kGpuSnapshotMaxFrameDim + 1);
    PutBe32(&bad, Fixture::kFrameWidthOffset + 4, 1);
    PutBe32(&bad, Fixture::kFrameSizeOffset, (kGpuSnapshotMaxFrameDim + 1) * 4);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("out of range")));

    // Empty frame with non-zero dimensions: the encoder refuses to produce
    // it, so patch a header-only blob by hand (w at 16, h at 20, size at 24).
    ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> empty_frame,
                         EncodeGpuSnapshot({}, [](uint32_t, gfxstream::Stream*) {}, {},
                                           [](uint32_t, gfxstream::Stream*) {},
                                           GpuSnapshotFrame{}, GpuSnapshotCounters{}));
    PutBe32(&empty_frame, 16, 3);
    PutBe32(&empty_frame, 20, 5);
    EXPECT_THAT(DecodeCounting(empty_frame, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("empty frame")));
}

TEST(GpuSnapshotCodecTest, EncodeRefusesAnInconsistentFrame) {
    const auto encode = [](const GpuSnapshotFrame& frame) {
        return EncodeGpuSnapshot({}, [](uint32_t, gfxstream::Stream*) {}, {},
                                 [](uint32_t, gfxstream::Stream*) {}, frame,
                                 GpuSnapshotCounters{})
                .status();
    };
    EXPECT_THAT(encode(GpuSnapshotFrame{.width = 3, .height = 5}),
                StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("empty frame")));
    EXPECT_THAT(encode(GpuSnapshotFrame{.width = 2, .height = 2, .bgra = {1, 2, 3}}),
                StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("does not match")));
    EXPECT_THAT(encode(GpuSnapshotFrame{.width = 0, .height = 0, .bgra = {1, 2, 3, 4}}),
                StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("does not match")));
    EXPECT_THAT(encode(GpuSnapshotFrame{.width = kGpuSnapshotMaxFrameDim + 1,
                                        .height = 1,
                                        .bgra = std::vector<uint8_t>(
                                                size_t{kGpuSnapshotMaxFrameDim + 1} * 4)}),
                StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("max edge")));
}

TEST(GpuSnapshotCodecTest, FrameBytesHelperNeverWraps) {
    EXPECT_EQ(GpuSnapshotFrameBytes(0, 10), 0U);
    EXPECT_EQ(GpuSnapshotFrameBytes(10, 0), 0U);
    EXPECT_EQ(GpuSnapshotFrameBytes(2, 2), 16U);
    EXPECT_EQ(GpuSnapshotFrameBytes(kGpuSnapshotMaxFrameDim, kGpuSnapshotMaxFrameDim),
              uint64_t{kGpuSnapshotMaxFrameDim} * kGpuSnapshotMaxFrameDim * 4);
    EXPECT_EQ(GpuSnapshotFrameBytes(kGpuSnapshotMaxFrameDim + 1, 1), 0U);
    EXPECT_EQ(GpuSnapshotFrameBytes(2147437308U, 2147529989U), 0U);
    EXPECT_FALSE(IsReplayableGpuSnapshotFrame(GpuSnapshotFrame{}));
    EXPECT_TRUE(IsReplayableGpuSnapshotFrame(
            GpuSnapshotFrame{.width = 1, .height = 1, .bgra = {0, 0, 0, 0}}));
}

TEST(GpuSnapshotCodecTest, RejectsTrailingBytesAndOversizedBlob) {
    Fixture f;
    std::vector<uint8_t> bad = f.meta;
    bad.push_back(0);
    EXPECT_THAT(DecodeCounting(bad, nullptr, nullptr).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("trailing")));

    // Size alone is enough to reject; the pointer is never dereferenced.
    EXPECT_THAT(DecodeGpuSnapshot(f.meta.data(), MaxGpuSnapshotBytes() + 1,
                                  [](uint32_t, gfxstream::Stream*) { return absl::OkStatus(); },
                                  [](uint32_t, gfxstream::Stream*) { return absl::OkStatus(); })
                        .status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("exceeds")));
}

TEST(GpuSnapshotCodecTest, EntryReaderErrorAbortsDecodeAndIsReturnedVerbatim) {
    Fixture f;
    bool pipe_called = false;
    const auto result = DecodeGpuSnapshot(
            f.meta.data(), f.meta.size(),
            [](uint32_t handle, gfxstream::Stream*) {
                return absl::NotFoundError(absl::StrCat("blob ", handle, " was not restored"));
            },
            [&](uint32_t, gfxstream::Stream*) {
                pipe_called = true;
                return absl::OkStatus();
            });
    EXPECT_THAT(result.status(),
                StatusIs(absl::StatusCode::kNotFound, HasSubstr("blob 7 was not restored")));
    EXPECT_FALSE(pipe_called);
}

TEST(GpuSnapshotCodecTest, EntryReaderThatOverreadsIsReportedAsDataLoss) {
    Fixture f;
    const auto result = DecodeGpuSnapshot(
            f.meta.data(), f.meta.size(),
            [](uint32_t, gfxstream::Stream* s) {
                // Consumes the payload plus everything that follows.
                std::vector<uint8_t> sink(1024);
                s->read(sink.data(), sink.size());
                return absl::OkStatus();
            },
            [](uint32_t, gfxstream::Stream*) { return absl::OkStatus(); });
    EXPECT_THAT(result.status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("ASG consumer state")));
}

TEST(GpuSnapshotCodecTest, EncodeRefusesTooManyEntriesOrAnOversizedFrame) {
    std::vector<uint32_t> too_many(kGpuSnapshotMaxEntries + 1, 1);
    EXPECT_THAT(EncodeGpuSnapshot(too_many, [](uint32_t, gfxstream::Stream*) {}, {},
                                  [](uint32_t, gfxstream::Stream*) {}, GpuSnapshotFrame{},
                                  GpuSnapshotCounters{})
                        .status(),
                StatusIs(absl::StatusCode::kResourceExhausted));

    GpuSnapshotFrame huge;
    huge.width = 16384;
    huge.height = 4096;  // 256 MiB of BGRA: over the limit once the header is added.
    huge.bgra.resize(static_cast<size_t>(huge.width) * huge.height * 4);
    EXPECT_THAT(EncodeGpuSnapshot({}, [](uint32_t, gfxstream::Stream*) {}, {},
                                  [](uint32_t, gfxstream::Stream*) {}, huge, GpuSnapshotCounters{})
                        .status(),
                StatusIs(absl::StatusCode::kResourceExhausted, HasSubstr("exceeds")));
}

}  // namespace
}  // namespace goldfish::legacy
