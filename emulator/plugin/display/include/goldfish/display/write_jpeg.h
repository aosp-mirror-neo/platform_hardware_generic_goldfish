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
#include <string>
#include <vector>

namespace goldfish::display {

constexpr int kDefaultJpegQuality = 85;

/**
 * @brief Encodes an RGB24 (`n_channels == 3`) or RGBX/RGBA8888 (`n_channels == 4`) pixel
 *        buffer into a JPEG image container.
 *
 * For 4-channel inputs, the fourth byte of each pixel (alpha/padding) is ignored.
 * Existing capacity in `out_data` is reused when large enough.
 *
 * @param n_channels Number of channels per pixel (must be 3 for RGB or 4 for RGBX/RGBA).
 * @param width Image width in pixels (must be > 0).
 * @param height Image height in pixels (must be > 0).
 * @param pixels Pointer to row-major pixel data.
 * @param out_data Destination string receiving the encoded JPEG bytes.
 * @param quality JPEG compression quality in [1, 100] (defaults to `kDefaultJpegQuality`).
 * @param row_stride Byte offset between consecutive rows, or 0 for tightly packed rows
 *        (`width * n_channels`). Non-zero values smaller than `width * n_channels` are
 *        rejected.
 * @return True if encoding succeeded, false if any argument is invalid or libjpeg fails.
 */
bool WriteJpeg(unsigned int n_channels, unsigned int width, unsigned int height, const void* pixels,
               std::string* out_data, int quality = kDefaultJpegQuality, size_t row_stride = 0);

/**
 * @brief Encodes an RGB24 (`n_channels == 3`) or RGBX/RGBA8888 (`n_channels == 4`) pixel
 *        buffer into a JPEG byte vector.
 *
 * @param n_channels Number of channels per pixel (must be 3 for RGB or 4 for RGBX/RGBA).
 * @param width Image width in pixels (must be > 0).
 * @param height Image height in pixels (must be > 0).
 * @param pixels Pointer to row-major pixel data.
 * @param out_data Destination vector receiving the encoded JPEG bytes.
 * @param quality JPEG compression quality in [1, 100] (defaults to `kDefaultJpegQuality`).
 * @param row_stride Byte offset between consecutive rows, or 0 for tightly packed rows
 *        (`width * n_channels`). Non-zero values smaller than `width * n_channels` are
 *        rejected.
 * @return True if encoding succeeded, false if any argument is invalid or libjpeg fails.
 */
bool WriteJpeg(unsigned int n_channels, unsigned int width, unsigned int height, const void* pixels,
               std::vector<uint8_t>& out_data, int quality = kDefaultJpegQuality,
               size_t row_stride = 0);

/**
 * @brief Convenience wrapper that encodes an RGBX/RGBA8888 pixel buffer into a JPEG byte
 *        string.
 *
 * @param width Image width in pixels (must be > 0).
 * @param height Image height in pixels (must be > 0).
 * @param rgba_pixels Pointer to 4-byte-per-pixel row-major pixel data (alpha byte ignored).
 * @param out_data Destination string receiving the encoded JPEG bytes.
 * @param quality JPEG compression quality in [1, 100] (defaults to `kDefaultJpegQuality`).
 * @param row_stride Byte offset between consecutive rows, or 0 for tightly packed rows
 *        (`width * 4`).
 * @return True if encoding succeeded, false if any argument is invalid or libjpeg fails.
 */
inline bool EncodeRgbaToJpeg(uint32_t width, uint32_t height, const uint8_t* rgba_pixels,
                             std::string* out_data, int quality = kDefaultJpegQuality,
                             size_t row_stride = 0) {
    return WriteJpeg(4U, width, height, rgba_pixels, out_data, quality, row_stride);
}

}  // namespace goldfish::display
