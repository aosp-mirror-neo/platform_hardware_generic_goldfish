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

#include "goldfish/legacy/goldfish_gpu/verify/goldfish_gpu_verifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "goldfish/legacy/goldfish_gpu/renderer/renderer_config.h"

namespace goldfish::legacy {
namespace {

struct VerifyContext {
    VerifyPhase phase = VerifyPhase::kSteady;
    GoldfishGpuVerifyMode mode = GoldfishGpuVerifyMode::kLog;
    size_t violations = 0;
    std::string first_violation;

    void Report(std::string_view id, std::string_view detail) {
        ++violations;
        const std::string msg =
                absl::StrCat("verify[", VerifyPhaseName(phase), "] ", id, ": ", detail);
        if (first_violation.empty()) {
            first_violation = msg;
        }
        if (mode == GoldfishGpuVerifyMode::kStrict) {
            LOG(FATAL) << "goldfish_gpu: " << msg;
        } else {
            LOG(ERROR) << "goldfish_gpu: " << msg;
        }
    }
};

void VerifyLifetime(const GpuVerifyInput& in, VerifyContext* ctx) {
    if (!in.has_renderer || !in.has_render_lib) {
        ctx->Report("LIFE-RENDERER", absl::StrCat("renderer=", in.has_renderer,
                                                  " render_lib=", in.has_render_lib));
    }
    if (in.width == 0 || in.height == 0 || in.guest_api_level == 0) {
        ctx->Report("LIFE-CONFIG", absl::StrCat("width=", in.width, " height=", in.height,
                                                " guest_api_level=", in.guest_api_level));
    }
    if (in.asgs.next_handle == 0 || in.asgs.next_blob_id == 0 || in.pipes.next_id == 0 ||
        in.next_puid == 0 || in.next_timeline_id == 0) {
        ctx->Report("LIFE-COUNTERS",
                    absl::StrCat("next_asg_handle=", in.asgs.next_handle, " next_blob_id=",
                                 in.asgs.next_blob_id, " next_pipe_id=", in.pipes.next_id,
                                 " next_puid=", in.next_puid,
                                 " next_timeline_id=", in.next_timeline_id));
    }
    SnapshotPhase expected = SnapshotPhase::kRunning;
    bool meta_must_be_empty = true;
    switch (ctx->phase) {
    case VerifyPhase::kSteady:
    case VerifyPhase::kPostSave:
    case VerifyPhase::kPostLoad:
        expected = SnapshotPhase::kRunning;
        break;
    case VerifyPhase::kPostQuiesce:
        // Either the first Quiesce (kQuiesced) or a repeat after Save (kSaved).
        expected = in.snapshot_phase == SnapshotPhase::kSaved ? SnapshotPhase::kSaved
                                                               : SnapshotPhase::kQuiesced;
        meta_must_be_empty = expected == SnapshotPhase::kQuiesced;
        break;
    case VerifyPhase::kPreSaveStream:
        expected = SnapshotPhase::kQuiesced;
        break;
    case VerifyPhase::kPostSaveStream:
        expected = SnapshotPhase::kSaved;
        meta_must_be_empty = false;
        break;
    case VerifyPhase::kPreLoad:
    case VerifyPhase::kPostLoadStream:
        expected = SnapshotPhase::kPreLoaded;
        break;
    }
    const bool meta_ok = meta_must_be_empty
                                 ? in.cached_meta_size == 0
                                 : (in.cached_meta_size >= MinGpuSnapshotBytes() &&
                                    in.cached_meta_size <= MaxGpuSnapshotBytes());
    if (in.snapshot_phase != expected || !meta_ok) {
        ctx->Report("LIFE-SNAPSHOT-FLAGS",
                    absl::StrCat("expected phase ", SnapshotPhaseName(expected),
                                 meta_must_be_empty ? " with no" : " with a valid",
                                 " cached meta blob, got ", SnapshotPhaseName(in.snapshot_phase),
                                 " with cached_meta.size()=", in.cached_meta_size));
    }
    if (ctx->phase == VerifyPhase::kPreLoad && in.has_texture_loader) {
        ctx->Report("LIFE-PRELOAD-TEXTURE-LOADER", "texture loader must be null in PRE_LOAD");
    }
}

void VerifyAsgEntries(const GpuVerifyInput& in, VerifyContext* ctx) {
    const AsgBlobTable::View& view = in.asgs;
    const bool running = ctx->phase == VerifyPhase::kSteady ||
                         ctx->phase == VerifyPhase::kPostSave ||
                         ctx->phase == VerifyPhase::kPostLoad;
    const bool quiesced_for_save = ctx->phase == VerifyPhase::kPostQuiesce ||
                                   ctx->phase == VerifyPhase::kPreSaveStream ||
                                   ctx->phase == VerifyPhase::kPostSaveStream;
    const bool loading = ctx->phase == VerifyPhase::kPreLoad ||
                         ctx->phase == VerifyPhase::kPostLoadStream ||
                         ctx->phase == VerifyPhase::kPostLoad;

    if (view.registered.size() > kGpuSnapshotMaxEntries ||
        view.blobs.size() > kGpuSnapshotMaxEntries ||
        view.registered.size() > view.blobs.size()) {
        ctx->Report("ASG-COUNT-BOUND",
                    absl::StrCat("registered=", view.registered.size(),
                                 " blobs=", view.blobs.size(), " max=", kGpuSnapshotMaxEntries));
    }

    std::unordered_map<uint32_t, const void*> blob_identity;
    for (const auto& [blob_id, e] : view.blobs) {
        blob_identity[blob_id] = e.entry;
    }
    std::unordered_set<const void*> seen_consumers;
    for (const auto& [handle, e] : view.registered) {
        if (e.entry == nullptr) {
            ctx->Report("ASG-ENTRY-NONNULL",
                        absl::StrCat("null entry in asg_entries_ at handle=", handle));
            continue;
        }
        if (e.context_handle != handle || handle == 0 || handle >= view.next_handle ||
            e.blob_id == 0 || e.blob_id >= view.next_blob_id) {
            ctx->Report("ASG-HANDLE-MATCH",
                        absl::StrCat("handle=", handle, " entry->context_handle=",
                                     e.context_handle, " next_asg_handle=", view.next_handle,
                                     " blob_id=", e.blob_id,
                                     " next_blob_id=", view.next_blob_id));
        }
        auto blob_it = blob_identity.find(e.blob_id);
        if (blob_it == blob_identity.end() || blob_it->second != e.entry) {
            ctx->Report("ASG-BLOB-BIJECTION",
                        absl::StrCat("handle=", handle, " blob_id=", e.blob_id,
                                     " not mapped to the same entry in live_blobs_"));
        }
        if (e.storage == nullptr || e.size <= kAsgRingRegionSize ||
            reinterpret_cast<uintptr_t>(e.storage) % kAsgStorageAlignment != 0) {
            ctx->Report("ASG-STORAGE-VALID",
                        absl::StrCat("handle=", handle, " storage=", e.storage != nullptr,
                                     " size=", e.size, " align_rem=",
                                     reinterpret_cast<uintptr_t>(e.storage) %
                                             kAsgStorageAlignment));
        }
        if (e.consumer != nullptr) {
            if (!seen_consumers.insert(e.consumer).second) {
                ctx->Report("ASG-CONSUMER-UNIQUE",
                            absl::StrCat("duplicate consumer pointer on handle=", handle));
            }
            if (ctx->phase == VerifyPhase::kPreLoad) {
                ctx->Report("ASG-PRELOAD-EMPTY",
                            absl::StrCat("handle=", handle,
                                         " still has a non-null consumer in PRE_LOAD"));
            }
        }
        if (e.exiting) {
            ctx->Report("ASG-NOT-EXITING",
                        absl::StrCat("handle=", handle, " has exiting=1 while registered"));
        }
        if (e.pause_pre_snapshot_requested && e.resume_post_snapshot_requested) {
            ctx->Report("ASG-SNAPSHOT-FLAGS-EXCLUSIVE",
                        absl::StrCat("handle=", handle,
                                     " has both pause_pre_snapshot_requested and "
                                     "resume_post_snapshot_requested set"));
        }
        if (ctx->phase == VerifyPhase::kPreLoad &&
            (e.wakeup_requested || e.pause_pre_snapshot_requested ||
             e.resume_post_snapshot_requested)) {
            ctx->Report("ASG-PRELOAD-CLEAN",
                        absl::StrCat("handle=", handle, " has pending consumer flags in PRE_LOAD"));
        }
        if (quiesced_for_save && e.resume_post_snapshot_requested) {
            ctx->Report("ASG-QUIESCED-NO-RESUME",
                        absl::StrCat("handle=", handle,
                                     " has resume_post_snapshot_requested=1 while quiesced"));
        }
        if (running && e.pause_pre_snapshot_requested) {
            ctx->Report("ASG-RESUMED-NO-PAUSE",
                        absl::StrCat("handle=", handle,
                                     " has pause_pre_snapshot_requested=1 while running"));
        }
    }

    if (running && view.retired_count != 0) {
        ctx->Report("ASG-RETIRED-DRAINED",
                    absl::StrCat(view.retired_count,
                                 " retired blob(s) still pending destroy while running"));
    }

    std::unordered_map<uint32_t, const void*> registered_identity;
    for (const auto& [handle, e] : view.registered) {
        registered_identity[handle] = e.entry;
    }
    struct StorageSpan {
        uint32_t blob_id;
        uintptr_t start;
        uintptr_t end;
    };
    std::vector<StorageSpan> spans;
    spans.reserve(view.blobs.size());
    for (const auto& [blob_id, e] : view.blobs) {
        if (e.entry == nullptr) {
            ctx->Report("ASG-BLOB-NONNULL",
                        absl::StrCat("null entry in live_blobs_ at blob_id=", blob_id));
            continue;
        }
        if (e.blob_id != blob_id || blob_id == 0 || blob_id >= view.next_blob_id ||
            e.context_handle == 0 || e.context_handle >= view.next_handle) {
            ctx->Report("ASG-BLOB-ID-MATCH",
                        absl::StrCat("blob_id=", blob_id, " entry->blob_id=", e.blob_id,
                                     " context_handle=", e.context_handle));
        }
        if (e.storage == nullptr || e.size <= kAsgRingRegionSize) {
            ctx->Report("ASG-STORAGE-VALID",
                        absl::StrCat("blob_id=", blob_id, " storage=", e.storage != nullptr,
                                     " size=", e.size));
            continue;
        }
        auto reg_it = registered_identity.find(e.context_handle);
        const bool is_retired =
                reg_it == registered_identity.end() || reg_it->second != e.entry;
        if (is_retired && loading && e.consumer != nullptr) {
            ctx->Report("ASG-RETIRED-BLOB-NO-CONSUMER",
                        absl::StrCat("retired blob_id=", blob_id,
                                     " still owns a consumer during load"));
        }
        const auto start = reinterpret_cast<uintptr_t>(e.storage);
        const uintptr_t end = start + static_cast<uintptr_t>(e.size);
        if (end <= start) {
            ctx->Report("ASG-STORAGE-VALID",
                        absl::StrCat("blob_id=", blob_id, " storage range overflow"));
            continue;
        }
        spans.push_back(StorageSpan{blob_id, start, end});
    }
    std::sort(spans.begin(), spans.end(),
              [](const StorageSpan& a, const StorageSpan& b) { return a.start < b.start; });
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i - 1].end > spans[i].start) {
            ctx->Report("ASG-NO-OVERLAP", absl::StrCat("blob_id=", spans[i - 1].blob_id,
                                                       " and blob_id=", spans[i].blob_id,
                                                       " overlap in host memory"));
        }
    }
}

