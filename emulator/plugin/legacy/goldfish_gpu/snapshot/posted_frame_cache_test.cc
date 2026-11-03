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

#include "goldfish/legacy/goldfish_gpu/snapshot/posted_frame_cache.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace goldfish::legacy {
namespace {

struct Captured {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> bgra;
    int calls = 0;
};

PostedFrameCache::Source CaptureInto(Captured* c) {
    PostedFrameCache::Source src;
    src.on_post = [c](uint32_t w, uint32_t h, const uint8_t* px) {
        c->w = w;
        c->h = h;
        c->bgra.assign(px, px + static_cast<size_t>(w) * h * 4);
        ++c->calls;
    };
    return src;
}

TEST(PostedFrameCacheTest, CopiesPixelsForcesAlphaAndRemembersTheFrame) {
    PostedFrameCache cache;
    Captured c;
    std::vector<uint8_t> px(16, 0x10);  // alpha 0x10 everywhere
    cache.OnPost(CaptureInto(&c), 0, 2, 2, px.data());
    ASSERT_EQ(c.calls, 1);
    EXPECT_EQ(c.w, 2U);
    EXPECT_EQ(c.h, 2U);
    for (size_t i = 0; i < c.bgra.size(); ++i) {
        EXPECT_EQ(c.bgra[i], (i % 4 == 3) ? 0xff : 0x10) << i;
    }
    const GpuSnapshotFrame last = cache.Last();
    EXPECT_EQ(last.width, 2U);
    EXPECT_EQ(last.bgra, c.bgra);
}

TEST(PostedFrameCacheTest, UsesReadbackWhenProvided) {
    PostedFrameCache cache;
    Captured c;
    PostedFrameCache::Source src = CaptureInto(&c);
    src.read_pixels = [](void* pixels, uint32_t bytes, uint32_t) {
        auto* out = static_cast<uint8_t*>(pixels);
        for (uint32_t i = 0; i < bytes; ++i) {
            out[i] = static_cast<uint8_t>(i & 0x7f);
        }
    };
    cache.OnPost(src, 0, 1, 2, /*pixels=*/nullptr);
    ASSERT_EQ(c.calls, 1);
    EXPECT_EQ(c.bgra, (std::vector<uint8_t>{0, 1, 2, 0xff, 4, 5, 6, 0xff}));
}

TEST(PostedFrameCacheTest, BeforeReadbackRunsOnlyForReadbackPostsWithPixels) {
    PostedFrameCache cache;
    Captured c;
    int flushes = 0;
    PostedFrameCache::Source src = CaptureInto(&c);
    src.before_readback = [&] { ++flushes; };
    std::vector<uint8_t> px(16, 1);
    cache.OnPost(src, 0, 2, 2, px.data());  // copy path: no readback, no flush
    EXPECT_EQ(flushes, 0);
    src.read_pixels = [](void* pixels, uint32_t bytes, uint32_t) {
        std::memset(pixels, 0, bytes);
    };
    cache.OnPost(src, 0, 2, 2, nullptr);  // readback without a GL pixel pointer
    EXPECT_EQ(flushes, 0);
    cache.OnPost(src, 0, 2, 2, px.data());
    EXPECT_EQ(flushes, 1);
    EXPECT_EQ(c.calls, 3);
}

TEST(PostedFrameCacheTest, IgnoresOtherDisplaysBadDimensionsAndMissingPixels) {
    PostedFrameCache cache;
    Captured c;
    std::vector<uint8_t> px(16, 1);
    cache.OnPost(CaptureInto(&c), /*display_id=*/1, 2, 2, px.data());
    cache.OnPost(CaptureInto(&c), 0, 0, 2, px.data());
    cache.OnPost(CaptureInto(&c), 0, 2, -1, px.data());
    cache.OnPost(CaptureInto(&c), 0, 2, 2, nullptr);
    EXPECT_EQ(c.calls, 0);
    EXPECT_TRUE(cache.Last().bgra.empty());
    EXPECT_EQ(cache.Last().width, 0U);
}

TEST(PostedFrameCacheTest, ReplayOnlyForConsistentFrames) {
    PostedFrameCache cache;
    Captured c;
    const auto on_post = CaptureInto(&c).on_post;
    EXPECT_FALSE(cache.Replay(on_post)) << "nothing posted yet";
    EXPECT_FALSE(cache.Replay(nullptr));

    cache.Set(GpuSnapshotFrame{.width = 2, .height = 2, .bgra = std::vector<uint8_t>(16, 7)});
    EXPECT_TRUE(cache.Replay(on_post));
    EXPECT_EQ(c.calls, 1);
    EXPECT_EQ(c.w, 2U);

    cache.Set(GpuSnapshotFrame{.width = 2, .height = 2, .bgra = std::vector<uint8_t>(15, 7)});
    EXPECT_FALSE(cache.Replay(on_post)) << "size mismatch";
    cache.Clear();
    EXPECT_FALSE(cache.Replay(on_post));
    EXPECT_EQ(c.calls, 1);
}

TEST(PostedFrameCacheTest, LastShapeMatchesLastWithoutPixels) {
    PostedFrameCache cache;
    GpuSnapshotFrameShape shape = cache.LastShape();
    EXPECT_EQ(shape.width, 0U);
    EXPECT_EQ(shape.height, 0U);
    EXPECT_EQ(shape.byte_count, 0U);

    cache.Set(GpuSnapshotFrame{.width = 3, .height = 2, .bgra = std::vector<uint8_t>(24, 1)});
    shape = cache.LastShape();
    EXPECT_EQ(shape.width, 3U);
    EXPECT_EQ(shape.height, 2U);
    EXPECT_EQ(shape.byte_count, 24U);
    EXPECT_TRUE(IsReplayableGpuSnapshotFrame(shape.width, shape.height, shape.byte_count));

    // Oversized live posts are dropped rather than cached (they could never
    // be stored in a snapshot).
    Captured c;
    std::vector<uint8_t> px(16, 1);
    cache.OnPost(CaptureInto(&c), 0, static_cast<int>(kGpuSnapshotMaxFrameDim) + 1, 1, px.data());
    EXPECT_EQ(c.calls, 0);
    EXPECT_EQ(cache.LastShape().width, 3U);
}

}  // namespace
}  // namespace goldfish::legacy
