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

#include <cstddef>
#include <cstdint>

#include "absl/log/check.h"
#include "absl/numeric/int128.h"
#include "absl/time/time.h"

namespace goldfish::audio {

void VirtualAudioPacer::Configure(uint32_t sample_rate_hz, uint32_t bytes_per_frame,
                                  absl::Duration now) {
    DCHECK_GT(sample_rate_hz, 0U);
    DCHECK_GT(bytes_per_frame, 0U);
    sample_rate_hz_ = sample_rate_hz;
    bytes_per_frame_ = bytes_per_frame;
    Reset(now);
}

void VirtualAudioPacer::Reset(absl::Duration now) {
    anchor_ = now;
    consumed_bytes_ = 0;
}

uint64_t VirtualAudioPacer::BytesFor(absl::Duration elapsed) const {
    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    const auto elapsed_ns = static_cast<uint64_t>(absl::ToInt64Nanoseconds(elapsed));
    const uint64_t bytes_per_second = uint64_t{sample_rate_hz_} * bytes_per_frame_;
    return static_cast<uint64_t>(absl::uint128(elapsed_ns) * bytes_per_second /
                                 kNanosecondsPerSecond);
}

size_t VirtualAudioPacer::AvailableBytes(absl::Duration now) {
    if (bytes_per_frame_ == 0) {
        return 0;
    }
    if (now < anchor_) {
        Reset(now);
        return 0;
    }

    const uint64_t due_bytes = BytesFor(now - anchor_);
    const uint64_t max_backlog_bytes = BytesFor(kMaxBacklog) / bytes_per_frame_ * bytes_per_frame_;
    if (due_bytes > consumed_bytes_ + max_backlog_bytes) {
        // Forgive debt older than kMaxBacklog while keeping the anchor, so timing stays exact.
        consumed_bytes_ = due_bytes - max_backlog_bytes;
    }
    const uint64_t owed_bytes = due_bytes > consumed_bytes_ ? due_bytes - consumed_bytes_ : 0;
    return static_cast<size_t>(owed_bytes / bytes_per_frame_ * bytes_per_frame_);
}

void VirtualAudioPacer::Consume(size_t bytes) {
    consumed_bytes_ += bytes;
}

}  // namespace goldfish::audio
