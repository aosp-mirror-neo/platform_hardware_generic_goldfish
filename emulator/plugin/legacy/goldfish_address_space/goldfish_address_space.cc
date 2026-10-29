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

#include "goldfish_address_space.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <unistd.h>
#endif

#include "absl/base/thread_annotations.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/legacy/common/adapters.h"

namespace goldfish::legacy {
namespace {

uint64_t QueryHostPageSize() {
#if defined(_WIN32)
    return 4096ULL;
#else
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 && std::has_single_bit(static_cast<uint64_t>(page_size))) {
        return static_cast<uint64_t>(page_size);
    }
    return 4096ULL;
#endif
}

class AlignedHostBuffer {
  public:
    AlignedHostBuffer() = default;

    AlignedHostBuffer(const AlignedHostBuffer&) = delete;
    AlignedHostBuffer& operator=(const AlignedHostBuffer&) = delete;

    static std::shared_ptr<AlignedHostBuffer> Allocate(uint64_t size, uint64_t align) {
        if (size == 0ULL || !std::has_single_bit(align)) {
            return nullptr;
        }
        const uint64_t eff_align = std::max<uint64_t>(align, sizeof(void*));
        void* raw = nullptr;
#if defined(_WIN32)
        raw = _aligned_malloc(static_cast<size_t>(size), static_cast<size_t>(eff_align));
#else
        if (posix_memalign(&raw, static_cast<size_t>(eff_align), static_cast<size_t>(size)) != 0) {
            raw = nullptr;
        }
#endif
        if (raw == nullptr) {
            return nullptr;
        }
        std::memset(raw, 0, static_cast<size_t>(size));
        auto buf = std::make_shared<AlignedHostBuffer>();
        buf->ptr_.reset(raw);
        buf->size_ = size;
        return buf;
    }

    void* Data() const { return ptr_.get(); }
    uint64_t Size() const { return size_; }

    void Reset() {
        ptr_.reset();
        size_ = 0ULL;
    }

  private:
    struct AlignedDeleter {
        void operator()(void* ptr) const {
#if defined(_WIN32)
            _aligned_free(ptr);
#else
            std::free(ptr);
#endif
        }
    };

    std::unique_ptr<void, AlignedDeleter> ptr_;
    uint64_t size_ = 0ULL;
};

// ============================================================================
// Subdevice 0: GraphicsAsgSubdevice
// ============================================================================
class GraphicsAsgSubdevice final : public IAddressSpaceSubdevice {
  public:
    static std::unique_ptr<GraphicsAsgSubdevice> Create(Bar1Allocator* allocator, IBar1Mapper* bar1,
                                                        IGoldfishGpu& gpu, uint64_t page_size) {
        DCHECK(allocator != nullptr);
        DCHECK(bar1 != nullptr);

        absl::StatusOr<AsgBlob> blob = gpu.CreateAsgBlob(kAsgTotalAllocationSize);
        if (!blob.ok()) {
            LOG(ERROR) << "goldfish_address_space: CreateAsgBlob failed: " << blob.status();
            return nullptr;
        }
        if (blob->external_addr == nullptr || blob->size < kAsgTotalAllocationSize) {
            LOG(ERROR) << "goldfish_address_space: CreateAsgBlob returned invalid buffer "
                       << "(external_addr=" << static_cast<const void*>(blob->external_addr)
                       << ", size=" << blob->size << ", required=" << kAsgTotalAllocationSize
                       << ")";
            gpu.DestroyAsg(*blob);
            return nullptr;
        }

        const auto bar_off = allocator->AllocateHostRegion(kAsgTotalAllocationSize, page_size);
        if (!bar_off.has_value()) {
            LOG(ERROR) << "goldfish_address_space: BAR 1 out of space for ASG ring+buffer "
                       << "(size=" << kAsgTotalAllocationSize << ", page_size=" << page_size
                       << ", bar1_size=" << bar1->Size() << ")";
            gpu.DestroyAsg(*blob);
            return nullptr;
        }

        if (!bar1->Map(*bar_off, blob->external_addr, kAsgTotalAllocationSize)) {
            LOG(ERROR) << "goldfish_address_space: BAR 1 Map failed for ASG ring+buffer "
                       << "(offset=" << *bar_off << ", size=" << kAsgTotalAllocationSize
                       << ", end=" << (*bar_off + kAsgTotalAllocationSize)
                       << ", bar1_size=" << bar1->Size() << ")";
            allocator->Free(*bar_off);
            gpu.DestroyAsg(*blob);
            return nullptr;
        }

        std::memset(blob->external_addr, 0, static_cast<size_t>(kAsgTotalAllocationSize));
        WriteDefaultRingConfig(blob->external_addr);

        return std::unique_ptr<GraphicsAsgSubdevice>(
                new GraphicsAsgSubdevice(allocator, bar1, gpu, *blob, *bar_off));
    }

    ~GraphicsAsgSubdevice() override { TeardownNonVirtual(); }

    AddressSpaceSubdeviceType Type() const override { return AddressSpaceSubdeviceType::kGraphics; }

