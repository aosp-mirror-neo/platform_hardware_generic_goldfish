// Copyright 2025 The Android Open Source Project
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
#include "goldfish/display/cpu_read_pipeline.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/config.h"
#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"

#include "android/status/status_macros.h"
#include "libyuv/convert_from_argb.h"
#include "libyuv/rotate_argb.h"
#include "libyuv/scale_argb.h"

#ifdef ABSL_HAVE_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

namespace goldfish::display {
namespace {

// R,G,B,0xFF is 0xFFBBGGRR as a uint32_t only on little-endian hosts.
static_assert(std::endian::native == std::endian::little);

constexpr size_t kBytesPerSlotPixel = 4;
constexpr uint32_t kOpaqueAlphaMask = 0xFF000000U;

#if defined(__clang__) || defined(__GNUC__)
using PixelVec4 = uint32_t __attribute__((vector_size(16)));
constexpr PixelVec4 kOpaqueAlphaMaskVec = {kOpaqueAlphaMask, kOpaqueAlphaMask, kOpaqueAlphaMask,
                                           kOpaqueAlphaMask};
#endif

// Under ASAN, the bytes of a leased buffer past the requested size are
// poisoned, so an overrun that stays inside a larger, reused buffer is caught.
void PoisonTail(uint8_t* bytes, size_t used, size_t capacity) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    ASAN_POISON_MEMORY_REGION(bytes + used, capacity - used);
#else
    (void)bytes, (void)used, (void)capacity;
#endif
}

void Unpoison(uint8_t* bytes, size_t capacity) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    ASAN_UNPOISON_MEMORY_REGION(bytes, capacity);
#else
    (void)bytes, (void)capacity;
#endif
}

// Process-wide pool of 32-bit scratch images, so steady-state reads don't
// allocate. Storage is default-initialised: every byte is written before it
// is read. When the last loan returns, the pool keeps only as many buffers as
// were on loan at once since it was last idle, and none larger than the
// largest request in that time, so a burst of large or concurrent reads
// doesn't pin memory.
class ScratchPool {
  public:
    struct Storage {
        std::unique_ptr<uint8_t[]> bytes;
        size_t capacity = 0;
    };

    // Never returns null bytes, even for a zero-byte request: the loan is only
    // repaid by a ScratchBuffer that holds memory.
    Storage Acquire(size_t bytes) ABSL_LOCKS_EXCLUDED(pool_mutex_) {
        const size_t size = std::max<size_t>(bytes, 1);
        Storage storage;
        {
            absl::MutexLock lock(&pool_mutex_);
            ++on_loan_;
            peak_on_loan_ = std::max(peak_on_loan_, on_loan_);
            largest_request_ = std::max(largest_request_, size);
            // free_ is sorted by capacity, so this is the smallest that fits.
            auto best = std::lower_bound(
                    free_.begin(), free_.end(), size,
                    [](const Storage& free, size_t wanted) { return free.capacity < wanted; });
            if (best != free_.end()) {
                storage = std::move(*best);
                free_.erase(best);
            }
        }
        if (storage.bytes == nullptr) {
            storage = Storage{std::make_unique_for_overwrite<uint8_t[]>(size), size};
        }
        PoisonTail(storage.bytes.get(), bytes, storage.capacity);
        return storage;
    }

    void Return(Storage storage) ABSL_LOCKS_EXCLUDED(pool_mutex_) {
        Unpoison(storage.bytes.get(), storage.capacity);
        std::vector<Storage> trimmed;  // Freed after the lock is released.
        absl::MutexLock lock(&pool_mutex_);
        --on_loan_;
        auto position = std::upper_bound(
                free_.begin(), free_.end(), storage.capacity,
                [](size_t capacity, const Storage& free) { return capacity < free.capacity; });
        free_.insert(position, std::move(storage));
        // The smallest buffer fits the fewest requests.
        if (free_.size() > kMaxRetained) TrimSmallest(trimmed);
        if (on_loan_ != 0) return;
        // Idle: keep the largest `peak_on_loan_` buffers that the last busy
        // period could have used.
        while (!free_.empty() && free_.back().capacity > largest_request_) {
            trimmed.push_back(std::move(free_.back()));
            free_.pop_back();
        }
        while (free_.size() > peak_on_loan_) TrimSmallest(trimmed);
        peak_on_loan_ = 0;
        largest_request_ = 0;
    }

