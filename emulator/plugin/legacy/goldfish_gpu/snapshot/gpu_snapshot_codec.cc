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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu_c_api.h"
#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_streams.h"

namespace goldfish::legacy {
namespace {

constexpr uint32_t kGpuMetaMagic = 0x47464750;  // "GFGP"
constexpr uint32_t kGpuMetaVersion = 1;
constexpr uint64_t kMaxBytes = GOLDFISH_GPU_SNAPSHOT_MAX_BYTES;
// magic(4) + version(4) + asg_count(4) + pipe_count(4) + w(4) + h(4) +
// frame_size(4) + next_asg(4) + next_pipe(4) + next_puid(8) + next_tl(8) = 52.
constexpr size_t kMinBytes = 52;

}  // namespace

size_t MinGpuSnapshotBytes() { return kMinBytes; }
size_t MaxGpuSnapshotBytes() { return static_cast<size_t>(kMaxBytes); }

absl::StatusOr<std::vector<uint8_t>> EncodeGpuSnapshot(const std::vector<uint32_t>& asg_handles,
                                                       GpuSnapshotEntryWriter write_asg,
                                                       const std::vector<uint32_t>& pipe_ids,
                                                       GpuSnapshotEntryWriter write_pipe,
                                                       const GpuSnapshotFrame& frame,
                                                       const GpuSnapshotCounters& counters) {
    if (asg_handles.size() > kGpuSnapshotMaxEntries || pipe_ids.size() > kGpuSnapshotMaxEntries) {
        return absl::ResourceExhaustedError(
                absl::StrCat("GoldfishGpu snapshot has ", asg_handles.size(), " ASG entries and ",
                             pipe_ids.size(), " pipe entries; at most ", kGpuSnapshotMaxEntries,
                             " of each can be restored"));
    }
    const bool has_frame = !frame.bgra.empty();
    if (has_frame && !IsReplayableGpuSnapshotFrame(frame)) {
        return absl::InvalidArgumentError(absl::StrCat(
                "GoldfishGpu snapshot frame of ", frame.bgra.size(), " bytes does not match ",
                frame.width, "x", frame.height, "x4 (max edge ", kGpuSnapshotMaxFrameDim, ")"));
    }
    if (!has_frame && (frame.width != 0 || frame.height != 0)) {
        return absl::InvalidArgumentError(
                absl::StrCat("GoldfishGpu snapshot has an empty frame with non-zero dimensions ",
                             frame.width, "x", frame.height));
    }
    std::vector<uint8_t> bytes;
    BufferStream meta(&bytes);
    meta.putBe32(kGpuMetaMagic);
    meta.putBe32(kGpuMetaVersion);

    meta.putBe32(static_cast<uint32_t>(asg_handles.size()));
    for (uint32_t handle : asg_handles) {
        meta.putBe32(handle);
        write_asg(handle, &meta);
    }

    meta.putBe32(static_cast<uint32_t>(pipe_ids.size()));
    for (uint32_t id : pipe_ids) {
        meta.putBe32(id);
        write_pipe(id, &meta);
    }

    meta.putBe32(frame.width);
    meta.putBe32(frame.height);
    meta.putBe32(static_cast<uint32_t>(frame.bgra.size()));
    if (has_frame) {
        meta.write(frame.bgra.data(), frame.bgra.size());
    }
    meta.putBe32(counters.next_asg_handle);
    meta.putBe32(counters.next_pipe_id);
    meta.putBe64(counters.next_puid);
    meta.putBe64(counters.next_timeline_id);
    if (bytes.size() > kMaxBytes) {
        return absl::ResourceExhaustedError(
                absl::StrCat("GoldfishGpu snapshot meta blob of ", bytes.size(), " bytes exceeds ",
                             kMaxBytes, "; the snapshot could not be loaded"));
    }
    return bytes;
}

absl::StatusOr<GpuSnapshotTail> DecodeGpuSnapshot(const uint8_t* data, size_t size,
                                                  GpuSnapshotEntryReader read_asg,
                                                  GpuSnapshotEntryReader read_pipe) {
    if (size > kMaxBytes) {
        return absl::DataLossError(absl::StrCat("GoldfishGpu snapshot meta blob of ", size,
                                                " bytes exceeds ", kMaxBytes));
    }
    BufferStream meta(data, size);
    const uint32_t magic = meta.getBe32();
    const uint32_t version = meta.getBe32();
    if (absl::Status st = meta.ReadStatus("GoldfishGpu snapshot header"); !st.ok()) {
        return st;
    }
    if (magic != kGpuMetaMagic) {
        return absl::DataLossError(absl::StrCat("bad GoldfishGpu snapshot magic 0x",
                                                absl::Hex(magic), " (expected 0x",
                                                absl::Hex(kGpuMetaMagic), ")"));
    }
    if (version != kGpuMetaVersion) {
        return absl::FailedPreconditionError(
                absl::StrCat("unsupported GoldfishGpu snapshot version ", version, " (expected ",
                             kGpuMetaVersion, ")"));
    }

    const uint32_t asg_count = meta.getBe32();
    if (absl::Status st = meta.ReadStatus("GoldfishGpu asg_count"); !st.ok()) {
        return st;
    }
    if (asg_count > kGpuSnapshotMaxEntries) {
        return absl::DataLossError(absl::StrCat("GoldfishGpu snapshot asg_count ", asg_count,
                                                " exceeds ", kGpuSnapshotMaxEntries));
    }
    for (uint32_t i = 0; i < asg_count; ++i) {
        const uint32_t handle = meta.getBe32();
        if (absl::Status st =
                    meta.ReadStatus(absl::StrCat("GoldfishGpu ASG entry ", i, " of ", asg_count));
            !st.ok()) {
            return st;
        }
        if (absl::Status st = read_asg(handle, &meta); !st.ok()) {
            return st;
        }
        if (absl::Status st = meta.ReadStatus(
                    absl::StrCat("GoldfishGpu ASG consumer state for blob handle ", handle));
            !st.ok()) {
            return st;
        }
    }

    const uint32_t pipe_count = meta.getBe32();
    if (absl::Status st = meta.ReadStatus("GoldfishGpu pipe_count"); !st.ok()) {
        return st;
    }
    if (pipe_count > kGpuSnapshotMaxEntries) {
        return absl::DataLossError(absl::StrCat("GoldfishGpu snapshot pipe_count ", pipe_count,
                                                " exceeds ", kGpuSnapshotMaxEntries));
    }
    for (uint32_t i = 0; i < pipe_count; ++i) {
        const uint32_t id = meta.getBe32();
        if (absl::Status st = meta.ReadStatus(
                    absl::StrCat("GoldfishGpu pipe entry ", i, " of ", pipe_count));
            !st.ok()) {
            return st;
        }
        if (absl::Status st = read_pipe(id, &meta); !st.ok()) {
            return st;
        }
        if (absl::Status st = meta.ReadStatus(
                    absl::StrCat("GoldfishGpu render channel state for pipe ", id));
            !st.ok()) {
            return st;
        }
    }

    GpuSnapshotTail tail;
    tail.frame.width = meta.getBe32();
    tail.frame.height = meta.getBe32();
    const uint32_t frame_size = meta.getBe32();
    if (absl::Status st = meta.ReadStatus("GoldfishGpu frame header"); !st.ok()) {
        return st;
    }
    if (frame_size == 0) {
        if (tail.frame.width != 0 || tail.frame.height != 0) {
            return absl::DataLossError(
                    absl::StrCat("GoldfishGpu snapshot has an empty frame with non-zero dimensions ",
                                 tail.frame.width, "x", tail.frame.height));
        }
    } else {
        const uint64_t expected_size = GpuSnapshotFrameBytes(tail.frame.width, tail.frame.height);
        if (expected_size == 0) {
            return absl::DataLossError(absl::StrCat(
                    "GoldfishGpu snapshot frame dimensions ", tail.frame.width, "x",
                    tail.frame.height, " are out of range (1..", kGpuSnapshotMaxFrameDim, ")"));
        }
        if (frame_size != expected_size) {
            return absl::DataLossError(absl::StrCat(
                    "GoldfishGpu snapshot frame size ", frame_size, " does not match ",
                    tail.frame.width, "x", tail.frame.height, "x4 = ", expected_size));
        }
        if (frame_size > meta.Remaining()) {
            return absl::DataLossError(absl::StrCat("GoldfishGpu snapshot frame size ", frame_size,
                                                    " exceeds the ", meta.Remaining(),
                                                    " bytes left in the ", size, "-byte blob"));
        }
        tail.frame.bgra.resize(frame_size);
        if (meta.read(tail.frame.bgra.data(), frame_size) != static_cast<ssize_t>(frame_size)) {
            return meta.ReadStatus("GoldfishGpu frame buffer");
        }
    }
    tail.counters.next_asg_handle = meta.getBe32();
    tail.counters.next_pipe_id = meta.getBe32();
    tail.counters.next_puid = meta.getBe64();
    tail.counters.next_timeline_id = meta.getBe64();
    if (absl::Status st = meta.ReadStatus("GoldfishGpu counter footer"); !st.ok()) {
        return st;
    }
    if (meta.Remaining() != 0) {
        return absl::DataLossError(absl::StrCat("GoldfishGpu snapshot has ", meta.Remaining(),
                                                " unexpected trailing bytes in the ", size,
                                                "-byte blob"));
    }
    return tail;
}

}  // namespace goldfish::legacy
