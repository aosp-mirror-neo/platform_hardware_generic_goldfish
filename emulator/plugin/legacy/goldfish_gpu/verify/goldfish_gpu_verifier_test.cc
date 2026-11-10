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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"

#include "android/status/status_matcher_macros.h"

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// Stable fake identities/addresses for the views (never dereferenced).
uint8_t* const kStorageA = reinterpret_cast<uint8_t*>(0x10000);
uint8_t* const kStorageB = reinterpret_cast<uint8_t*>(0x20000);
const void* const kEntryA = reinterpret_cast<const void*>(0x1000);
const void* const kEntryB = reinterpret_cast<const void*>(0x2000);
const void* const kConsumerA = reinterpret_cast<const void*>(0xA);
const void* const kConsumerB = reinterpret_cast<const void*>(0xB);
const gfxstream::RenderChannel* const kChannel1 =
        reinterpret_cast<const gfxstream::RenderChannel*>(0x31);

AsgBlobTable::EntryView Asg(const void* entry, uint32_t handle, uint32_t blob_id,
                            uint8_t* storage, const void* consumer) {
    AsgBlobTable::EntryView v;
    v.entry = entry;
    v.context_handle = handle;
    v.blob_id = blob_id;
    v.size = kAsgRingRegionSize + 4096;
    v.storage = storage;
    v.consumer = consumer;
    return v;
}

PipeChannel::View Pipe(uint32_t id, bool paused, bool reader_started) {
    PipeChannel::View v;
    v.id = id;
    v.channel = kChannel1;
    v.paused_for_snapshot = paused;
    v.reader_started = reader_started;
    return v;
}

// A consistent running GPU with one started ASG and one pipe.
GpuVerifyInput Healthy(VerifyPhase phase = VerifyPhase::kSteady) {
    GpuVerifyInput in;
    in.phase = phase;
    in.mode = GoldfishGpuVerifyMode::kEnforce;
    in.has_renderer = true;
    in.has_render_lib = true;
    in.width = 2;
    in.height = 2;
    in.guest_api_level = 30;
    in.asgs.next_handle = 2;
    in.asgs.next_blob_id = 2;
    const AsgBlobTable::EntryView a = Asg(kEntryA, 1, 1, kStorageA, kConsumerA);
    in.asgs.registered = {{1, a}};
    in.asgs.blobs = {{1, a}};
    in.pipes.next_id = 2;
    in.pipes.pipes = {Pipe(1, /*paused=*/false, /*reader_started=*/true)};
    in.frame.width = 2;
    in.frame.height = 2;
    in.frame.byte_count = 16;
    return in;
}

std::string FirstViolation(const GpuVerifyInput& in) {
    GpuVerifyResult result;
    (void)VerifyGoldfishGpuState(in, &result);
    return result.first_violation;
}

TEST(GoldfishGpuVerifierTest, HealthyStateHasNoViolations) {
    GpuVerifyResult result;
    EXPECT_OK(VerifyGoldfishGpuState(Healthy(), &result));
    EXPECT_EQ(result.violations, 0U);
}

TEST(GoldfishGpuVerifierTest, ModesControlTheOutcome) {
    GpuVerifyInput in = Healthy();
    in.has_render_lib = false;
    GpuVerifyResult result;
    EXPECT_THAT(VerifyGoldfishGpuState(in, &result),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("LIFE-RENDERER")));
    EXPECT_EQ(result.violations, 1U);

    in.mode = GoldfishGpuVerifyMode::kLog;
    EXPECT_OK(VerifyGoldfishGpuState(in, &result)) << "kLog only logs";
    EXPECT_EQ(result.violations, 1U);

    in.mode = GoldfishGpuVerifyMode::kOff;
    result = {};
    EXPECT_OK(VerifyGoldfishGpuState(in, &result));
    EXPECT_EQ(result.violations, 0U) << "kOff skips the checks";
}

TEST(GoldfishGpuVerifierTest, StrictModeAborts) {
    GpuVerifyInput in = Healthy();
    in.mode = GoldfishGpuVerifyMode::kStrict;
    in.width = 0;
    EXPECT_DEATH((void)VerifyGoldfishGpuState(in), "LIFE-CONFIG");
}

