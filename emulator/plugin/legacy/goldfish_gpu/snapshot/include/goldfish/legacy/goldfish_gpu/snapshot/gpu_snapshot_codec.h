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
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu_c_api.h"
#include "render-utils/stream.h"

// Encoder/decoder of the GoldfishGpu snapshot meta blob that goes into QEMU's
// VMState ("goldfish_gpu_end" section). The format is:
//
//   be32 magic "GFGP"      be32 version
//   be32 asg_count         asg_count x { be32 context_handle, <ASG consumer payload> }
//   be32 pipe_count        pipe_count x { be32 pipe_id, <render channel payload> }
//   be32 frame_w  be32 frame_h  be32 frame_bytes  frame_bytes x u8 (BGRA)
//   be32 next_asg_handle   be32 next_pipe_id   be64 next_puid   be64 next_timeline_id
//
// The per-entry payloads are written and read by gfxstream (through the
// `gfxstream::Stream*` handed to the callbacks); the codec only owns the
// framing, the bounds and the error reporting, so it is testable without a
// renderer.

namespace goldfish::legacy {

// Upper bound on the ASG consumers and render channels restored from one
// blob; each render channel starts a render thread, so a corrupt count must
// not be allowed to spawn an unbounded number of them. Matches the pipe limit
// in GoldfishPipeDevice::Load.
inline constexpr uint32_t kGpuSnapshotMaxEntries = 65536;

struct GpuSnapshotFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // empty, or width * height * 4 bytes
};

// Largest frame edge the snapshot accepts. Keeps width * height * 4 far below
// 2^64 so the size checks below cannot wrap, and well inside what the display
// surface code (`int` dimensions) can represent. Same value as the C ABI's
// GOLDFISH_GPU_MAX_FRAME_DIM, which the post-callback consumers rely on.
inline constexpr uint32_t kGpuSnapshotMaxFrameDim = GOLDFISH_GPU_MAX_FRAME_DIM;

// Byte size of a width x height BGRA frame, or 0 when either dimension is 0 or
// exceeds kGpuSnapshotMaxFrameDim. Never overflows.
inline uint64_t GpuSnapshotFrameBytes(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > kGpuSnapshotMaxFrameDim ||
        height > kGpuSnapshotMaxFrameDim) {
        return 0;
    }
    return uint64_t{width} * uint64_t{height} * 4;
}

// True when a non-empty frame of `width` x `height` carrying `byte_count`
// bytes has in-range dimensions and exactly width * height * 4 bytes.
inline bool IsReplayableGpuSnapshotFrame(uint32_t width, uint32_t height, size_t byte_count) {
    const uint64_t bytes = GpuSnapshotFrameBytes(width, height);
    return bytes != 0 && byte_count == bytes;
}

// True when `frame` is non-empty, has in-range dimensions and carries exactly
// width * height * 4 bytes.
inline bool IsReplayableGpuSnapshotFrame(const GpuSnapshotFrame& frame) {
    return IsReplayableGpuSnapshotFrame(frame.width, frame.height, frame.bgra.size());
}

// Dimensions and byte count of a frame, for checks that do not need pixels.
struct GpuSnapshotFrameShape {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t byte_count = 0;

    static GpuSnapshotFrameShape Of(const GpuSnapshotFrame& frame) {
        return {frame.width, frame.height, frame.bgra.size()};
    }
};

struct GpuSnapshotCounters {
    uint32_t next_asg_handle = 1;
    uint32_t next_pipe_id = 1;
    uint64_t next_puid = 1;
    uint64_t next_timeline_id = 1;
};

// Writes the gfxstream payload of entry `id` to `stream`.
using GpuSnapshotEntryWriter = absl::FunctionRef<void(uint32_t id, gfxstream::Stream* stream)>;

// Builds a meta blob. `asg_handles` and `pipe_ids` are emitted in the given
// order; the writers are invoked once per entry. Fails with
// RESOURCE_EXHAUSTED when the blob would exceed GOLDFISH_GPU_SNAPSHOT_MAX_BYTES.
absl::StatusOr<std::vector<uint8_t>> EncodeGpuSnapshot(const std::vector<uint32_t>& asg_handles,
                                                       GpuSnapshotEntryWriter write_asg,
                                                       const std::vector<uint32_t>& pipe_ids,
                                                       GpuSnapshotEntryWriter write_pipe,
                                                       const GpuSnapshotFrame& frame,
                                                       const GpuSnapshotCounters& counters);

// Consumes the gfxstream payload of entry `id` from `stream`. A non-OK status
// aborts the decode and is returned to the caller unchanged; reading past the
// end of the blob is detected by the decoder after the callback returns.
using GpuSnapshotEntryReader =
        absl::FunctionRef<absl::Status(uint32_t id, gfxstream::Stream* stream)>;

struct GpuSnapshotTail {
    GpuSnapshotFrame frame;
    GpuSnapshotCounters counters;
};

// Parses a blob written by EncodeGpuSnapshot(). Every bounds, magic, version,
// frame-size and trailing-byte violation is a DATA_LOSS (unsupported version:
// FAILED_PRECONDITION) error naming the offending field. Entry callbacks run
// in stream order, so a caller that creates state in them must be prepared
// for the decode to fail afterwards.
absl::StatusOr<GpuSnapshotTail> DecodeGpuSnapshot(const uint8_t* data, size_t size,
                                                  GpuSnapshotEntryReader read_asg,
                                                  GpuSnapshotEntryReader read_pipe);

// Smallest blob EncodeGpuSnapshot() can produce (no entries, no frame).
size_t MinGpuSnapshotBytes();
// Largest blob EncodeGpuSnapshot() produces and DecodeGpuSnapshot() accepts.
size_t MaxGpuSnapshotBytes();

}  // namespace goldfish::legacy