    void Ping(AddressSpacePing& ping) override {
        switch (static_cast<AsgControlCommand>(ping.metadata)) {
        case kAsgCmdGetRing:
            ping.offset = bar_offset_;
            ping.size = kAsgRingStorageSize;
            ping.metadata = bar_offset_;
            break;
        case kAsgCmdGetBuffer:
            ping.offset = bar_offset_ + kAsgRingStorageSize;
            ping.size = kAsgBufferSize;
            ping.metadata = bar_offset_ + kAsgRingStorageSize;
            break;
        case kAsgCmdSetVersion: {
            const uint64_t guest_version = ping.size;
            const uint64_t negotiated = std::min<uint64_t>(guest_version, 1ULL);
            WriteDefaultRingConfig(blob_.external_addr);
            if (!consumer_started_) {
                const absl::Status status = gpu_.StartAsg(blob_);
                if (!status.ok()) {
                    LOG(ERROR) << "goldfish_address_space: StartAsg failed: " << status;
                    ping.metadata = std::numeric_limits<uint64_t>::max();
                    break;
                }
                consumer_started_ = true;
            }
            ping.size = negotiated;
            ping.metadata = 0ULL;
            break;
        }
        case kAsgCmdNotifyAvailable: {
            if (!consumer_started_) {
                const absl::Status start_status = gpu_.StartAsg(blob_);
                if (!start_status.ok()) {
                    LOG(ERROR) << "goldfish_address_space: lazy StartAsg failed: " << start_status;
                    ping.metadata = std::numeric_limits<uint64_t>::max();
                    break;
                }
                consumer_started_ = true;
            }
            const absl::Status status = gpu_.PingAsg(blob_);
            if (!status.ok()) {
                LOG(WARNING) << "goldfish_address_space: PingAsg failed: " << status;
                ping.metadata = std::numeric_limits<uint64_t>::max();
                break;
            }
            ping.metadata = 0ULL;
            break;
        }
        case kAsgCmdGetConfig:
            RestoreBufferConfigFields(blob_.external_addr);
            ping.metadata = 0ULL;
            break;
        default:
            LOG(WARNING) << "goldfish_address_space: unknown ASG command " << ping.metadata;
            ping.metadata = std::numeric_limits<uint64_t>::max();
            break;
        }
    }

    void Teardown() override { TeardownNonVirtual(); }

  private:
    void TeardownNonVirtual() {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        const AsgBlob blob_copy = blob_;
        IGoldfishGpu* gpu_ptr = &gpu_;
        bar1_->UnmapAndRetire(bar_offset_,
                              [gpu_ptr, blob_copy]() { gpu_ptr->DestroyAsg(blob_copy); });
        allocator_->Free(bar_offset_);
        blob_ = AsgBlob{};
        consumer_started_ = false;
    }

    static void WriteDefaultRingConfig(uint8_t* base) {
        AsgRingConfig cfg{};
        cfg.buffer_size = static_cast<uint32_t>(kAsgBufferSize);
        cfg.flush_interval = kAsgFlushInterval;
        cfg.host_consumed_pos = 0U;
        cfg.guest_write_pos = 0U;
        cfg.transfer_mode = 1U;
        cfg.transfer_size = 0U;
        cfg.in_error = 0U;
        std::memcpy(base + kAsgRingConfigOffsetBytes, &cfg, sizeof(cfg));
    }

    static void RestoreBufferConfigFields(uint8_t* base) {
        AsgRingConfig cfg{};
        std::memcpy(&cfg, base + kAsgRingConfigOffsetBytes, sizeof(cfg));
        cfg.buffer_size = static_cast<uint32_t>(kAsgBufferSize);
        cfg.flush_interval = kAsgFlushInterval;
        std::memcpy(base + kAsgRingConfigOffsetBytes, &cfg, sizeof(cfg));
    }

    GraphicsAsgSubdevice(Bar1Allocator* allocator, IBar1Mapper* bar1, IGoldfishGpu& gpu,
                         AsgBlob blob, uint64_t bar_offset)
            : allocator_(allocator), bar1_(bar1), gpu_(gpu), blob_(blob), bar_offset_(bar_offset) {}

    Bar1Allocator* const allocator_;
    IBar1Mapper* const bar1_;
    IGoldfishGpu& gpu_;
    AsgBlob blob_;
    const uint64_t bar_offset_;
    bool consumer_started_ = false;
    bool torn_down_ = false;
};

// ============================================================================
// Subdevice 5: HostMemoryAllocatorSubdevice
// ============================================================================
class HostMemoryAllocatorSubdevice final : public IAddressSpaceSubdevice {
  public:
    HostMemoryAllocatorSubdevice(Bar1Allocator* allocator, IBar1Mapper* bar1, uint64_t page_size,
                                 PhysStartProvider phys_start_provider)
            : allocator_(allocator)
            , bar1_(bar1)
            , phys_start_provider_(std::move(phys_start_provider))
            , page_size_(page_size == 0ULL ? 4096ULL : page_size) {
        DCHECK(allocator_ != nullptr);
        DCHECK(bar1_ != nullptr);
        DCHECK(phys_start_provider_ != nullptr);
    }

    ~HostMemoryAllocatorSubdevice() override { TeardownNonVirtual(); }

    AddressSpaceSubdeviceType Type() const override {
        return AddressSpaceSubdeviceType::kHostMemoryAllocator;
    }

    void Ping(AddressSpacePing& ping) override {
        switch (static_cast<HostMemoryAllocatorCommand>(ping.metadata)) {
        case kHostMemCmdAllocate:
            HandleAllocate(ping);
            break;
        case kHostMemCmdUnallocate:
            HandleUnallocate(ping);
            break;
        default:
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            break;
        }
    }

