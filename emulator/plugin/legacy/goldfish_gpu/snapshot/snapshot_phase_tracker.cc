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

#include "goldfish/legacy/goldfish_gpu/snapshot/snapshot_phase_tracker.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "absl/log/log.h"

namespace goldfish::legacy {

std::string_view SnapshotPhaseName(SnapshotPhase phase) {
    switch (phase) {
    case SnapshotPhase::kRunning:
        return "RUNNING";
    case SnapshotPhase::kQuiesced:
        return "QUIESCED";
    case SnapshotPhase::kSaved:
        return "SAVED";
    case SnapshotPhase::kPreLoaded:
        return "PRE_LOADED";
    }
    return "UNKNOWN";
}

bool SnapshotPhaseTracker::Enter(SnapshotPhase to,
                                 std::initializer_list<SnapshotPhase> allowed_from) {
    if (phase_ == to) {
        return false;
    }
    if (std::find(allowed_from.begin(), allowed_from.end(), phase_) == allowed_from.end()) {
        ++illegal_transitions_;
        LOG(ERROR) << "goldfish_gpu: snapshot phase " << SnapshotPhaseName(phase_) << " -> "
                   << SnapshotPhaseName(to) << " is not a legal transition; continuing";
    }
    if (!(phase_ == SnapshotPhase::kQuiesced && to == SnapshotPhase::kSaved)) {
        cached_meta_.clear();
    }
    phase_ = to;
    return true;
}

void SnapshotPhaseTracker::Reset() {
    phase_ = SnapshotPhase::kRunning;
    cached_meta_.clear();
}

}  // namespace goldfish::legacy
