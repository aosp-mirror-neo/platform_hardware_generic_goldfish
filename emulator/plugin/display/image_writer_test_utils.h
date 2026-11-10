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
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <ctime>
#endif

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace goldfish::display::test_utils {

inline absl::Duration CurrentThreadCpuTime() {
#if defined(__unix__) || defined(__APPLE__)
    struct timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return absl::Seconds(ts.tv_sec) + absl::Nanoseconds(ts.tv_nsec);
    }
#endif
    return absl::Now() - absl::UnixEpoch();
}

inline std::vector<uint8_t> MakeSyntheticScreenshot(unsigned int width, unsigned int height,
                                                    unsigned int channels) {
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * channels);
    for (unsigned int y = 0; y < height; ++y) {
        uint8_t* row = frame.data() + (static_cast<size_t>(y) * width * channels);
        const unsigned int band = y / 64U;
        for (unsigned int x = 0; x < width; ++x) {
            uint8_t* px = row + (static_cast<size_t>(x) * channels);
            // Simulate a typical Android UI frame: status/nav bars, card surfaces,
            // and periodic text/icon glyph regions.
            if ((x % 128U) < 96U && (y % 64U) < 48U) {
                px[0] = static_cast<uint8_t>((240U - (band * 7U)) & 0xFFU);
                px[1] = static_cast<uint8_t>((244U - (band * 5U)) & 0xFFU);
                px[2] = static_cast<uint8_t>((248U - (band * 3U)) & 0xFFU);
            } else {
                px[0] = static_cast<uint8_t>((x * 3U + y * 5U) & 0xFFU);
                px[1] = static_cast<uint8_t>((x * 7U ^ y * 11U) & 0xFFU);
                px[2] = static_cast<uint8_t>((x + y * 13U) & 0xFFU);
            }
            if (channels == 4U) {
                px[3] = 0xFFU;
            }
        }
    }
    return frame;
}

}  // namespace goldfish::display::test_utils
