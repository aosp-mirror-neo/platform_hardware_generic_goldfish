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

#include "goldfish/audio/pcm_pull_pipeline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "absl/log/check.h"

#include "common_audio/resampler/sinc_resampler.h"

namespace goldfish::audio {
namespace {

constexpr size_t kDefaultChunkFrames = 512;
constexpr size_t kMinResamplerChunkFrames = 64;

void DecodeInterleavedToFloat(SampleFormat format, const uint8_t* src, size_t sample_count,
                              float* dst) {
    switch (format) {
    case SampleFormat::kU8:
        for (size_t i = 0; i < sample_count; ++i) {
            dst[i] = static_cast<float>(static_cast<int>(src[i]) - 128) * (1.0F / 128.0F);
        }
        return;
    case SampleFormat::kS16:
        for (size_t i = 0; i < sample_count; ++i) {
            int16_t s = 0;
            std::memcpy(&s, src + i * sizeof(int16_t), sizeof(int16_t));
            dst[i] = static_cast<float>(s) * (1.0F / 32768.0F);
        }
        return;
    case SampleFormat::kS32:
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t s = 0;
            std::memcpy(&s, src + i * sizeof(int32_t), sizeof(int32_t));
            dst[i] = static_cast<float>(static_cast<double>(s) * (1.0 / 2147483648.0));
        }
        return;
    case SampleFormat::kF32:
        for (size_t i = 0; i < sample_count; ++i) {
            float s = 0.0F;
            std::memcpy(&s, src + i * sizeof(float), sizeof(float));
            dst[i] = std::isnan(s) ? 0.0F : std::clamp(s, -1.0F, 1.0F);
        }
        return;
    }
}

}  // namespace

class PcmPullPipeline::ChannelResamplerState {
  public:
    ChannelResamplerState(PcmPullPipeline* owner, uint32_t channels, double io_ratio,
                          size_t request_frames)
            : owner_(owner)
            , request_frames_(request_frames)
            , staged_planar_(channels, std::vector<float>(request_frames, 0.0F)) {
        callbacks_.reserve(channels);
        resamplers_.reserve(channels);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            callbacks_.push_back(std::make_unique<ChannelCallback>(this, ch));
            resamplers_.push_back(std::make_unique<webrtc::SincResampler>(io_ratio, request_frames_,
                                                                          callbacks_.back().get()));
        }
    }

    size_t MaxStepFrames() const { return std::max<size_t>(1, resamplers_[0]->ChunkSize() / 2); }

    void Flush() {
        staged_valid_ = false;
        for (auto& resampler : resamplers_) {
            resampler->Flush();
        }
    }

    void ResampleStep(size_t step_frames, const PullSourceFn& pull_source,
                      std::vector<std::vector<float>>& dst_planar) {
        active_pull_fn_ = &pull_source;
        staged_valid_ = false;
        for (size_t ch = 0; ch < resamplers_.size(); ++ch) {
            resamplers_[ch]->Resample(step_frames, dst_planar[ch].data());
        }
        active_pull_fn_ = nullptr;
        staged_valid_ = false;
    }

  private:
    class ChannelCallback final : public webrtc::SincResamplerCallback {
      public:
        ChannelCallback(ChannelResamplerState* state, uint32_t channel)
                : state_(state), channel_(channel) {}

        void Run(size_t frames, float* destination) override {
            state_->OnChannelRefill(channel_, frames, destination);
        }

      private:
        ChannelResamplerState* state_;
        uint32_t channel_;
    };

    void OnChannelRefill(uint32_t channel, size_t frames, float* destination) {
        DCHECK_LE(frames, request_frames_);
        DCHECK(active_pull_fn_ != nullptr);
        if (channel == 0) {
            DCHECK(!staged_valid_);
            owner_->PullSourceDecodedPlanar(frames, *active_pull_fn_, staged_planar_);
            staged_valid_ = true;
        } else {
            DCHECK(staged_valid_);
        }
        std::memcpy(destination, staged_planar_[channel].data(), frames * sizeof(float));
    }

    PcmPullPipeline* owner_;
    size_t request_frames_;
    std::vector<std::vector<float>> staged_planar_;
    std::vector<std::unique_ptr<ChannelCallback>> callbacks_;
    std::vector<std::unique_ptr<webrtc::SincResampler>> resamplers_;
    const PullSourceFn* active_pull_fn_ = nullptr;
    bool staged_valid_ = false;
};

