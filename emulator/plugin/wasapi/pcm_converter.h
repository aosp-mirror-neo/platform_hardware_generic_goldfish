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

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace goldfish::wasapi {

constexpr uint32_t kMinSampleRateHz = 8000U;
constexpr uint32_t kMaxSampleRateHz = 384000U;
constexpr uint32_t kMaxAudioChannels = 32U;

constexpr uint16_t kWaveFormatTagPcm = 0x0001U;
constexpr uint16_t kWaveFormatTagIeeeFloat = 0x0003U;
constexpr uint16_t kWaveFormatTagExtensible = 0xFFFEU;

enum class PcmSampleFormat : uint8_t {
    kUnknown = 0,
    kU8,
    kS8,
    kS16,
    kS24Packed,
    kS24In32,
    kS32,
    kF32,
};

size_t BytesPerSample(PcmSampleFormat format);
uint8_t SilenceByte(PcmSampleFormat format);
std::string_view SampleFormatName(PcmSampleFormat format);

struct PcmFormat {
    PcmSampleFormat format = PcmSampleFormat::kUnknown;
    uint32_t sample_rate = 0U;
    uint32_t channels = 0U;

    size_t BytesPerSample() const { return ::goldfish::wasapi::BytesPerSample(format); }
    size_t BytesPerFrame() const { return static_cast<size_t>(channels) * BytesPerSample(); }
    bool IsValid() const {
        return format != PcmSampleFormat::kUnknown && sample_rate >= kMinSampleRateHz &&
               sample_rate <= kMaxSampleRateHz && channels >= 1U && channels <= kMaxAudioChannels;
    }
};

// Platform-neutral projection of Windows WAVEFORMATEX / WAVEFORMATEXTENSIBLE
// fields so format parsing and channel adaptation can be unit-tested on all hosts.
struct RawWaveFormatFields {
    uint16_t format_tag = 0U;
    uint16_t channels = 0U;
    uint32_t samples_per_sec = 0U;
    uint16_t block_align = 0U;
    uint16_t bits_per_sample = 0U;
    uint16_t cb_size = 0U;
    uint16_t valid_bits_per_sample = 0U;
    bool subformat_is_pcm = false;
    bool subformat_is_ieee_float = false;
};

bool ParseWaveFormat(const RawWaveFormatFields& raw, PcmFormat* out);

// Fills `frames` of interleaved audio in `dst` with format-appropriate silence.
void FillSilenceFrames(void* dst, size_t frames, const PcmFormat& fmt);

// Walks `dist` bytes backward from `pos` in a ring buffer of size `size`
// (equivalent to QEMU's audio_ring_posb).
inline size_t RingPosBack(size_t pos, size_t dist, size_t size) {
    return pos >= dist ? (pos - dist) : (size - dist + pos);
}

// Drains up to `hw_frames` from QEMU's output ring buffer into `hw_dst`, adapting
// channel counts (`sw_fmt.channels` -> `hw_channels`) and filling any underrun
// tail with silence. Updates `*pending_emul` and returns frames copied from ring.
size_t ReadRingToHardwareChannelOnly(const uint8_t* ring_buf, size_t size_emul, size_t pos_emul,
                                     size_t* pending_emul, uint8_t* hw_dst, size_t hw_frames,
                                     const PcmFormat& sw_fmt, uint32_t hw_channels);

// Writes up to `hw_frames` from WASAPI capture packet `hw_src` into QEMU's input
// ring buffer, adapting channel counts (`hw_channels` -> `sw_fmt.channels`) and
// honoring WASAPI's `AUDCLNT_BUFFERFLAGS_SILENT` (`hw_is_silent`). Updates
// `*pos_emul` and `*pending_emul` and returns frames written to the ring.
size_t WriteHardwareToRingChannelOnly(const uint8_t* hw_src, size_t hw_frames, bool hw_is_silent,
                                      uint8_t* ring_buf, size_t size_emul, size_t* pos_emul,
                                      size_t* pending_emul, const PcmFormat& sw_fmt,
                                      uint32_t hw_channels);

}  // namespace goldfish::wasapi
