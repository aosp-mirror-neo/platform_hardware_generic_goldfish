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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "android/status/status_matcher_macros.h"
#include "goldfish/archive/deque_archive.h"
#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/legacy/common/fake_guest_memory.h"
#include "goldfish/legacy/common/i_bar1_mapper.h"
#include "goldfish/legacy/goldfish_gpu/fake_goldfish_gpu.h"

struct QEMUFile {
    goldfish::archive::DequeArchive archive;
};

extern "C" size_t qemu_get_buffer(  // NOLINT(readability-identifier-naming)
        QEMUFile* f, uint8_t* buf, size_t size) {
    if (f == nullptr || buf == nullptr || size == 0) {
        return 0;
    }
    const size_t n = std::min(size, f->archive.storage.size());
    if (n > 0) {
        (void)f->archive.Read(buf, n);
    }
    return n;
}

extern "C" void qemu_put_buffer(  // NOLINT(readability-identifier-naming)
        QEMUFile* f, const uint8_t* buf, size_t size) {
    if (f != nullptr && buf != nullptr && size > 0) {
        f->archive.Write(buf, size);
    }
}

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

class FakeBar1State {
  public:
    struct Mapping {
        void* hva = nullptr;
        uint64_t size = 0;
    };

    absl::flat_hash_map<uint64_t, Mapping> mappings;
    std::vector<IBar1Mapper::OnRetiredCallback> deferred_retire_cbs;
    uint64_t bar1_size = 1ULL << 30;  // 1 GiB
    uint64_t host_page_size = 4096ULL;
    uint64_t guest_phys_base = 0x100000000ULL;
    bool defer_rcu = false;
    absl::Mutex bql_mu;

    void FlushRcu() {
        std::vector<IBar1Mapper::OnRetiredCallback> cbs;
        cbs.swap(deferred_retire_cbs);
        for (auto& cb : cbs) {
            if (cb) {
                cb();
            }
        }
    }
};

class FakeBar1Mapper : public IBar1Mapper {
  public:
    using BqlCallback = std::function<void()>;

    explicit FakeBar1Mapper(std::shared_ptr<FakeBar1State> state) : state_(std::move(state)) {}

    uint64_t Size() const override { return state_->bar1_size; }
    uint64_t HostPageSize() const override { return state_->host_page_size; }
    uint64_t GuestPhysBase() const override { return state_->guest_phys_base; }

    bool Map(uint64_t offset, void* host_ptr, uint64_t size) override {
        const uint64_t host_page = state_->host_page_size;
        if (host_ptr == nullptr || size == 0ULL || host_page == 0ULL ||
            (offset % host_page) != 0ULL || (size % host_page) != 0ULL ||
            (reinterpret_cast<uintptr_t>(host_ptr) % static_cast<uintptr_t>(host_page)) != 0U ||
            offset > state_->bar1_size || size > state_->bar1_size - offset) {
            return false;
        }
        // The real mapper refuses a sub-region that overlaps a live one.
        for (const auto& [off, m] : state_->mappings) {
            if (offset < off + m.size && off < offset + size) {
                return false;
            }
        }
        state_->mappings[offset] = FakeBar1State::Mapping{.hva = host_ptr, .size = size};
        return true;
    }

    void Unmap(uint64_t offset) override { state_->mappings.erase(offset); }

    void UnmapAndRetire(uint64_t offset, OnRetiredCallback on_retired) override {
        state_->mappings.erase(offset);
        if (on_retired) {
            if (state_->defer_rcu) {
                state_->deferred_retire_cbs.push_back(std::move(on_retired));
            } else {
                on_retired();
            }
        }
    }

    void RunUnderBql(const BqlCallback& fn) override {
        absl::MutexLock lock(&state_->bql_mu);
        fn();
    }

  private:
    const std::shared_ptr<FakeBar1State> state_;
};

class TestGoldfishGpu : public FakeGoldfishGpu {
  public:
    TestGoldfishGpu() : TestGoldfishGpu(false) {}
    explicit TestGoldfishGpu(bool keep_stale_callbacks)
            : asg_backing_(kAsgTotalAllocationSize + 131072ULL, 0)
            , keep_stale_callbacks_(keep_stale_callbacks) {}

    void SetMemoryMappingCallbacks(MapUserMemoryCallback map_cb, UnmapUserMemoryCallback unmap_cb,
                                   ResolveDmaHostAddrCallback resolve_cb) override {
        if (map_cb == nullptr && keep_stale_callbacks_) {
            return;
        }
        map_user_memory_cb = std::move(map_cb);
        unmap_user_memory_cb = std::move(unmap_cb);
        resolve_dma_cb = std::move(resolve_cb);
    }

    absl::StatusOr<AsgBlob> CreateAsgBlob(uint64_t size) override {
        uintptr_t raw = reinterpret_cast<uintptr_t>(asg_backing_.data());
        uintptr_t aligned = (raw + 65535ULL) & ~static_cast<uintptr_t>(65535ULL);
        AsgBlob blob;
        blob.external_addr = reinterpret_cast<uint8_t*>(aligned);
        blob.size = size;
        blob.context_handle = 7;
        blob.blob_id = 11;
        create_asg_calls++;
        return blob;
    }

    absl::Status StartAsg(const AsgBlob& /*blob*/) override {
        start_asg_calls++;
        return absl::OkStatus();
    }

    absl::Status PingAsg(const AsgBlob& /*blob*/) override {
        ping_asg_calls++;
        return absl::OkStatus();
    }

    void CloseAsg(const AsgBlob& /*blob*/) override { destroy_asg_calls++; }

    // The GPU's own VMState sections drive its snapshot; the device must never call these.
    absl::Status Quiesce(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::StatusOr<std::vector<uint8_t>> Save(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return std::vector<uint8_t>{};
    }
    absl::Status PostSave(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status PreLoad(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status Load(std::string_view /*snapshot_name*/, const uint8_t* /*meta_data*/,
                      size_t /*meta_size*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }
    absl::Status PostLoad(std::string_view /*snapshot_name*/) override {
        gpu_snapshot_calls++;
        return absl::OkStatus();
    }

    MapUserMemoryCallback map_user_memory_cb;
    UnmapUserMemoryCallback unmap_user_memory_cb;
    ResolveDmaHostAddrCallback resolve_dma_cb;
    int create_asg_calls = 0;
    int start_asg_calls = 0;
    int ping_asg_calls = 0;
    int destroy_asg_calls = 0;
    int gpu_snapshot_calls = 0;

  private:
    std::vector<uint8_t> asg_backing_;
    const bool keep_stale_callbacks_ = false;
};

TEST(GoldfishAddressSpaceTest, AllocateAndDeallocateGuestBlockAndMapUserMemory) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    auto bar1_state = std::make_shared<FakeBar1State>();
    auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
    TestGoldfishGpu gpu;
    int irq_pulses = 0;

    GoldfishAddressSpaceDevice dev(
            std::move(mem), std::move(bar1),
            [&irq_pulses](bool high) {
                if (high) {
                    irq_pulses++;
                }
            },
            gpu);

    dev.Write(kAsRegBlockSizeLow, 0x4000);
    dev.Write(kAsRegBlockSizeHigh, 0);
    dev.Write(kAsRegCommand, kAsCmdAllocateBlock);
    EXPECT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);
    EXPECT_EQ(irq_pulses, 1);

    uint64_t offset = (static_cast<uint64_t>(dev.Read(kAsRegBlockOffsetHigh)) << 32) |
                      dev.Read(kAsRegBlockOffsetLow);
    EXPECT_EQ(offset, 0U);

    alignas(4096) uint8_t host_buf[16384]{};
    const uint64_t gpa = bar1_state->guest_phys_base + offset;
    ASSERT_TRUE(gpu.map_user_memory_cb(gpa, host_buf, sizeof(host_buf)));
    EXPECT_EQ(gpu.resolve_dma_cb(gpa + 128), host_buf + 128);

    ASSERT_TRUE(gpu.unmap_user_memory_cb(gpa, sizeof(host_buf)));
    EXPECT_EQ(gpu.resolve_dma_cb(gpa), nullptr);

    dev.Write(kAsRegBlockOffsetLow, static_cast<uint32_t>(offset));
    dev.Write(kAsRegBlockOffsetHigh, static_cast<uint32_t>(offset >> 32));
    dev.Write(kAsRegCommand, kAsCmdDeallocateBlock);
    EXPECT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);
}

