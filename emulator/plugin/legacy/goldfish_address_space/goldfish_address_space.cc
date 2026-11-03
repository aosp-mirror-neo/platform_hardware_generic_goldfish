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
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

#include "android/status/status_macros.h"
#include "goldfish/archive/qemu_file_reader.h"
#include "goldfish/archive/qemu_file_writer.h"
#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/legacy/common/adapters.h"

namespace goldfish::legacy {
namespace {

constexpr uint32_t kAddressSpaceSnapshotMagic = 0x47464153U;  // "GFAS"
constexpr uint32_t kAddressSpaceSnapshotVersion = 1U;
// Most handles a guest may hold at once. Enforced on the live kAsCmdGenHandle path (ENOMEM) so
// that whatever the device accepted can also be loaded back; UniqueIdAllocator recycles ids, so
// live ids never exceed the peak handle count either.
constexpr uint32_t kMaxSnapshotHandles = 65536U;
// Largest ASG ring+buffer blob a snapshot may carry (the live device always allocates exactly
// kAsgTotalAllocationSize; the slack only tolerates a GPU that rounds the blob up).
constexpr uint64_t kMaxAsgBlobSize = 256ULL << 20;

template <typename... Args>
absl::Status ReadFields(archive::IReader& r, std::string_view context, Args&... args) {
    absl::Status st = archive::ReadValue(r, args...);
    if (!st.ok()) {
        return absl::DataLossError(
                absl::StrCat("truncated or corrupt ", context, ": ", st.message()));
    }
    return absl::OkStatus();
}

void WriteBytes(archive::IWriter& w, const void* src, size_t size) {
    if (size > 0) {
        w.Write(src, size);
    }
}

absl::Status ReadBytes(archive::IReader& r, void* dst, size_t size, std::string_view context) {
    if (size == 0) {
        return absl::OkStatus();
    }
    absl::Status st = r.Read(dst, size);
    if (!st.ok()) {
        return absl::DataLossError(absl::StrCat("truncated ", context, ": ", st.message()));
    }
    return absl::OkStatus();
}

// Every BAR 1 span, SharedSlots backing block and HostMemoryAllocator region is at least one
// page of the BAR 1 window, so a snapshot can never legitimately describe more of them than the
// window has pages (+1 for a possible trailing free span). Deriving the bound from the geometry
// instead of a constant keeps save and load symmetric: nothing the live device hands out can
// exceed it. The per-record truncation checks are what bound the memory a corrupt count can
// make the loader touch.
uint64_t MaxSnapshotRecords(uint64_t bar1_size, uint64_t page_size) {
    return bar1_size / std::max<uint64_t>(page_size, 1ULL) + 1ULL;
}

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

        const auto ring_region_size_opt = AlignUp(kAsgRingStorageSize, page_size);
        if (!ring_region_size_opt.has_value()) {
            LOG(ERROR) << "goldfish_address_space: ASG ring size overflow "
                       << "(size=" << kAsgRingStorageSize << ", page_size=" << page_size << ")";
            return nullptr;
        }
        const uint64_t ring_region_size = *ring_region_size_opt;
        const auto alloc_size_opt = AlignUp(ring_region_size + kAsgBufferSize, page_size);
        if (!alloc_size_opt.has_value()) {
            LOG(ERROR) << "goldfish_address_space: ASG allocation size overflow "
                       << "(size=" << (ring_region_size + kAsgBufferSize)
                       << ", page_size=" << page_size << ")";
            return nullptr;
        }
        const uint64_t alloc_size = *alloc_size_opt;

        absl::StatusOr<AsgBlob> blob = gpu.CreateAsgBlob(alloc_size);
        if (!blob.ok()) {
            LOG(ERROR) << "goldfish_address_space: CreateAsgBlob failed: " << blob.status();
            return nullptr;
        }
        if (blob->external_addr == nullptr || blob->size < alloc_size) {
            LOG(ERROR) << "goldfish_address_space: CreateAsgBlob returned invalid buffer "
                       << "(external_addr=" << static_cast<const void*>(blob->external_addr)
                       << ", size=" << blob->size << ", required=" << alloc_size << ")";
            gpu.CloseAsg(*blob);
            return nullptr;
        }

