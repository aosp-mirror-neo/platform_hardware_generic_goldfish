// Copyright (C) 2026 The Android Open Source Project
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

#include "goldfish/audio/virtual_audio_pacer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "absl/time/time.h"

namespace goldfish::audio {
namespace {

class VirtualAudioPacerTest : public ::testing::Test {
  protected:
    absl::Duration now_ = absl::Seconds(100);
    VirtualAudioPacer pacer_;
};

TEST_F(VirtualAudioPacerTest, UnconfiguredPacerReturnsZeroBytes) {
    now_ += absl::Milliseconds(50);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 0U);
}

TEST_F(VirtualAudioPacerTest, ComputesExactFrameAlignedQuotaAcross10MsTicks) {
    // 48 kHz stereo S16 = 4 bytes/frame -> 480 frames (1920 bytes) per 10 ms.
    pacer_.Configure(48000, 4, now_);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 0U);

    now_ += absl::Milliseconds(10);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 1920U);

    pacer_.Consume(1920U);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 0U);

    now_ += absl::Milliseconds(10);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 1920U);
}

TEST_F(VirtualAudioPacerTest, PreservesSubFrameRemainderWithoutCumulativeDrift) {
    // 44.1 kHz mono S16 = 2 bytes/frame. At 1 ms steps (44.1 frames) the fraction must carry over.
    pacer_.Configure(44100, 2, now_);

    size_t total_bytes = 0;
    for (int step = 0; step < 1000; ++step) {
        now_ += absl::Milliseconds(1);
        const size_t avail = pacer_.AvailableBytes(now_);
        EXPECT_EQ(avail % 2U, 0U);
        pacer_.Consume(avail);
        total_bytes += avail;
    }

    // Exactly 1.000 second at 44100 Hz * 2 bytes/frame.
    EXPECT_EQ(total_bytes, 88200U);
}

TEST_F(VirtualAudioPacerTest, PartialConsumptionRetainsUnreadQuota) {
    pacer_.Configure(16000, 2, now_);  // 160 frames (320 bytes) per 10 ms.

    now_ += absl::Milliseconds(10);
    ASSERT_EQ(pacer_.AvailableBytes(now_), 320U);

    pacer_.Consume(120U);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 200U);
}

TEST_F(VirtualAudioPacerTest, BackwardClockJumpOnSnapshotRestoreReanchorsWithoutStall) {
    pacer_.Configure(48000, 2, now_);  // 960 bytes per 10 ms.

    now_ += absl::Milliseconds(10);
    ASSERT_EQ(pacer_.AvailableBytes(now_), 960U);
    pacer_.Consume(960U);

    // Snapshot load rewinds QEMU_CLOCK_VIRTUAL by 50 seconds.
    now_ -= absl::Seconds(50);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 0U);

    // The next 10 ms tick from the restored epoch produces a full quota.
    now_ += absl::Milliseconds(10);
    EXPECT_EQ(pacer_.AvailableBytes(now_), 960U);
}

TEST_F(VirtualAudioPacerTest, LongStallOwesAtMostMaxBacklogAtAnySampleRate) {
    for (const uint32_t rate : {8000U, 48000U}) {
        pacer_.Configure(rate, 2, now_);
        const size_t max_backlog_bytes =
                static_cast<size_t>(rate * absl::ToDoubleSeconds(VirtualAudioPacer::kMaxBacklog)) *
                2U;

        // A 10 second stall only owes kMaxBacklog worth of audio.
        now_ += absl::Seconds(10);
        EXPECT_EQ(pacer_.AvailableBytes(now_), max_backlog_bytes) << rate << " Hz";

        // Draining it resumes normal real-time pacing.
        pacer_.Consume(max_backlog_bytes);
        EXPECT_EQ(pacer_.AvailableBytes(now_), 0U);
        now_ += absl::Milliseconds(10);
        EXPECT_EQ(pacer_.AvailableBytes(now_), rate / 100U * 2U) << rate << " Hz";
    }
}

}  // namespace
}  // namespace goldfish::audio