    internal::ScratchPoolStats Stats() ABSL_LOCKS_EXCLUDED(pool_mutex_) {
        absl::MutexLock lock(&pool_mutex_);
        internal::ScratchPoolStats stats{.on_loan = on_loan_, .free_count = free_.size()};
        for (const Storage& free : free_) stats.free_bytes += free.capacity;
        return stats;
    }

  private:
    static constexpr size_t kMaxRetained = 4;

    void TrimSmallest(std::vector<Storage>& trimmed) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pool_mutex_) {
        trimmed.push_back(std::move(free_.front()));
        free_.erase(free_.begin());
    }

    absl::Mutex pool_mutex_;
    std::vector<Storage> free_ ABSL_GUARDED_BY(pool_mutex_);  // By ascending capacity.
    size_t on_loan_ ABSL_GUARDED_BY(pool_mutex_) = 0;
    size_t peak_on_loan_ ABSL_GUARDED_BY(pool_mutex_) = 0;
    size_t largest_request_ ABSL_GUARDED_BY(pool_mutex_) = 0;
};

ScratchPool& Scratch() {
    static absl::NoDestructor<ScratchPool> pool;
    return *pool;
}

// A 32-bit R,G,B,X image in memory someone else owns.
struct Image32 {
    const uint8_t* data;
    int stride;
    int width;
    int height;
};

struct MutableImage32 {
    uint8_t* data;
    int stride;
    int width;
    int height;

    Image32 View() const { return {data, stride, width, height}; }
};

// Plane::stride == 0 means tight.
int StrideOf(const Plane& plane, size_t row_bytes) {
    return static_cast<int>(plane.stride == 0 ? row_bytes : plane.stride);
}

// ImageRotation is counter-clockwise; libyuv::RotationMode is clockwise.
libyuv::RotationMode ToLibyuvRotation(ImageRotation rotation) {
    switch (rotation) {
    case ImageRotation::kRotation0:
        return libyuv::kRotate0;
    case ImageRotation::kRotation90:
        return libyuv::kRotate270;
    case ImageRotation::kRotation180:
        return libyuv::kRotate180;
    case ImageRotation::kRotation270:
        return libyuv::kRotate90;
    }
    return libyuv::kRotate0;
}

bool IsQuarterTurn(ImageRotation rotation) {
    return rotation == ImageRotation::kRotation90 || rotation == ImageRotation::kRotation270;
}

absl::Status LibyuvStatus(int result, std::string_view function) {
    if (result != 0) {
        return absl::InternalError(absl::StrFormat("libyuv::%s failed (%d)", function, result));
    }
    return absl::OkStatus();
}

absl::Status CheckSource(const SlotPixels& src, const ReadRequest& read, bool nv12) {
    if (src.data == nullptr) return absl::InvalidArgumentError("Slot has no pixels");
    constexpr uint64_t kMaxInt = std::numeric_limits<int>::max();
    const uint64_t row_bytes = uint64_t{src.dimensions.width} * kBytesPerSlotPixel;
    if (src.stride < row_bytes || src.stride > kMaxInt) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Slot stride %d does not fit %d-pixel rows", src.stride, src.dimensions.width));
    }
    // A rotated read turns columns into rows, so scratch strides reach
    // height * 4 bytes; libyuv takes them as int.
    if (uint64_t{src.dimensions.height} * kBytesPerSlotPixel > kMaxInt) {
        return absl::InvalidArgumentError(
                absl::StrFormat("Slot height %d is too large", src.dimensions.height));
    }
    const Rect crop = read.crop.value_or(Rect{0, 0, src.dimensions.width, src.dimensions.height});
    if (crop.width == 0 || crop.height == 0 ||
        uint64_t{crop.x} + crop.width > src.dimensions.width ||
        uint64_t{crop.y} + crop.height > src.dimensions.height) {
        return absl::InvalidArgumentError("Crop is outside the slot");
    }
    // Scratch buffers are sized for the output, so it must not upscale. With
    // the checks above, this also keeps every output row within INT_MAX.
    const bool quarter = IsQuarterTurn(read.rotation);
    const uint32_t max_width = quarter ? crop.height : crop.width;
    const uint32_t max_height = quarter ? crop.width : crop.height;
    if (read.output.width == 0 || read.output.height == 0 || read.output.width > max_width ||
        read.output.height > max_height) {
        return absl::InvalidArgumentError(
                absl::StrFormat("Output %dx%d is empty or larger than the rotated %dx%d crop",
                                read.output.width, read.output.height, max_width, max_height));
    }
    if (nv12 && (read.output.width % 2 != 0 || read.output.height % 2 != 0)) {
        return absl::InvalidArgumentError(absl::StrFormat("NV12 output %dx%d is not even",
                                                          read.output.width, read.output.height));
    }
    return absl::OkStatus();
}

