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

#include "goldfish/display/write_jpeg.h"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace goldfish::display {
namespace {

constexpr int kFastDctQualityThreshold = 96;
constexpr size_t kMinInitialBufferBytes = 16384U;
constexpr size_t kMaxInitialBufferBytes = static_cast<size_t>(16U) * 1024U * 1024U;
constexpr size_t kVectorChunkBytes = 32768U;
constexpr JDIMENSION kScanlineBatchRows = 16U;

struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

[[noreturn]] void OnJpegErrorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(err->setjmp_buffer, 1);
}

struct ReusableJpegCompressor {
    jpeg_compress_struct cinfo{};
    JpegErrorMgr jerr{};
    bool initialized = false;

    ReusableJpegCompressor() = default;
    ReusableJpegCompressor(const ReusableJpegCompressor&) = delete;
    ReusableJpegCompressor& operator=(const ReusableJpegCompressor&) = delete;

    ~ReusableJpegCompressor() { Reset(); }

    void EnsureInitialized() {
        if (!initialized) {
            cinfo = {};
            cinfo.err = jpeg_std_error(&jerr.pub);
            jerr.pub.error_exit = OnJpegErrorExit;
            jpeg_create_compress(&cinfo);
            initialized = true;
        }
    }

    void Reset() {
        if (initialized) {
            cinfo.dest = nullptr;
            jpeg_destroy_compress(&cinfo);
            initialized = false;
        }
    }
};

size_t EstimateInitialOutputBytes(unsigned int width, unsigned int height) {
    const size_t pixel_count = static_cast<size_t>(width) * height;
    return std::clamp<size_t>(pixel_count / 4U, kMinInitialBufferBytes, kMaxInitialBufferBytes);
}

void ResizeStringUninit(std::string* str, size_t size) {
#if defined(__cpp_lib_string_resize_and_overwrite) && \
        __cpp_lib_string_resize_and_overwrite >= 202110L
    str->resize_and_overwrite(size, [](char* /*buf*/, size_t count) { return count; });
#else
    str->resize(size);
#endif
}

struct StringDestinationMgr {
    jpeg_destination_mgr pub{};
    std::string* out = nullptr;
    size_t initial_capacity = 0;
};

void InitStringDestination(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<StringDestinationMgr*>(cinfo->dest);
    const size_t target_size = std::max(dest->out->capacity(), dest->initial_capacity);
    ResizeStringUninit(dest->out, target_size);
    const size_t full_cap = dest->out->capacity();
    if (full_cap > dest->out->size()) {
        ResizeStringUninit(dest->out, full_cap);
    }
    dest->pub.next_output_byte = reinterpret_cast<JOCTET*>(dest->out->data());
    dest->pub.free_in_buffer = dest->out->size();
}

boolean EmptyStringOutputBuffer(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<StringDestinationMgr*>(cinfo->dest);
    const size_t old_size = dest->out->size();
    if (old_size > std::numeric_limits<size_t>::max() / 2U) {
        OnJpegErrorExit(reinterpret_cast<j_common_ptr>(cinfo));
    }
    const size_t next_size = std::max<size_t>(kMinInitialBufferBytes, old_size * 2U);
    ResizeStringUninit(dest->out, next_size);
    const size_t full_cap = dest->out->capacity();
    if (full_cap > dest->out->size()) {
        ResizeStringUninit(dest->out, full_cap);
    }
    dest->pub.next_output_byte = reinterpret_cast<JOCTET*>(dest->out->data() + old_size);
    dest->pub.free_in_buffer = dest->out->size() - old_size;
    return TRUE;
}

void TermStringDestination(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<StringDestinationMgr*>(cinfo->dest);
    const size_t written = dest->out->size() - dest->pub.free_in_buffer;
    dest->out->resize(written);
}

struct VectorDestinationMgr {
    jpeg_destination_mgr pub{};
    std::vector<uint8_t>* out = nullptr;
    size_t initial_capacity = 0;
    JOCTET chunk[kVectorChunkBytes];
};

void InitVectorDestination(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestinationMgr*>(cinfo->dest);
    dest->out->clear();
    if (dest->out->capacity() < dest->initial_capacity) {
        dest->out->reserve(dest->initial_capacity);
    }
    dest->pub.next_output_byte = dest->chunk;
    dest->pub.free_in_buffer = kVectorChunkBytes;
}

boolean EmptyVectorOutputBuffer(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestinationMgr*>(cinfo->dest);
    dest->out->insert(dest->out->end(), dest->chunk, dest->chunk + kVectorChunkBytes);
    dest->pub.next_output_byte = dest->chunk;
    dest->pub.free_in_buffer = kVectorChunkBytes;
    return TRUE;
}

void TermVectorDestination(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestinationMgr*>(cinfo->dest);
    const size_t remaining = kVectorChunkBytes - dest->pub.free_in_buffer;
    if (remaining > 0U) {
        dest->out->insert(dest->out->end(), dest->chunk, dest->chunk + remaining);
    }
}