void VerifyPipeChannels(const GpuVerifyInput& in, VerifyContext* ctx) {
    const OpenglesPipeTable::View& view = in.pipes;
    if (view.pipes.size() > kGpuSnapshotMaxEntries) {
        ctx->Report("PIPE-COUNT-BOUND", absl::StrCat("pipe count ", view.pipes.size(),
                                                     " exceeds ", kGpuSnapshotMaxEntries));
    }
    if (ctx->phase == VerifyPhase::kPreLoad && !view.pipes.empty()) {
        ctx->Report("PIPE-PRELOAD-EMPTY",
                    absl::StrCat("pipe count ", view.pipes.size(), " must be 0 in PRE_LOAD"));
    }

    std::unordered_set<const void*> seen_channels;
    for (const PipeChannel::View& pipe : view.pipes) {
        if (pipe.channel == nullptr) {
            ctx->Report("PIPE-ENTRY-NONNULL",
                        absl::StrCat("pipe id=", pipe.id, " has a null channel"));
            continue;
        }
        if (pipe.id == 0 || pipe.id >= view.next_id) {
            ctx->Report("PIPE-ID-MATCH",
                        absl::StrCat("id=", pipe.id, " next_pipe_id=", view.next_id));
        }
        if (!seen_channels.insert(pipe.channel).second) {
            ctx->Report("PIPE-CHANNEL-UNIQUE",
                        absl::StrCat("duplicate RenderChannel pointer on pipe id=", pipe.id));
        }
        if (pipe.closing) {
            ctx->Report("PIPE-NOT-CLOSING",
                        absl::StrCat("pipe id=", pipe.id, " has closing=1 while registered"));
        }
        if (ctx->phase == VerifyPhase::kPostQuiesce ||
            ctx->phase == VerifyPhase::kPreSaveStream ||
            ctx->phase == VerifyPhase::kPostSaveStream ||
            ctx->phase == VerifyPhase::kPostLoadStream) {
            if (!pipe.paused_for_snapshot || pipe.reading_in_progress) {
                ctx->Report("PIPE-QUIESCED",
                            absl::StrCat("pipe id=", pipe.id,
                                         " paused_for_snapshot=", pipe.paused_for_snapshot,
                                         " reading_in_progress=", pipe.reading_in_progress));
            }
            if (ctx->phase == VerifyPhase::kPostLoadStream && pipe.reader_started) {
                ctx->Report("PIPE-LOAD-THREAD-DEFERRED",
                            absl::StrCat("pipe id=", pipe.id,
                                         " started reader_thread before PostLoad"));
            }
        } else if (ctx->phase == VerifyPhase::kSteady || ctx->phase == VerifyPhase::kPostSave ||
                   ctx->phase == VerifyPhase::kPostLoad) {
            if (pipe.paused_for_snapshot || !pipe.reader_started) {
                ctx->Report("PIPE-RUNNING",
                            absl::StrCat("pipe id=", pipe.id,
                                         " paused_for_snapshot=", pipe.paused_for_snapshot,
                                         " reader_started=", pipe.reader_started));
            }
        }
    }
}

