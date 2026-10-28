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

#include "pcm_converter.h"

#include <algorithm>
#include <cstring>

namespace goldfish::wasapi {
namespace {

constexpr uint16_t kExtensibleMinCbSize = 22U;

}  // namespace

size_t BytesPerSample(PcmSampleFormat format) {
    switch (format) {
    case PcmSampleFormat::kU8:
    case PcmSampleFormat::kS8:
        return 1U;
    case PcmSampleFormat::kS16:
        return 2U;
    case PcmSampleFormat::kS24Packed:
        return 3U;
    case PcmSampleFormat::kS24In32:
    case PcmSampleFormat::kS32:
    case PcmSampleFormat::kF32:
        return 4U;
    case PcmSampleFormat::kUnknown:
    default:
        return 0U;
    }
}

uint8_t SilenceByte(PcmSampleFormat format) {
    return format == PcmSampleFormat::kU8 ? 0x80U : 0x00U;
}

std::string_view SampleFormatName(PcmSampleFormat format) {
    switch (format) {
    case PcmSampleFormat::kU8:
        return "u8";
    case PcmSampleFormat::kS8:
        return "s8";
    case PcmSampleFormat::kS16:
        return "s16";
    case PcmSampleFormat::kS24Packed:
        return "s24";
    case PcmSampleFormat::kS24In32:
        return "s24_32";
    case PcmSampleFormat::kS32:
        return "s32";
    case PcmSampleFormat::kF32:
        return "f32";
    case PcmSampleFormat::kUnknown:
    default:
        return "unknown";
    }
}

bool ParseWaveFormat(const RawWaveFormatFields& raw, PcmFormat* out) {
    if (out == nullptr) {
        return false;
    }
    *out = PcmFormat{};

    if (raw.channels == 0U || raw.channels > kMaxAudioChannels ||
        raw.samples_per_sec < kMinSampleRateHz || raw.samples_per_sec > kMaxSampleRateHz ||
        raw.bits_per_sample == 0U || (raw.bits_per_sample % 8U) != 0U) {
        return false;
    }

    const uint32_t expected_block_align =
            static_cast<uint32_t>(raw.channels) * (static_cast<uint32_t>(raw.bits_per_sample) / 8U);
    if (static_cast<uint32_t>(raw.block_align) != expected_block_align) {
        return false;
    }

    bool is_pcm = false;
    bool is_float = false;
    uint16_t valid_bits = raw.bits_per_sample;

    switch (raw.format_tag) {
    case kWaveFormatTagPcm:
        is_pcm = true;
        break;
    case kWaveFormatTagIeeeFloat:
        is_float = true;
        break;
    case kWaveFormatTagExtensible:
        if (raw.cb_size < kExtensibleMinCbSize) {
            return false;
        }
        is_pcm = raw.subformat_is_pcm;
        is_float = raw.subformat_is_ieee_float;
        if (raw.valid_bits_per_sample != 0U) {
            valid_bits = raw.valid_bits_per_sample;
        }
        break;
    default:
        return false;
    }

    if (valid_bits > raw.bits_per_sample) {
        return false;
    }

    PcmSampleFormat sample_fmt = PcmSampleFormat::kUnknown;
    if (is_float) {
        if (raw.bits_per_sample == 32U && valid_bits == 32U) {
            sample_fmt = PcmSampleFormat::kF32;
        }
    } else if (is_pcm) {
        switch (raw.bits_per_sample) {
        case 8U:
            sample_fmt = PcmSampleFormat::kU8;
            break;
        case 16U:
            sample_fmt = PcmSampleFormat::kS16;
            break;
        case 24U:
            sample_fmt = PcmSampleFormat::kS24Packed;
            break;
        case 32U:
            sample_fmt = (valid_bits == 24U) ? PcmSampleFormat::kS24In32 : PcmSampleFormat::kS32;
            break;
        default:
            break;
        }
    }

    if (sample_fmt == PcmSampleFormat::kUnknown) {
        return false;
    }

    out->format = sample_fmt;
    out->sample_rate = raw.samples_per_sec;
    out->channels = raw.channels;
    return out->IsValid();
}

void FillSilenceFrames(void* dst, size_t frames, const PcmFormat& fmt) {
    if (dst == nullptr || frames == 0U || !fmt.IsValid()) {
        return;
    }
    std::memset(dst, SilenceByte(fmt.format), frames * fmt.BytesPerFrame());
}

size_t ReadRingToHardwareChannelOnly(const uint8_t* ring_buf, size_t size_emul, size_t pos_emul,
                                     size_t* pending_emul, uint8_t* hw_dst, size_t hw_frames,
                                     const PcmFormat& sw_fmt, uint32_t hw_channels) {
    if (hw_dst == nullptr || hw_frames == 0U || !sw_fmt.IsValid() || hw_channels == 0U) {
        return 0U;
    }

    const PcmFormat hw_fmt{sw_fmt.format, sw_fmt.sample_rate, hw_channels};
    const size_t q_frame_size = sw_fmt.BytesPerFrame();
    const size_t h_frame_size = hw_fmt.BytesPerFrame();

    if (ring_buf == nullptr || pending_emul == nullptr || size_emul == 0U || q_frame_size == 0U) {
        FillSilenceFrames(hw_dst, hw_frames, hw_fmt);
        return 0U;
    }

    size_t pending = *pending_emul;
    const size_t avail_frames = pending / q_frame_size;
    const size_t frames_to_copy = std::min(hw_frames, avail_frames);
    size_t remaining_frames = frames_to_copy;
    uint8_t* out = hw_dst;
    const uint8_t silence = SilenceByte(sw_fmt.format);

    while (remaining_frames > 0U) {
        const size_t start = RingPosBack(pos_emul, pending, size_emul);
        const size_t contig_bytes = std::min(pending, size_emul - start);
        const size_t chunk_frames = std::min(remaining_frames, contig_bytes / q_frame_size);
        if (chunk_frames == 0U) {
            break;
        }

        const size_t read_bytes = chunk_frames * q_frame_size;
        const uint8_t* in = ring_buf + start;

        if (q_frame_size == h_frame_size) {
            std::memcpy(out, in, read_bytes);
            out += read_bytes;
        } else if (q_frame_size < h_frame_size) {
            const size_t pad_bytes = h_frame_size - q_frame_size;
            for (size_t i = 0U; i < chunk_frames; ++i) {
                std::memcpy(out, in, q_frame_size);
                in += q_frame_size;
                out += q_frame_size;
                std::memset(out, silence, pad_bytes);
                out += pad_bytes;
            }
        } else {
            for (size_t i = 0U; i < chunk_frames; ++i) {
                std::memcpy(out, in, h_frame_size);
                in += q_frame_size;
                out += h_frame_size;
            }
        }

        pending -= read_bytes;
        remaining_frames -= chunk_frames;
    }

    *pending_emul = pending;
    const size_t copied_frames = frames_to_copy - remaining_frames;
    if (copied_frames < hw_frames) {
        FillSilenceFrames(out, hw_frames - copied_frames, hw_fmt);
    }
    return copied_frames;
}

size_t WriteHardwareToRingChannelOnly(const uint8_t* hw_src, size_t hw_frames, bool hw_is_silent,
                                      uint8_t* ring_buf, size_t size_emul, size_t* pos_emul,
                                      size_t* pending_emul, const PcmFormat& sw_fmt,
                                      uint32_t hw_channels) {
    if (ring_buf == nullptr || pos_emul == nullptr || pending_emul == nullptr || size_emul == 0U ||
        hw_frames == 0U || !sw_fmt.IsValid() || hw_channels == 0U) {
        return 0U;
    }

    const size_t q_frame_size = sw_fmt.BytesPerFrame();
    const size_t h_frame_size = static_cast<size_t>(hw_channels) * sw_fmt.BytesPerSample();
    if (q_frame_size == 0U || h_frame_size == 0U || *pending_emul >= size_emul) {
        return 0U;
    }

    size_t pos = *pos_emul;
    size_t pending = *pending_emul;
    const size_t free_frames = (size_emul - pending) / q_frame_size;
    const size_t frames_to_write = std::min(hw_frames, free_frames);
    size_t remaining_frames = frames_to_write;
    const uint8_t* in = hw_src;
    const bool treat_as_silent = hw_is_silent || (hw_src == nullptr);
    const uint8_t silence = SilenceByte(sw_fmt.format);

    while (remaining_frames > 0U) {
        const size_t contig_frames = (size_emul - pos) / q_frame_size;
        const size_t chunk_frames = std::min(remaining_frames, contig_frames);
        if (chunk_frames == 0U) {
            break;
        }

        const size_t write_bytes = chunk_frames * q_frame_size;
        uint8_t* out = ring_buf + pos;

        if (treat_as_silent) {
            std::memset(out, silence, write_bytes);
        } else if (q_frame_size == h_frame_size) {
            std::memcpy(out, in, write_bytes);
            in += write_bytes;
        } else if (q_frame_size < h_frame_size) {
            for (size_t i = 0U; i < chunk_frames; ++i) {
                std::memcpy(out, in, q_frame_size);
                out += q_frame_size;
                in += h_frame_size;
            }
        } else {
            const size_t pad_bytes = q_frame_size - h_frame_size;
            for (size_t i = 0U; i < chunk_frames; ++i) {
                std::memcpy(out, in, h_frame_size);
                in += h_frame_size;
                out += h_frame_size;
                std::memset(out, silence, pad_bytes);
                out += pad_bytes;
            }
        }

        pos = (pos + write_bytes) % size_emul;
        pending += write_bytes;
        remaining_frames -= chunk_frames;
    }

    *pos_emul = pos;
    *pending_emul = pending;
    return frames_to_write - remaining_frames;
}

}  // namespace goldfish::wasapi