bool ValidateJpegArgs(unsigned int n_channels, unsigned int width, unsigned int height,
                      const void* pixels, int quality, size_t row_stride,
                      size_t* effective_stride) {
    constexpr auto kMaxJpegDimension = static_cast<unsigned int>(JPEG_MAX_DIMENSION);
    if ((n_channels != 3U && n_channels != 4U) || width == 0U || height == 0U ||
        width > kMaxJpegDimension || height > kMaxJpegDimension || pixels == nullptr ||
        quality < 1 || quality > 100) {
        return false;
    }
    if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / n_channels) {
        return false;
    }
    const size_t min_stride = static_cast<size_t>(width) * n_channels;
    if (row_stride != 0U && row_stride < min_stride) {
        return false;
    }
    const size_t actual_stride = (row_stride == 0U) ? min_stride : row_stride;
    if (static_cast<size_t>(height) - 1U >
        (std::numeric_limits<size_t>::max() - min_stride) / actual_stride) {
        return false;
    }
    *effective_stride = actual_stride;
    return true;
}

bool EncodeJpegToDest(unsigned int n_channels, unsigned int width, unsigned int height,
                      const uint8_t* byte_pixels, int quality, size_t stride,
                      jpeg_destination_mgr* dest) {
    thread_local ReusableJpegCompressor compressor;

    if (setjmp(compressor.jerr.setjmp_buffer)) {
        compressor.Reset();
        return false;
    }

    compressor.EnsureInitialized();
    jpeg_compress_struct& cinfo = compressor.cinfo;
    cinfo.dest = dest;
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = static_cast<int>(n_channels);
    cinfo.in_color_space = (n_channels == 3U) ? JCS_RGB : JCS_EXT_RGBX;

    jpeg_set_defaults(&cinfo);
    cinfo.dct_method = (quality < kFastDctQualityThreshold) ? JDCT_IFAST : JDCT_ISLOW;
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row_pointers[kScanlineBatchRows];
    while (cinfo.next_scanline < cinfo.image_height) {
        const JDIMENSION remaining = cinfo.image_height - cinfo.next_scanline;
        const JDIMENSION batch = std::min(remaining, kScanlineBatchRows);
        const uint8_t* batch_base =
                byte_pixels + (static_cast<size_t>(cinfo.next_scanline) * stride);
        for (JDIMENSION r = 0U; r < batch; ++r) {
            row_pointers[r] = const_cast<JSAMPROW>(batch_base + (static_cast<size_t>(r) * stride));
        }
        const JDIMENSION written = jpeg_write_scanlines(&cinfo, row_pointers, batch);
        if (written == 0U) {
            compressor.Reset();
            return false;
        }
    }

    jpeg_finish_compress(&cinfo);
    cinfo.dest = nullptr;
    return true;
}

}  // namespace

bool WriteJpeg(unsigned int n_channels, unsigned int width, unsigned int height, const void* pixels,
               std::string* out_data, int quality, size_t row_stride) {
    if (out_data == nullptr) {
        return false;
    }
    size_t stride = 0;
    if (!ValidateJpegArgs(n_channels, width, height, pixels, quality, row_stride, &stride)) {
        out_data->clear();
        return false;
    }

    StringDestinationMgr dest{};
    dest.pub.init_destination = InitStringDestination;
    dest.pub.empty_output_buffer = EmptyStringOutputBuffer;
    dest.pub.term_destination = TermStringDestination;
    dest.out = out_data;
    dest.initial_capacity = EstimateInitialOutputBytes(width, height);

    if (!EncodeJpegToDest(n_channels, width, height, static_cast<const uint8_t*>(pixels), quality,
                          stride, &dest.pub)) {
        out_data->clear();
        return false;
    }
    return !out_data->empty();
}

bool WriteJpeg(unsigned int n_channels, unsigned int width, unsigned int height, const void* pixels,
               std::vector<uint8_t>& out_data, int quality, size_t row_stride) {
    size_t stride = 0;
    if (!ValidateJpegArgs(n_channels, width, height, pixels, quality, row_stride, &stride)) {
        out_data.clear();
        return false;
    }

    VectorDestinationMgr dest{};
    dest.pub.init_destination = InitVectorDestination;
    dest.pub.empty_output_buffer = EmptyVectorOutputBuffer;
    dest.pub.term_destination = TermVectorDestination;
    dest.out = &out_data;
    dest.initial_capacity = EstimateInitialOutputBytes(width, height);

    if (!EncodeJpegToDest(n_channels, width, height, static_cast<const uint8_t*>(pixels), quality,
                          stride, &dest.pub)) {
        out_data.clear();
        return false;
    }
    return !out_data.empty();
}

}  // namespace goldfish::display
