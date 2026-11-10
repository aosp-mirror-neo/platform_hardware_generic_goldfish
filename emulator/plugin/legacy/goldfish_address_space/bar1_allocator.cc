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

#include "bar1_allocator.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace goldfish::legacy {

std::optional<uint64_t> AlignUp(uint64_t value, uint64_t align) {
    DCHECK(std::has_single_bit(align));
    const uint64_t mask = align - 1ULL;
    if (value > std::numeric_limits<uint64_t>::max() - mask) {
        return std::nullopt;
    }
    return (value + mask) & ~mask;
}

uint64_t Bar1Allocator::AlignDown(uint64_t value, uint64_t align) {
    DCHECK(std::has_single_bit(align));
    return value & ~(align - 1ULL);
}

Bar1Allocator::Bar1Allocator(uint64_t total_size, uint64_t min_align)
        : total_size_(total_size), min_align_(min_align == 0ULL ? 4096ULL : min_align) {
    DCHECK(std::has_single_bit(min_align_));
    Reset();
}

void Bar1Allocator::SetMinAlign(uint64_t min_align) {
    DCHECK(std::has_single_bit(min_align));
    DCHECK(spans_.empty());
    min_align_ = min_align;
}

void Bar1Allocator::Reset() {
    spans_.clear();
}

std::optional<uint64_t> Bar1Allocator::AllocateGuestBlock(uint64_t size, uint64_t align) {
    if (size == 0ULL || total_size_ == 0ULL) {
        return std::nullopt;
    }
    const uint64_t eff_align = std::max(align, min_align_);
    if (!std::has_single_bit(eff_align)) {
        return std::nullopt;
    }
    const auto aligned_size_opt = AlignUp(size, min_align_);
    if (!aligned_size_opt.has_value()) {
        return std::nullopt;
    }
    const uint64_t aligned_size = *aligned_size_opt;
    if (aligned_size > total_size_) {
        return std::nullopt;
    }

    if (spans_.empty()) {
        spans_.push_back(Span{
            .offset = 0ULL, .size = total_size_, .allocated = false, .guest_block = false});
    }

    for (size_t i = 0; i < spans_.size(); ++i) {
        const Span span = spans_[i];
        if (span.allocated) {
            continue;
        }
        const auto cand_opt = AlignUp(span.offset, eff_align);
        if (!cand_opt.has_value()) {
            continue;
        }
        const uint64_t cand = *cand_opt;
        const uint64_t span_end = span.offset + span.size;
        if (cand < span.offset || cand > span_end || span_end - cand < aligned_size) {
            continue;
        }

        const uint64_t prefix = cand - span.offset;
        const uint64_t suffix = (span_end - cand) - aligned_size;

        auto it = spans_.erase(spans_.begin() + static_cast<std::ptrdiff_t>(i));
        if (prefix > 0ULL) {
            it = spans_.insert(it, Span{.offset = span.offset,
                                        .size = prefix,
                                        .allocated = false,
                                        .guest_block = false});
            ++it;
        }
        it = spans_.insert(
                it,
                Span{.offset = cand, .size = aligned_size, .allocated = true, .guest_block = true});
        ++it;
        if (suffix > 0ULL) {
            spans_.insert(it, Span{.offset = cand + aligned_size,
                                   .size = suffix,
                                   .allocated = false,
                                   .guest_block = false});
        }
        return cand;
    }
    return std::nullopt;
}

std::optional<uint64_t> Bar1Allocator::AllocateHostRegion(uint64_t size, uint64_t align) {
    if (size == 0ULL || total_size_ == 0ULL) {
        return std::nullopt;
    }
    const uint64_t eff_align = std::max(align, min_align_);
    if (!std::has_single_bit(eff_align)) {
        return std::nullopt;
    }
    const auto aligned_size_opt = AlignUp(size, min_align_);
    if (!aligned_size_opt.has_value()) {
        return std::nullopt;
    }
    const uint64_t aligned_size = *aligned_size_opt;
    if (aligned_size > total_size_) {
        return std::nullopt;
    }

    if (spans_.empty()) {
        spans_.push_back(Span{
            .offset = 0ULL, .size = total_size_, .allocated = false, .guest_block = false});
    }

    for (size_t idx = spans_.size(); idx > 0; --idx) {
        const size_t i = idx - 1;
        const Span span = spans_[i];
        if (span.allocated || span.size < aligned_size) {
            continue;
        }
        const uint64_t span_end = span.offset + span.size;
        const uint64_t cand = AlignDown(span_end - aligned_size, eff_align);
        if (cand < span.offset) {
            continue;
        }

        const uint64_t prefix = cand - span.offset;
        const uint64_t suffix = span_end - (cand + aligned_size);

        auto it = spans_.erase(spans_.begin() + static_cast<std::ptrdiff_t>(i));
        if (prefix > 0ULL) {
            it = spans_.insert(it, Span{.offset = span.offset,
                                        .size = prefix,
                                        .allocated = false,
                                        .guest_block = false});
            ++it;
        }
        it = spans_.insert(
                it,
                Span{
                    .offset = cand, .size = aligned_size, .allocated = true, .guest_block = false});
        ++it;
        if (suffix > 0ULL) {
            spans_.insert(it, Span{.offset = cand + aligned_size,
                                   .size = suffix,
                                   .allocated = false,
                                   .guest_block = false});
        }
        return cand;
    }
    return std::nullopt;
}

