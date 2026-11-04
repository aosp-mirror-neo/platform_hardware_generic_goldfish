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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"

namespace goldfish::legacy {

void PostedFrameCache::OnPost(const Source& source, uint32_t display_id, int width, int height,
                              const unsigned char* pixels) {
    if (display_id != 0 || width <= 0 || height <= 0) {
        return;
    }
    const auto w = static_cast<uint32_t>(width);
    const auto h = static_cast<uint32_t>(height);
    const uint64_t frame_bytes = GpuSnapshotFrameBytes(w, h);
    if (frame_bytes == 0) {
        return;  // out-of-range dimensions; never cache or forward them
    }
    const auto byte_size = static_cast<size_t>(frame_bytes);

    absl::MutexLock post_lock(&post_mu_);
    std::vector<uint8_t>& bgra = scratch_;
    bgra.resize(byte_size);
    if (source.read_pixels != nullptr) {
        if (source.before_readback && pixels != nullptr) {
            source.before_readback();
        }
        source.read_pixels(bgra.data(), static_cast<uint32_t>(byte_size), /*displayId=*/0);
    } else {
        if (pixels == nullptr) {
            return;
        }
        std::memcpy(bgra.data(), pixels, byte_size);
    }
    for (size_t i = 3; i < byte_size; i += 4) {
        bgra[i] = 0xff;
    }

    if (source.on_post) {
        source.on_post(w, h, bgra.data());
    }
    absl::MutexLock lock(&mu_);
    last_.width = w;
    last_.height = h;
    // The old frame becomes the next scratch buffer; it is usually already
    // the right size.
    std::swap(last_.bgra, bgra);
}

GpuSnapshotFrame PostedFrameCache::Last() const {
    absl::MutexLock lock(&mu_);
    return last_;
}

GpuSnapshotFrameShape PostedFrameCache::LastShape() const {
    absl::MutexLock lock(&mu_);
    return GpuSnapshotFrameShape::Of(last_);
}

void PostedFrameCache::Set(GpuSnapshotFrame frame) {
    absl::MutexLock lock(&mu_);
    last_ = std::move(frame);
}

void PostedFrameCache::Clear() {
    absl::MutexLock lock(&mu_);
    last_ = GpuSnapshotFrame{};
}

bool PostedFrameCache::Replay(const GoldfishPostCallback& on_post) const {
    if (!on_post) {
        return false;
    }
    GpuSnapshotFrame frame = Last();
    if (!IsReplayableGpuSnapshotFrame(frame)) {
        return false;
    }
    on_post(frame.width, frame.height, frame.bgra.data());
    return true;
}

}  // namespace goldfish::legacy