    void Teardown() override { TeardownNonVirtual(); }

  private:
    void TeardownNonVirtual() {
        for (auto& [bar_offset, buf] : regions_) {
            std::shared_ptr<AlignedHostBuffer> retired_buf = std::move(buf);
            bar1_->UnmapAndRetire(bar_offset, [retired_buf]() { retired_buf->Reset(); });
        }
        regions_.clear();
    }
    std::optional<uint64_t> TranslateToBarOffset(uint64_t gpa_or_offset) const {
        const uint64_t phys_start = phys_start_provider_();
        const uint64_t bar_offset = (phys_start != 0ULL && gpa_or_offset >= phys_start)
                                            ? (gpa_or_offset - phys_start)
                                            : gpa_or_offset;
        if (bar_offset >= bar1_->Size()) {
            return std::nullopt;
        }
        return bar_offset;
    }

    void HandleAllocate(AddressSpacePing& ping) {
        if (ping.size == 0ULL) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        const auto bar_offset_opt = TranslateToBarOffset(ping.offset);
        if (!bar_offset_opt.has_value()) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        const uint64_t bar_offset = *bar_offset_opt;
        if ((bar_offset % page_size_) != 0ULL) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        const auto aligned_size_opt = AlignUp(ping.size, page_size_);
        if (!aligned_size_opt.has_value()) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        const uint64_t aligned_size = *aligned_size_opt;
        if (!allocator_->IsInsideGuestBlock(bar_offset, aligned_size) ||
            regions_.contains(bar_offset)) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }

        auto buf = AlignedHostBuffer::Allocate(aligned_size, page_size_);
        if (buf == nullptr) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEnomem));
            return;
        }
        if (!bar1_->Map(bar_offset, buf->Data(), aligned_size)) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        regions_.emplace(bar_offset, std::move(buf));
        ping.metadata = 0ULL;
    }

    void HandleUnallocate(AddressSpacePing& ping) {
        const auto bar_offset_opt = TranslateToBarOffset(ping.offset);
        if (!bar_offset_opt.has_value()) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        auto it = regions_.find(*bar_offset_opt);
        if (it == regions_.end()) {
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            return;
        }
        const uint64_t bar_offset = it->first;
        std::shared_ptr<AlignedHostBuffer> retired_buf = std::move(it->second);
        regions_.erase(it);
        bar1_->UnmapAndRetire(bar_offset, [retired_buf]() { retired_buf->Reset(); });
        ping.metadata = 0ULL;
    }

    Bar1Allocator* const allocator_;
    IBar1Mapper* const bar1_;
    const PhysStartProvider phys_start_provider_;
    const uint64_t page_size_;
    absl::flat_hash_map<uint64_t, std::shared_ptr<AlignedHostBuffer>> regions_;
};

}  // namespace

// ============================================================================
// Subdevice 6: SharedSlotsPool & SharedSlotsSubdevice
// ============================================================================
class SharedSlotsPool {
  public:
    SharedSlotsPool(Bar1Allocator* allocator, IBar1Mapper* bar1, uint64_t page_size)
            : allocator_(allocator)
            , bar1_(bar1)
            , page_size_(page_size == 0ULL ? 4096ULL : page_size) {
        DCHECK(allocator_ != nullptr);
        DCHECK(bar1_ != nullptr);
    }

    ~SharedSlotsPool() { Reset(); }

    SharedSlotsPool(const SharedSlotsPool&) = delete;
    SharedSlotsPool& operator=(const SharedSlotsPool&) = delete;

    void SetPageSize(uint64_t page_size) {
        DCHECK(std::has_single_bit(page_size));
        page_size_ = page_size;
    }

