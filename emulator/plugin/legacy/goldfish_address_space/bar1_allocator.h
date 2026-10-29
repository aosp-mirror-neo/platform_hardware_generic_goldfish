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

#include <cstdint>
#include <optional>
#include <vector>

namespace goldfish::legacy {

std::optional<uint64_t> AlignUp(uint64_t value, uint64_t align);

class Bar1Allocator {
  public:
    Bar1Allocator(uint64_t total_size, uint64_t min_align);

    void Reset();

    // Allocates from the bottom of BAR 1 (used by guest COMMAND_ALLOCATE_BLOCK).
    std::optional<uint64_t> AllocateGuestBlock(uint64_t size, uint64_t align);

    // Allocates from the top of BAR 1 (used by host-managed ASG and shared-slot subdevices).
    std::optional<uint64_t> AllocateHostRegion(uint64_t size, uint64_t align);

    bool Free(uint64_t offset);

    bool IsInsideGuestBlock(uint64_t offset, uint64_t size) const;
    std::optional<uint64_t> GuestBlockSize(uint64_t offset) const;

    uint64_t TotalSize() const { return total_size_; }
    uint64_t MinAlign() const { return min_align_; }
    bool IsEmpty() const { return spans_.empty(); }
    void SetMinAlign(uint64_t min_align);

  private:
    struct Span {
        uint64_t offset = 0;
        uint64_t size = 0;
        bool allocated = false;
        bool guest_block = false;
    };

    static uint64_t AlignDown(uint64_t value, uint64_t align);

    const uint64_t total_size_;
    uint64_t min_align_;
    std::vector<Span> spans_;
};

}  // namespace goldfish::legacy
