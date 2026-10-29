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

#include <stdint.h>

#include "goldfish/legacy/common/c/device_env.h"
#include "goldfish/legacy/common/c/mmio_core.h"

#ifdef __cplusplus
#include <atomic>
#include <functional>
#include <map>
#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

#include "bar1_allocator.h"
#include "goldfish/legacy/common/adapters.h"
#include "goldfish/legacy/common/i_bar1_mapper.h"
#include "goldfish/legacy/common/i_guest_memory.h"
#include "goldfish/legacy/common/i_mmio_device.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "goldfish/unique_id_allocator.h"

namespace goldfish::legacy {

enum GoldfishAddressSpaceReg : uint32_t {
    kAsRegCommand = 0x00,
    kAsRegStatus = 0x04,
    kAsRegGuestPageSize = 0x08,
    kAsRegBlockSizeLow = 0x0c,
    kAsRegBlockSizeHigh = 0x10,
    kAsRegBlockOffsetLow = 0x14,
    kAsRegBlockOffsetHigh = 0x18,
    kAsRegPing = 0x1c,
    kAsRegPingInfoAddrLow = 0x20,
    kAsRegPingInfoAddrHigh = 0x24,
    kAsRegHandle = 0x28,
    kAsRegPhysStartLow = 0x2c,
    kAsRegPhysStartHigh = 0x30,
};

enum GoldfishAddressSpaceCommand : uint32_t {
    kAsCmdAllocateBlock = 1,
    kAsCmdDeallocateBlock = 2,
    kAsCmdGenHandle = 3,
    kAsCmdDestroyHandle = 4,
    kAsCmdTellPingInfoAddr = 5,
};

enum GoldfishAddressSpaceStatus : uint32_t {
    kAsStatusOk = 0,
    kAsErrnoEnomem = 12,
    kAsErrnoEinval = 22,
    kAsErrnoEnosys = 38,
};

enum class AddressSpaceSubdeviceType : uint32_t {
    kNoDevice = 0xffff,
    kGraphics = 0,
    kMedia = 1,
    kSensors = 2,
    kPower = 3,
    kGenericPipe = 4,
    kHostMemoryAllocator = 5,
    kSharedSlotsHostMemoryAllocator = 6,
    kVirtioGpuGraphics = 10,
};

enum AsgControlCommand : uint64_t {
    kAsgCmdGetRing = 0,
    kAsgCmdGetBuffer = 1,
    kAsgCmdSetVersion = 2,
    kAsgCmdNotifyAvailable = 3,
    kAsgCmdGetConfig = 4,
};

enum HostMemoryAllocatorCommand : uint64_t {
    kHostMemCmdAllocate = 1,
    kHostMemCmdUnallocate = 2,
};

enum SharedSlotsCommand : uint64_t {
    kSharedSlotsCmdAllocate = 1,
    kSharedSlotsCmdUnallocate = 2,
    kSharedSlotsCmdCheckIfSupported = 3,
};

struct AddressSpacePing {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t metadata = 0;
    uint32_t version = 0;
    uint32_t wait_fd = 0;
    uint32_t wait_flags = 0;
    uint32_t direction = 0;
};
static_assert(sizeof(AddressSpacePing) == 40);

struct AsgRingConfig {
    uint32_t buffer_size = 0;
    uint32_t flush_interval = 0;
    uint32_t host_consumed_pos = 0;
    uint32_t guest_write_pos = 0;
    uint32_t transfer_mode = 0;
    uint32_t transfer_size = 0;
    uint32_t in_error = 0;
};
static_assert(sizeof(AsgRingConfig) == 28);

constexpr uint64_t kAsgRingConfigOffsetBytes = 2180ULL;
constexpr uint64_t kAsgRingStorageSize = 12288ULL;  // 3 * 4096 bytes
constexpr uint64_t kAsgBufferSize = 1048576ULL;     // 1 MiB
constexpr uint64_t kAsgTotalAllocationSize = kAsgRingStorageSize + kAsgBufferSize;
constexpr uint32_t kAsgFlushInterval = 262144U;  // 256 KiB

using PhysStartProvider = std::function<uint64_t()>;

class IAddressSpaceSubdevice {
  public:
    virtual ~IAddressSpaceSubdevice() = default;
    virtual AddressSpaceSubdeviceType Type() const = 0;
    virtual void Ping(AddressSpacePing& ping) = 0;
    virtual void Teardown() = 0;
};

class SharedSlotsPool;

class GoldfishAddressSpaceDevice final : public IMmioDevice {
  public:
    GoldfishAddressSpaceDevice(std::unique_ptr<IGuestMemory> memory,
                               std::unique_ptr<IBar1Mapper> bar1, SetIrqCallback set_irq,
                               IGoldfishGpu& gpu);
    ~GoldfishAddressSpaceDevice() override;

