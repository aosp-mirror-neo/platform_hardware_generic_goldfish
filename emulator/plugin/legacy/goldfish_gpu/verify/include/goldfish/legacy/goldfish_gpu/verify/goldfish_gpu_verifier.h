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
#include <string>
#include <string_view>

#include "absl/status/status.h"

#include "goldfish/legacy/goldfish_gpu/transport/asg_blob_table.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_codec.h"
#include "goldfish/legacy/goldfish_gpu/transport/opengles_pipe_table.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_phase_tracker.h"

// State-invariant checks over a read-only view of GoldfishGpu (mirroring
// virtio_gpu_gfxstream_verify). The facade assembles a GpuVerifyInput at each
// lifecycle/snapshot boundary; the checks themselves never touch live state,
// so a test can feed any (broken) view and assert on the reported ids.

namespace goldfish::legacy {

enum class VerifyPhase {
    kSteady,
    kPostQuiesce,
    kPreSaveStream,
    kPostSaveStream,
    kPostSave,
    kPreLoad,
    kPostLoadStream,
    kPostLoad,
};

std::string_view VerifyPhaseName(VerifyPhase phase);

struct GpuVerifyInput {
    VerifyPhase phase = VerifyPhase::kSteady;
    GoldfishGpuVerifyMode mode = GoldfishGpuVerifyMode::kLog;

    // Lifetime.
    bool has_renderer = false;
    bool has_render_lib = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t guest_api_level = 0;
    uint64_t next_puid = 1;
    uint64_t next_timeline_id = 1;
    SnapshotPhase snapshot_phase = SnapshotPhase::kRunning;
    size_t cached_meta_size = 0;
    bool has_texture_loader = false;

    // Tables.
    AsgBlobTable::View asgs;
    OpenglesPipeTable::View pipes;
    size_t active_color_buffer_count = 0;
    bool has_zero_color_buffer_handle = false;
    // Shape only: the verifier never looks at pixels, and copying the frame
    // (up to ~127 MB) on every lifecycle call is not acceptable in kLog mode.
    GpuSnapshotFrameShape frame;

    // GpuSnapshotFiles::ArtifactsPresent() for the snapshot being verified;
    // only consulted in the *Stream phases.
    absl::Status snapshot_artifacts = absl::OkStatus();
};

struct GpuVerifyResult {
    size_t violations = 0;
    std::string first_violation;
};

// Runs every check. Each violation is logged (ERROR, or FATAL in kStrict).
// Returns OK unless `in.mode` is kEnforce and something was violated, in which
// case FAILED_PRECONDITION names the first violation. kOff skips everything.
absl::Status VerifyGoldfishGpuState(const GpuVerifyInput& in, GpuVerifyResult* result = nullptr);

}  // namespace goldfish::legacy
