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
#pragma once

#include <cstddef>
#include <cstdint>

#include "absl/time/time.h"

namespace goldfish::audio {

/**
 * Turns elapsed virtual time into a byte budget so pull-based injection delivers audio in real
 * time instead of overrunning the guest's capture ring buffer.
 *
 * Usage: on every read, ask `AvailableBytes(now)` how much is due, deliver at most that much, then
 * report it with `Consume()`.
 *
 * Invariants:
 * - Returned byte counts are always whole frames.
 * - Budgets are computed from the elapsed time since the anchor (128-bit math), so fractional
 *   frame rates such as 44.1 kHz never drift.
 * - A backward clock jump (snapshot load) re-anchors to the new time.
 * - At most `kMaxBacklog` of unconsumed audio is owed; older debt is dropped so a stalled guest
 *   never receives a long burst.
 *
 * Thread Safety: Not synchronized. Callers serialize access (the VM lock in `AudioInputMux`).
 */
class VirtualAudioPacer {
  public:
    /**
     * Maximum amount of unconsumed audio the pacer keeps owing to the guest.
     */
    static constexpr absl::Duration kMaxBacklog = absl::Milliseconds(200);

    /**
     * Sets the stream geometry and anchors the budget at @p now.
     *
     * @param sample_rate_hz Stream sample rate in Hz (`> 0`).
     * @param bytes_per_frame Size of one interleaved PCM frame in bytes (`> 0`).
     * @param now Current virtual time.
     */
    void Configure(uint32_t sample_rate_hz, uint32_t bytes_per_frame, absl::Duration now);

    /**
     * Re-anchors the budget at @p now so nothing is owed.
     */
    void Reset(absl::Duration now);

    /**
     * Returns the whole-frame bytes due at @p now that have not been consumed, capped at
     * `kMaxBacklog`. Returns `0` before `Configure()`.
     */
    size_t AvailableBytes(absl::Duration now);

    /**
     * Records @p bytes as delivered to the guest.
     */
    void Consume(size_t bytes);

  private:
    uint64_t BytesFor(absl::Duration elapsed) const;

    absl::Duration anchor_ = absl::ZeroDuration();
    uint64_t consumed_bytes_ = 0;
    uint32_t sample_rate_hz_ = 0;
    uint32_t bytes_per_frame_ = 0;
};

}  // namespace goldfish::audio