    uint64_t Allocate(uint32_t owner_id, uint64_t requested_size, uint64_t* out_bar_offset,
                      uint64_t* out_allocated_size) {
        if (requested_size == 0ULL || out_bar_offset == nullptr || out_allocated_size == nullptr) {
            return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
        }
        const auto aligned_opt = AlignUp(requested_size, page_size_);
        if (!aligned_opt.has_value()) {
            return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
        }
        const uint64_t aligned_size = *aligned_opt;

        for (auto& [block_offset, block] : blocks_) {
            if (auto sub_off = TryCarve(&block, owner_id, aligned_size)) {
                *out_bar_offset = *sub_off;
                *out_allocated_size = aligned_size;
                return 0ULL;
            }
        }

        constexpr uint64_t kDefaultBackingSize = 64ULL * 1024ULL;
        const uint64_t backing_size = std::max(aligned_size, kDefaultBackingSize);
        const auto bar_off = allocator_->AllocateHostRegion(backing_size, page_size_);
        if (!bar_off.has_value()) {
            return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEnomem));
        }
        auto buf = AlignedHostBuffer::Allocate(backing_size, page_size_);
        if (buf == nullptr) {
            allocator_->Free(*bar_off);
            return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEnomem));
        }
        if (!bar1_->Map(*bar_off, buf->Data(), backing_size)) {
            allocator_->Free(*bar_off);
            return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
        }

        BackingBlock block;
        block.buffer = std::move(buf);
        block.bar_offset = *bar_off;
        block.size = backing_size;
        block.spans.emplace(0ULL, SubSpan{.size = backing_size, .owner_id = 0U, .free = true});
        const auto sub_off = TryCarve(&block, owner_id, aligned_size);
        DCHECK(sub_off.has_value());
        blocks_.emplace(*bar_off, std::move(block));
        *out_bar_offset = *sub_off;
        *out_allocated_size = aligned_size;
        return 0ULL;
    }

    uint64_t Unallocate(uint32_t owner_id, uint64_t bar_offset) {
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            BackingBlock& block = it->second;
            if (bar_offset < block.bar_offset || bar_offset >= block.bar_offset + block.size) {
                continue;
            }
            const uint64_t rel = bar_offset - block.bar_offset;
            auto span_it = block.spans.find(rel);
            if (span_it == block.spans.end() || span_it->second.free ||
                span_it->second.owner_id != owner_id) {
                return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            }
            MarkSpanFree(&block, span_it);
            if (IsEntirelyFree(block)) {
                RetireBlock(it);
            }
            return 0ULL;
        }
        return static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
    }

    void ReleaseOwner(uint32_t owner_id) {
        for (auto it = blocks_.begin(); it != blocks_.end();) {
            BackingBlock& block = it->second;
            for (auto span_it = block.spans.begin(); span_it != block.spans.end(); ++span_it) {
                if (!span_it->second.free && span_it->second.owner_id == owner_id) {
                    span_it = MarkSpanFree(&block, span_it);
                }
            }
            if (IsEntirelyFree(block)) {
                it = RetireBlock(it);
            } else {
                ++it;
            }
        }
    }

    void Reset() {
        for (auto& [bar_offset, block] : blocks_) {
            std::shared_ptr<AlignedHostBuffer> retired_buf = std::move(block.buffer);
            bar1_->UnmapAndRetire(bar_offset, [retired_buf]() { retired_buf->Reset(); });
            allocator_->Free(bar_offset);
        }
        blocks_.clear();
    }

  private:
    struct SubSpan {
        uint64_t size = 0;
        uint32_t owner_id = 0;
        bool free = true;
    };

    struct BackingBlock {
        std::shared_ptr<AlignedHostBuffer> buffer;
        uint64_t bar_offset = 0;
        uint64_t size = 0;
        std::map<uint64_t, SubSpan> spans;
    };

    static std::optional<uint64_t> TryCarve(BackingBlock* block, uint32_t owner_id,
                                            uint64_t aligned_size) {
        for (auto it = block->spans.begin(); it != block->spans.end(); ++it) {
            if (!it->second.free || it->second.size < aligned_size) {
                continue;
            }
            const uint64_t rel_off = it->first;
            const uint64_t rem = it->second.size - aligned_size;
            it->second.size = aligned_size;
            it->second.owner_id = owner_id;
            it->second.free = false;
            if (rem > 0ULL) {
                block->spans.emplace(rel_off + aligned_size,
                                     SubSpan{.size = rem, .owner_id = 0U, .free = true});
            }
            return block->bar_offset + rel_off;
        }
        return std::nullopt;
    }

    static std::map<uint64_t, SubSpan>::iterator MarkSpanFree(
            BackingBlock* block, std::map<uint64_t, SubSpan>::iterator it) {
        it->second.free = true;
        it->second.owner_id = 0U;

        auto next_it = std::next(it);
        if (next_it != block->spans.end() && next_it->second.free) {
            it->second.size += next_it->second.size;
            block->spans.erase(next_it);
        }
        if (it != block->spans.begin()) {
            auto prev_it = std::prev(it);
            if (prev_it->second.free) {
                prev_it->second.size += it->second.size;
                block->spans.erase(it);
                return prev_it;
            }
        }
        return it;
    }

    static bool IsEntirelyFree(const BackingBlock& block) {
        return block.spans.size() == 1U && block.spans.begin()->second.free;
    }

    std::map<uint64_t, BackingBlock>::iterator RetireBlock(
            std::map<uint64_t, BackingBlock>::iterator it) {
        const uint64_t bar_offset = it->first;
        std::shared_ptr<AlignedHostBuffer> retired_buf = std::move(it->second.buffer);
        auto next_it = blocks_.erase(it);
        bar1_->UnmapAndRetire(bar_offset, [retired_buf]() { retired_buf->Reset(); });
        allocator_->Free(bar_offset);
        return next_it;
    }

    Bar1Allocator* const allocator_;
    IBar1Mapper* const bar1_;
    uint64_t page_size_;
    std::map<uint64_t, BackingBlock> blocks_;
};

namespace {

class SharedSlotsSubdevice final : public IAddressSpaceSubdevice {
  public:
    SharedSlotsSubdevice(SharedSlotsPool* pool, uint32_t owner_id,
                         PhysStartProvider phys_start_provider)
            : pool_(pool)
            , phys_start_provider_(std::move(phys_start_provider))
            , owner_id_(owner_id) {
        DCHECK(pool_ != nullptr);
        DCHECK(phys_start_provider_ != nullptr);
    }

    ~SharedSlotsSubdevice() override { TeardownNonVirtual(); }

    AddressSpaceSubdeviceType Type() const override {
        return AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator;
    }

