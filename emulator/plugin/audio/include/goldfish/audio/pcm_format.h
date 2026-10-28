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

#include <array>
#include <cstddef>
#include <cstdint>

namespace goldfish::audio {

/**
 * Maximum interleaved audio channel count supported by QEMU's audio subsystem.
 */
inline constexpr uint32_t kMaxAudioChannels = 16;

/**
 * Supported interleaved PCM sample formats for external audio injection sources.
 */
enum class SampleFormat : uint8_t {
    kU8,   ///< Unsigned 8-bit PCM (0x80 midpoint silence).
    kS16,  ///< Native-endian signed 16-bit PCM (default for WebRTC & gRPC).
    kS32,  ///< Native-endian signed 32-bit PCM.
    kF32,  ///< Normalized [-1.0f, 1.0f] interleaved 32-bit float PCM.
};

/**
 * Stream parameters describing an external caller's injected PCM audio format.
 */
struct AudioStreamConfig {
    uint32_t sample_rate_hz = 48000;           ///< Sample rate in Hz.
    uint32_t channels = 1;                     ///< Interleaved channel count (1..16).
    SampleFormat format = SampleFormat::kS16;  ///< PCM sample format.

    bool operator==(const AudioStreamConfig&) const = default;
};

/**
 * Guest stream PCM encodings matching QEMU's `AudioFormat` enumeration.
 */
enum class GuestPcmFormat : uint8_t {
    kS8,
    kU8,
    kS16,
    kU16,
    kS32,
    kU32,
    kF32,
};

/**
 * Stream parameters requested by a guest capture voice (`SWVoiceIn`).
 */
struct GuestAudioConfig {
    uint32_t sample_rate_hz = 48000;
    uint32_t channels = 1;
    GuestPcmFormat format = GuestPcmFormat::kS16;
    bool big_endian = false;

    bool operator==(const GuestAudioConfig&) const = default;
};

/**
 * Per-channel guest input gain and mute state matching QEMU's `Volume` struct (`255 == 1.0f` gain).
 */
struct GuestChannelVolume {
    bool mute = false;
    uint32_t channels = 1;
    std::array<uint8_t, kMaxAudioChannels> gain = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    };

    bool IsNominal() const {
        if (mute) {
            return false;
        }
        const uint32_t count = channels < kMaxAudioChannels ? channels : kMaxAudioChannels;
        for (uint32_t i = 0; i < count; ++i) {
            if (gain[i] != 255) {
                return false;
            }
        }
        return true;
    }
};

/**
 * Returns the byte width of a single sample in @p format.
 */
constexpr size_t BytesPerSample(SampleFormat format) {
    switch (format) {
    case SampleFormat::kU8:
        return 1;
    case SampleFormat::kS16:
        return 2;
    case SampleFormat::kS32:
    case SampleFormat::kF32:
        return 4;
    }
    return 2;
}

/**
 * Returns the byte width of a single guest sample in @p format.
 */
constexpr size_t BytesPerGuestSample(GuestPcmFormat format) {
    switch (format) {
    case GuestPcmFormat::kS8:
    case GuestPcmFormat::kU8:
        return 1;
    case GuestPcmFormat::kS16:
    case GuestPcmFormat::kU16:
        return 2;
    case GuestPcmFormat::kS32:
    case GuestPcmFormat::kU32:
    case GuestPcmFormat::kF32:
        return 4;
    }
    return 2;
}

/**
 * Fills @p byte_count bytes in @p dst with format-appropriate silence (`0` for signed/float
 * formats; midpoint `0x80` / `0x8000` / `0x80000000` for unsigned formats).
 */
void FillSourceSilence(SampleFormat format, void* dst, size_t byte_count);

/**
 * Fills @p byte_count bytes in @p dst with guest-format silence, honoring @p big_endian for
 * multi-byte unsigned midpoint representations.
 */
void FillGuestSilence(GuestPcmFormat format, bool big_endian, void* dst, size_t byte_count);

/**
 * Quantizes and encodes a normalized `[-1.0, 1.0]` sample into @p dst in @p format, byte-swapping
 * multi-byte values when @p swap_endian is true.
 */
void EncodeGuestSample(GuestPcmFormat format, bool swap_endian, double sample, uint8_t* dst);

}  // namespace goldfish::audio