// Copies `pixels` R,G,B,X pixels from `src` to `dst` (`src == dst` is allowed
// for in-place updates) and sets every fourth byte (alpha) to 0xFF. Explicit
// 16-byte vectors via `std::memcpy` preserve arbitrary byte alignment while
// avoiding scalar 32-bit loads/stores when `CopyOpaque` is inlined or strided
// 1-byte alpha stores in `MakeOpaque`.
void SetOpaqueAlphaSpan(const uint8_t* src, uint8_t* dst, size_t pixels) {
    size_t x = 0;
#if defined(__clang__) || defined(__GNUC__)
    for (; x + 8 <= pixels; x += 8) {
        PixelVec4 p0;
        PixelVec4 p1;
        std::memcpy(&p0, src + x * kBytesPerSlotPixel, sizeof(p0));
        std::memcpy(&p1, src + (x + 4) * kBytesPerSlotPixel, sizeof(p1));
        p0 |= kOpaqueAlphaMaskVec;
        p1 |= kOpaqueAlphaMaskVec;
        std::memcpy(dst + x * kBytesPerSlotPixel, &p0, sizeof(p0));
        std::memcpy(dst + (x + 4) * kBytesPerSlotPixel, &p1, sizeof(p1));
    }
    if (x + 4 <= pixels) {
        PixelVec4 p0;
        std::memcpy(&p0, src + x * kBytesPerSlotPixel, sizeof(p0));
        p0 |= kOpaqueAlphaMaskVec;
        std::memcpy(dst + x * kBytesPerSlotPixel, &p0, sizeof(p0));
        x += 4;
    }
    for (size_t tail = pixels & 3U; tail > 0; --tail, ++x) {
        uint32_t px = 0;
        std::memcpy(&px, src + x * kBytesPerSlotPixel, sizeof(px));
        px |= kOpaqueAlphaMask;
        std::memcpy(dst + x * kBytesPerSlotPixel, &px, sizeof(px));
    }
#else
    for (; x < pixels; ++x) {
        uint32_t px = 0;
        std::memcpy(&px, src + x * kBytesPerSlotPixel, sizeof(px));
        px |= kOpaqueAlphaMask;
        std::memcpy(dst + x * kBytesPerSlotPixel, &px, sizeof(px));
    }
#endif
}

// Copies R,G,B and sets the fourth byte to 0xFF in a single pass.
void CopyOpaque(Image32 src, MutableImage32 dst) {
    const size_t width = static_cast<size_t>(src.width);
    const size_t height = static_cast<size_t>(src.height);
    const size_t row_bytes = width * kBytesPerSlotPixel;
    if (static_cast<size_t>(src.stride) == row_bytes &&
        static_cast<size_t>(dst.stride) == row_bytes) {
        SetOpaqueAlphaSpan(src.data, dst.data, width * height);
        return;
    }
    for (size_t y = 0; y < height; ++y) {
        SetOpaqueAlphaSpan(src.data + y * static_cast<size_t>(src.stride),
                           dst.data + y * static_cast<size_t>(dst.stride), width);
    }
}

