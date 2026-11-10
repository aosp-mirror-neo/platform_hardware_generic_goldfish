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

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_codec.h"
#include "render-utils/Renderer.h"

// Keeps the last frame gfxstream posted for display 0 as opaque BGRA, so that
// it can be stored in a snapshot and replayed to the UI after a load (the
// guest does not repaint until something changes on screen).
//
// Thread contract: OnPost() runs on a gfxstream render thread; the other
// methods run on QEMU threads. OnPost() invokes `on_post` while holding
// `post_mu_` (the pixels handed over live in the scratch buffer that lock
// protects), so `on_post` must not call back into OnPost(); `mu_` is never
// held across it. Replay() invokes `on_post` with no lock held and may run
// concurrently with OnPost() (main thread right after a load vs. a render
// thread that already resumed), so `on_post` has to be safe to call from two
// threads at once — goldfish_pipe_on_frame() serializes on its own mutex.

namespace goldfish::legacy {

class PostedFrameCache {
  public:
    struct Source {
        // Non-null when the renderer wants the frame read back rather than
        // copied from `pixels` (Renderer::asyncReadbackSupported()).
        gfxstream::Renderer::ReadPixelsCallback read_pixels = nullptr;
        // Runs on the posting thread right before `read_pixels`, only when
        // gfxstream passed a pixel pointer (i.e. the post comes from a GL
        // context). Windows/ANGLE: glFlush(), so the pending glReadPixels is
        // submitted before the readback thread maps the PBO (b/445476051).
        std::function<void()> before_readback;
        GoldfishPostCallback on_post;
    };

    // gfxstream post callback: copies or reads back the frame, forces alpha to
    // 0xff, forwards it to `source.on_post` and remembers it. Frames for other
    // displays, with non-positive dimensions, or without a pixel source are
    // ignored.
    void OnPost(const Source& source, uint32_t display_id, int width, int height,
                const unsigned char* pixels) ABSL_LOCKS_EXCLUDED(post_mu_, mu_);

    // Copies the remembered frame (up to ~127 MB); use LastShape() when only
    // the dimensions and byte count are needed.
    GpuSnapshotFrame Last() const ABSL_LOCKS_EXCLUDED(mu_);
    // Dimensions and byte count of the remembered frame, without copying it.
    GpuSnapshotFrameShape LastShape() const ABSL_LOCKS_EXCLUDED(mu_);
    void Set(GpuSnapshotFrame frame) ABSL_LOCKS_EXCLUDED(mu_);
    void Clear() ABSL_LOCKS_EXCLUDED(mu_);

    // Sends the remembered frame to `on_post` if it is consistent (non-zero
    // dimensions and width * height * 4 bytes). Returns whether it did.
    bool Replay(const GoldfishPostCallback& on_post) const ABSL_LOCKS_EXCLUDED(mu_);

  private:
    mutable absl::Mutex mu_;
    GpuSnapshotFrame last_ ABSL_GUARDED_BY(mu_);
    // Scratch buffer reused across posts instead of zero-filling a new vector
    // per frame (about 10 MB at 1080x2400). Lock order: post_mu_ -> mu_.
    absl::Mutex post_mu_ ABSL_ACQUIRED_BEFORE(mu_);
    std::vector<uint8_t> scratch_ ABSL_GUARDED_BY(post_mu_);
};

}  // namespace goldfish::legacy
