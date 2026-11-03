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

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace goldfish::legacy {
namespace {

using P = SnapshotPhase;

TEST(SnapshotPhaseTrackerTest, SaveSequence) {
    SnapshotPhaseTracker t;
    EXPECT_TRUE(t.IsRunning());
    EXPECT_TRUE(t.Enter(P::kQuiesced, {P::kRunning}));
    EXPECT_TRUE(t.IsQuiescedForSave());
    EXPECT_FALSE(t.Enter(P::kQuiesced, {P::kRunning})) << "second Quiesce is a no-op";
    EXPECT_TRUE(t.Enter(P::kSaved, {P::kQuiesced}));
    t.SetCachedMeta({1, 2, 3});
    EXPECT_FALSE(t.Enter(P::kSaved, {P::kQuiesced})) << "second Save returns the cached blob";
    EXPECT_EQ(t.CachedMeta(), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_TRUE(t.IsQuiescedForSave());
    t.Reset();
    EXPECT_TRUE(t.IsRunning());
    EXPECT_TRUE(t.CachedMeta().empty());
    EXPECT_EQ(t.illegal_transitions(), 0U);
}

TEST(SnapshotPhaseTrackerTest, LoadSequenceFromRunningAndFromQuiesced) {
    SnapshotPhaseTracker t;
    EXPECT_TRUE(t.Enter(P::kPreLoaded, {P::kRunning, P::kQuiesced, P::kSaved}));
    EXPECT_TRUE(t.IsPreLoaded());
    EXPECT_FALSE(t.IsQuiescedForSave());
    EXPECT_FALSE(t.Enter(P::kPreLoaded, {P::kRunning})) << "second PreLoad is a no-op";
    t.Reset();

    // vm_stop(RESTORE_VM) quiesces first, then the quiesce section's pre_load runs.
    EXPECT_TRUE(t.Enter(P::kQuiesced, {P::kRunning}));
    EXPECT_TRUE(t.Enter(P::kSaved, {P::kQuiesced}));
    t.SetCachedMeta({9});
    EXPECT_TRUE(t.Enter(P::kPreLoaded, {P::kRunning, P::kQuiesced, P::kSaved}));
    EXPECT_TRUE(t.CachedMeta().empty()) << "a stale save blob must not survive into a load";
    EXPECT_EQ(t.illegal_transitions(), 0U);
}

TEST(SnapshotPhaseTrackerTest, IllegalTransitionIsCountedButPerformed) {
    SnapshotPhaseTracker t;
    // Save without Quiesce: today's behaviour is to log and carry on.
    EXPECT_TRUE(t.Enter(P::kSaved, {P::kQuiesced}));
    EXPECT_EQ(t.phase(), P::kSaved);
    EXPECT_EQ(t.illegal_transitions(), 1U);
}

TEST(SnapshotPhaseTrackerTest, CachedMetaIsDroppedOnEveryTransitionExceptQuiescedToSaved) {
    SnapshotPhaseTracker t;
    t.Enter(P::kQuiesced, {P::kRunning});
    t.SetCachedMeta({1});
    t.Enter(P::kSaved, {P::kQuiesced});
    EXPECT_FALSE(t.CachedMeta().empty());
    t.Enter(P::kRunning, {P::kSaved});
    EXPECT_TRUE(t.CachedMeta().empty());
}

TEST(SnapshotPhaseTrackerTest, Names) {
    EXPECT_EQ(SnapshotPhaseName(P::kRunning), "RUNNING");
    EXPECT_EQ(SnapshotPhaseName(P::kQuiesced), "QUIESCED");
    EXPECT_EQ(SnapshotPhaseName(P::kSaved), "SAVED");
    EXPECT_EQ(SnapshotPhaseName(P::kPreLoaded), "PRE_LOADED");
}

}  // namespace
}  // namespace goldfish::legacy
