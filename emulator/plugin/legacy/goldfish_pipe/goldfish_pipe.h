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

typedef struct QEMUFile QEMUFile;

// Snapshot bounds enforced by `GoldfishPipeDevice::Save` and `GoldfishPipeDevice::Load`.
//
// Stream layout (LEB128 varint integers via `goldfish::archive`, see `GoldfishPipeDevice::Save`;
// byte counts below are varint maxima: <=5 for 32-bit, <=10 for 64-bit, 1 for uint8_t):
//   header:   magic(<=5) version(<=5) open_buf(<=10) signal_buf(<=10) signal_size(<=5)
//             dev_ver(<=5) pipe_count(<=5)
//   per pipe: id(<=5) cmd_gpa(<=10) rw_max(<=5) wanted(<=5) signalled(<=5) hs_done(1)
//             hs_len(<=5) handshake(<= 256) svc_len(<=5) service state
//   service state: GLProcessPipe <= 95 bytes, refcount <= 15 bytes,
//             opengles <= 15 bytes + the unread host->guest bytes of that pipe.
// Every per-pipe field except the opengles/hal read queue fits in MAX_PIPE_ENTRY_BYTES
// (297 + 95 = 392 <= 512); the read queues of all pipes together are capped at MAX_QUEUED_BYTES.
// The GoldfishGpu state (ASG consumers, render channels, last frame) is NOT part of this stream:
// it travels in the standalone "goldfish_gpu_end" VMState section (see goldfish_gpu_low_level.c),
// which runs at MIG_PRI_LOW after this device's MIG_PRI_DEFAULT section.

/** @brief Maximum number of open pipes a snapshot may describe. */
#define GOLDFISH_PIPE_SNAPSHOT_MAX_PIPES 65536U
/** @brief Per-pipe bytes excluding the opengles read queue (fixed fields + handshake + service). */
#define GOLDFISH_PIPE_SNAPSHOT_MAX_PIPE_ENTRY_BYTES 512U
/** @brief Total unread host->guest opengles bytes across all pipes (256 MiB). */
#define GOLDFISH_PIPE_SNAPSHOT_MAX_QUEUED_BYTES (256U << 20)
/** @brief Maximum header bytes (LEB128 varints) including the pipe count. */
#define GOLDFISH_PIPE_SNAPSHOT_HEADER_BYTES 45U

#ifdef __cplusplus
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/legacy/common/adapters.h"
#include "goldfish/legacy/common/i_guest_memory.h"
#include "goldfish/legacy/common/i_mmio_device.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