    void Ping(AddressSpacePing& ping) override {
        switch (static_cast<SharedSlotsCommand>(ping.metadata)) {
        case kSharedSlotsCmdAllocate: {
            uint64_t bar_offset = 0ULL;
            uint64_t allocated_size = 0ULL;
            const uint64_t res =
                    pool_->Allocate(owner_id_, ping.size, &bar_offset, &allocated_size);
            if (res == 0ULL) {
                ping.offset = bar_offset;
                ping.size = allocated_size;
            }
            ping.metadata = res;
            break;
        }
        case kSharedSlotsCmdUnallocate: {
            uint64_t res = pool_->Unallocate(owner_id_, ping.offset);
            if (res != 0ULL) {
                const uint64_t phys_start = phys_start_provider_();
                if (phys_start != 0ULL && ping.offset >= phys_start) {
                    res = pool_->Unallocate(owner_id_, ping.offset - phys_start);
                }
            }
            ping.metadata = res;
            break;
        }
        case kSharedSlotsCmdCheckIfSupported:
            ping.metadata = 0ULL;
            break;
        default:
            ping.metadata = static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval));
            break;
        }
    }

    void Teardown() override { TeardownNonVirtual(); }

  private:
    void TeardownNonVirtual() {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        pool_->ReleaseOwner(owner_id_);
    }
    SharedSlotsPool* const pool_;
    const PhysStartProvider phys_start_provider_;
    const uint32_t owner_id_;
    bool torn_down_ = false;
};

}  // namespace

// ============================================================================
// Bar1MappingTable & TrackingBar1Mapper
// ============================================================================
class GoldfishAddressSpaceDevice::Bar1MappingTable {
  public:
    explicit Bar1MappingTable(uint64_t bar1_size) : bar1_size_(bar1_size) {}

    void SetBar1PhysBase(uint64_t phys_base) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        bar1_phys_base_ = phys_base;
    }

    void Insert(uint64_t offset, void* host_ptr, uint64_t size) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        mappings_[offset] = Entry{.host_ptr = static_cast<uint8_t*>(host_ptr), .size = size};
    }

    void Erase(uint64_t offset) ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        mappings_.erase(offset);
    }

    void Clear() ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        mappings_.clear();
        bar1_phys_base_ = 0ULL;
    }

    void* Resolve(uint64_t gpa_or_offset) const ABSL_LOCKS_EXCLUDED(mu_) {
        absl::MutexLock lock(&mu_);
        uint64_t bar_offset = gpa_or_offset;
        if (bar1_phys_base_ != 0ULL && gpa_or_offset >= bar1_phys_base_) {
            const uint64_t candidate = gpa_or_offset - bar1_phys_base_;
            if (candidate < bar1_size_) {
                bar_offset = candidate;
            }
        }
        if (bar_offset >= bar1_size_ || mappings_.empty()) {
            return nullptr;
        }
        auto it = mappings_.upper_bound(bar_offset);
        if (it == mappings_.begin()) {
            return nullptr;
        }
        --it;
        const uint64_t start = it->first;
        const Entry& entry = it->second;
        if (bar_offset - start >= entry.size) {
            return nullptr;
        }
        return entry.host_ptr + (bar_offset - start);
    }

  private:
    struct Entry {
        uint8_t* host_ptr = nullptr;
        uint64_t size = 0;
    };

    const uint64_t bar1_size_;
    uint64_t bar1_phys_base_ ABSL_GUARDED_BY(mu_) = 0;
    std::map<uint64_t, Entry> mappings_ ABSL_GUARDED_BY(mu_);
    mutable absl::Mutex mu_;
};

namespace {

template <typename TableT>
class TrackingBar1Mapper final : public IBar1Mapper {
  public:
    using BqlCallback = std::function<void()>;

    TrackingBar1Mapper(std::unique_ptr<IBar1Mapper> inner, std::shared_ptr<TableT> table)
            : inner_(std::move(inner)), table_(std::move(table)) {
        DCHECK(inner_ != nullptr);
        DCHECK(table_ != nullptr);
    }

    uint64_t Size() const override { return inner_->Size(); }
    uint64_t HostPageSize() const override { return inner_->HostPageSize(); }
    uint64_t GuestPhysBase() const override { return inner_->GuestPhysBase(); }

    bool Map(uint64_t offset, void* host_ptr, uint64_t size) override {
        if (!inner_->Map(offset, host_ptr, size)) {
            return false;
        }
        table_->Insert(offset, host_ptr, size);
        return true;
    }

    void Unmap(uint64_t offset) override {
        table_->Erase(offset);
        inner_->Unmap(offset);
    }

    void UnmapAndRetire(uint64_t offset, OnRetiredCallback on_retired) override {
        table_->Erase(offset);
        inner_->UnmapAndRetire(offset, std::move(on_retired));
    }

    void RunUnderBql(const BqlCallback& fn) override { inner_->RunUnderBql(fn); }

  private:
    const std::unique_ptr<IBar1Mapper> inner_;
    const std::shared_ptr<TableT> table_;
};

}  // namespace

