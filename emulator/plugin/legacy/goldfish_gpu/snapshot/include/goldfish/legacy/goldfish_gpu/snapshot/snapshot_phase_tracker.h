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

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

// The snapshot phase of GoldfishGpu, as driven by goldfish_gpu_low_level.c:
//
//   kRunning --Quiesce--> kQuiesced --PreSave--> kSaved --PostSave--> kRunning
//   kRunning | kQuiesced | kSaved --PreLoad--> kPreLoaded --Load--> kPreLoaded
//                                                        --PostLoad--> kRunning
//
// Save() does not transition: it only reads the meta blob cached in kSaved.
// Quiesce()/PreLoad() are idempotent because the VM change-state handler and
// the "goldfish_gpu_begin" VMState section can both call them. The cached
// meta blob bridges the two save-side sections: the "goldfish_gpu_begin"
// section's pre_save computes it (gfxstream's ASG consumer save must precede
// the goldfish_address_space blob memory snapshot) and the later
// "goldfish_gpu_end" section's put emits it.
//
// The tracker is not thread-safe: the owner serializes access with its mutex.

namespace goldfish::legacy {

enum class SnapshotPhase {
    kRunning,
    kQuiesced,   // renderer paused, no meta blob yet
    kSaved,      // renderer paused, meta blob cached
    kPreLoaded,  // live consumers/channels destroyed, waiting for Load/PostLoad
};

std::string_view SnapshotPhaseName(SnapshotPhase phase);

class SnapshotPhaseTracker {
  public:
    SnapshotPhase phase() const { return phase_; }
    bool IsRunning() const { return phase_ == SnapshotPhase::kRunning; }
    // True while the renderer is paused by Quiesce() (kQuiesced or kSaved).
    bool IsQuiescedForSave() const {
        return phase_ == SnapshotPhase::kQuiesced || phase_ == SnapshotPhase::kSaved;
    }
    bool IsPreLoaded() const { return phase_ == SnapshotPhase::kPreLoaded; }

    // Moves to `to`. Returns false (and does nothing) when already in `to`, so
    // callers can make Quiesce()/PreLoad() idempotent. A transition from a
    // phase outside `allowed_from` is logged as an error but still performed:
    // the VMState sequence has already committed to it, and refusing would
    // leave the renderer paused. The cached meta blob is dropped on every
    // transition except kQuiesced -> kSaved.
    bool Enter(SnapshotPhase to, std::initializer_list<SnapshotPhase> allowed_from);

    // Back to kRunning; drops the cached meta blob.
    void Reset();

    void SetCachedMeta(std::vector<uint8_t> meta) { cached_meta_ = std::move(meta); }
    const std::vector<uint8_t>& CachedMeta() const { return cached_meta_; }

    // Number of illegal transitions Enter() has logged (for tests/verifier).
    uint64_t illegal_transitions() const { return illegal_transitions_; }

  private:
    SnapshotPhase phase_ = SnapshotPhase::kRunning;
    std::vector<uint8_t> cached_meta_;
    uint64_t illegal_transitions_ = 0;
};

}  // namespace goldfish::legacy