TEST(GoldfishAddressSpaceTest, GraphicsAsgSubdeviceDefersDestroyUntilRcuRetirement) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    auto bar1_state = std::make_shared<FakeBar1State>();
    bar1_state->defer_rcu = true;
    auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
    TestGoldfishGpu gpu;

    {
        GoldfishAddressSpaceDevice dev(std::move(mem), std::move(bar1), [](bool /*high*/) {}, gpu);

        dev.Write(kAsRegCommand, kAsCmdGenHandle);
        uint32_t handle = dev.Read(kAsRegHandle);
        ASSERT_NE(handle, 0U);

        constexpr uint64_t kPingGpa = 0x1000;
        dev.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(kPingGpa));
        dev.Write(kAsRegPingInfoAddrHigh, 0);
        dev.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);

        AddressSpacePing ping{};
        ping.metadata = static_cast<uint64_t>(AddressSpaceSubdeviceType::kGraphics);
        mem_ptr->Put(kPingGpa, ping);
        dev.Write(kAsRegPing, handle);
        EXPECT_EQ(gpu.create_asg_calls, 1);

        ping = AddressSpacePing{};
        ping.metadata = kAsgCmdSetVersion;
        ping.size = 1;
        mem_ptr->Put(kPingGpa, ping);
        dev.Write(kAsRegPing, handle);
        ping = mem_ptr->Get<AddressSpacePing>(kPingGpa);
        EXPECT_EQ(ping.metadata, 0ULL);
        EXPECT_EQ(ping.size, 1ULL);
        EXPECT_EQ(gpu.start_asg_calls, 1);

        ping = AddressSpacePing{};
        ping.metadata = kAsgCmdNotifyAvailable;
        mem_ptr->Put(kPingGpa, ping);
        dev.Write(kAsRegPing, handle);
        EXPECT_EQ(gpu.ping_asg_calls, 1);
    }

    // Device has been destroyed, but RCU has not yet retired the BAR 1 subregion.
    EXPECT_EQ(gpu.destroy_asg_calls, 0);
    bar1_state->FlushRcu();
    EXPECT_EQ(gpu.destroy_asg_calls, 1);
}

TEST(GoldfishAddressSpaceTest, StaleGpuCallbacksAfterDeviceDestructionAreSafeNoOps) {
    auto bar1_state = std::make_shared<FakeBar1State>();
    TestGoldfishGpu gpu(/*keep_stale_callbacks=*/true);
    alignas(4096) uint8_t host_buf[16384]{};
    const uint64_t gpa = bar1_state->guest_phys_base;

    {
        auto mem = std::make_unique<FakeGuestMemory>(0x10000);
        auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
        GoldfishAddressSpaceDevice dev(std::move(mem), std::move(bar1), [](bool /*high*/) {}, gpu);

        dev.Write(kAsRegBlockSizeLow, 0x4000);
        dev.Write(kAsRegBlockSizeHigh, 0);
        dev.Write(kAsRegCommand, kAsCmdAllocateBlock);
        ASSERT_TRUE(gpu.map_user_memory_cb(gpa, host_buf, sizeof(host_buf)));
        ASSERT_EQ(gpu.resolve_dma_cb(gpa), host_buf);
    }

    // After GoldfishAddressSpaceDevice destruction, stale callbacks must not UAF.
    EXPECT_FALSE(gpu.map_user_memory_cb(gpa, host_buf, sizeof(host_buf)));
    EXPECT_FALSE(gpu.unmap_user_memory_cb(gpa, sizeof(host_buf)));
    EXPECT_EQ(gpu.resolve_dma_cb(gpa), nullptr);
}

TEST(GoldfishAddressSpaceTest, ConcurrentResolveDmaHostAddrWhileMappingAndUnmapping) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    auto bar1_state = std::make_shared<FakeBar1State>();
    auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
    TestGoldfishGpu gpu;

    GoldfishAddressSpaceDevice dev(std::move(mem), std::move(bar1), [](bool /*high*/) {}, gpu);

    dev.Write(kAsRegBlockSizeLow, 0x20000);
    dev.Write(kAsRegBlockSizeHigh, 0);
    dev.Write(kAsRegCommand, kAsCmdAllocateBlock);
    ASSERT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);

    alignas(4096) uint8_t host_buf[16384]{};
    const uint64_t gpa = bar1_state->guest_phys_base;
    std::atomic<bool> done{false};

    std::thread reader([&]() {
        while (!done.load(std::memory_order_relaxed)) {
            void* p = gpu.resolve_dma_cb(gpa + 64);
            if (p != nullptr) {
                EXPECT_EQ(p, host_buf + 64);
            }
        }
    });

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(gpu.map_user_memory_cb(gpa, host_buf, sizeof(host_buf)));
        ASSERT_TRUE(gpu.unmap_user_memory_cb(gpa, sizeof(host_buf)));
    }
    done.store(true, std::memory_order_relaxed);
    reader.join();
}

TEST(GoldfishAddressSpaceTest, Bar1AllocatorBottomUpAndTopDownCollisionAndCoalescing) {
    constexpr uint64_t kTotalSize = 64ULL * 1024ULL;
    constexpr uint64_t kMinAlign = 4096ULL;
    Bar1Allocator alloc(kTotalSize, kMinAlign);

    // Bottom-up guest block allocations start from 0.
    const auto g1 = alloc.AllocateGuestBlock(8192ULL, 4096ULL);
    ASSERT_TRUE(g1.has_value());
    EXPECT_EQ(*g1, 0ULL);
    EXPECT_TRUE(alloc.IsInsideGuestBlock(0ULL, 8192ULL));
    EXPECT_EQ(alloc.GuestBlockSize(0ULL), 8192ULL);

    // Next guest block with 16 KiB alignment skips [8 KiB, 16 KiB) and lands at 16 KiB.
    const auto g2 = alloc.AllocateGuestBlock(16384ULL, 16384ULL);
    ASSERT_TRUE(g2.has_value());
    EXPECT_EQ(*g2, 16384ULL);
    EXPECT_TRUE(alloc.IsInsideGuestBlock(16384ULL, 16384ULL));

    // Top-down host region allocation starts from the top (64 KiB - 16 KiB = 48 KiB).
    const auto h1 = alloc.AllocateHostRegion(16384ULL, 16384ULL);
    ASSERT_TRUE(h1.has_value());
    EXPECT_EQ(*h1, 48ULL * 1024ULL);
    // Host regions must NOT be treated as guest blocks.
    EXPECT_FALSE(alloc.IsInsideGuestBlock(*h1, 16384ULL));
    EXPECT_FALSE(alloc.GuestBlockSize(*h1).has_value());

    // Remaining free spans are [8 KiB, 16 KiB) (8 KiB) and [32 KiB, 48 KiB) (16 KiB).
    // A 20 KiB allocation from either end must fail without colliding.
    EXPECT_FALSE(alloc.AllocateGuestBlock(20ULL * 1024ULL, 4096ULL).has_value());
    EXPECT_FALSE(alloc.AllocateHostRegion(20ULL * 1024ULL, 4096ULL).has_value());

    // An 8 KiB guest allocation fits into the [8 KiB, 16 KiB) gap.
    const auto g3 = alloc.AllocateGuestBlock(8192ULL, 4096ULL);
    ASSERT_TRUE(g3.has_value());
    EXPECT_EQ(*g3, 8192ULL);

    // Free g1, g3, g2, and h1; adjacent free spans must coalesce back into [0, 64 KiB).
    EXPECT_TRUE(alloc.Free(*g1));
    EXPECT_TRUE(alloc.Free(*g2));
    EXPECT_TRUE(alloc.Free(*g3));
    EXPECT_TRUE(alloc.Free(*h1));
    EXPECT_FALSE(alloc.Free(*h1));

    const auto full = alloc.AllocateGuestBlock(kTotalSize, kMinAlign);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(*full, 0ULL);
}

