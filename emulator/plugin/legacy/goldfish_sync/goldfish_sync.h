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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "goldfish/legacy/common/c/device_env.h"
#include "goldfish/legacy/common/c/mmio_core.h"

#ifdef __cplusplus
#include <deque>
#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/legacy/common/adapters.h"
#include "goldfish/legacy/common/i_guest_memory.h"
#include "goldfish/legacy/common/i_mmio_device.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

namespace goldfish::legacy {

enum GoldfishSyncReg : uint32_t {
    kSyncRegBatchCommand = 0x00,
    kSyncRegBatchGuestCommand = 0x04,
    kSyncRegBatchCommandAddr = 0x08,
    kSyncRegBatchCommandAddrHigh = 0x0c,
    kSyncRegBatchGuestCommandAddr = 0x10,
    kSyncRegBatchGuestCommandAddrHigh = 0x14,
    kSyncRegInit = 0x18,
};

enum GoldfishSyncCommandType : uint32_t {
    kSyncCmdReady = 0,
    kSyncCmdCreateSyncTimeline = 1,
    kSyncCmdCreateSyncFence = 2,
    kSyncCmdSyncTimelineInc = 3,
    kSyncCmdDestroySyncTimeline = 4,
    kSyncCmdTriggerHostWait = 5,
};

struct GoldfishSyncCommand {
    uint64_t handle = 0;
    uint64_t hostcmd_handle = 0;
    uint32_t cmd = 0;
    uint32_t time_arg = 0;
};
static_assert(sizeof(GoldfishSyncCommand) == 24);

struct GoldfishSyncGuestCommand {
    uint64_t host_command = 0;
    uint64_t glsync_handle = 0;
    uint64_t thread_handle = 0;
    uint64_t guest_timeline_handle = 0;
};
static_assert(sizeof(GoldfishSyncGuestCommand) == 32);

class GoldfishSyncDevice final : public IMmioDevice {
  public:
    GoldfishSyncDevice(std::unique_ptr<IGuestMemory> memory, SetIrqCallback set_irq,
                       IGoldfishGpu& gpu);
    ~GoldfishSyncDevice() override;

    GoldfishSyncDevice(const GoldfishSyncDevice&) = delete;
    GoldfishSyncDevice& operator=(const GoldfishSyncDevice&) = delete;

    uint32_t Read(uint32_t offset) ABSL_LOCKS_EXCLUDED(mu_) override;
    void Write(uint32_t offset, uint32_t value) ABSL_LOCKS_EXCLUDED(mu_) override;
    void Reset() ABSL_LOCKS_EXCLUDED(mu_) override;

    void QueueTimelineInc(uint64_t guest_timeline, uint32_t how_much) ABSL_LOCKS_EXCLUDED(mu_);
    void QueueHostCommand(const GoldfishSyncCommand& cmd) ABSL_LOCKS_EXCLUDED(mu_);

    // Snapshot hooks. The GoldfishGpu is quiesced/saved/loaded by its own VMState sections
    // (goldfish_gpu_low_level.c), which run before this device's section; nothing GPU-related
    // happens here.
    void SaveState(archive::IWriter& writer) ABSL_LOCKS_EXCLUDED(mu_);
    // Restores the state written by SaveState(). Returns DATA_LOSS for a
    // truncated or corrupt stream (naming the field, value and limit) and
    // FAILED_PRECONDITION for an unsupported snapshot version. The device is
    // left untouched on failure.
    absl::Status LoadState(archive::IReader& reader) ABSL_LOCKS_EXCLUDED(mu_);

  private:
    struct CallbackState {
        GoldfishSyncDevice* dev ABSL_GUARDED_BY(mu) = nullptr;
        absl::Mutex mu;
    };

    const std::unique_ptr<IGuestMemory> memory_;
    const std::shared_ptr<CallbackState> callback_state_;
    const SetIrqCallback set_irq_;
    IGoldfishGpu& gpu_;

    std::deque<GoldfishSyncCommand> pending_ ABSL_GUARDED_BY(mu_);
    uint64_t batch_cmd_addr_ ABSL_GUARDED_BY(mu_) = 0;
    uint64_t batch_guestcmd_addr_ ABSL_GUARDED_BY(mu_) = 0;
    bool irq_level_ ABSL_GUARDED_BY(mu_) = false;
    absl::Mutex mu_;
};

}  // namespace goldfish::legacy

extern "C" {
#endif

typedef struct QEMUFile QEMUFile;

struct GoldfishMmioCore GoldfishSyncCoreCreate(const struct GoldfishDeviceEnv* env);
bool GoldfishSyncCoreSave(void* core, QEMUFile* f);
bool GoldfishSyncCoreLoad(void* core, QEMUFile* f);

#ifdef __cplusplus
}
#endif