        const auto bar_off = allocator->AllocateHostRegion(alloc_size, page_size);
        if (!bar_off.has_value()) {
            LOG(ERROR) << "goldfish_address_space: BAR 1 out of space for ASG ring+buffer "
                       << "(size=" << alloc_size << ", page_size=" << page_size
                       << ", bar1_size=" << bar1->Size() << ")";
            gpu.CloseAsg(*blob);
            return nullptr;
        }

        if (!bar1->Map(*bar_off, blob->external_addr, alloc_size)) {
            LOG(ERROR) << "goldfish_address_space: BAR 1 Map failed for ASG ring+buffer "
                       << "(offset=" << *bar_off << ", size=" << alloc_size
                       << ", end=" << (*bar_off + alloc_size)
                       << ", bar1_size=" << bar1->Size() << ")";
            allocator->Free(*bar_off);
            gpu.CloseAsg(*blob);
            return nullptr;
        }

        std::memset(blob->external_addr, 0, static_cast<size_t>(alloc_size));
        WriteDefaultRingConfig(blob->external_addr + kAsgBufferSize);

        return std::unique_ptr<GraphicsAsgSubdevice>(
                new GraphicsAsgSubdevice(allocator, bar1, gpu, *blob, *bar_off, alloc_size));
    }

    ~GraphicsAsgSubdevice() override { TeardownNonVirtual(); }

    AddressSpaceSubdeviceType Type() const override { return AddressSpaceSubdeviceType::kGraphics; }