    GoldfishAddressSpaceDevice(const GoldfishAddressSpaceDevice&) = delete;
    GoldfishAddressSpaceDevice& operator=(const GoldfishAddressSpaceDevice&) = delete;

    uint32_t Read(uint32_t offset) override;
    void Write(uint32_t offset, uint32_t value) override;
    void Reset() override;

    bool MapUserMemory(uint64_t gpa, void* hva, uint64_t size);
    bool UnmapUserMemory(uint64_t gpa, uint64_t size);
    void* ResolveDmaHostAddr(uint64_t gpa) const;

  private:
    class Bar1MappingTable;

    struct UserMemoryMapping {
        uint64_t bar_offset = 0;
        uint64_t size = 0;
    };

    struct UserMemoryLink {
        std::shared_ptr<IBar1Mapper> bar1;
        std::atomic<GoldfishAddressSpaceDevice*> device{nullptr};
    };

    struct HandleSlot {
        std::unique_ptr<IAddressSpaceSubdevice> subdevice;
        uint64_t ping_gpa = 0;
    };

    uint64_t EffectivePhysStart() const;
    void UpdateMappingTablePhysBase();
    void UnmapUserMappingsInRange(uint64_t block_offset, uint64_t block_size);
    void ExecuteCommand(uint32_t cmd);
    void ExecutePing(uint32_t handle);
    void TeardownAllSubdevices();
    bool MapUserMemoryUnderBql(uint64_t gpa, void* hva, uint64_t size);
    bool UnmapUserMemoryUnderBql(uint64_t gpa, uint64_t size);

    const std::unique_ptr<IGuestMemory> memory_;
    const std::shared_ptr<Bar1MappingTable> mapping_table_;
    const std::shared_ptr<IBar1Mapper> bar1_;
    const std::shared_ptr<UserMemoryLink> user_memory_link_;
    const SetIrqCallback set_irq_;
    IGoldfishGpu& gpu_;

    const uint64_t host_page_size_;
    uint64_t effective_page_size_;
    Bar1Allocator allocator_;
    const std::unique_ptr<SharedSlotsPool> shared_slots_pool_;

    absl::flat_hash_map<uint32_t, HandleSlot> handles_;
    std::map<uint64_t, UserMemoryMapping> user_mappings_;
    UniqueIdAllocator handle_allocator_;

    uint64_t block_size_ = 0;
    uint64_t block_offset_ = 0;
    uint64_t ping_info_addr_ = 0;
    uint64_t phys_start_ = 0;
    uint32_t status_ = kAsStatusOk;
    uint32_t guest_page_size_ = 4096;
    uint32_t handle_ = 0;
};

}  // namespace goldfish::legacy

extern "C" {
#endif

struct GoldfishMmioCore GoldfishAddressSpaceCoreCreate(const struct GoldfishDeviceEnv* env);
uint64_t GoldfishAddressSpaceHostPageSize(void);

#ifdef __cplusplus
}
#endif
