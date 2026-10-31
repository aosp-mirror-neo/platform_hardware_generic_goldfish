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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/legacy/common/fake_guest_memory.h"
#include "goldfish/legacy/common/i_bar1_mapper.h"
#include "goldfish/legacy/goldfish_gpu/fake_goldfish_gpu.h"

namespace goldfish::legacy {
namespace {

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
        if (host_ptr == nullptr || size == 0ULL) {
            return false;
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
            : asg_backing_(kAsgTotalAllocationSize + 65536ULL, 0)
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

    MapUserMemoryCallback map_user_memory_cb;
    UnmapUserMemoryCallback unmap_user_memory_cb;
    ResolveDmaHostAddrCallback resolve_dma_cb;
    int create_asg_calls = 0;
    int start_asg_calls = 0;
    int ping_asg_calls = 0;
    int destroy_asg_calls = 0;

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

}  // namespace
}  // namespace goldfish::legacy
