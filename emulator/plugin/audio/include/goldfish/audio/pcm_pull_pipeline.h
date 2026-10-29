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
#include <functional>
#include <memory>
#include <vector>

#include "goldfish/audio/pcm_format.h"

namespace goldfish::audio {

/**
 * Pull-based audio conversion pipeline that adapts an external `AudioStreamConfig` source into a
 * guest `GuestAudioConfig` destination with sample-format conversion, channel remixing, guest
 * volume scaling, underrun silence padding, and windowed-sinc sample-rate conversion.
 *
 * Data Flow: Callers invoke `Pull()` with the desired number of guest frames. When source and
 * destination formats match and volume is nominal, `Pull()` writes directly into the caller's
 * buffer with zero intermediate copies. Otherwise, `Pull()` pulls source frames in bounded scratch
 * blocks, silence-pads underruns, downmixes excess channels prior to resampling, resamples via
 * `webrtc::SincResampler`, upmixes remaining channels, applies guest volume, and encodes target
 * PCM bytes.
 *
 * Thread Safety: Not internally synchronized. Callers must serialize calls to `Pull()`,
 * `SetVolume()`, and `Reset()`.
 */
class PcmPullPipeline {
  public:
    /**
     * Pulls up to `max_frames` interleaved source frames into `dst_buffer` and returns the number
     * of frames written (`<= max_frames`).
     */
    using PullSourceFn = std::function<size_t(void* dst_buffer, size_t max_frames)>;

    /**
     * Constructs a pull conversion pipeline from @p source_config to @p target_config and
     * pre-allocates internal conversion and resampler buffers.
     *
     * @param source_config Injected source PCM format (`sample_rate_hz > 0`, `1 <= channels <=
     * 16`).
     * @param target_config Guest capture PCM format (`sample_rate_hz > 0`, `1 <= channels <= 16`).
     */
    PcmPullPipeline(AudioStreamConfig source_config, GuestAudioConfig target_config);
    ~PcmPullPipeline();

    PcmPullPipeline(const PcmPullPipeline&) = delete;
    PcmPullPipeline& operator=(const PcmPullPipeline&) = delete;

    /**
     * Updates the guest per-channel gain and mute settings applied to converted output.
     */
    void SetVolume(const GuestChannelVolume& volume);

    /**
     * Flushes internal resampler delay lines and staged multi-channel frames.
     */
    void Reset();

    /**
     * Produces exactly @p target_frames interleaved guest PCM frames into @p dst_buffer by pulling
     * from @p pull_source as needed and silence-padding any source underrun.
     *
     * When @p input_muted is `true` (or @p input_gain `<= 0.0f`), `Pull()` still advances
     * @p pull_source and any internal resamplers at `1.0x` so active streams never stall or
     * accumulate lag, while outputting exact guest-format silence.
     *
     * @param target_frames Number of guest frames to write into @p dst_buffer.
     * @param dst_buffer Output buffer of size `target_frames * target_bytes_per_frame()` bytes.
     * @param pull_source Callback supplying raw source frames on demand.
     * @param input_muted When true, silences output after consuming source frames.
     * @param input_gain Linear amplitude multiplier (`1.0f` = unity) applied before clamping.
     */
    void Pull(size_t target_frames, void* dst_buffer, const PullSourceFn& pull_source,
              bool input_muted = false, float input_gain = 1.0F);

    const AudioStreamConfig& source_config() const { return source_config_; }
    const GuestAudioConfig& target_config() const { return target_config_; }
    size_t target_bytes_per_frame() const {
        return static_cast<size_t>(target_config_.channels) *
               BytesPerGuestSample(target_config_.format);
    }

  private:
    class ChannelResamplerState;

    bool CanUseDirectPassthrough(bool input_muted, float input_gain) const;
    void PullConvertedChunk(size_t chunk_frames, uint8_t* dst_bytes,
                            const PullSourceFn& pull_source, bool input_muted, float input_gain);
    void PullSourceDecodedPlanar(size_t frames, const PullSourceFn& pull_source,
                                 std::vector<std::vector<float>>& planar_mid);

    AudioStreamConfig source_config_;
    GuestAudioConfig target_config_;
    GuestChannelVolume volume_;
    uint32_t mid_channels_ = 1;
    std::vector<uint8_t> source_scratch_;
    std::vector<float> source_float_interleaved_;
    std::vector<std::vector<float>> mid_planar_scratch_;
    std::unique_ptr<ChannelResamplerState> resampler_state_;
};

}  // namespace goldfish::audio