TEST(GoldfishAddressSpaceTest, HostMemoryAllocatorSubdeviceAllocateAndUnallocate) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    auto bar1_state = std::make_shared<FakeBar1State>();
    bar1_state->defer_rcu = true;
    auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
    TestGoldfishGpu gpu;

    GoldfishAddressSpaceDevice dev(std::move(mem), std::move(bar1), [](bool /*high*/) {}, gpu);

    // First allocate a guest block in BAR 1.
    dev.Write(kAsRegBlockSizeLow, 0x8000);
    dev.Write(kAsRegBlockSizeHigh, 0);
    dev.Write(kAsRegCommand, kAsCmdAllocateBlock);
    ASSERT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);
    const uint64_t block_offset = (static_cast<uint64_t>(dev.Read(kAsRegBlockOffsetHigh)) << 32) |
                                  dev.Read(kAsRegBlockOffsetLow);

    // Open HostMemoryAllocatorSubdevice (type 5).
    dev.Write(kAsRegCommand, kAsCmdGenHandle);
    const uint32_t handle = dev.Read(kAsRegHandle);
    ASSERT_NE(handle, 0U);

    constexpr uint64_t kPingGpa = 0x1000;
    dev.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(kPingGpa));
    dev.Write(kAsRegPingInfoAddrHigh, 0);
    dev.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);
    ASSERT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);

    AddressSpacePing ping{};
    ping.metadata = static_cast<uint64_t>(AddressSpaceSubdeviceType::kHostMemoryAllocator);
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    ASSERT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);

    // Allocating outside the guest block must fail with -EINVAL.
    ping = AddressSpacePing{};
    ping.metadata = kHostMemCmdAllocate;
    ping.offset = 0x200000ULL;
    ping.size = 4096ULL;
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpa).metadata,
              static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval)));

    // Allocating inside the guest block (using GPA) succeeds and maps into BAR 1.
    const uint64_t target_gpa = bar1_state->guest_phys_base + block_offset + 4096ULL;
    ping = AddressSpacePing{};
    ping.metadata = kHostMemCmdAllocate;
    ping.offset = target_gpa;
    ping.size = 4096ULL;
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpa).metadata, 0ULL);
    EXPECT_TRUE(bar1_state->mappings.contains(block_offset + 4096ULL));
    EXPECT_NE(gpu.resolve_dma_cb(target_gpa), nullptr);

    // Duplicate allocation at the same offset is rejected.
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpa).metadata,
              static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval)));

    // Unallocating removes the BAR 1 mapping and queues RCU retirement.
    ping = AddressSpacePing{};
    ping.metadata = kHostMemCmdUnallocate;
    ping.offset = target_gpa;
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpa).metadata, 0ULL);
    EXPECT_FALSE(bar1_state->mappings.contains(block_offset + 4096ULL));
    EXPECT_EQ(gpu.resolve_dma_cb(target_gpa), nullptr);
    EXPECT_EQ(bar1_state->deferred_retire_cbs.size(), 1U);
    bar1_state->FlushRcu();
}

TEST(GoldfishAddressSpaceTest, SharedSlotsSubdeviceCarvingOwnerIsolationAndCoalescing) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    auto bar1_state = std::make_shared<FakeBar1State>();
    bar1_state->defer_rcu = true;
    auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
    TestGoldfishGpu gpu;

    GoldfishAddressSpaceDevice dev(std::move(mem), std::move(bar1), [](bool /*high*/) {}, gpu);

    auto open_shared_slots = [&](uint64_t ping_gpa) -> uint32_t {
        dev.Write(kAsRegCommand, kAsCmdGenHandle);
        const uint32_t h = dev.Read(kAsRegHandle);
        dev.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(ping_gpa));
        dev.Write(kAsRegPingInfoAddrHigh, 0);
        dev.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);
        AddressSpacePing p{};
        p.metadata =
                static_cast<uint64_t>(AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator);
        mem_ptr->Put(ping_gpa, p);
        dev.Write(kAsRegPing, h);
        return h;
    };

    constexpr uint64_t kPingGpaA = 0x1000;
    constexpr uint64_t kPingGpaB = 0x2000;
    const uint32_t handle_a = open_shared_slots(kPingGpaA);
    const uint32_t handle_b = open_shared_slots(kPingGpaB);
    ASSERT_NE(handle_a, handle_b);

    // CheckIfSupported returns 0.
    AddressSpacePing ping{};
    ping.metadata = kSharedSlotsCmdCheckIfSupported;
    mem_ptr->Put(kPingGpaA, ping);
    dev.Write(kAsRegPing, handle_a);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpaA).metadata, 0ULL);

    // Allocate 16 KiB on handle A and 16 KiB on handle B; both carve from the same 64 KiB block.
    ping = AddressSpacePing{.size = 16384ULL, .metadata = kSharedSlotsCmdAllocate};
    mem_ptr->Put(kPingGpaA, ping);
    dev.Write(kAsRegPing, handle_a);
    const auto alloc_a1 = mem_ptr->Get<AddressSpacePing>(kPingGpaA);
    ASSERT_EQ(alloc_a1.metadata, 0ULL);
    EXPECT_EQ(alloc_a1.size, 16384ULL);

    ping = AddressSpacePing{.size = 16384ULL, .metadata = kSharedSlotsCmdAllocate};
    mem_ptr->Put(kPingGpaB, ping);
    dev.Write(kAsRegPing, handle_b);
    const auto alloc_b1 = mem_ptr->Get<AddressSpacePing>(kPingGpaB);
    ASSERT_EQ(alloc_b1.metadata, 0ULL);
    EXPECT_EQ(alloc_b1.offset, alloc_a1.offset + 16384ULL);

    // Allocate a second 16 KiB sub-span on handle A so freeing alloc_a1 tests coalescing.
    ping = AddressSpacePing{.size = 16384ULL, .metadata = kSharedSlotsCmdAllocate};
    mem_ptr->Put(kPingGpaA, ping);
    dev.Write(kAsRegPing, handle_a);
    const auto alloc_a2 = mem_ptr->Get<AddressSpacePing>(kPingGpaA);
    ASSERT_EQ(alloc_a2.metadata, 0ULL);
    EXPECT_EQ(alloc_a2.offset, alloc_b1.offset + 16384ULL);
    EXPECT_EQ(bar1_state->mappings.size(), 1U);

    // Owner isolation: handle B cannot unallocate handle A's sub-span.
    ping = AddressSpacePing{.offset = alloc_a1.offset, .metadata = kSharedSlotsCmdUnallocate};
    mem_ptr->Put(kPingGpaB, ping);
    dev.Write(kAsRegPing, handle_b);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpaB).metadata,
              static_cast<uint64_t>(-static_cast<int64_t>(kAsErrnoEinval)));

    // Destroying handle A triggers ReleaseOwner(handle_a), freeing alloc_a1 and alloc_a2 while
    // keeping the backing block alive for alloc_b1.
    dev.Write(kAsRegHandle, handle_a);
    dev.Write(kAsRegCommand, kAsCmdDestroyHandle);
    ASSERT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);
    EXPECT_EQ(bar1_state->mappings.size(), 1U);

    // Unallocating handle B's remaining sub-span (using GPA translation) coalesces all sub-spans
    // and retires the entire 64 KiB backing block.
    ping = AddressSpacePing{.offset = bar1_state->guest_phys_base + alloc_b1.offset,
                            .metadata = kSharedSlotsCmdUnallocate};
    mem_ptr->Put(kPingGpaB, ping);
    dev.Write(kAsRegPing, handle_b);
    EXPECT_EQ(mem_ptr->Get<AddressSpacePing>(kPingGpaB).metadata, 0ULL);
    EXPECT_TRUE(bar1_state->mappings.empty());
    EXPECT_EQ(bar1_state->deferred_retire_cbs.size(), 1U);
    bar1_state->FlushRcu();
}