TEST(GoldfishGpuVerifierTest, LifetimeChecks) {
    GpuVerifyInput in = Healthy();
    in.next_puid = 0;
    EXPECT_THAT(FirstViolation(in), HasSubstr("LIFE-COUNTERS"));

    in = Healthy();
    in.snapshot_phase = SnapshotPhase::kQuiesced;
    EXPECT_THAT(FirstViolation(in), HasSubstr("LIFE-SNAPSHOT-FLAGS"));

    in = Healthy(VerifyPhase::kPostSaveStream);
    in.snapshot_phase = SnapshotPhase::kSaved;
    in.cached_meta_size = 0;  // a saved phase needs a valid blob
    in.pipes.pipes = {Pipe(1, true, true)};
    EXPECT_THAT(FirstViolation(in), HasSubstr("LIFE-SNAPSHOT-FLAGS"));
    in.cached_meta_size = MinGpuSnapshotBytes();
    EXPECT_OK(VerifyGoldfishGpuState(in));

    in = Healthy(VerifyPhase::kPreLoad);
    in.snapshot_phase = SnapshotPhase::kPreLoaded;
    in.asgs.registered[0].second.consumer = nullptr;
    in.asgs.blobs[0].second.consumer = nullptr;
    in.pipes.pipes.clear();
    EXPECT_OK(VerifyGoldfishGpuState(in));
    in.has_texture_loader = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("LIFE-PRELOAD-TEXTURE-LOADER"));
}

TEST(GoldfishGpuVerifierTest, AsgTableChecks) {
    GpuVerifyInput in = Healthy();
    in.asgs.blobs.clear();  // registered without a blob
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-COUNT-BOUND"));

    in = Healthy();
    in.asgs.blobs[0].second.entry = kEntryB;  // different entry under the same blob id
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-BLOB-BIJECTION"));

    in = Healthy();
    in.asgs.registered[0].second.context_handle = 9;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-HANDLE-MATCH"));

    in = Healthy();
    in.asgs.registered[0].second.storage = reinterpret_cast<const uint8_t*>(0x10001);  // misaligned
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-STORAGE-VALID"));

    in = Healthy();
    const AsgBlobTable::EntryView b = Asg(kEntryB, 2, 2, kStorageB, kConsumerA);  // dup consumer
    in.asgs.next_handle = 3;
    in.asgs.next_blob_id = 3;
    in.asgs.registered.emplace_back(2, b);
    in.asgs.blobs.emplace_back(2, b);
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-CONSUMER-UNIQUE"));

    in = Healthy();
    in.asgs.registered[0].second.exiting = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-NOT-EXITING"));

    in = Healthy();
    in.asgs.registered[0].second.pause_pre_snapshot_requested = true;
    in.asgs.registered[0].second.resume_post_snapshot_requested = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-SNAPSHOT-FLAGS-EXCLUSIVE"));

    in = Healthy();
    in.asgs.registered[0].second.pause_pre_snapshot_requested = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-RESUMED-NO-PAUSE"));

    in = Healthy();
    in.asgs.retired_count = 1;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-RETIRED-DRAINED"));

    // Two blobs whose storage ranges overlap.
    in = Healthy();
    AsgBlobTable::EntryView overlap = Asg(kEntryB, 2, 2, kStorageA, kConsumerB);
    in.asgs.next_handle = 3;
    in.asgs.next_blob_id = 3;
    in.asgs.registered.emplace_back(2, overlap);
    in.asgs.blobs.emplace_back(2, overlap);
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-NO-OVERLAP"));
}

TEST(GoldfishGpuVerifierTest, AsgQuiescedAndLoadPhaseChecks) {
    GpuVerifyInput in = Healthy(VerifyPhase::kPostQuiesce);
    in.snapshot_phase = SnapshotPhase::kQuiesced;
    in.pipes.pipes = {Pipe(1, true, true)};
    EXPECT_OK(VerifyGoldfishGpuState(in));
    in.asgs.registered[0].second.resume_post_snapshot_requested = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-QUIESCED-NO-RESUME"));

    in = Healthy(VerifyPhase::kPreLoad);
    in.snapshot_phase = SnapshotPhase::kPreLoaded;
    in.pipes.pipes.clear();
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-PRELOAD-EMPTY")) << "consumer still alive";
    in.asgs.registered[0].second.consumer = nullptr;
    in.asgs.blobs[0].second.consumer = nullptr;
    in.asgs.registered[0].second.wakeup_requested = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-PRELOAD-CLEAN"));

    // A retired blob (in blobs, not registered) that still owns a consumer during load.
    in = Healthy(VerifyPhase::kPostLoadStream);
    in.snapshot_phase = SnapshotPhase::kPreLoaded;
    in.pipes.pipes = {Pipe(1, true, false)};
    in.asgs.next_blob_id = 3;
    in.asgs.blobs.emplace_back(2, Asg(kEntryB, 1, 2, kStorageB, kConsumerB));
    in.asgs.retired_count = 1;
    EXPECT_THAT(FirstViolation(in), HasSubstr("ASG-RETIRED-BLOB-NO-CONSUMER"));
}