// ============================================================================
// GoldfishAddressSpaceDevice
// ============================================================================
GoldfishAddressSpaceDevice::GoldfishAddressSpaceDevice(std::unique_ptr<IGuestMemory> memory,
                                                       std::unique_ptr<IBar1Mapper> bar1,
                                                       SetIrqCallback set_irq, IGoldfishGpu& gpu)
        : memory_(std::move(memory))
        , mapping_table_(std::make_shared<Bar1MappingTable>(bar1->Size()))
        , bar1_(std::make_shared<TrackingBar1Mapper<Bar1MappingTable>>(std::move(bar1),
                                                                       mapping_table_))
        , user_memory_link_(std::make_shared<UserMemoryLink>())
        , set_irq_(std::move(set_irq))
        , gpu_(gpu)
        , host_page_size_(bar1_->HostPageSize() == 0ULL ? 4096ULL : bar1_->HostPageSize())
        , effective_page_size_(std::max<uint64_t>(4096ULL, host_page_size_))
        , allocator_(bar1_->Size(), effective_page_size_)
        , shared_slots_pool_(std::make_unique<SharedSlotsPool>(&allocator_, bar1_.get(),
                                                               effective_page_size_)) {
    DCHECK(memory_ != nullptr);
    DCHECK(bar1_ != nullptr);
    DCHECK(set_irq_ != nullptr);
    DCHECK(std::has_single_bit(host_page_size_));

    user_memory_link_->bar1 = bar1_;
    user_memory_link_->device.store(this, std::memory_order_release);

    std::shared_ptr<UserMemoryLink> link = user_memory_link_;
    std::shared_ptr<Bar1MappingTable> table = mapping_table_;
    gpu_.SetMemoryMappingCallbacks(
            [link](uint64_t gpa, void* hva, uint64_t size) -> bool {
                bool ok = false;
                link->bar1->RunUnderBql([&]() {
                    GoldfishAddressSpaceDevice* self = link->device.load(std::memory_order_acquire);
                    if (self != nullptr) {
                        ok = self->MapUserMemoryUnderBql(gpa, hva, size);
                    }
                });
                return ok;
            },
            [link](uint64_t gpa, uint64_t size) -> bool {
                bool ok = false;
                link->bar1->RunUnderBql([&]() {
                    GoldfishAddressSpaceDevice* self = link->device.load(std::memory_order_acquire);
                    if (self != nullptr) {
                        ok = self->UnmapUserMemoryUnderBql(gpa, size);
                    }
                });
                return ok;
            },
            [table](uint64_t gpa) -> void* { return table->Resolve(gpa); });
}

GoldfishAddressSpaceDevice::~GoldfishAddressSpaceDevice() {
    user_memory_link_->device.store(nullptr, std::memory_order_release);
    gpu_.SetMemoryMappingCallbacks(nullptr, nullptr, nullptr);
    TeardownAllSubdevices();
}

uint64_t GoldfishAddressSpaceDevice::EffectivePhysStart() const {
    const uint64_t live_bar1 = bar1_->GuestPhysBase();
    if (live_bar1 != 0ULL) {
        return live_bar1;
    }
    return phys_start_;
}

void GoldfishAddressSpaceDevice::UpdateMappingTablePhysBase() {
    mapping_table_->SetBar1PhysBase(EffectivePhysStart());
}

void GoldfishAddressSpaceDevice::TeardownAllSubdevices() {
    for (auto& [handle, slot] : handles_) {
        if (slot.subdevice != nullptr) {
            slot.subdevice->Teardown();
        }
    }
    handles_.clear();
    shared_slots_pool_->Reset();

    for (const auto& [gpa, mapping] : user_mappings_) {
        bar1_->Unmap(mapping.bar_offset);
    }
    user_mappings_.clear();
    mapping_table_->Clear();
}

void GoldfishAddressSpaceDevice::Reset() {
    TeardownAllSubdevices();
    effective_page_size_ = std::max<uint64_t>(4096ULL, host_page_size_);
    allocator_.Reset();
    allocator_.SetMinAlign(effective_page_size_);
    shared_slots_pool_->SetPageSize(effective_page_size_);
    handle_allocator_.Reset();

    status_ = kAsStatusOk;
    guest_page_size_ = 4096U;
    block_size_ = 0ULL;
    block_offset_ = 0ULL;
    ping_info_addr_ = 0ULL;
    handle_ = 0U;
    phys_start_ = 0ULL;

    set_irq_(false);
}

uint32_t GoldfishAddressSpaceDevice::Read(uint32_t offset) {
    switch (offset) {
    case kAsRegCommand:
        return 0U;
    case kAsRegStatus:
        return status_;
    case kAsRegGuestPageSize:
        return guest_page_size_;
    case kAsRegBlockSizeLow:
        return static_cast<uint32_t>(block_size_ & 0xFFFFFFFFULL);
    case kAsRegBlockSizeHigh:
        return static_cast<uint32_t>(block_size_ >> 32);
    case kAsRegBlockOffsetLow:
        return static_cast<uint32_t>(block_offset_ & 0xFFFFFFFFULL);
    case kAsRegBlockOffsetHigh:
        return static_cast<uint32_t>(block_offset_ >> 32);
    case kAsRegPing:
        return 0U;
    case kAsRegPingInfoAddrLow:
        return static_cast<uint32_t>(ping_info_addr_ & 0xFFFFFFFFULL);
    case kAsRegPingInfoAddrHigh:
        return static_cast<uint32_t>(ping_info_addr_ >> 32);
    case kAsRegHandle:
        return handle_;
    case kAsRegPhysStartLow:
        return static_cast<uint32_t>(phys_start_ & 0xFFFFFFFFULL);
    case kAsRegPhysStartHigh:
        return static_cast<uint32_t>(phys_start_ >> 32);
    default:
        LOG(WARNING) << "goldfish_address_space: unexpected read at 0x" << std::hex << offset;
        return 0U;
    }
}