TEST(GoldfishAddressSpaceTest, GuestPageSize16KPropagatesToSharedSlotsAndBar1Allocator) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    auto bar1_state = std::make_shared<FakeBar1State>();
    bar1_state->host_page_size = 4096ULL;
    auto bar1 = std::make_unique<FakeBar1Mapper>(bar1_state);
    TestGoldfishGpu gpu;

    GoldfishAddressSpaceDevice dev(std::move(mem), std::move(bar1), [](bool /*high*/) {}, gpu);

    // 16 KiB-page guest writes 16384 to GUEST_PAGE_SIZE on a 4 KiB-page host.
    dev.Write(kAsRegGuestPageSize, 16384U);
    EXPECT_EQ(dev.Read(kAsRegGuestPageSize), 16384U);

    // Guest block allocations must be aligned/padded to 16 KiB pages.
    dev.Write(kAsRegBlockSizeLow, 4096U);
    dev.Write(kAsRegBlockSizeHigh, 0U);
    dev.Write(kAsRegCommand, kAsCmdAllocateBlock);
    ASSERT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);
    EXPECT_EQ(dev.Read(kAsRegBlockSizeLow), 32768U);

    dev.Write(kAsRegCommand, kAsCmdGenHandle);
    const uint32_t handle = dev.Read(kAsRegHandle);
    ASSERT_NE(handle, 0U);

    constexpr uint64_t kPingGpa = 0x1000;
    dev.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(kPingGpa));
    dev.Write(kAsRegPingInfoAddrHigh, 0);
    dev.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);

    AddressSpacePing ping{};
    ping.metadata =
            static_cast<uint64_t>(AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator);
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);

    // Allocate two 4 KiB requests; both must be rounded up and aligned to 16 KiB boundaries.
    ping = AddressSpacePing{};
    ping.metadata = kSharedSlotsCmdAllocate;
    ping.size = 4096ULL;
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    const auto first = mem_ptr->Get<AddressSpacePing>(kPingGpa);
    EXPECT_EQ(first.metadata, 0ULL);
    EXPECT_EQ(first.size, 16384ULL);
    EXPECT_EQ(first.offset % 16384ULL, 0ULL);

    ping = AddressSpacePing{};
    ping.metadata = kSharedSlotsCmdAllocate;
    ping.size = 4096ULL;
    mem_ptr->Put(kPingGpa, ping);
    dev.Write(kAsRegPing, handle);
    const auto second = mem_ptr->Get<AddressSpacePing>(kPingGpa);
    EXPECT_EQ(second.metadata, 0ULL);
    EXPECT_EQ(second.size, 16384ULL);
    EXPECT_EQ(second.offset % 16384ULL, 0ULL);
    EXPECT_EQ(second.offset, first.offset + 16384ULL);
}

TEST(GoldfishAddressSpaceTest, SnapshotSaveAndRestorePreservesAllSubdevicesAndAllocator) {
    auto mem1 = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem1_ptr = mem1.get();
    auto bar1_state1 = std::make_shared<FakeBar1State>();
    auto bar1_1 = std::make_unique<FakeBar1Mapper>(bar1_state1);
    TestGoldfishGpu gpu1;
    GoldfishAddressSpaceDevice dev1(std::move(mem1), std::move(bar1_1), [](bool /*high*/) {}, gpu1);

    // 1. Allocate a guest block at offset 0.
    dev1.Write(kAsRegBlockSizeLow, 0x8000);
    dev1.Write(kAsRegBlockSizeHigh, 0);
    dev1.Write(kAsRegCommand, kAsCmdAllocateBlock);
    ASSERT_EQ(dev1.Read(kAsRegStatus), kAsStatusOk);
    const uint64_t guest_block_off =
            (static_cast<uint64_t>(dev1.Read(kAsRegBlockOffsetHigh)) << 32) |
            dev1.Read(kAsRegBlockOffsetLow);
    EXPECT_EQ(guest_block_off, 0ULL);

    // 2. Open GraphicsAsgSubdevice (handle 1).
    dev1.Write(kAsRegCommand, kAsCmdGenHandle);
    const uint32_t asg_handle = dev1.Read(kAsRegHandle);
    EXPECT_EQ(asg_handle, 1U);
    constexpr uint64_t kAsgPingGpa = 0x1000;
    dev1.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(kAsgPingGpa));
    dev1.Write(kAsRegPingInfoAddrHigh, 0);
    dev1.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);
    AddressSpacePing ping{};
    ping.metadata = static_cast<uint64_t>(AddressSpaceSubdeviceType::kGraphics);
    mem1_ptr->Put(kAsgPingGpa, ping);
    dev1.Write(kAsRegPing, asg_handle);

    ping = AddressSpacePing{.size = 1, .metadata = kAsgCmdSetVersion};
    mem1_ptr->Put(kAsgPingGpa, ping);
    dev1.Write(kAsRegPing, asg_handle);

    ping = AddressSpacePing{.metadata = kAsgCmdGetRing};
    mem1_ptr->Put(kAsgPingGpa, ping);
    dev1.Write(kAsRegPing, asg_handle);
    const uint64_t asg_bar_offset = mem1_ptr->Get<AddressSpacePing>(kAsgPingGpa).offset;
    void* asg_hva1 = gpu1.resolve_dma_cb(bar1_state1->guest_phys_base + asg_bar_offset);
    ASSERT_NE(asg_hva1, nullptr);
    static_cast<uint8_t*>(asg_hva1)[128] = 0x5A;

    // 3. Open HostMemoryAllocatorSubdevice (handle 2) and allocate 4 KiB inside guest block.
    dev1.Write(kAsRegCommand, kAsCmdGenHandle);
    const uint32_t hma_handle = dev1.Read(kAsRegHandle);
    EXPECT_EQ(hma_handle, 2U);
    constexpr uint64_t kHmaPingGpa = 0x2000;
    dev1.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(kHmaPingGpa));
    dev1.Write(kAsRegPingInfoAddrHigh, 0);
    dev1.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);
    ping = AddressSpacePing{
        .metadata = static_cast<uint64_t>(AddressSpaceSubdeviceType::kHostMemoryAllocator)};
    mem1_ptr->Put(kHmaPingGpa, ping);
    dev1.Write(kAsRegPing, hma_handle);

    const uint64_t hma_gpa = bar1_state1->guest_phys_base + guest_block_off + 4096ULL;
    ping = AddressSpacePing{.offset = hma_gpa, .size = 4096ULL, .metadata = kHostMemCmdAllocate};
    mem1_ptr->Put(kHmaPingGpa, ping);
    dev1.Write(kAsRegPing, hma_handle);
    ASSERT_EQ(mem1_ptr->Get<AddressSpacePing>(kHmaPingGpa).metadata, 0ULL);
    void* hma_hva1 = gpu1.resolve_dma_cb(hma_gpa);
    ASSERT_NE(hma_hva1, nullptr);
    static_cast<uint8_t*>(hma_hva1)[7] = 0xA5;

    // 4. Open SharedSlotsSubdevice (handle 3) and allocate 8 KiB.
    dev1.Write(kAsRegCommand, kAsCmdGenHandle);
    const uint32_t ss_handle = dev1.Read(kAsRegHandle);
    EXPECT_EQ(ss_handle, 3U);
    constexpr uint64_t kSsPingGpa = 0x3000;
    dev1.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(kSsPingGpa));
    dev1.Write(kAsRegPingInfoAddrHigh, 0);
    dev1.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);
    ping = AddressSpacePing{.metadata = static_cast<uint64_t>(
                                    AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator)};
    mem1_ptr->Put(kSsPingGpa, ping);
    dev1.Write(kAsRegPing, ss_handle);
    ping = AddressSpacePing{.size = 8192ULL, .metadata = kSharedSlotsCmdAllocate};
    mem1_ptr->Put(kSsPingGpa, ping);
    dev1.Write(kAsRegPing, ss_handle);
    const uint64_t ss_bar_off = mem1_ptr->Get<AddressSpacePing>(kSsPingGpa).offset;
    void* ss_hva1 = gpu1.resolve_dma_cb(bar1_state1->guest_phys_base + ss_bar_off);
    ASSERT_NE(ss_hva1, nullptr);
    static_cast<uint8_t*>(ss_hva1)[19] = 0x3C;

    archive::DequeArchive saved;
    dev1.SaveState(saved);
    ASSERT_FALSE(saved.Empty());
    EXPECT_EQ(gpu1.gpu_snapshot_calls, 0);

    // Restore into a second device instance.
    auto mem2 = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem2_ptr = mem2.get();
    auto bar1_state2 = std::make_shared<FakeBar1State>();
    auto bar1_2 = std::make_unique<FakeBar1Mapper>(bar1_state2);
    TestGoldfishGpu gpu2;
    GoldfishAddressSpaceDevice dev2(std::move(mem2), std::move(bar1_2), [](bool /*high*/) {}, gpu2);

    ASSERT_OK(dev2.LoadState(saved));
    EXPECT_TRUE(saved.Empty());
    EXPECT_EQ(gpu2.gpu_snapshot_calls, 0);

    // Verify ASG ring+buffer data and PingAsg work without calling StartAsg again.
    void* asg_hva2 = gpu2.resolve_dma_cb(bar1_state2->guest_phys_base + asg_bar_offset);
    ASSERT_NE(asg_hva2, nullptr);
    EXPECT_EQ(static_cast<uint8_t*>(asg_hva2)[128], 0x5AU);
    ping = AddressSpacePing{.metadata = kAsgCmdNotifyAvailable};
    mem2_ptr->Put(kAsgPingGpa, ping);
    dev2.Write(kAsRegPing, asg_handle);
    EXPECT_EQ(gpu2.start_asg_calls, 0);
    EXPECT_EQ(gpu2.ping_asg_calls, 1);

    // Verify HostMemoryAllocatorSubdevice region data and unallocation.
    void* hma_hva2 = gpu2.resolve_dma_cb(hma_gpa);
    ASSERT_NE(hma_hva2, nullptr);
    EXPECT_EQ(static_cast<uint8_t*>(hma_hva2)[7], 0xA5U);

    // Verify SharedSlotsSubdevice region data.
    void* ss_hva2 = gpu2.resolve_dma_cb(bar1_state2->guest_phys_base + ss_bar_off);
    ASSERT_NE(ss_hva2, nullptr);
    EXPECT_EQ(static_cast<uint8_t*>(ss_hva2)[19], 0x3CU);

    // Verify Vulkan MapUserMemory into the restored guest block succeeds.
    alignas(4096) uint8_t vk_host_buf[4096]{};
    const uint64_t vk_gpa = bar1_state2->guest_phys_base + guest_block_off + 8192ULL;
    EXPECT_TRUE(gpu2.map_user_memory_cb(vk_gpa, vk_host_buf, sizeof(vk_host_buf)));
    EXPECT_EQ(gpu2.resolve_dma_cb(vk_gpa), vk_host_buf);

    // Next generated handle must be 4.
    dev2.Write(kAsRegCommand, kAsCmdGenHandle);
    EXPECT_EQ(dev2.Read(kAsRegHandle), 4U);
}

