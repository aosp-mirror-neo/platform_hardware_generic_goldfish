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

#include "goldfish/audio/pcm_format.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace goldfish::audio {

void EncodeGuestSample(GuestPcmFormat format, bool swap_endian, double sample, uint8_t* dst) {
    const double clamped = std::clamp(sample, -1.0, 1.0);
    switch (format) {
    case GuestPcmFormat::kS8: {
        const auto v = static_cast<int8_t>(std::clamp(std::lround(clamped * 128.0), -128L, 127L));
        std::memcpy(dst, &v, sizeof(v));
        return;
    }
    case GuestPcmFormat::kU8: {
        const auto v =
                static_cast<uint8_t>(std::clamp(std::lround(clamped * 128.0) + 128L, 0L, 255L));
        std::memcpy(dst, &v, sizeof(v));
        return;
    }
    case GuestPcmFormat::kS16: {
        const auto s =
                static_cast<int16_t>(std::clamp(std::lround(clamped * 32768.0), -32768L, 32767L));
        uint16_t raw = std::bit_cast<uint16_t>(s);
        if (swap_endian) {
            raw = std::byteswap(raw);
        }
        std::memcpy(dst, &raw, sizeof(raw));
        return;
    }
    case GuestPcmFormat::kU16: {
        auto raw = static_cast<uint16_t>(
                std::clamp(std::lround(clamped * 32768.0) + 32768L, 0L, 65535L));
        if (swap_endian) {
            raw = std::byteswap(raw);
        }
        std::memcpy(dst, &raw, sizeof(raw));
        return;
    }
    case GuestPcmFormat::kS32: {
        const auto s = static_cast<int32_t>(
                std::clamp(std::llround(clamped * 2147483648.0), -2147483648LL, 2147483647LL));
        uint32_t raw = std::bit_cast<uint32_t>(s);
        if (swap_endian) {
            raw = std::byteswap(raw);
        }
        std::memcpy(dst, &raw, sizeof(raw));
        return;
    }
    case GuestPcmFormat::kU32: {
        auto raw = static_cast<uint32_t>(
                std::clamp(std::llround(clamped * 2147483648.0) + 2147483648LL, 0LL, 4294967295LL));
        if (swap_endian) {
            raw = std::byteswap(raw);
        }
        std::memcpy(dst, &raw, sizeof(raw));
        return;
    }
    case GuestPcmFormat::kF32: {
        const auto clamped_f = static_cast<float>(clamped);
        uint32_t raw = std::bit_cast<uint32_t>(clamped_f);
        if (swap_endian) {
            raw = std::byteswap(raw);
        }
        std::memcpy(dst, &raw, sizeof(raw));
        return;
    }
    }
}

void EncodeGuestSamples(const GuestAudioConfig& config, const float* samples, size_t sample_count,
                        uint8_t* dst) {
    const bool swap_endian = config.big_endian != (std::endian::native == std::endian::big);
    const size_t bytes_per_sample = BytesPerGuestSample(config.format);
    for (size_t i = 0; i < sample_count; ++i) {
        EncodeGuestSample(config.format, swap_endian, samples[i], dst + i * bytes_per_sample);
    }
}

void FillSourceSilence(SampleFormat format, void* dst, size_t byte_count) {
    if (byte_count == 0) {
        return;
    }
    const int fill = (format == SampleFormat::kU8) ? 0x80 : 0x00;
    std::memset(dst, fill, byte_count);
}