void GoldfishAddressSpaceDevice::Write(uint32_t offset, uint32_t value) {
    switch (offset) {
    case kAsRegCommand:
        ExecuteCommand(value);
        break;
    case kAsRegStatus:
        status_ = value;
        break;
    case kAsRegGuestPageSize:
        if (!std::has_single_bit(value) || !allocator_.IsEmpty() || !handles_.empty()) {
            status_ = kAsErrnoEinval;
            break;
        }
        guest_page_size_ = value;
        effective_page_size_ = std::max(static_cast<uint64_t>(guest_page_size_), host_page_size_);
        allocator_.SetMinAlign(effective_page_size_);
        shared_slots_pool_->SetPageSize(effective_page_size_);
        break;
    case kAsRegBlockSizeLow:
        block_size_ = (block_size_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    case kAsRegBlockSizeHigh:
        block_size_ = (block_size_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case kAsRegBlockOffsetLow:
        block_offset_ = (block_offset_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    case kAsRegBlockOffsetHigh:
        block_offset_ =
                (block_offset_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case kAsRegPing:
        ExecutePing(value);
        break;
    case kAsRegPingInfoAddrLow:
        ping_info_addr_ = (ping_info_addr_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    case kAsRegPingInfoAddrHigh:
        ping_info_addr_ =
                (ping_info_addr_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case kAsRegHandle:
        handle_ = value;
        break;
    case kAsRegPhysStartLow:
        phys_start_ = (phys_start_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        UpdateMappingTablePhysBase();
        break;
    case kAsRegPhysStartHigh:
        phys_start_ = (phys_start_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        UpdateMappingTablePhysBase();
        break;
    default:
        LOG(WARNING) << "goldfish_address_space: unexpected write at 0x" << std::hex << offset;
        break;
    }
}

void GoldfishAddressSpaceDevice::UnmapUserMappingsInRange(uint64_t block_offset,
                                                          uint64_t block_size) {
    const uint64_t block_end = block_offset + block_size;
    for (auto it = user_mappings_.begin(); it != user_mappings_.end();) {
        const uint64_t map_start = it->second.bar_offset;
        const uint64_t map_end = map_start + it->second.size;
        if (map_start < block_end && block_offset < map_end) {
            bar1_->Unmap(map_start);
            it = user_mappings_.erase(it);
        } else {
            ++it;
        }
    }
}

void GoldfishAddressSpaceDevice::ExecuteCommand(uint32_t cmd) {
    UpdateMappingTablePhysBase();
    switch (cmd) {
    case kAsCmdAllocateBlock: {
        if (block_size_ == 0ULL) {
            status_ = kAsErrnoEinval;
            break;
        }
        // Note: this extra-page padding matches legacy goldfish_address_space_alloc_shared
        // (external/qemu/hw/pci/goldfish_address_space.c:577-583) and is intentional — guest
        // GoldfishAddressSpaceBlockProvider sub-allocates blocks to cover block_size_ bytes at
        // an arbitrary intra-page offset, which spans ceil(size / P) + 1 pages when unaligned.
        // Replacing it with AlignUp(block_size_, effective_page_size_) would break
        // MapUserMemoryUnderBql's IsInsideGuestBlock check.
        const uint64_t pad = 2ULL * effective_page_size_ - 2ULL;
        if (block_size_ > std::numeric_limits<uint64_t>::max() - pad) {
            status_ = kAsErrnoEinval;
            break;
        }
        const uint64_t aligned_size =
                ((block_size_ + pad) / effective_page_size_) * effective_page_size_;
        const auto offset_opt = allocator_.AllocateGuestBlock(aligned_size, effective_page_size_);
        if (!offset_opt.has_value()) {
            status_ = kAsErrnoEnomem;
            break;
        }
        block_offset_ = *offset_opt;
        block_size_ = aligned_size;
        status_ = kAsStatusOk;
        break;
    }
    case kAsCmdDeallocateBlock: {
        const auto guest_size = allocator_.GuestBlockSize(block_offset_);
        if (!guest_size.has_value()) {
            status_ = kAsErrnoEinval;
            break;
        }
        UnmapUserMappingsInRange(block_offset_, *guest_size);
        if (!allocator_.Free(block_offset_)) {
            status_ = kAsErrnoEinval;
            break;
        }
        status_ = kAsStatusOk;
        break;
    }
    case kAsCmdGenHandle: {
        const uint32_t handle = handle_allocator_.Get();
        handles_.emplace(handle, HandleSlot{});
        handle_ = handle;
        status_ = kAsStatusOk;
        break;
    }
    case kAsCmdDestroyHandle: {
        auto it = handles_.find(handle_);
        if (it == handles_.end()) {
            status_ = kAsErrnoEinval;
            break;
        }
        if (it->second.subdevice != nullptr) {
            it->second.subdevice->Teardown();
        }
        handles_.erase(it);
        handle_allocator_.Put(handle_);
        status_ = kAsStatusOk;
        break;
    }
    case kAsCmdTellPingInfoAddr: {
        auto it = handles_.find(handle_);
        if (it == handles_.end() || ping_info_addr_ == 0ULL) {
            status_ = kAsErrnoEinval;
            break;
        }
        it->second.ping_gpa = ping_info_addr_;
        status_ = kAsStatusOk;
        break;
    }
    default:
        status_ = kAsErrnoEnosys;
        break;
    }

    set_irq_(true);
    set_irq_(false);
}

void GoldfishAddressSpaceDevice::ExecutePing(uint32_t handle) {
    UpdateMappingTablePhysBase();
    auto it = handles_.find(handle);
    if (it == handles_.end() || it->second.ping_gpa == 0ULL) {
        status_ = kAsErrnoEinval;
        return;
    }

    AddressSpacePing ping{};
    if (!memory_->Read(it->second.ping_gpa, &ping, sizeof(ping))) {
        status_ = kAsErrnoEinval;
        return;
    }

    HandleSlot& slot = it->second;
    if (slot.subdevice == nullptr) {
        const auto dev_type = static_cast<AddressSpaceSubdeviceType>(ping.metadata & 0xFFFFFFFFULL);
        switch (dev_type) {
        case AddressSpaceSubdeviceType::kGraphics:
            slot.subdevice = GraphicsAsgSubdevice::Create(&allocator_, bar1_.get(), gpu_,
                                                          effective_page_size_);
            if (slot.subdevice == nullptr) {
                status_ = kAsErrnoEnomem;
                return;
            }
            break;
        case AddressSpaceSubdeviceType::kHostMemoryAllocator:
            slot.subdevice = std::make_unique<HostMemoryAllocatorSubdevice>(
                    &allocator_, bar1_.get(), effective_page_size_,
                    [this]() { return EffectivePhysStart(); });
            break;
        case AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator:
            slot.subdevice = std::make_unique<SharedSlotsSubdevice>(
                    shared_slots_pool_.get(), handle, [this]() { return EffectivePhysStart(); });
            break;
        default:
            LOG(WARNING) << "goldfish_address_space: unsupported subdevice type "
                         << static_cast<uint32_t>(dev_type);
            status_ = kAsErrnoEnosys;
            return;
        }
        ping.metadata = static_cast<uint64_t>(dev_type);
    } else {
        slot.subdevice->Ping(ping);
    }

    if (!memory_->Write(it->second.ping_gpa, &ping, sizeof(ping))) {
        status_ = kAsErrnoEinval;
        return;
    }
    status_ = kAsStatusOk;
}

bool GoldfishAddressSpaceDevice::MapUserMemory(uint64_t gpa, void* hva, uint64_t size) {
    bool ok = false;
    bar1_->RunUnderBql([&]() { ok = MapUserMemoryUnderBql(gpa, hva, size); });
    return ok;
}

bool GoldfishAddressSpaceDevice::UnmapUserMemory(uint64_t gpa, uint64_t size) {
    bool ok = false;
    bar1_->RunUnderBql([&]() { ok = UnmapUserMemoryUnderBql(gpa, size); });
    return ok;
}

void* GoldfishAddressSpaceDevice::ResolveDmaHostAddr(uint64_t gpa) const {
    return mapping_table_->Resolve(gpa);
}

bool GoldfishAddressSpaceDevice::MapUserMemoryUnderBql(uint64_t gpa, void* hva, uint64_t size) {
    if (hva == nullptr || size == 0ULL) {
        return false;
    }
    UpdateMappingTablePhysBase();
    const uint64_t phys_start = EffectivePhysStart();
    if (phys_start != 0ULL && gpa < phys_start) {
        return false;
    }
    const uint64_t bar_offset = (phys_start != 0ULL) ? (gpa - phys_start) : gpa;
    if ((bar_offset % host_page_size_) != 0ULL || (size % host_page_size_) != 0ULL ||
        (reinterpret_cast<uintptr_t>(hva) % static_cast<uintptr_t>(host_page_size_)) != 0U) {
        return false;
    }
    if (!allocator_.IsInsideGuestBlock(bar_offset, size) || user_mappings_.contains(gpa)) {
        return false;
    }
    if (!bar1_->Map(bar_offset, hva, size)) {
        return false;
    }
    user_mappings_.emplace(gpa, UserMemoryMapping{.bar_offset = bar_offset, .size = size});
    return true;
}

bool GoldfishAddressSpaceDevice::UnmapUserMemoryUnderBql(uint64_t gpa, uint64_t size) {
    auto it = user_mappings_.find(gpa);
    if (it == user_mappings_.end()) {
        return false;
    }
    if (size != 0ULL && it->second.size != size) {
        return false;
    }
    const uint64_t bar_offset = it->second.bar_offset;
    user_mappings_.erase(it);
    bar1_->Unmap(bar_offset);
    return true;
}

}  // namespace goldfish::legacy

extern "C" GoldfishMmioCore GoldfishAddressSpaceCoreCreate(const GoldfishDeviceEnv* env) {
    DCHECK(env != nullptr);
    auto dev = std::make_unique<goldfish::legacy::GoldfishAddressSpaceDevice>(
            goldfish::legacy::WrapGuestMemory(env->memory), goldfish::legacy::WrapBar1(env->bar1),
            goldfish::legacy::WrapIrq(env->irq), goldfish::legacy::GetGoldfishGpu());
    return goldfish::legacy::ToMmioCore(std::move(dev));
}

extern "C" uint64_t GoldfishAddressSpaceHostPageSize(void) {
    return goldfish::legacy::QueryHostPageSize();
}