// A device plus the fakes it borrows, kept alive together for the bad-input snapshot tests.
struct PopulatedDevice {
    std::shared_ptr<FakeBar1State> bar1_state = std::make_shared<FakeBar1State>();
    TestGoldfishGpu gpu;
    FakeGuestMemory* mem = nullptr;
    std::unique_ptr<GoldfishAddressSpaceDevice> dev;  // Last: must die before `gpu`.
};

struct PopulateOptions {
    bool asg = false;
    bool host_memory = false;
    bool shared_slots = false;
};

uint32_t OpenSubdevice(GoldfishAddressSpaceDevice& dev, FakeGuestMemory& mem, uint64_t ping_gpa,
                       AddressSpaceSubdeviceType type) {
    dev.Write(kAsRegCommand, kAsCmdGenHandle);
    const uint32_t handle = dev.Read(kAsRegHandle);
    dev.Write(kAsRegPingInfoAddrLow, static_cast<uint32_t>(ping_gpa));
    dev.Write(kAsRegPingInfoAddrHigh, static_cast<uint32_t>(ping_gpa >> 32));
    dev.Write(kAsRegCommand, kAsCmdTellPingInfoAddr);
    AddressSpacePing ping{};
    ping.metadata = static_cast<uint64_t>(type);
    mem.Put(ping_gpa, ping);
    dev.Write(kAsRegPing, handle);
    return handle;
}

// Builds a device with a 32 KiB guest block and the requested subdevices, each holding at least
// one live allocation so the saved blob carries real payload bytes.
std::unique_ptr<PopulatedDevice> MakePopulatedDevice(const PopulateOptions& opts) {
    auto pd = std::make_unique<PopulatedDevice>();
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    pd->mem = mem.get();
    pd->dev = std::make_unique<GoldfishAddressSpaceDevice>(
            std::move(mem), std::make_unique<FakeBar1Mapper>(pd->bar1_state), [](bool /*high*/) {},
            pd->gpu);
    GoldfishAddressSpaceDevice& dev = *pd->dev;

    dev.Write(kAsRegBlockSizeLow, 0x8000);
    dev.Write(kAsRegBlockSizeHigh, 0);
    dev.Write(kAsRegCommand, kAsCmdAllocateBlock);
    EXPECT_EQ(dev.Read(kAsRegStatus), kAsStatusOk);
    const uint64_t guest_block_off =
            (static_cast<uint64_t>(dev.Read(kAsRegBlockOffsetHigh)) << 32) |
            dev.Read(kAsRegBlockOffsetLow);

    if (opts.asg) {
        constexpr uint64_t kPingGpa = 0x1000;
        const uint32_t h =
                OpenSubdevice(dev, *pd->mem, kPingGpa, AddressSpaceSubdeviceType::kGraphics);
        pd->mem->Put(kPingGpa, AddressSpacePing{.size = 1, .metadata = kAsgCmdSetVersion});
        dev.Write(kAsRegPing, h);
        EXPECT_EQ(pd->mem->Get<AddressSpacePing>(kPingGpa).metadata, 0ULL);
    }
    if (opts.host_memory) {
        constexpr uint64_t kPingGpa = 0x2000;
        const uint32_t h = OpenSubdevice(dev, *pd->mem, kPingGpa,
                                         AddressSpaceSubdeviceType::kHostMemoryAllocator);
        const uint64_t gpa = pd->bar1_state->guest_phys_base + guest_block_off + 4096ULL;
        pd->mem->Put(
                kPingGpa,
                AddressSpacePing{.offset = gpa, .size = 4096ULL, .metadata = kHostMemCmdAllocate});
        dev.Write(kAsRegPing, h);
        EXPECT_EQ(pd->mem->Get<AddressSpacePing>(kPingGpa).metadata, 0ULL);
    }
    if (opts.shared_slots) {
        constexpr uint64_t kPingGpa = 0x3000;
        const uint32_t h =
                OpenSubdevice(dev, *pd->mem, kPingGpa,
                              AddressSpaceSubdeviceType::kSharedSlotsHostMemoryAllocator);
        pd->mem->Put(kPingGpa,
                     AddressSpacePing{.size = 8192ULL, .metadata = kSharedSlotsCmdAllocate});
        dev.Write(kAsRegPing, h);
        EXPECT_EQ(pd->mem->Get<AddressSpacePing>(kPingGpa).metadata, 0ULL);
    }
    return pd;
}