void VerifyColorBuffersAndFrame(const GpuVerifyInput& in, VerifyContext* ctx) {
    if (ctx->phase == VerifyPhase::kPreLoad && in.active_color_buffer_count != 0) {
        ctx->Report("CB-PRELOAD-EMPTY", absl::StrCat("active color buffers=",
                                                     in.active_color_buffer_count,
                                                     " must be 0 in PRE_LOAD"));
    }
    if (in.has_zero_color_buffer_handle) {
        ctx->Report("CB-HANDLE-NONZERO", "active color buffers contain handle 0");
    }
    const GpuSnapshotFrameShape& frame = in.frame;
    if (frame.byte_count == 0) {
        if (frame.width != 0 || frame.height != 0) {
            ctx->Report("FRAME-CONSISTENT",
                        absl::StrCat("empty last frame with dimensions ", frame.width, "x",
                                     frame.height));
        }
    } else {
        const uint64_t expected_size = GpuSnapshotFrameBytes(frame.width, frame.height);
        if (!IsReplayableGpuSnapshotFrame(frame.width, frame.height, frame.byte_count) ||
            frame.byte_count > MaxGpuSnapshotBytes()) {
            ctx->Report("FRAME-CONSISTENT",
                        absl::StrCat("last frame size ", frame.byte_count,
                                     " does not match ", frame.width, "x", frame.height,
                                     "x4=", expected_size, " (max edge ",
                                     kGpuSnapshotMaxFrameDim, ")"));
        }
    }
}