TEST(GoldfishGpuVerifierTest, PipeTableChecks) {
    GpuVerifyInput in = Healthy();
    in.pipes.pipes[0].id = 5;  // >= next_id
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-ID-MATCH"));

    in = Healthy();
    in.pipes.pipes.push_back(Pipe(1, false, true));  // same channel pointer twice
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-CHANNEL-UNIQUE"));

    in = Healthy();
    in.pipes.pipes[0].closing = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-NOT-CLOSING"));

    in = Healthy();
    in.pipes.pipes[0].paused_for_snapshot = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-RUNNING"));

    in = Healthy(VerifyPhase::kPreSaveStream);
    in.snapshot_phase = SnapshotPhase::kQuiesced;
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-QUIESCED")) << "not paused while quiesced";

    in = Healthy(VerifyPhase::kPostLoadStream);
    in.snapshot_phase = SnapshotPhase::kPreLoaded;
    in.pipes.pipes = {Pipe(1, true, /*reader_started=*/true)};
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-LOAD-THREAD-DEFERRED"));

    in = Healthy(VerifyPhase::kPreLoad);
    in.snapshot_phase = SnapshotPhase::kPreLoaded;
    in.asgs.registered[0].second.consumer = nullptr;
    in.asgs.blobs[0].second.consumer = nullptr;
    in.pipes.pipes = {Pipe(1, true, false)};
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-PRELOAD-EMPTY"));

    in = Healthy();
    in.pipes.pipes[0].channel = nullptr;
    EXPECT_THAT(FirstViolation(in), HasSubstr("PIPE-ENTRY-NONNULL"));
}

TEST(GoldfishGpuVerifierTest, ColorBufferFrameAndArtifactChecks) {
    GpuVerifyInput in = Healthy();
    in.has_zero_color_buffer_handle = true;
    EXPECT_THAT(FirstViolation(in), HasSubstr("CB-HANDLE-NONZERO"));

    in = Healthy(VerifyPhase::kPreLoad);
    in.snapshot_phase = SnapshotPhase::kPreLoaded;
    in.asgs.registered[0].second.consumer = nullptr;
    in.asgs.blobs[0].second.consumer = nullptr;
    in.pipes.pipes.clear();
    in.active_color_buffer_count = 1;
    EXPECT_THAT(FirstViolation(in), HasSubstr("CB-PRELOAD-EMPTY"));

    in = Healthy();
    in.frame.byte_count = 15;
    EXPECT_THAT(FirstViolation(in), HasSubstr("FRAME-CONSISTENT"));
    in.frame.byte_count = 0;
    EXPECT_THAT(FirstViolation(in), HasSubstr("FRAME-CONSISTENT")) << "empty frame with dims";
    in.frame.width = 0;
    in.frame.height = 0;
    EXPECT_OK(VerifyGoldfishGpuState(in));

    in = Healthy();
    in.snapshot_artifacts = absl::DataLossError("missing gfxstream_render.bin");
    EXPECT_OK(VerifyGoldfishGpuState(in)) << "artifacts are only checked in the stream phases";
    in = Healthy(VerifyPhase::kPostSaveStream);
    in.snapshot_phase = SnapshotPhase::kSaved;
    in.cached_meta_size = MinGpuSnapshotBytes();
    in.pipes.pipes = {Pipe(1, true, true)};
    in.snapshot_artifacts = absl::DataLossError("missing gfxstream_render.bin");
    EXPECT_THAT(FirstViolation(in), HasSubstr("DISK-SNAPSHOT-COMPLETE"));
}

TEST(GoldfishGpuVerifierTest, PhaseNames) {
    EXPECT_EQ(VerifyPhaseName(VerifyPhase::kSteady), "STEADY");
    EXPECT_EQ(VerifyPhaseName(VerifyPhase::kPostQuiesce), "POST_QUIESCE");
    EXPECT_EQ(VerifyPhaseName(VerifyPhase::kPostLoad), "POST_LOAD");
}

}  // namespace
}  // namespace goldfish::legacy