bool Bar1Allocator::Free(uint64_t offset) {
    for (size_t i = 0; i < spans_.size(); ++i) {
        if (spans_[i].offset != offset) {
            continue;
        }
        if (!spans_[i].allocated) {
            return false;
        }
        spans_[i].allocated = false;
        spans_[i].guest_block = false;

        if (i + 1 < spans_.size() && !spans_[i + 1].allocated) {
            spans_[i].size += spans_[i + 1].size;
            spans_.erase(spans_.begin() + static_cast<std::ptrdiff_t>(i + 1));
        }
        if (i > 0 && !spans_[i - 1].allocated) {
            spans_[i - 1].size += spans_[i].size;
            spans_.erase(spans_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (spans_.size() == 1 && !spans_[0].allocated) {
            spans_.clear();
        }
        return true;
    }
    return false;
}

bool Bar1Allocator::IsInsideGuestBlock(uint64_t offset, uint64_t size) const {
    if (size == 0ULL || offset > std::numeric_limits<uint64_t>::max() - size) {
        return false;
    }
    const uint64_t end = offset + size;
    for (const Span& span : spans_) {
        if (span.allocated && span.guest_block && offset >= span.offset &&
            end <= span.offset + span.size) {
            return true;
        }
    }
    return false;
}

std::optional<uint64_t> Bar1Allocator::GuestBlockSize(uint64_t offset) const {
    for (const Span& span : spans_) {
        if (span.allocated && span.guest_block && span.offset == offset) {
            return span.size;
        }
    }
    return std::nullopt;
}

bool Bar1Allocator::IsHostRegion(uint64_t offset, uint64_t size) const {
    const auto aligned_size_opt = AlignUp(size, min_align_);
    if (size == 0ULL || !aligned_size_opt.has_value()) {
        return false;
    }
    for (const Span& span : spans_) {
        if (span.offset == offset) {
            return span.allocated && !span.guest_block && span.size == *aligned_size_opt;
        }
    }
    return false;
}

absl::Status Bar1Allocator::RestoreSpans(uint64_t min_align, std::vector<Span> spans) {
    if (!std::has_single_bit(min_align)) {
        return absl::DataLossError(
                absl::StrCat("bar1 min_align ", min_align, " is not a power of two"));
    }
    if (!spans.empty()) {
        uint64_t expected_offset = 0ULL;
        for (size_t i = 0; i < spans.size(); ++i) {
            const Span& span = spans[i];
            if (span.offset != expected_offset) {
                return absl::DataLossError(absl::StrCat(
                        "bar1 span ", i, " offset ", span.offset,
                        " does not follow the previous span (expected ", expected_offset, ")"));
            }
            if (span.size == 0ULL || span.size > total_size_ - expected_offset) {
                return absl::DataLossError(absl::StrCat("bar1 span ", i, " size ", span.size,
                                                        " is outside [1, ",
                                                        total_size_ - expected_offset, "]"));
            }
            if (!span.allocated && span.guest_block) {
                return absl::DataLossError(
                        absl::StrCat("bar1 span ", i, " at offset ", span.offset,
                                     " is marked guest_block but not allocated"));
            }
            expected_offset += span.size;
        }
        if (expected_offset != total_size_) {
            return absl::DataLossError(absl::StrCat("bar1 spans cover ", expected_offset,
                                                    " bytes but BAR 1 size is ", total_size_));
        }
    }
    min_align_ = min_align;
    spans_ = std::move(spans);
    return absl::OkStatus();
}

}  // namespace goldfish::legacy