void VerifySnapshotArtifacts(const GpuVerifyInput& in, VerifyContext* ctx) {
    if (ctx->phase != VerifyPhase::kPostSaveStream && ctx->phase != VerifyPhase::kPostLoadStream) {
        return;
    }
    if (!in.snapshot_artifacts.ok()) {
        ctx->Report("DISK-SNAPSHOT-COMPLETE", in.snapshot_artifacts.message());
    }
}

}  // namespace

std::string_view VerifyPhaseName(VerifyPhase phase) {
    switch (phase) {
    case VerifyPhase::kSteady:
        return "STEADY";
    case VerifyPhase::kPostQuiesce:
        return "POST_QUIESCE";
    case VerifyPhase::kPreSaveStream:
        return "PRE_SAVE_STREAM";
    case VerifyPhase::kPostSaveStream:
        return "POST_SAVE_STREAM";
    case VerifyPhase::kPostSave:
        return "POST_SAVE";
    case VerifyPhase::kPreLoad:
        return "PRE_LOAD";
    case VerifyPhase::kPostLoadStream:
        return "POST_LOAD_STREAM";
    case VerifyPhase::kPostLoad:
        return "POST_LOAD";
    }
    return "UNKNOWN";
}

absl::Status VerifyGoldfishGpuState(const GpuVerifyInput& in, GpuVerifyResult* result) {
    if (in.mode == GoldfishGpuVerifyMode::kOff) {
        return absl::OkStatus();
    }
    VerifyContext ctx;
    ctx.phase = in.phase;
    ctx.mode = in.mode;
    VerifyLifetime(in, &ctx);
    VerifyAsgEntries(in, &ctx);
    VerifyPipeChannels(in, &ctx);
    VerifyColorBuffersAndFrame(in, &ctx);
    VerifySnapshotArtifacts(in, &ctx);
    if (result != nullptr) {
        result->violations = ctx.violations;
        result->first_violation = ctx.first_violation;
    }
    if (ctx.violations > 0) {
        LOG(ERROR) << "goldfish_gpu: verify[" << VerifyPhaseName(in.phase) << "] failed with "
                   << ctx.violations << " invariant violation(s) (mode=" << VerifyModeName(in.mode)
                   << ")";
        if (in.mode == GoldfishGpuVerifyMode::kEnforce) {
            return absl::FailedPreconditionError(absl::StrCat(
                    "goldfish_gpu: ", ctx.first_violation, " (", ctx.violations, " violation(s))"));
        }
    }
    return absl::OkStatus();
}

}  // namespace goldfish::legacy