// Parsed representation of a snapshot written by `SaveState(archive::IWriter&)` so bad-input
// tests can mutate individual fields without hardcoding VarInt byte offsets.
struct ParsedSnapshot {
    struct SpanEntry {
        uint64_t offset = 0;
        uint64_t size = 0;
        bool allocated = false;
        bool guest_block = false;
    };
    struct SharedSlotSpan {
        uint64_t rel_off = 0;
        uint64_t size = 0;
        uint32_t owner_id = 0;
        bool free = false;
    };
    struct SharedSlotBlock {
        uint64_t bar_offset = 0;
        uint64_t size = 0;
        std::vector<uint8_t> bytes;
        std::optional<uint32_t> span_count_override;
        std::vector<SharedSlotSpan> spans;
    };
    struct HostMemoryRegion {
        uint64_t bar_offset = 0;
        uint64_t size = 0;
        std::optional<uint64_t> size_override;
        std::vector<uint8_t> bytes;
    };
    struct HandleEntry {
        uint32_t handle = 0;
        uint64_t ping_gpa = 0;
        uint32_t type = 0;
        // GraphicsAsgSubdevice fields:
        uint64_t asg_bar_offset = 0;
        uint32_t asg_context_handle = 0;
        uint64_t asg_blob_size = 0;
        bool asg_consumer_started = false;
        std::vector<uint8_t> asg_bytes;
        // HostMemoryAllocatorSubdevice fields:
        std::optional<uint32_t> region_count_override;
        std::vector<HostMemoryRegion> regions;
    };

    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t effective_page_size = 0;
    uint64_t block_size = 0;
    uint64_t block_offset = 0;
    uint64_t ping_info_addr = 0;
    uint64_t phys_start = 0;
    uint32_t status = 0;
    uint32_t guest_page_size = 0;
    uint32_t handle = 0;
    uint64_t min_align = 0;
    std::optional<uint32_t> span_count_override;
    std::vector<SpanEntry> spans;
    std::optional<uint32_t> block_count_override;
    std::vector<SharedSlotBlock> blocks;
    uint32_t last_handle_id = 0;
    std::optional<uint32_t> handle_count_override;
    std::vector<HandleEntry> handles;

    static ParsedSnapshot FromDevice(const GoldfishAddressSpaceDevice& dev) {
        archive::DequeArchive ar;
        dev.SaveState(ar);
        ParsedSnapshot p;
        uint32_t span_count = 0;
        EXPECT_OK(archive::ReadValue(ar, p.magic, p.version, p.effective_page_size, p.block_size,
                                     p.block_offset, p.ping_info_addr, p.phys_start, p.status,
                                     p.guest_page_size, p.handle, p.min_align, span_count));
        p.spans.resize(span_count);
        for (SpanEntry& s : p.spans) {
            EXPECT_OK(archive::ReadValue(ar, s.offset, s.size, s.allocated, s.guest_block));
        }
        uint32_t block_count = 0;
        EXPECT_OK(archive::ReadValue(ar, block_count));
        p.blocks.resize(block_count);
        for (SharedSlotBlock& b : p.blocks) {
            EXPECT_OK(archive::ReadValue(ar, b.bar_offset, b.size));
            b.bytes.resize(static_cast<size_t>(b.size));
            if (!b.bytes.empty()) {
                EXPECT_OK(ar.Read(b.bytes.data(), b.bytes.size()));
            }
            uint32_t b_spans = 0;
            EXPECT_OK(archive::ReadValue(ar, b_spans));
            b.spans.resize(b_spans);
            for (SharedSlotSpan& sp : b.spans) {
                EXPECT_OK(archive::ReadValue(ar, sp.rel_off, sp.size, sp.owner_id, sp.free));
            }
        }
        uint32_t handle_count = 0;
        EXPECT_OK(archive::ReadValue(ar, p.last_handle_id, handle_count));
        p.handles.resize(handle_count);
        for (HandleEntry& h : p.handles) {
            EXPECT_OK(archive::ReadValue(ar, h.handle, h.ping_gpa, h.type));
            const auto sub_type = static_cast<AddressSpaceSubdeviceType>(h.type);
            if (sub_type == AddressSpaceSubdeviceType::kGraphics) {
                EXPECT_OK(archive::ReadValue(ar, h.asg_bar_offset, h.asg_context_handle,
                                             h.asg_blob_size, h.asg_consumer_started));
                h.asg_bytes.resize(static_cast<size_t>(h.asg_blob_size));
                if (!h.asg_bytes.empty()) {
                    EXPECT_OK(ar.Read(h.asg_bytes.data(), h.asg_bytes.size()));
                }
            } else if (sub_type == AddressSpaceSubdeviceType::kHostMemoryAllocator) {
                uint32_t region_count = 0;
                EXPECT_OK(archive::ReadValue(ar, region_count));
                h.regions.resize(region_count);
                for (HostMemoryRegion& reg : h.regions) {
                    EXPECT_OK(archive::ReadValue(ar, reg.bar_offset, reg.size));
                    reg.bytes.resize(static_cast<size_t>(reg.size));
                    if (!reg.bytes.empty()) {
                        EXPECT_OK(ar.Read(reg.bytes.data(), reg.bytes.size()));
                    }
                }
            }
        }
        EXPECT_TRUE(ar.Empty());
        return p;
    }

    archive::DequeArchive ToArchive() const {
        archive::DequeArchive w;
        w << magic << version << effective_page_size << block_size << block_offset << ping_info_addr
          << phys_start << status << guest_page_size << handle << min_align
          << span_count_override.value_or(static_cast<uint32_t>(spans.size()));
        for (const SpanEntry& s : spans) {
            w << s.offset << s.size << s.allocated << s.guest_block;
        }
        w << block_count_override.value_or(static_cast<uint32_t>(blocks.size()));
        for (const SharedSlotBlock& b : blocks) {
            w << b.bar_offset << b.size;
            if (!b.bytes.empty()) {
                w.Write(b.bytes.data(), b.bytes.size());
            }
            w << b.span_count_override.value_or(static_cast<uint32_t>(b.spans.size()));
            for (const SharedSlotSpan& sp : b.spans) {
                w << sp.rel_off << sp.size << sp.owner_id << sp.free;
            }
        }
        w << last_handle_id
          << handle_count_override.value_or(static_cast<uint32_t>(handles.size()));
        for (const HandleEntry& h : handles) {
            w << h.handle << h.ping_gpa << h.type;
            const auto sub_type = static_cast<AddressSpaceSubdeviceType>(h.type);
            if (sub_type == AddressSpaceSubdeviceType::kGraphics) {
                w << h.asg_bar_offset << h.asg_context_handle << h.asg_blob_size
                  << h.asg_consumer_started;
                if (!h.asg_bytes.empty()) {
                    w.Write(h.asg_bytes.data(), h.asg_bytes.size());
                }
            } else if (sub_type == AddressSpaceSubdeviceType::kHostMemoryAllocator) {
                w << h.region_count_override.value_or(static_cast<uint32_t>(h.regions.size()));
                for (const HostMemoryRegion& reg : h.regions) {
                    w << reg.bar_offset << reg.size_override.value_or(reg.size);
                    if (!reg.bytes.empty()) {
                        w.Write(reg.bytes.data(), reg.bytes.size());
                    }
                }
            }
        }
        return w;
    }
};