PcmPullPipeline::PcmPullPipeline(AudioStreamConfig source_config, GuestAudioConfig target_config)
        : source_config_(source_config)
        , target_config_(target_config)
        , mid_channels_(std::min(source_config.channels, target_config.channels)) {
    DCHECK_GT(source_config_.sample_rate_hz, 0U);
    DCHECK_GT(target_config_.sample_rate_hz, 0U);
    DCHECK_GE(source_config_.channels, 1U);
    DCHECK_LE(source_config_.channels, kMaxAudioChannels);
    DCHECK_GE(target_config_.channels, 1U);
    DCHECK_LE(target_config_.channels, kMaxAudioChannels);

    volume_.channels = target_config_.channels;

    size_t max_source_pull_frames = kDefaultChunkFrames;
    if (source_config_.sample_rate_hz != target_config_.sample_rate_hz) {
        const double io_ratio = static_cast<double>(source_config_.sample_rate_hz) /
                                static_cast<double>(target_config_.sample_rate_hz);
        const size_t min_for_ratio =
                static_cast<size_t>(
                        std::ceil(io_ratio * static_cast<double>(kMinResamplerChunkFrames))) +
                webrtc::SincResampler::kKernelSize;
        const size_t request_frames = std::max(
                static_cast<size_t>(webrtc::SincResampler::kDefaultRequestSize), min_for_ratio);
        resampler_state_ = std::make_unique<ChannelResamplerState>(this, mid_channels_, io_ratio,
                                                                   request_frames);
        max_source_pull_frames = std::max(max_source_pull_frames, request_frames);
    }

    const size_t src_bpf =
            static_cast<size_t>(source_config_.channels) * BytesPerSample(source_config_.format);
    source_scratch_.resize(max_source_pull_frames * src_bpf, 0);
    source_float_interleaved_.resize(
            max_source_pull_frames * static_cast<size_t>(source_config_.channels), 0.0F);

    const size_t max_output_chunk =
            resampler_state_ ? std::min(kDefaultChunkFrames, resampler_state_->MaxStepFrames())
                             : kDefaultChunkFrames;
    mid_planar_scratch_.assign(mid_channels_, std::vector<float>(max_output_chunk, 0.0F));
}

PcmPullPipeline::~PcmPullPipeline() = default;

void PcmPullPipeline::SetVolume(const GuestChannelVolume& volume) {
    volume_ = volume;
}

void PcmPullPipeline::Reset() {
    if (resampler_state_) {
        resampler_state_->Flush();
    }
}

bool PcmPullPipeline::CanUseDirectPassthrough(bool input_muted, float input_gain) const {
    if (input_muted || input_gain != 1.0F ||
        source_config_.sample_rate_hz != target_config_.sample_rate_hz ||
        source_config_.channels != target_config_.channels ||
        target_config_.big_endian != (std::endian::native == std::endian::big) ||
        !volume_.IsNominal()) {
        return false;
    }

    switch (source_config_.format) {
    case SampleFormat::kU8:
        return target_config_.format == GuestPcmFormat::kU8;
    case SampleFormat::kS16:
        return target_config_.format == GuestPcmFormat::kS16;
    case SampleFormat::kS32:
        return target_config_.format == GuestPcmFormat::kS32;
    case SampleFormat::kF32:
        return target_config_.format == GuestPcmFormat::kF32;
    }
    return false;
}

void PcmPullPipeline::Pull(size_t target_frames, void* dst_buffer, const PullSourceFn& pull_source,
                           bool input_muted, float input_gain) {
    if (target_frames == 0) {
        return;
    }

    auto* dst_bytes = static_cast<uint8_t*>(dst_buffer);
    const size_t dst_bpf = target_bytes_per_frame();

    if (CanUseDirectPassthrough(input_muted, input_gain)) {
        const size_t got = std::min(target_frames, pull_source(dst_buffer, target_frames));
        if (got < target_frames) {
            FillGuestSilence(target_config_.format, target_config_.big_endian,
                             dst_bytes + got * dst_bpf, (target_frames - got) * dst_bpf);
        }
        return;
    }

    const size_t max_step = mid_planar_scratch_[0].size();
    size_t remaining = target_frames;
    while (remaining > 0) {
        const size_t step = std::min(remaining, max_step);
        PullConvertedChunk(step, dst_bytes, pull_source, input_muted, input_gain);
        dst_bytes += step * dst_bpf;
        remaining -= step;
    }
}