void MakeOpaque(MutableImage32 image) {
    const size_t width = static_cast<size_t>(image.width);
    const size_t height = static_cast<size_t>(image.height);
    const size_t row_bytes = width * kBytesPerSlotPixel;
    if (static_cast<size_t>(image.stride) == row_bytes) {
        SetOpaqueAlphaSpan(image.data, image.data, width * height);
        return;
    }
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = image.data + y * static_cast<size_t>(image.stride);
        SetOpaqueAlphaSpan(row, row, width);
    }
}

// Scales a one-pixel-wide image vertically with centre-aligned bilinear
// sampling. libyuv::ARGBScale point-samples sources one pixel wide
// (ScaleFilterReduce), so columns are filtered here instead, with libyuv's
// 16.16 positions and 8-bit weights.
void ScaleColumn(Image32 src, MutableImage32 dst) {
    const int64_t dy = (int64_t{src.height} << 16) / dst.height;
    const int64_t max_y = int64_t{src.height - 1} << 16;
    int64_t y = dy / 2 - 32768;
    for (int row = 0; row < dst.height; ++row, y += dy) {
        const int64_t at = std::clamp<int64_t>(y, 0, max_y);
        const auto top = static_cast<int>(at >> 16);
        const uint8_t* a = src.data + ptrdiff_t{top} * src.stride;
        const uint8_t* b = src.data + ptrdiff_t{std::min(top + 1, src.height - 1)} * src.stride;
        const auto f = static_cast<uint32_t>((at >> 8) & 255);
        uint8_t* out = dst.data + ptrdiff_t{row} * dst.stride;
        for (int c = 0; c < 4; ++c) {
            out[c] = static_cast<uint8_t>((a[c] * (256 - f) + b[c] * f + 128) >> 8);
        }
    }
}

// Runs the crop / scale / rotate part of the pipeline and hands the 32-bit
// result to `convert`. Scaling happens before rotating: with centre-aligned
// bilinear sampling the result is equivalent, and a downscale then rotates
// fewer pixels. `final_rgba`, if set, receives the last transform directly
// instead of a scratch buffer.
template <typename Convert>
absl::Status Transform(const SlotPixels& src, const ReadRequest& read,
                       const MutableImage32* final_rgba, Convert convert) {
    const Rect crop = read.crop.value_or(Rect{0, 0, src.dimensions.width, src.dimensions.height});
    Image32 current{src.data + size_t{crop.y} * src.stride + size_t{crop.x} * kBytesPerSlotPixel,
                    static_cast<int>(src.stride), static_cast<int>(crop.width),
                    static_cast<int>(crop.height)};
    const bool rotate = read.rotation != ImageRotation::kRotation0;
    // The scaled size before rotation.
    const int scaled_width =
            static_cast<int>(IsQuarterTurn(read.rotation) ? read.output.height : read.output.width);
    const int scaled_height =
            static_cast<int>(IsQuarterTurn(read.rotation) ? read.output.width : read.output.height);
    const bool scale = scaled_width != current.width || scaled_height != current.height;
    const size_t output_bytes = size_t{read.output.width} * read.output.height * kBytesPerSlotPixel;
    const int scaled_stride = scaled_width * static_cast<int>(kBytesPerSlotPixel);

    std::optional<ScratchBuffer> scaled_buffer;
    std::optional<ScratchBuffer> rotated_buffer;
    if (scale) {
        MutableImage32 target;
        if (!rotate && final_rgba != nullptr) {
            target = *final_rgba;
        } else {
            scaled_buffer.emplace(AcquireScratchBuffer(output_bytes));
            target = {scaled_buffer->data(), scaled_stride, scaled_width, scaled_height};
        }
        if (current.width == 1) {
            ScaleColumn(current, target);  // No upscaling: the target is one wide too.
        } else {
            RETURN_IF_ERROR(LibyuvStatus(
                    libyuv::ARGBScale(current.data, current.stride, current.width, current.height,
                                      target.data, target.stride, target.width, target.height,
                                      libyuv::kFilterBilinear),
                    "ARGBScale"));
        }
        current = target.View();
    }
    if (rotate) {
        MutableImage32 target;
        const int width = static_cast<int>(read.output.width);
        const int height = static_cast<int>(read.output.height);
        if (final_rgba != nullptr) {
            target = *final_rgba;
        } else {
            rotated_buffer.emplace(AcquireScratchBuffer(output_bytes));
            target = {rotated_buffer->data(), width * static_cast<int>(kBytesPerSlotPixel), width,
                      height};
        }
        RETURN_IF_ERROR(LibyuvStatus(
                libyuv::ARGBRotate(current.data, current.stride, target.data, target.stride,
                                   current.width, current.height, ToLibyuvRotation(read.rotation)),
                "ARGBRotate"));
        current = target.View();
    }
    return convert(current, scale || rotate);
}

