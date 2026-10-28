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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace goldfish::audio {

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

}  // namespace goldfish::audio