namespace goldfish::legacy {

enum GoldfishPipeReg : uint32_t {
    kPipeRegCmd = 0x00,
    kPipeRegSignalBufferHigh = 0x04,
    kPipeRegSignalBuffer = 0x08,
    kPipeRegSignalBufferCount = 0x0c,
    kPipeRegOpenBufferHigh = 0x14,
    kPipeRegOpenBuffer = 0x18,
    kPipeRegVersion = 0x24,
    kPipeRegGetSignalled = 0x30,
};

enum GoldfishPipeCommandCode : int32_t {
    kPipeCmdOpen = 1,
    kPipeCmdClose = 2,
    kPipeCmdPoll = 3,
    kPipeCmdWrite = 4,
    kPipeCmdWakeOnWrite = 5,
    kPipeCmdRead = 6,
    kPipeCmdWakeOnRead = 7,
    kPipeCmdWakeOnDoneIo = 8,
    kPipeCmdDmaHostMap = 9,
    kPipeCmdDmaHostUnmap = 10,
    kPipeCmdCall = 11,
};

enum GoldfishPipeError : int32_t {
    kPipeStatusOk = 0,
    kPipeErrorInval = -1,
    kPipeErrorAgain = -2,
    kPipeErrorNomem = -3,
    kPipeErrorIo = -4,
};

enum GoldfishPipePollFlags : uint32_t {
    kPipePollIn = 1 << 0,
    kPipePollOut = 1 << 1,
    kPipePollHup = 1 << 2,
};

enum GoldfishPipeWakeFlags : uint32_t {
    kPipeWakeClosed = 1 << 0,
    kPipeWakeRead = 1 << 1,
    kPipeWakeWrite = 1 << 2,
    kPipeWakeUnlockDma = 1 << 3,
    kPipeWakeUnlockDmaShared = 1 << 4,
};

constexpr uint32_t kGoldfishPipeDeviceVersion = 2;
constexpr uint32_t kMaxPipeBuffersCount = 1024;
constexpr uint32_t kMaxPipeBufferSize = 16U << 20;  // 16 MiB
constexpr size_t kMaxHandshakeLength = 256;
constexpr uint32_t kMaxSnapshotPipeCount = GOLDFISH_PIPE_SNAPSHOT_MAX_PIPES;
constexpr uint32_t kMaxSnapshotPipeEntryBytes = GOLDFISH_PIPE_SNAPSHOT_MAX_PIPE_ENTRY_BYTES;
constexpr uint64_t kMaxSnapshotQueuedBytes = GOLDFISH_PIPE_SNAPSHOT_MAX_QUEUED_BYTES;

struct OpenCommandBuffer {
    uint64_t command_buffer_ptr = 0;
    uint32_t rw_params_max_count = 0;
};

struct PipeCommandHeader {
    int32_t cmd = 0;
    int32_t id = 0;
    int32_t status = 0;
    int32_t reserved = 0;
    uint32_t buffers_count = 0;
    uint32_t consumed_size = 0;
};
static_assert(sizeof(PipeCommandHeader) == 24);

struct SignalledPipeEntry {
    uint32_t id = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(SignalledPipeEntry) == 8);

struct GuestIoBuffer {
    uint64_t gpa = 0;
    uint32_t size = 0;
};

using PipeWakeCallback = std::function<void(uint32_t wake_flags)>;

class IPipeService {
  public:
    virtual ~IPipeService() = default;
    virtual int32_t OnWrite(const uint8_t* data, size_t size, size_t* consumed) = 0;
    virtual int32_t OnRead(uint8_t* data, size_t size, size_t* produced) = 0;
    virtual uint32_t OnPoll() const = 0;
    virtual void OnClose() = 0;
    // Appends the service state to `out`; `queued_bytes` receives the number of
    // host->guest bytes that are buffered on the host and included in `out`
    // (counted against kMaxSnapshotQueuedBytes; 0 for services without a queue).
    virtual void Save(archive::IWriter& out, uint64_t* queued_bytes) const = 0;
    // Restores the service state written by `Save()` from `in`. Returns a
    // DATA_LOSS error naming the field, value and limit on corrupt input.
    virtual absl::Status Load(archive::IReader& in) = 0;
};

class GoldfishPipeDevice final : public IMmioDevice {
  public:
    GoldfishPipeDevice(std::unique_ptr<IGuestMemory> memory, SetIrqCallback set_irq,
                       IGoldfishGpu& gpu);
    ~GoldfishPipeDevice() override;

    GoldfishPipeDevice(const GoldfishPipeDevice&) = delete;
    GoldfishPipeDevice& operator=(const GoldfishPipeDevice&) = delete;

    uint32_t Read(uint32_t offset) ABSL_LOCKS_EXCLUDED(mu_) override;
    void Write(uint32_t offset, uint32_t value) ABSL_LOCKS_EXCLUDED(mu_) override;
    void Reset() ABSL_LOCKS_EXCLUDED(mu_) override;

    // Snapshot hooks. The GoldfishGpu itself is quiesced, saved, loaded, and resumed by its own
    // VMState sections (goldfish_gpu_low_level.c).
    absl::Status Save(std::string_view snapshot_name, archive::IWriter& out)
            ABSL_LOCKS_EXCLUDED(mu_);
    absl::Status PreLoad(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_);
    absl::Status Load(std::string_view snapshot_name, archive::IReader& in)
            ABSL_LOCKS_EXCLUDED(mu_);
    absl::Status PostLoad(std::string_view snapshot_name) ABSL_LOCKS_EXCLUDED(mu_);

