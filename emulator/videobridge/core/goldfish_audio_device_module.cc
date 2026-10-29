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

#include "goldfish/videobridge/goldfish_audio_device_module.h"

#include <algorithm>
#include <utility>

namespace goldfish::videobridge {

int32_t GoldfishAudioDeviceModule::RegisterAudioCallback(::webrtc::AudioTransport* transport) {
    absl::MutexLock lock(mutex_);
    transport_ = transport;
    // Leftovers belong to the engine that produced them.
    block_offset_ = kBlockSamples;
    return 0;
}

size_t GoldfishAudioDeviceModule::PullPlayout(std::span<int16_t> dst) {
    absl::MutexLock lock(mutex_);
    if (transport_ == nullptr) {
        return 0;
    }

    size_t written = 0;
    while (written < dst.size()) {
        if (block_offset_ == kBlockSamples) {
            size_t samples_out = 0;
            int64_t elapsed_time_ms = 0;
            int64_t ntp_time_ms = 0;
            transport_->NeedMorePlayData(kBlockSamples / kPlayoutChannels,
                                         sizeof(int16_t) * kPlayoutChannels, kPlayoutChannels,
                                         kPlayoutSampleRateHz, block_.data(), samples_out,
                                         &elapsed_time_ms, &ntp_time_ms);
            // A short block is padded with silence so the stream keeps its timing.
            std::fill(block_.begin() + std::min(samples_out, kBlockSamples), block_.end(), 0);
            block_offset_ = 0;
        }
        const size_t n = std::min(dst.size() - written, kBlockSamples - block_offset_);
        std::copy_n(block_.begin() + block_offset_, n, dst.begin() + written);
        block_offset_ += n;
        written += n;
    }
    return written;
}

void GoldfishAudioDeviceModule::SetPlayoutListener(PlayoutListener listener) {
    absl::MutexLock lock(playout_mutex_);
    listener_ = std::move(listener);
    if (listener_ && playing_) {
        listener_(true);
    }
}

int32_t GoldfishAudioDeviceModule::StartPlayout() {
    SetPlaying(true);
    return 0;
}

int32_t GoldfishAudioDeviceModule::StopPlayout() {
    SetPlaying(false);
    return 0;
}

bool GoldfishAudioDeviceModule::Playing() const {
    absl::MutexLock lock(playout_mutex_);
    return playing_;
}

void GoldfishAudioDeviceModule::SetPlaying(bool playing) {
    absl::MutexLock lock(playout_mutex_);
    // WebRTC stops playout more than once when a session closes.
    if (playing_ == playing) {
        return;
    }
    playing_ = playing;
    if (listener_) {
        listener_(playing);
    }
}

}  // namespace goldfish::videobridge