void PcmPullPipeline::PullConvertedChunk(size_t chunk_frames, uint8_t* dst_bytes,
                                         const PullSourceFn& pull_source, bool input_muted,
                                         float input_gain) {
    if (resampler_state_) {
        resampler_state_->ResampleStep(chunk_frames, pull_source, mid_planar_scratch_);
    } else {
        PullSourceDecodedPlanar(chunk_frames, pull_source, mid_planar_scratch_);
    }

    if (volume_.mute || input_muted || !std::isfinite(input_gain) || input_gain <= 0.0F) {
        FillGuestSilence(target_config_.format, target_config_.big_endian, dst_bytes,
                         chunk_frames * target_bytes_per_frame());
        return;
    }

    const uint32_t dst_channels = target_config_.channels;
    const size_t dst_bps = BytesPerGuestSample(target_config_.format);
    const bool swap_endian = target_config_.big_endian != (std::endian::native == std::endian::big);

    std::array<float, kMaxAudioChannels> gains{};
    for (uint32_t ch = 0; ch < dst_channels; ++ch) {
        gains[ch] = (static_cast<float>(volume_.gain[ch]) / 255.0F) * input_gain;
    }

    uint8_t* cursor = dst_bytes;
    for (size_t i = 0; i < chunk_frames; ++i) {
        for (uint32_t ch = 0; ch < dst_channels; ++ch) {
            const float sample = mid_planar_scratch_[ch % mid_channels_][i] * gains[ch];
            EncodeGuestSample(target_config_.format, swap_endian, sample, cursor);
            cursor += dst_bps;
        }
    }
}

void PcmPullPipeline::PullSourceDecodedPlanar(size_t frames, const PullSourceFn& pull_source,
                                              std::vector<std::vector<float>>& planar_mid) {
    const uint32_t src_channels = source_config_.channels;
    const size_t src_bpf =
            static_cast<size_t>(src_channels) * BytesPerSample(source_config_.format);
    DCHECK_LE(frames * src_bpf, source_scratch_.size());

    const size_t got = std::min(frames, pull_source(source_scratch_.data(), frames));
    if (got < frames) {
        FillSourceSilence(source_config_.format, source_scratch_.data() + got * src_bpf,
                          (frames - got) * src_bpf);
    }

    const size_t total_samples = frames * static_cast<size_t>(src_channels);
    DecodeInterleavedToFloat(source_config_.format, source_scratch_.data(), total_samples,
                             source_float_interleaved_.data());

    if (mid_channels_ == src_channels) {
        for (size_t i = 0; i < frames; ++i) {
            for (uint32_t ch = 0; ch < mid_channels_; ++ch) {
                planar_mid[ch][i] = source_float_interleaved_[i * src_channels + ch];
            }
        }
        return;
    }

    if (mid_channels_ == 1) {
        const float inv_channels = 1.0F / static_cast<float>(src_channels);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.0F;
            for (uint32_t ch = 0; ch < src_channels; ++ch) {
                sum += source_float_interleaved_[i * src_channels + ch];
            }
            planar_mid[0][i] = sum * inv_channels;
        }
        return;
    }

    std::array<float, kMaxAudioChannels> inv_contributors{};
    for (uint32_t ch = 0; ch < mid_channels_; ++ch) {
        uint32_t count = 0;
        for (uint32_t src_ch = ch; src_ch < src_channels; src_ch += mid_channels_) {
            ++count;
        }
        inv_contributors[ch] = count > 0 ? (1.0F / static_cast<float>(count)) : 0.0F;
    }

    for (size_t i = 0; i < frames; ++i) {
        for (uint32_t ch = 0; ch < mid_channels_; ++ch) {
            float sum = 0.0F;
            for (uint32_t src_ch = ch; src_ch < src_channels; src_ch += mid_channels_) {
                sum += source_float_interleaved_[i * src_channels + src_ch];
            }
            planar_mid[ch][i] = sum * inv_contributors[ch];
        }
    }
}

}  // namespace goldfish::audio