// Zero-copy IReader over a contiguous byte slice so prefix truncation tests can check prefixes of
// a 1 MiB+ snapshot without copying the buffer on every iteration.
class SliceReader final : public archive::IReader {
  public:
    SliceReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    absl::Status Read(void* dst, size_t size) override {
        if (size > size_ - pos_) {
            return absl::UnavailableError("Not enough bytes in SliceReader");
        }
        if (size > 0) {
            std::memcpy(dst, data_ + pos_, size);
            pos_ += size;
        }
        return absl::OkStatus();
    }

  private:
    const uint8_t* const data_;
    const size_t size_;
    size_t pos_ = 0;
};

// Feeds truncated prefixes of `saved` to a fresh device and checks each one is rejected cleanly
// and that the device still loads the complete archive afterwards.
void ExpectEveryPrefixRejected(const archive::DequeArchive& saved) {
    const std::vector<uint8_t> bytes(saved.storage.begin(), saved.storage.end());
    auto target = MakePopulatedDevice(PopulateOptions{});
    for (size_t n = 0; n < bytes.size(); ++n) {
        // Test every byte in the first and last 512 bytes (headers, counts, subdevice records,
        // boundaries) and sample every 4 KiB page across the bulk payload interior.
        if (n >= 512 && n + 512 < bytes.size() && (n % 4096) != 0) {
            continue;
        }
        SliceReader prefix(bytes.data(), n);
        const absl::Status st = target->dev->LoadState(prefix);
        if (st.ok()) {
            ADD_FAILURE() << "prefix of " << n << " of " << bytes.size() << " bytes loaded";
            break;
        }
        if (st.code() != absl::StatusCode::kDataLoss) {
            ADD_FAILURE() << "prefix of " << n << " bytes: " << st;
            break;
        }
    }
    archive::DequeArchive full = saved;
    ASSERT_OK(target->dev->LoadState(full));
    EXPECT_TRUE(full.Empty());
}

TEST(GoldfishAddressSpaceTest, LoadStateRejectsEveryTruncatedPrefixWithAsgAndHostMemory) {
    auto source = MakePopulatedDevice(PopulateOptions{.asg = true, .host_memory = true});
    archive::DequeArchive saved;
    source->dev->SaveState(saved);
    ASSERT_GT(saved.storage.size(), kAsgTotalAllocationSize + 4096ULL);
    ExpectEveryPrefixRejected(saved);
}

TEST(GoldfishAddressSpaceTest, LoadStateRejectsEveryTruncatedPrefixWithSharedSlotsAndHostMemory) {
    auto source = MakePopulatedDevice(PopulateOptions{.host_memory = true, .shared_slots = true});
    archive::DequeArchive saved;
    source->dev->SaveState(saved);
    ASSERT_GT(saved.storage.size(), 64ULL * 1024ULL + 4096ULL);
    ExpectEveryPrefixRejected(saved);
}

TEST(GoldfishAddressSpaceTest, LoadStateRejectsBadHeader) {
    auto source = MakePopulatedDevice(PopulateOptions{.host_memory = true});
    const ParsedSnapshot snap = ParsedSnapshot::FromDevice(*source->dev);
    auto target = MakePopulatedDevice(PopulateOptions{});

    archive::DequeArchive empty;
    EXPECT_THAT(target->dev->LoadState(empty), StatusIs(absl::StatusCode::kDataLoss));

    ParsedSnapshot bad_magic = snap;
    bad_magic.magic = 0x12345678U;
    archive::DequeArchive bad_magic_ar = bad_magic.ToArchive();
    EXPECT_THAT(target->dev->LoadState(bad_magic_ar),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("magic")));

    ParsedSnapshot bad_version = snap;
    bad_version.version = 2U;
    archive::DequeArchive bad_version_ar = bad_version.ToArchive();
    EXPECT_THAT(target->dev->LoadState(bad_version_ar),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("version 2")));

    archive::DequeArchive good_ar = snap.ToArchive();
    ASSERT_OK(target->dev->LoadState(good_ar));
    EXPECT_TRUE(good_ar.Empty());
}

// A host-owned block (ASG ring+buffer, SharedSlots backing) must be restored exactly onto the
// host-region span the allocator snapshot describes. If a corrupt snapshot points it at a guest
// block, the subdevice's teardown would later Free() the guest's block out from under it.
TEST(GoldfishAddressSpaceTest, LoadStateRejectsHostBlocksThatAreNotHostRegions) {
    auto source = MakePopulatedDevice(
            PopulateOptions{.asg = true, .host_memory = true, .shared_slots = true});
    const ParsedSnapshot snap = ParsedSnapshot::FromDevice(*source->dev);
    ASSERT_FALSE(snap.handles.empty());
    ASSERT_EQ(static_cast<AddressSpaceSubdeviceType>(snap.handles[0].type),
              AddressSpaceSubdeviceType::kGraphics);
    ASSERT_FALSE(snap.blocks.empty());
    auto target = MakePopulatedDevice(PopulateOptions{});

    // Guest blocks are allocated bottom-up, so offset 0 is the populated guest block.
    ParsedSnapshot bad_asg = snap;
    bad_asg.handles[0].asg_bar_offset = 0ULL;
    archive::DequeArchive ar = bad_asg.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("asg bar_offset 0 size " +
                                   std::to_string(kAsgTotalAllocationSize) +
                                   " is not a host region")));
    // Rejected before the GPU was asked for a blob, and the device is back in reset state.
    EXPECT_EQ(target->gpu.destroy_asg_calls, 0);
    EXPECT_TRUE(target->bar1_state->mappings.empty());

    ParsedSnapshot bad_block = snap;
    bad_block.blocks[0].bar_offset = 0ULL;
    ar = bad_block.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("shared_slots block 0 bar_offset 0 size 65536 is not a host "
                                   "region")));
    EXPECT_TRUE(target->bar1_state->mappings.empty());

    // An ASG offset that is page-aligned, inside BAR 1, but one page off the real host region.
    ParsedSnapshot shifted_asg = snap;
    shifted_asg.handles[0].asg_bar_offset -= 4096ULL;
    ar = shifted_asg.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("is not a host region")));
    EXPECT_TRUE(target->bar1_state->mappings.empty());

    ar = snap.ToArchive();
    ASSERT_OK(target->dev->LoadState(ar));
    EXPECT_TRUE(ar.Empty());
    EXPECT_FALSE(target->bar1_state->mappings.empty());
}

// A load that fails *after* an ASG subdevice was already rebuilt must release that subdevice
// (GPU blob closed, BAR 1 mapping gone) and leave the device in reset state.
TEST(GoldfishAddressSpaceTest, FailedLoadAfterAsgRestoreReleasesTheAsg) {
    auto source = MakePopulatedDevice(PopulateOptions{.asg = true, .host_memory = true});
    const ParsedSnapshot snap = ParsedSnapshot::FromDevice(*source->dev);
    ASSERT_EQ(snap.handles.size(), 2U);
    ASSERT_EQ(static_cast<AddressSpaceSubdeviceType>(snap.handles[0].type),
              AddressSpaceSubdeviceType::kGraphics);
    ASSERT_EQ(static_cast<AddressSpaceSubdeviceType>(snap.handles[1].type),
              AddressSpaceSubdeviceType::kHostMemoryAllocator);
    ASSERT_FALSE(snap.handles[1].regions.empty());
    auto target = MakePopulatedDevice(PopulateOptions{});

    ParsedSnapshot bad = snap;
    bad.handles[1].regions[0].bar_offset = 0x10000000ULL;  // not inside any guest block
    archive::DequeArchive ar = bad.ToArchive();
    const int destroyed_before = target->gpu.destroy_asg_calls;
    EXPECT_THAT(target->dev->LoadState(ar), StatusIs(absl::StatusCode::kNotFound));
    target->bar1_state->FlushRcu();
    EXPECT_EQ(target->gpu.destroy_asg_calls, destroyed_before + 1)
            << "the ASG rebuilt before the failure must be closed again";
    EXPECT_TRUE(target->bar1_state->mappings.empty());

    ar = snap.ToArchive();
    ASSERT_OK(target->dev->LoadState(ar));
    EXPECT_TRUE(ar.Empty());
}