void FillGuestSilence(GuestPcmFormat format, bool big_endian, void* dst, size_t byte_count) {
    if (byte_count == 0) {
        return;
    }
    auto* out = static_cast<uint8_t*>(dst);
    switch (format) {
    case GuestPcmFormat::kU8:
        std::memset(out, 0x80, byte_count);
        return;
    case GuestPcmFormat::kS8:
    case GuestPcmFormat::kS16:
    case GuestPcmFormat::kS32:
    case GuestPcmFormat::kF32:
        std::memset(out, 0x00, byte_count);
        return;
    case GuestPcmFormat::kU16: {
        const uint8_t hi = big_endian ? 0x80 : 0x00;
        const uint8_t lo = big_endian ? 0x00 : 0x80;
        size_t i = 0;
        for (; i + 1 < byte_count; i += 2) {
            out[i] = hi;
            out[i + 1] = lo;
        }
        if (i < byte_count) {
            out[i] = 0;
        }
        return;
    }
    case GuestPcmFormat::kU32: {
        const std::array<uint8_t, 4> pattern =
                big_endian ? std::array<uint8_t, 4>{0x80, 0x00, 0x00, 0x00}
                           : std::array<uint8_t, 4>{0x00, 0x00, 0x00, 0x80};
        size_t i = 0;
        for (; i + 3 < byte_count; i += 4) {
            std::memcpy(out + i, pattern.data(), pattern.size());
        }
        for (; i < byte_count; ++i) {
            out[i] = 0;
        }
        return;
    }
    }
}

void ScaleGuestPcmBuffer(GuestPcmFormat format, bool big_endian, float gain, void* buffer,
                         size_t byte_count) {
    if (byte_count == 0 || buffer == nullptr || gain == 1.0F) {
        return;
    }
    if (!std::isfinite(gain) || gain <= 0.0F) {
        FillGuestSilence(format, big_endian, buffer, byte_count);
        return;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);
    const size_t bps = BytesPerGuestSample(format);
    const size_t sample_count = byte_count / bps;
    const bool swap_endian = big_endian != (std::endian::native == std::endian::big);

    const double gain_d = static_cast<double>(gain);

    for (size_t i = 0; i < sample_count; ++i) {
        uint8_t* sample_ptr = bytes + i * bps;
        double decoded = 0.0;
        switch (format) {
        case GuestPcmFormat::kS8: {
            int8_t s = 0;
            std::memcpy(&s, sample_ptr, sizeof(s));
            decoded = static_cast<double>(s) * (1.0 / 128.0);
            break;
        }
        case GuestPcmFormat::kU8: {
            decoded = static_cast<double>(static_cast<int>(*sample_ptr) - 128) * (1.0 / 128.0);
            break;
        }
        case GuestPcmFormat::kS16: {
            uint16_t raw = 0;
            std::memcpy(&raw, sample_ptr, sizeof(raw));
            if (swap_endian) {
                raw = std::byteswap(raw);
            }
            decoded = static_cast<double>(std::bit_cast<int16_t>(raw)) * (1.0 / 32768.0);
            break;
        }
        case GuestPcmFormat::kU16: {
            uint16_t raw = 0;
            std::memcpy(&raw, sample_ptr, sizeof(raw));
            if (swap_endian) {
                raw = std::byteswap(raw);
            }
            decoded = static_cast<double>(static_cast<int32_t>(raw) - 32768) * (1.0 / 32768.0);
            break;
        }
        case GuestPcmFormat::kS32: {
            uint32_t raw = 0;
            std::memcpy(&raw, sample_ptr, sizeof(raw));
            if (swap_endian) {
                raw = std::byteswap(raw);
            }
            decoded = static_cast<double>(std::bit_cast<int32_t>(raw)) * (1.0 / 2147483648.0);
            break;
        }
        case GuestPcmFormat::kU32: {
            uint32_t raw = 0;
            std::memcpy(&raw, sample_ptr, sizeof(raw));
            if (swap_endian) {
                raw = std::byteswap(raw);
            }
            decoded = (static_cast<double>(raw) - 2147483648.0) * (1.0 / 2147483648.0);
            break;
        }
        case GuestPcmFormat::kF32: {
            uint32_t raw = 0;
            std::memcpy(&raw, sample_ptr, sizeof(raw));
            if (swap_endian) {
                raw = std::byteswap(raw);
            }
            const float f = std::bit_cast<float>(raw);
            decoded = std::isnan(f) ? 0.0 : static_cast<double>(f);
            break;
        }
        }
        EncodeGuestSample(format, swap_endian, decoded * gain_d, sample_ptr);
    }
}

}  // namespace goldfish::audio