absl::Status ReadRgba(const SlotPixels& src, const ReadRequest& read, const Rgba8888Target& dst) {
    const size_t row_bytes = size_t{read.output.width} * 4;
    const MutableImage32 out{dst.plane.data.data(), StrideOf(dst.plane, row_bytes),
                             static_cast<int>(read.output.width),
                             static_cast<int>(read.output.height)};
    return Transform(src, read, &out, [&](Image32 result, bool transformed) {
        if (transformed) {
            MakeOpaque(out);  // The last transform wrote straight into dst.
        } else {
            CopyOpaque(result, out);
        }
        return absl::OkStatus();
    });
}

absl::Status ReadRgb888(const SlotPixels& src, const ReadRequest& read, const Rgb888Target& dst) {
    const size_t row_bytes = size_t{read.output.width} * 3;
    return Transform(src, read, nullptr, [&](Image32 result, bool) {
        // libyuv "ARGB" is B,G,R,A in memory and "RGB24" is B,G,R: this keeps
        // bytes 0, 1, 2, which on an R,G,B,X slot are R,G,B.
        return LibyuvStatus(
                libyuv::ARGBToRGB24(result.data, result.stride, dst.plane.data.data(),
                                    StrideOf(dst.plane, row_bytes), result.width, result.height),
                "ARGBToRGB24");
    });
}

absl::Status ReadNv12(const SlotPixels& src, const ReadRequest& read, const Nv12Target& dst) {
    const size_t row_bytes = read.output.width;
    return Transform(src, read, nullptr, [&](Image32 result, bool) {
        // libyuv "ABGR" is R,G,B,A in memory; BT.601 limited range.
        return LibyuvStatus(
                libyuv::ABGRToNV12(result.data, result.stride, dst.y.data.data(),
                                   StrideOf(dst.y, row_bytes), dst.uv.data.data(),
                                   StrideOf(dst.uv, row_bytes), result.width, result.height),
                "ABGRToNV12");
    });
}

}  // namespace

ScratchBuffer::ScratchBuffer(ScratchBuffer&& other) noexcept
        : bytes_(std::move(other.bytes_)), capacity_(std::exchange(other.capacity_, 0)) {}

ScratchBuffer::~ScratchBuffer() {
    if (bytes_ != nullptr) {
        Scratch().Return({std::move(bytes_), capacity_});
    }
}

ScratchBuffer AcquireScratchBuffer(size_t bytes) {
    ScratchPool::Storage storage = Scratch().Acquire(bytes);
    return ScratchBuffer(std::move(storage.bytes), storage.capacity);
}

namespace internal {

ScratchPoolStats GetScratchPoolStatsForTesting() {
    return Scratch().Stats();
}

}  // namespace internal

absl::Status CpuReadPipeline(const SlotPixels& src, const ReadRequest& read,
                             const ReadTarget& dst) {
    RETURN_IF_ERROR(CheckSource(src, read, std::holds_alternative<Nv12Target>(dst)));
    if (const auto* rgba = std::get_if<Rgba8888Target>(&dst)) return ReadRgba(src, read, *rgba);
    if (const auto* rgb = std::get_if<Rgb888Target>(&dst)) return ReadRgb888(src, read, *rgb);
    return ReadNv12(src, read, std::get<Nv12Target>(dst));
}

}  // namespace goldfish::display