TEST(GoldfishAddressSpaceTest, LoadStateRejectsHugeCountsNamingTheLimit) {
    auto source = MakePopulatedDevice(
            PopulateOptions{.asg = true, .host_memory = true, .shared_slots = true});
    const ParsedSnapshot snap = ParsedSnapshot::FromDevice(*source->dev);
    ASSERT_FALSE(snap.blocks.empty());
    ASSERT_GE(snap.handles.size(), 2U);
    ASSERT_FALSE(snap.handles[1].regions.empty());
    auto target = MakePopulatedDevice(PopulateOptions{});

    ParsedSnapshot bad = snap;
    bad.span_count_override = 0xffffffffU;
    archive::DequeArchive ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("span_count 4294967295 exceeds 262145")));

    bad = snap;
    bad.block_count_override = 0xffffffffU;
    ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("block_count 4294967295 exceeds 262145")));

    bad = snap;
    bad.blocks[0].span_count_override = 0xffffffffU;
    ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("span_count 4294967295 is outside [1, 17]")));

    bad = snap;
    bad.handle_count_override = 0xffffffffU;
    ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("handle_count 4294967295 exceeds 65536")));

    bad = snap;
    bad.handles[1].region_count_override = 0xffffffffU;
    ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("region_count 4294967295 exceeds 262145")));

    // A region size larger than the bytes that follow (but still inside the guest block) is
    // rejected when the stream ends early.
    bad = snap;
    bad.handles[1].regions[0].size_override = 8192ULL;
    // Truncate the archive right after the 4096-byte region payload so the 8192-byte read hits EOF.
    bad.handles.resize(2);
    ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kDataLoss,
                         HasSubstr("truncated host_memory region 0 bytes")));

    // A region whose offset is not inside any restored guest block names the offset.
    bad = snap;
    bad.handles[1].regions[0].bar_offset = 0x10000000ULL;
    ar = bad.ToArchive();
    EXPECT_THAT(target->dev->LoadState(ar),
                StatusIs(absl::StatusCode::kNotFound,
                         HasSubstr("bar_offset 268435456 size 4096 is not inside")));

    ar = snap.ToArchive();
    ASSERT_OK(target->dev->LoadState(ar));
    EXPECT_TRUE(ar.Empty());
}

TEST(GoldfishAddressSpaceTest, CoreSaveAndLoadRoundTripsThroughQemuFile) {
    auto source = MakePopulatedDevice(PopulateOptions{.host_memory = true});
    QEMUFile file;
    const IMmioDevice* core1 = source->dev.get();
    ASSERT_TRUE(GoldfishAddressSpaceCoreSave(core1, &file));
    EXPECT_FALSE(file.archive.Empty());

    auto target = MakePopulatedDevice(PopulateOptions{});
    IMmioDevice* core2 = target->dev.get();
    ASSERT_TRUE(GoldfishAddressSpaceCoreLoad(core2, &file));
    EXPECT_TRUE(file.archive.Empty());
}

TEST(GoldfishAddressSpaceTest, GraphicsAsgSubdeviceAlignsAllocationTo16KHostAndGuestPageSize) {
    auto mem1 = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem1_ptr = mem1.get();
    auto bar1_state1 = std::make_shared<FakeBar1State>();
    bar1_state1->host_page_size = 16384ULL;
    auto bar1_1 = std::make_unique<FakeBar1Mapper>(bar1_state1);
    TestGoldfishGpu gpu1;
    GoldfishAddressSpaceDevice dev1(std::move(mem1), std::move(bar1_1), [](bool /*high*/) {}, gpu1);
    dev1.Write(kAsRegGuestPageSize, 16384U);

    constexpr uint64_t kAsgPingGpa = 0x1000;
    const uint32_t asg_handle =
            OpenSubdevice(dev1, *mem1_ptr, kAsgPingGpa, AddressSpaceSubdeviceType::kGraphics);
    ASSERT_NE(asg_handle, 0U);
    EXPECT_EQ(gpu1.create_asg_calls, 1);

    // BAR 1 mapping must be 16 KiB-aligned in both offset and size.
    ASSERT_EQ(bar1_state1->mappings.size(), 1U);
    const auto& [mapped_off, mapping] = *bar1_state1->mappings.begin();
    EXPECT_EQ(mapped_off % 16384ULL, 0ULL);
    EXPECT_EQ(mapping.size, 1064960ULL);
    EXPECT_EQ(mapping.size % 16384ULL, 0ULL);

    // GetBuffer spans the full [mapped_off, mapped_off + 1064960) region so a 16 KiB-page guest
    // kernel's shared_blocks check covers the 16 KiB-rounded ring VMA at mapped_off + kAsgBufferSize.
    mem1_ptr->Put(kAsgPingGpa, AddressSpacePing{.metadata = kAsgCmdGetBuffer});
    dev1.Write(kAsRegPing, asg_handle);
    const auto buf_ping = mem1_ptr->Get<AddressSpacePing>(kAsgPingGpa);
    EXPECT_EQ(buf_ping.offset, mapped_off);
    EXPECT_EQ(buf_ping.metadata, mapped_off);
    EXPECT_EQ(buf_ping.size, 1064960ULL);

    mem1_ptr->Put(kAsgPingGpa, AddressSpacePing{.metadata = kAsgCmdGetRing});
    dev1.Write(kAsRegPing, asg_handle);
    const auto ring_ping = mem1_ptr->Get<AddressSpacePing>(kAsgPingGpa);
    EXPECT_EQ(ring_ping.offset, mapped_off + kAsgBufferSize);
    EXPECT_EQ(ring_ping.metadata, mapped_off + kAsgBufferSize);
    EXPECT_EQ(ring_ping.size, kAsgRingStorageSize);
    EXPECT_EQ(ring_ping.offset % 16384ULL, 0ULL);
    EXPECT_LE(ring_ping.offset + 16384ULL, buf_ping.offset + buf_ping.size);

    mem1_ptr->Put(kAsgPingGpa, AddressSpacePing{.size = 1, .metadata = kAsgCmdSetVersion});
    dev1.Write(kAsRegPing, asg_handle);
    EXPECT_EQ(mem1_ptr->Get<AddressSpacePing>(kAsgPingGpa).metadata, 0ULL);

    archive::DequeArchive saved;
    dev1.SaveState(saved);
    ASSERT_FALSE(saved.Empty());

    auto mem2 = std::make_unique<FakeGuestMemory>(0x10000);
    auto bar1_state2 = std::make_shared<FakeBar1State>();
    bar1_state2->host_page_size = 16384ULL;
    auto bar1_2 = std::make_unique<FakeBar1Mapper>(bar1_state2);
    TestGoldfishGpu gpu2;
    GoldfishAddressSpaceDevice dev2(std::move(mem2), std::move(bar1_2), [](bool /*high*/) {}, gpu2);
    ASSERT_OK(dev2.LoadState(saved));
    EXPECT_TRUE(saved.Empty());
    ASSERT_EQ(bar1_state2->mappings.size(), 1U);
    EXPECT_EQ(bar1_state2->mappings.at(mapped_off).size, 1064960ULL);
}

}  // namespace
}  // namespace goldfish::legacy