  private:
    struct WakerState {
        GoldfishPipeDevice* dev ABSL_GUARDED_BY(mu) = nullptr;
        absl::Mutex mu;
    };

    struct PipeSlot {
        std::string handshake_buf;
        std::shared_ptr<IPipeService> handler;
        uint64_t generation = 0;
        uint64_t command_buffer_gpa = 0;
        int32_t id = 0;
        uint32_t rw_params_max_count = 0;
        uint32_t wanted_flags = 0;
        uint32_t signalled_flags = 0;
        bool handshake_done = false;
    };

    struct RestoredState {
        std::map<int32_t, PipeSlot> pipes;
        uint64_t open_buffer_addr = 0;
        uint64_t signal_buffer_addr = 0;
        uint32_t signal_buffer_size = 0;
        uint32_t version = 0;
    };

    void SignalWakeWithGeneration(int32_t pipe_id, uint64_t generation, uint32_t wake_flags)
            ABSL_LOCKS_EXCLUDED(mu_);
    void ExecutePipeCommand(int32_t pipe_id, bool* out_raise_irq) ABSL_LOCKS_EXCLUDED(mu_);
    int32_t DoWrite(int32_t pipe_id, uint64_t generation, const std::vector<GuestIoBuffer>& bufs,
                    uint32_t* total_consumed) ABSL_LOCKS_EXCLUDED(mu_);
    int32_t DoRead(int32_t pipe_id, uint64_t generation, const std::vector<GuestIoBuffer>& bufs,
                   uint32_t* total_produced) ABSL_LOCKS_EXCLUDED(mu_);
    bool ReadIoBuffersLocked(const PipeSlot& slot, const PipeCommandHeader& hdr,
                             std::vector<GuestIoBuffer>* out, uint32_t* out_read_index)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
    std::shared_ptr<IPipeService> CreateServiceHandler(int32_t pipe_id, uint64_t generation,
                                                       std::string_view service_name)
            ABSL_LOCKS_EXCLUDED(mu_);
    absl::StatusOr<std::shared_ptr<IPipeService>> RestoreServiceHandler(
            int32_t pipe_id, uint64_t generation, std::string_view service_name,
            archive::IReader& in, uint64_t* queued_budget) ABSL_LOCKS_EXCLUDED(mu_);
    // Parses a snapshot stream into `out` without touching device state. On
    // error, handlers already restored into `out->pipes` are left for the
    // caller to close.
    absl::Status ParseSnapshot(archive::IReader& in, RestoredState* out)
            ABSL_LOCKS_EXCLUDED(mu_);
    bool HasSignalledPipesLocked() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
    uint32_t DrainSignalledPipesLocked(bool* out_lower_irq) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

    const std::unique_ptr<IGuestMemory> memory_;
    const std::shared_ptr<WakerState> waker_state_;
    const SetIrqCallback set_irq_;
    IGoldfishGpu& gpu_;

    std::map<int32_t, PipeSlot> pipes_ ABSL_GUARDED_BY(mu_);
    uint64_t open_buffer_addr_ ABSL_GUARDED_BY(mu_) = 0;
    uint64_t signal_buffer_addr_ ABSL_GUARDED_BY(mu_) = 0;
    uint64_t next_generation_ ABSL_GUARDED_BY(mu_) = 1;
    uint32_t signal_buffer_size_ ABSL_GUARDED_BY(mu_) = 0;
    uint32_t version_ ABSL_GUARDED_BY(mu_) = kGoldfishPipeDeviceVersion;
    absl::Mutex mu_;
};

}  // namespace goldfish::legacy

extern "C" {
#endif

struct GoldfishMmioCore GoldfishPipeCoreCreate(const struct GoldfishDeviceEnv* env);

bool GoldfishPipeCoreSave(void* core, const char* snapshot_name, QEMUFile* f);
bool GoldfishPipeCorePreLoad(void* core, const char* snapshot_name);
bool GoldfishPipeCoreLoad(void* core, const char* snapshot_name, QEMUFile* f);
bool GoldfishPipeCorePostLoad(void* core, const char* snapshot_name);

#ifdef __cplusplus
}
#endif