    void Ping(AddressSpacePing& ping) override {
        switch (static_cast<AsgControlCommand>(ping.metadata)) {
        case kAsgCmdGetRing:
            // Place the ring storage at [kAsgBufferSize, alloc_size_) and report the full
            // [0, alloc_size_) range from kAsgCmdGetBuffer. On 16 KiB-page guests (ps16k),
            // createGoldfishAddressSpaceStream claims sizeof(asg_ring_storage) == 12 KiB for the
            // ring and then mmaps it, which the 16 KiB kernel VMM rounds up to 16 KiB; having the
            // claimed buffer span cover the trailing ring page lets as_mmap succeed, while
            // ring_buffer_view_init and ring_config->buffer_size bound guest writes to
            // kAsgBufferSize.
            ping.offset = bar_offset_ + kAsgBufferSize;
            ping.size = kAsgRingStorageSize;
            ping.metadata = bar_offset_ + kAsgBufferSize;
            break;
        case kAsgCmdGetBuffer:
            ping.offset = bar_offset_;
            ping.size = alloc_size_;
            ping.metadata = bar_offset_;
            break;
        case kAsgCmdSetVersion: {
            const uint64_t guest_version = ping.size;
            const uint64_t negotiated = std::min<uint64_t>(guest_version, 1ULL);
            WriteDefaultRingConfig(blob_.external_addr + kAsgBufferSize);
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
            RestoreBufferConfigFields(blob_.external_addr + kAsgBufferSize);
            ping.metadata = 0ULL;
            break;
        default:
            LOG(WARNING) << "goldfish_address_space: unknown ASG command " << ping.metadata;
            ping.metadata = std::numeric_limits<uint64_t>::max();
            break;
        }
    }

    void Teardown() override { TeardownNonVirtual(); }

    void Save(archive::IWriter& w) const {
        w << bar_offset_ << blob_.context_handle << blob_.size << consumer_started_;
        WriteBytes(w, blob_.external_addr, static_cast<size_t>(blob_.size));
    }

    // Rebuilds one ASG subdevice from the bytes written by `Save()`. Fails with DATA_LOSS on a
    // truncated or inconsistent record, NOT_FOUND when the GPU no longer knows `context_handle`,
    // and INTERNAL when the BAR 1 mapping fails.
    static absl::StatusOr<std::unique_ptr<GraphicsAsgSubdevice>> Restore(Bar1Allocator* allocator,
                                                                         IBar1Mapper* bar1,
                                                                         IGoldfishGpu& gpu,
                                                                         archive::IReader& r) {
        uint64_t bar_offset = 0;
        uint32_t context_handle = 0;
        uint64_t blob_size = 0;
        bool consumer_started = false;
        RETURN_IF_ERROR(ReadFields(r, "asg subdevice header", bar_offset, context_handle, blob_size,
                                   consumer_started));
        if (blob_size < kAsgTotalAllocationSize || blob_size > kMaxAsgBlobSize) {
            return absl::DataLossError(absl::StrCat("asg blob_size ", blob_size, " is outside [",
                                                    kAsgTotalAllocationSize, ", ", kMaxAsgBlobSize,
                                                    "]"));
        }
        if (blob_size > bar1->Size() || bar_offset > bar1->Size() - blob_size) {
            return absl::DataLossError(absl::StrCat("asg bar_offset ", bar_offset, " + blob_size ",
                                                    blob_size, " exceeds BAR 1 size ",
                                                    bar1->Size()));
        }
        // The block must be exactly the host region the allocator snapshot says it is; anything
        // else (e.g. a guest block's offset) would be freed as a host region on teardown. This
        // also implies page alignment, so it is checked before asking the GPU for the blob.
        if (!allocator->IsHostRegion(bar_offset, blob_size)) {
            return absl::DataLossError(absl::StrCat("asg bar_offset ", bar_offset, " size ",
                                                    blob_size,
                                                    " is not a host region of the restored BAR 1 "
                                                    "allocator"));
        }
        absl::StatusOr<AsgBlob> blob = gpu.RestoreAsgBlob(context_handle, blob_size);
        if (!blob.ok()) {
            return absl::NotFoundError(
                    absl::StrCat("asg context_handle ", context_handle,
                                 " could not be restored: ", blob.status().message()));
        }
        if (blob->external_addr == nullptr || blob->size < blob_size) {
            gpu.CloseAsg(*blob);
            return absl::NotFoundError(absl::StrCat(
                    "asg context_handle ", context_handle, " restored blob of size ", blob->size,
                    blob->external_addr == nullptr ? " with no address" : "",
                    " is smaller than blob_size ", blob_size));
        }
        if (absl::Status st = ReadBytes(r, blob->external_addr, static_cast<size_t>(blob_size),
                                        "asg blob bytes");
            !st.ok()) {
            gpu.CloseAsg(*blob);
            return st;
        }
        if (!bar1->Map(bar_offset, blob->external_addr, blob_size)) {
            gpu.CloseAsg(*blob);
            return absl::InternalError(absl::StrCat("BAR 1 Map failed for asg blob at bar_offset ",
                                                    bar_offset, " size ", blob_size));
        }
        auto subdev = std::unique_ptr<GraphicsAsgSubdevice>(
                new GraphicsAsgSubdevice(allocator, bar1, gpu, *blob, bar_offset, blob_size));
        subdev->consumer_started_ = consumer_started;
        return subdev;
    }

  private:
    void TeardownNonVirtual() {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        const AsgBlob blob_copy = blob_;
        IGoldfishGpu* gpu_ptr = &gpu_;
        bar1_->UnmapAndRetire(bar_offset_,
                              [gpu_ptr, blob_copy]() { gpu_ptr->CloseAsg(blob_copy); });
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
                         AsgBlob blob, uint64_t bar_offset, uint64_t alloc_size)
            : allocator_(allocator)
            , bar1_(bar1)
            , gpu_(gpu)
            , blob_(blob)
            , bar_offset_(bar_offset)
            , alloc_size_(alloc_size) {}

    Bar1Allocator* const allocator_;
    IBar1Mapper* const bar1_;
    IGoldfishGpu& gpu_;
    AsgBlob blob_;
    const uint64_t bar_offset_;
    const uint64_t alloc_size_;
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

    void Save(archive::IWriter& w) const {
        std::vector<std::pair<uint64_t, const AlignedHostBuffer*>> sorted;
        sorted.reserve(regions_.size());
        for (const auto& [bar_offset, buf] : regions_) {
            sorted.emplace_back(bar_offset, buf.get());
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        w << static_cast<uint32_t>(sorted.size());
        for (const auto& [bar_offset, buf] : sorted) {
            w << bar_offset << buf->Size();
            WriteBytes(w, buf->Data(), static_cast<size_t>(buf->Size()));
        }
    }

    absl::Status Restore(archive::IReader& r) {
        uint32_t region_count = 0;
        RETURN_IF_ERROR(ReadFields(r, "host_memory region_count", region_count));
        const uint64_t max_regions = MaxSnapshotRecords(bar1_->Size(), page_size_);
        if (region_count > max_regions) {
            return absl::DataLossError(absl::StrCat("host_memory region_count ", region_count,
                                                    " exceeds ", max_regions));
        }
        for (uint32_t i = 0; i < region_count; ++i) {
            uint64_t bar_offset = 0;
            uint64_t size = 0;
            RETURN_IF_ERROR(ReadFields(r, absl::StrCat("host_memory region ", i, " header"),
                                       bar_offset, size));
            if (size == 0ULL || size > bar1_->Size()) {
                return absl::DataLossError(absl::StrCat("host_memory region ", i, " size ", size,
                                                        " is outside [1, ", bar1_->Size(), "]"));
            }
            if (!allocator_->IsInsideGuestBlock(bar_offset, size)) {
                return absl::NotFoundError(absl::StrCat("host_memory region ", i, " bar_offset ",
                                                        bar_offset, " size ", size,
                                                        " is not inside any restored guest block"));
            }
            if (regions_.contains(bar_offset)) {
                return absl::DataLossError(absl::StrCat("host_memory region ", i, " bar_offset ",
                                                        bar_offset, " appears twice"));
            }
            auto buf = AlignedHostBuffer::Allocate(size, page_size_);
            if (buf == nullptr) {
                return absl::ResourceExhaustedError(absl::StrCat(
                        "host_memory region ", i, ": failed to allocate ", size, " bytes"));
            }
            RETURN_IF_ERROR(ReadBytes(r, buf->Data(), static_cast<size_t>(size),
                                      absl::StrCat("host_memory region ", i, " bytes")));
            if (!bar1_->Map(bar_offset, buf->Data(), size)) {
                return absl::InternalError(absl::StrCat("BAR 1 Map failed for host_memory region ",
                                                        i, " at bar_offset ", bar_offset, " size ",
                                                        size));
            }
            regions_.emplace(bar_offset, std::move(buf));
        }
        return absl::OkStatus();
    }

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

    void Save(archive::IWriter& w) const {
        w << static_cast<uint32_t>(blocks_.size());
        for (const auto& [bar_offset, block] : blocks_) {
            w << block.bar_offset << block.size;
            WriteBytes(w, block.buffer->Data(), static_cast<size_t>(block.size));
            w << static_cast<uint32_t>(block.spans.size());
            for (const auto& [rel_off, span] : block.spans) {
                w << rel_off << span.size << span.owner_id << span.free;
            }
        }
    }

    absl::Status Restore(archive::IReader& r) {
        uint32_t block_count = 0;
        RETURN_IF_ERROR(ReadFields(r, "shared_slots block_count", block_count));
        const uint64_t max_blocks = MaxSnapshotRecords(bar1_->Size(), page_size_);
        if (block_count > max_blocks) {
            return absl::DataLossError(absl::StrCat("shared_slots block_count ", block_count,
                                                    " exceeds ", max_blocks));
        }
        for (uint32_t i = 0; i < block_count; ++i) {
            uint64_t bar_offset = 0;
            uint64_t size = 0;
            RETURN_IF_ERROR(ReadFields(r, absl::StrCat("shared_slots block ", i, " header"),
                                       bar_offset, size));
            if (size == 0ULL || size > bar1_->Size() || bar_offset > bar1_->Size() - size) {
                return absl::DataLossError(absl::StrCat("shared_slots block ", i, " bar_offset ",
                                                        bar_offset, " + size ", size,
                                                        " exceeds BAR 1 size ", bar1_->Size()));
            }
            if (!allocator_->IsHostRegion(bar_offset, size)) {
                return absl::DataLossError(absl::StrCat(
                        "shared_slots block ", i, " bar_offset ", bar_offset, " size ", size,
                        " is not a host region of the restored BAR 1 allocator"));
            }
            if (blocks_.contains(bar_offset)) {
                return absl::DataLossError(absl::StrCat("shared_slots block ", i, " bar_offset ",
                                                        bar_offset, " appears twice"));
            }
            auto buf = AlignedHostBuffer::Allocate(size, page_size_);
            if (buf == nullptr) {
                return absl::ResourceExhaustedError(absl::StrCat(
                        "shared_slots block ", i, ": failed to allocate ", size, " bytes"));
            }
            RETURN_IF_ERROR(ReadBytes(r, buf->Data(), static_cast<size_t>(size),
                                      absl::StrCat("shared_slots block ", i, " bytes")));
            uint32_t span_count = 0;
            RETURN_IF_ERROR(ReadFields(r, absl::StrCat("shared_slots block ", i, " span_count"),
                                       span_count));
            // Sub-spans are page-granular carve-outs of this block.
            const uint64_t max_spans = MaxSnapshotRecords(size, page_size_);
            if (span_count == 0U || span_count > max_spans) {
                return absl::DataLossError(absl::StrCat("shared_slots block ", i, " span_count ",
                                                        span_count, " is outside [1, ", max_spans,
                                                        "]"));
            }
            BackingBlock block;
            block.buffer = std::move(buf);
            block.bar_offset = bar_offset;
            block.size = size;
            uint64_t expected_rel = 0ULL;
            for (uint32_t s = 0; s < span_count; ++s) {
                uint64_t rel_off = 0;
                uint64_t span_size = 0;
                uint32_t owner_id = 0;
                bool is_free = false;
                RETURN_IF_ERROR(ReadFields(
                        r, absl::StrCat("shared_slots block ", i, " span ", s), rel_off, span_size,
                        owner_id, is_free));
                if (rel_off != expected_rel) {
                    return absl::DataLossError(absl::StrCat(
                            "shared_slots block ", i, " span ", s, " rel_off ", rel_off,
                            " does not follow the previous span (expected ", expected_rel, ")"));
                }
                if (span_size == 0ULL || span_size > size - expected_rel) {
                    return absl::DataLossError(absl::StrCat("shared_slots block ", i, " span ", s,
                                                            " size ", span_size, " is outside [1, ",
                                                            size - expected_rel, "]"));
                }
                expected_rel += span_size;
                block.spans.emplace(
                        rel_off,
                        SubSpan{.size = span_size, .owner_id = owner_id, .free = is_free});
            }
            if (expected_rel != size) {
                return absl::DataLossError(absl::StrCat("shared_slots block ", i, " spans cover ",
                                                        expected_rel, " bytes but block size is ",
                                                        size));
            }
            if (!bar1_->Map(bar_offset, block.buffer->Data(), size)) {
                return absl::InternalError(absl::StrCat("BAR 1 Map failed for shared_slots block ",
                                                        i, " at bar_offset ", bar_offset, " size ",
                                                        size));
            }
            blocks_.emplace(bar_offset, std::move(block));
        }
        return absl::OkStatus();
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
        if (handles_.size() >= kMaxSnapshotHandles) {
            // Keep the live device inside what LoadState() accepts, so a snapshot of any
            // reachable state can be restored.
            status_ = kAsErrnoEnomem;
            break;
        }
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

void GoldfishAddressSpaceDevice::SaveState(archive::IWriter& w) const {
    w << kAddressSpaceSnapshotMagic << kAddressSpaceSnapshotVersion << effective_page_size_
      << block_size_ << block_offset_ << ping_info_addr_ << phys_start_ << status_
      << guest_page_size_ << handle_ << allocator_.MinAlign();
    const auto& spans = allocator_.Spans();
    w << static_cast<uint32_t>(spans.size());
    for (const Bar1Allocator::Span& span : spans) {
        w << span.offset << span.size << span.allocated << span.guest_block;
    }

    shared_slots_pool_->Save(w);

    w << handle_allocator_.LastId();
    std::vector<std::pair<uint32_t, const HandleSlot*>> sorted_handles;
    sorted_handles.reserve(handles_.size());
    for (const auto& [h, slot] : handles_) {
        sorted_handles.emplace_back(h, &slot);
    }
    std::sort(sorted_handles.begin(), sorted_handles.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    w << static_cast<uint32_t>(sorted_handles.size());
    for (const auto& [h, slot] : sorted_handles) {
        const AddressSpaceSubdeviceType type = slot->subdevice != nullptr
                                                       ? slot->subdevice->Type()
                                                       : AddressSpaceSubdeviceType::kNoDevice;
        w << h << slot->ping_gpa << static_cast<uint32_t>(type);
        if (type == AddressSpaceSubdeviceType::kGraphics) {
            static_cast<const GraphicsAsgSubdevice*>(slot->subdevice.get())->Save(w);
        } else if (type == AddressSpaceSubdeviceType::kHostMemoryAllocator) {
            static_cast<const HostMemoryAllocatorSubdevice*>(slot->subdevice.get())->Save(w);
        }
    }
}

absl::Status GoldfishAddressSpaceDevice::LoadState(archive::IReader& reader) {
    bool mutated = false;
    absl::Status st = LoadStateImpl(reader, &mutated);
    if (!st.ok() && mutated) {
        // The device was already torn down for the restore; leave it in the reset state rather
        // than half-restored.
        Reset();
    }
    return st;
}

absl::Status GoldfishAddressSpaceDevice::LoadStateImpl(archive::IReader& r, bool* mutated) {
    uint32_t magic = 0;
    uint32_t version = 0;
    RETURN_IF_ERROR(ReadFields(r, "snapshot header", magic, version));
    if (magic != kAddressSpaceSnapshotMagic) {
        return absl::DataLossError(absl::StrCat("snapshot magic 0x", absl::Hex(magic),
                                                " does not match 0x",
                                                absl::Hex(kAddressSpaceSnapshotMagic)));
    }
    if (version != kAddressSpaceSnapshotVersion) {
        return absl::FailedPreconditionError(absl::StrCat("snapshot version ", version,
                                                          " is not the supported version ",
                                                          kAddressSpaceSnapshotVersion));
    }

    uint64_t effective_page_size = 0;
    uint64_t block_size = 0;
    uint64_t block_offset = 0;
    uint64_t ping_info_addr = 0;
    uint64_t phys_start = 0;
    uint32_t status = 0;
    uint32_t guest_page_size = 0;
    uint32_t handle = 0;
    RETURN_IF_ERROR(ReadFields(r, "device registers", effective_page_size, block_size, block_offset,
                               ping_info_addr, phys_start, status, guest_page_size, handle));
    if (!std::has_single_bit(effective_page_size)) {
        return absl::DataLossError(absl::StrCat("effective_page_size ", effective_page_size,
                                                " is not a power of two"));
    }
    if (!std::has_single_bit(guest_page_size)) {
        return absl::DataLossError(
                absl::StrCat("guest_page_size ", guest_page_size, " is not a power of two"));
    }

    uint64_t min_align = 0;
    uint32_t span_count = 0;
    RETURN_IF_ERROR(ReadFields(r, "bar1 span_count", min_align, span_count));
    const uint64_t max_spans = MaxSnapshotRecords(bar1_->Size(), host_page_size_);
    if (span_count > max_spans) {
        return absl::DataLossError(
                absl::StrCat("span_count ", span_count, " exceeds ", max_spans));
    }
    std::vector<Bar1Allocator::Span> spans;
    for (uint32_t i = 0; i < span_count; ++i) {
        Bar1Allocator::Span span{};
        RETURN_IF_ERROR(ReadFields(r, absl::StrCat("bar1 span ", i), span.offset, span.size,
                                   span.allocated, span.guest_block));
        spans.push_back(span);
    }

    *mutated = true;
    TeardownAllSubdevices();
    allocator_.Reset();
    effective_page_size_ = std::max(effective_page_size, host_page_size_);
    guest_page_size_ = guest_page_size;
    block_size_ = block_size;
    block_offset_ = block_offset;
    ping_info_addr_ = ping_info_addr;
    phys_start_ = phys_start;
    status_ = status;
    handle_ = handle;
    UpdateMappingTablePhysBase();

    RETURN_IF_ERROR(
            allocator_.RestoreSpans(std::max(min_align, host_page_size_), std::move(spans)));

    shared_slots_pool_->SetPageSize(effective_page_size_);
    RETURN_IF_ERROR(shared_slots_pool_->Restore(r));

    uint32_t last_handle_id = 0;
    uint32_t handle_count = 0;
    RETURN_IF_ERROR(ReadFields(r, "handle_count", last_handle_id, handle_count));
    if (handle_count > kMaxSnapshotHandles) {
        return absl::DataLossError(
                absl::StrCat("handle_count ", handle_count, " exceeds ", kMaxSnapshotHandles));
    }
    if (last_handle_id > kMaxSnapshotHandles) {
        return absl::DataLossError(
                absl::StrCat("last_handle_id ", last_handle_id, " exceeds ", kMaxSnapshotHandles));
    }

    uint32_t max_seen_handle = last_handle_id;
    for (uint32_t i = 0; i < handle_count; ++i) {
        uint32_t h = 0;
        uint64_t ping_gpa = 0;
        uint32_t raw_type = 0;
        RETURN_IF_ERROR(
                ReadFields(r, absl::StrCat("handle record ", i), h, ping_gpa, raw_type));
        const auto type = static_cast<AddressSpaceSubdeviceType>(raw_type);
        if (h == 0U) {
            return absl::DataLossError(absl::StrCat("handle record ", i, " has handle 0"));
        }
        if (h > kMaxSnapshotHandles) {
            return absl::DataLossError(
                    absl::StrCat("handle ", h, " exceeds ", kMaxSnapshotHandles));
        }
        if (handles_.contains(h)) {
            return absl::DataLossError(absl::StrCat("handle ", h, " appears twice"));
        }
        max_seen_handle = std::max(max_seen_handle, h);

        HandleSlot slot{};
        slot.ping_gpa = ping_gpa;
        switch (type) {
        case AddressSpaceSubdeviceType::kNoDevice:
            break;
        case AddressSpaceSubdeviceType::kGraphics: {
            ASSIGN_OR_RETURN(slot.subdevice,
                             GraphicsAsgSubdevice::Restore(&allocator_, bar1_.get(), gpu_, r));
            break;
        }
        case AddressSpaceSubdeviceType::kHostMemoryAllocator: {
            auto subdev = std::make_unique<HostMemoryAllocatorSubdevice>(
                    &allocator_, bar1_.get(), effective_page_size_,
                    [this]() { return EffectivePhysStart(); });
            RETURN_IF_ERROR(subdev->Restore(r));
            slot.subdevice = std::move(subdev);
            break;
        }
        case AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator:
            slot.subdevice = std::make_unique<SharedSlotsSubdevice>(
                    shared_slots_pool_.get(), h, [this]() { return EffectivePhysStart(); });
            break;
        default:
            return absl::DataLossError(absl::StrCat(
                    "handle ", h, " has unsupported subdevice type ", static_cast<uint32_t>(type)));
        }
        handles_.emplace(h, std::move(slot));
    }

    handle_allocator_.Reset();
    for (uint32_t id = 1U; id <= max_seen_handle; ++id) {
        (void)handle_allocator_.Get();
    }
    for (uint32_t id = 1U; id <= max_seen_handle; ++id) {
        if (!handles_.contains(id)) {
            handle_allocator_.Put(id);
        }
    }

    UpdateMappingTablePhysBase();
    return absl::OkStatus();
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

extern "C" bool GoldfishAddressSpaceCoreSave(const void* core, QEMUFile* f) {
    if (core == nullptr || f == nullptr) {
        return false;
    }
    const auto* dev = static_cast<const goldfish::legacy::GoldfishAddressSpaceDevice*>(
            static_cast<const goldfish::legacy::IMmioDevice*>(core));
    goldfish::archive::QEMUFileWriter writer(f);
    dev->SaveState(writer);
    return true;
}

extern "C" bool GoldfishAddressSpaceCoreLoad(void* core, QEMUFile* f) {
    if (core == nullptr || f == nullptr) {
        return false;
    }
    auto* dev = static_cast<goldfish::legacy::GoldfishAddressSpaceDevice*>(
            static_cast<goldfish::legacy::IMmioDevice*>(core));
    goldfish::archive::QEMUFileReader reader(f);
    const absl::Status st = dev->LoadState(reader);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_address_space: load failed: " << st;
        return false;
    }
    return true;
}
