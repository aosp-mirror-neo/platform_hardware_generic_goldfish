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

#include "goldfish_pipe.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "android/status/status_matcher_macros.h"
#include "goldfish/archive/deque_archive.h"
#include "goldfish/legacy/common/fake_guest_memory.h"
#include "goldfish/legacy/goldfish_gpu/fake_goldfish_gpu.h"

struct QEMUFile {
    goldfish::archive::DequeArchive archive;
};

extern "C" size_t qemu_get_buffer(  // NOLINT(readability-identifier-naming)
        QEMUFile* f, uint8_t* buf, size_t size) {
    if (f == nullptr || size == 0) {
        return 0;
    }
    const size_t avail = std::min(size, f->archive.Size());
    if (avail > 0 && !f->archive.Read(buf, avail).ok()) {
        return 0;
    }
    return avail;
}

extern "C" void qemu_put_buffer(  // NOLINT(readability-identifier-naming)
        QEMUFile* f, const uint8_t* buf, size_t size) {
    if (f != nullptr && size > 0) {
        f->archive.Write(buf, size);
    }
}

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

absl::Status LoadFromBytes(GoldfishPipeDevice& dev, std::string_view snap_name,
                           const uint8_t* data, size_t size) {
    archive::DequeArchive in;
    if (data != nullptr && size > 0) {
        in.storage.assign(data, data + size);
    }
    return dev.Load(snap_name, in);
}

absl::Status LoadFromBytes(GoldfishPipeDevice& dev, std::string_view snap_name,
                           const std::vector<uint8_t>& blob) {
    return LoadFromBytes(dev, snap_name, blob.data(), blob.size());
}

class TestGoldfishGpu : public FakeGoldfishGpu {
  public:
    uint64_t CreateProcessContext() override {
        created_puids.push_back(next_puid);
        return next_puid++;
    }

    void DestroyProcessContext(uint64_t puid) override { destroyed_puids.push_back(puid); }

    void ColorBufferRef(uint32_t handle) override { opened_color_buffers.push_back(handle); }

    void ColorBufferLastRef(uint32_t handle) override { closed_color_buffers.push_back(handle); }

    absl::StatusOr<uint32_t> CreateOpenglesPipe(OpenglesReadCallback on_host_data,
                                                OpenglesWritableCallback on_writable) override {
        last_read_cb = std::move(on_host_data);
        last_writable_cb = std::move(on_writable);
        uint32_t id = next_pipe_id++;
        opened_pipes.push_back(id);
        return id;
    }

    absl::Status WriteOpenglesPipe(uint32_t /*id*/, const uint8_t* data, size_t size) override {
        ++write_calls;
        if (write_calls == unavailable_write_call) {
            if (fire_writable_inside_unavailable_write && last_writable_cb != nullptr) {
                // gfxstream can report CanWrite synchronously from inside the
                // CanWrite re-arm that a full tryWrite() performs, i.e. before
                // WriteOpenglesPipe() has returned UNAVAILABLE to the caller.
                last_writable_cb();
            }
            return absl::UnavailableError("render channel full");
        }
        written_data.insert(written_data.end(), data, data + size);
        if (sync_echo_on_write && last_read_cb != nullptr) {
            last_read_cb(data, size);
        }
        return absl::OkStatus();
    }

    void CloseOpenglesPipe(uint32_t id) override { closed_pipes.push_back(id); }

    absl::Status RestoreOpenglesPipe(uint32_t id, OpenglesReadCallback on_host_data,
                                     OpenglesWritableCallback on_writable) override {
        last_read_cb = std::move(on_host_data);
        last_writable_cb = std::move(on_writable);
        bound_pipes.push_back(id);
        return absl::OkStatus();
    }

    absl::Status Quiesce(std::string_view snapshot_name) override {
        quiesced_snapshots.emplace_back(snapshot_name);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<uint8_t>> Save(std::string_view snapshot_name) override {
        saved_snapshots.emplace_back(snapshot_name);
        return gpu_meta_to_save;
    }

    absl::Status PostSave(std::string_view snapshot_name) override {
        post_saved_snapshots.emplace_back(snapshot_name);
        return absl::OkStatus();
    }

    absl::Status PreLoad(std::string_view snapshot_name) override {
        pre_loaded_snapshots.emplace_back(snapshot_name);
        return absl::OkStatus();
    }

    absl::Status Load(std::string_view snapshot_name, const uint8_t* meta_data,
                      size_t meta_size) override {
        loaded_snapshots.emplace_back(snapshot_name);
        if (meta_data != nullptr && meta_size > 0) {
            loaded_gpu_meta.assign(meta_data, meta_data + meta_size);
        } else {
            loaded_gpu_meta.clear();
        }
        return absl::OkStatus();
    }

    absl::Status PostLoad(std::string_view snapshot_name) override {
        post_loaded_snapshots.emplace_back(snapshot_name);
        return absl::OkStatus();
    }

    OpenglesReadCallback last_read_cb;
    OpenglesWritableCallback last_writable_cb;
    std::vector<uint64_t> created_puids;
    std::vector<uint64_t> destroyed_puids;
    std::vector<uint32_t> opened_color_buffers;
    std::vector<uint32_t> closed_color_buffers;
    std::vector<uint32_t> opened_pipes;
    std::vector<uint32_t> bound_pipes;
    std::vector<uint32_t> closed_pipes;
    std::vector<uint8_t> written_data;
    std::vector<uint8_t> gpu_meta_to_save = {0x47, 0x47, 0x50, 0x55};
    std::vector<uint8_t> loaded_gpu_meta;
    std::vector<std::string> quiesced_snapshots;
    std::vector<std::string> saved_snapshots;
    std::vector<std::string> post_saved_snapshots;
    std::vector<std::string> pre_loaded_snapshots;
    std::vector<std::string> loaded_snapshots;
    std::vector<std::string> post_loaded_snapshots;
    uint64_t next_puid = 0x123456789ABCDEF0ULL;
    uint32_t next_pipe_id = 1;
    // 1-based index of the WriteOpenglesPipe() call that returns UNAVAILABLE (0 = never).
    int unavailable_write_call = 0;
    int write_calls = 0;
    bool sync_echo_on_write = false;
    bool fire_writable_inside_unavailable_write = false;
};

void SetupBuffers(FakeGuestMemory* mem, uint64_t cmd_gpa, uint32_t max_bufs,
                  const std::vector<GuestIoBuffer>& bufs, uint32_t read_index) {
    const uint64_t ptrs_gpa = cmd_gpa + sizeof(PipeCommandHeader);
    const uint64_t sizes_gpa = ptrs_gpa + static_cast<uint64_t>(max_bufs) * sizeof(uint64_t);
    const uint64_t read_index_gpa = sizes_gpa + static_cast<uint64_t>(max_bufs) * sizeof(uint32_t);
    for (size_t i = 0; i < bufs.size(); ++i) {
        mem->Put<uint64_t>(ptrs_gpa + i * sizeof(uint64_t), bufs[i].gpa);
        mem->Put<uint32_t>(sizes_gpa + i * sizeof(uint32_t), bufs[i].size);
    }
    mem->Put<uint32_t>(read_index_gpa, read_index);
}

TEST(GoldfishPipeTest, GlProcessPipeCreatesAndDestroysProcessContextViaCall) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;

    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 10;

    dev.Write(0x24, 4);
    EXPECT_EQ(dev.Read(0x24), kGoldfishPipeDeviceVersion);

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));

    OpenCommandBuffer open_buf{.command_buffer_ptr = kCmdBufGpa, .rw_params_max_count = kMaxBufs};
    mem_ptr->Put(kOpenBufGpa, open_buf);

    PipeCommandHeader hdr{.cmd = kPipeCmdOpen, .id = kPipeId};
    mem_ptr->Put(kCmdBufGpa, hdr);
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa).status, 0);

    // Handshake "pipe:GLProcessPipe\0" + 4-byte confirmation int (100) + 8-byte read in a CALL.
    const std::string handshake = "pipe:GLProcessPipe";
    constexpr uint64_t kDataGpa = 0x3000;
    constexpr uint64_t kConfirmGpa = 0x3100;
    constexpr uint64_t kReplyGpa = 0x3200;
    mem_ptr->Write(kDataGpa, handshake.c_str(), handshake.size() + 1);
    const int32_t confirm_val = 100;
    mem_ptr->Put<int32_t>(kConfirmGpa, confirm_val);

    hdr = PipeCommandHeader{.cmd = kPipeCmdCall, .id = kPipeId, .buffers_count = 3};
    mem_ptr->Put(kCmdBufGpa, hdr);
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs,
                 {{kDataGpa, static_cast<uint32_t>(handshake.size() + 1)},
                  {kConfirmGpa, 4},
                  {kReplyGpa, 8}},
                 /*read_index=*/2);
    dev.Write(kPipeRegCmd, kPipeId);

    hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.status, kPipeStatusOk);
    EXPECT_EQ(hdr.consumed_size, static_cast<uint32_t>(handshake.size() + 1 + 4 + 8));
    ASSERT_EQ(gpu.created_puids.size(), 1U);
    EXPECT_EQ(mem_ptr->Get<uint64_t>(kReplyGpa), gpu.created_puids[0]);

    // Close pipe -> destroys process context.
    hdr = PipeCommandHeader{.cmd = kPipeCmdClose, .id = kPipeId};
    mem_ptr->Put(kCmdBufGpa, hdr);
    dev.Write(kPipeRegCmd, kPipeId);
    ASSERT_EQ(gpu.destroyed_puids.size(), 1U);
    EXPECT_EQ(gpu.destroyed_puids[0], gpu.created_puids[0]);
}

TEST(GoldfishPipeTest, OpenglesPipeReadWriteAndWake) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;

    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 20;

    dev.Write(0x24, 4);
    EXPECT_EQ(dev.Read(0x24), kGoldfishPipeDeviceVersion);

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);

    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdBufGpa,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);

    const std::string handshake = "pipe:opengles";
    constexpr uint64_t kDataGpa = 0x3000;
    mem_ptr->Write(kDataGpa, handshake.c_str(), handshake.size() + 1);
    const uint32_t payload = 0xCAFEBABE;
    mem_ptr->Put(kDataGpa + 64, payload);

    PipeCommandHeader hdr{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 2};
    mem_ptr->Put(kCmdBufGpa, hdr);
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs,
                 {{kDataGpa, static_cast<uint32_t>(handshake.size() + 1)}, {kDataGpa + 64, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_EQ(gpu.opened_pipes.size(), 1U);
    EXPECT_EQ(gpu.written_data.size(), 4U);

    // Request WAKE_ON_READ while empty, then push host data via callback.
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdWakeOnRead, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_FALSE(irq);

    const uint32_t host_reply_1 = 0xDEADBEEFU;
    const uint32_t host_reply_2 = 0xCAFE1234U;
    ASSERT_TRUE(gpu.last_read_cb != nullptr);
    gpu.last_read_cb(reinterpret_cast<const uint8_t*>(&host_reply_1), sizeof(host_reply_1));
    gpu.last_read_cb(reinterpret_cast<const uint8_t*>(&host_reply_2), sizeof(host_reply_2));
    EXPECT_TRUE(irq);

    EXPECT_EQ(dev.Read(kPipeRegGetSignalled), 1U);
    EXPECT_FALSE(irq);
    auto sig = mem_ptr->Get<SignalledPipeEntry>(kSignalBufGpa);
    EXPECT_EQ(sig.id, static_cast<uint32_t>(kPipeId));
    EXPECT_EQ(sig.flags, static_cast<uint32_t>(kPipeWakeRead));

    // Drain the first 2 bytes via a partial kPipeCmdRead.
    constexpr uint64_t kReadBuf1Gpa = 0x3100;
    constexpr uint64_t kReadBuf2Gpa = 0x3200;
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdRead, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kReadBuf1Gpa, 2}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);

    hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.consumed_size, 2U);

    // Drain the remaining 6 bytes across two scatter-gather guest buffers (2 bytes + 4 bytes).
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdRead, .id = kPipeId, .buffers_count = 2});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kReadBuf1Gpa + 2, 2}, {kReadBuf2Gpa, 8}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);

    hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.consumed_size, 6U);
    EXPECT_EQ(mem_ptr->Get<uint32_t>(kReadBuf1Gpa), host_reply_1);
    EXPECT_EQ(mem_ptr->Get<uint32_t>(kReadBuf2Gpa), host_reply_2);

    // Reading again when the queue is drained returns kPipeErrorAgain with 0 bytes produced.
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdRead, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kReadBuf2Gpa, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);

    hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.status, kPipeErrorAgain);
    EXPECT_EQ(hdr.consumed_size, 0U);
}

TEST(GoldfishPipeTest, RefcountPipeNotifiesColorBufferLastRefOnClose) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;

    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kCmdBufGpa1 = 0x2000;
    constexpr uint64_t kCmdBufGpa2 = 0x2400;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId1 = 25;
    constexpr int32_t kPipeId2 = 26;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));

    // Open pipe 1 for a valid 4-byte ColorBuffer handle (written in two 2-byte chunks).
    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdBufGpa1,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdBufGpa1, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeId1});
    dev.Write(kPipeRegCmd, kPipeId1);

    const std::string handshake = "pipe:refcount";
    constexpr uint64_t kHandshakeGpa = 0x3000;
    constexpr uint64_t kHandleGpa = 0x3100;
    constexpr uint64_t kReadGpa = 0x3200;
    mem_ptr->Write(kHandshakeGpa, handshake.c_str(), handshake.size() + 1);

    constexpr uint32_t kColorBufferHandle = 0x42420007U;
    mem_ptr->Put<uint32_t>(kHandleGpa, kColorBufferHandle);

    // Write handshake + first 2 bytes of the handle.
    mem_ptr->Put(kCmdBufGpa1,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId1, .buffers_count = 2});
    SetupBuffers(mem_ptr, kCmdBufGpa1, kMaxBufs,
                 {{kHandshakeGpa, static_cast<uint32_t>(handshake.size() + 1)}, {kHandleGpa, 2}},
                 0);
    dev.Write(kPipeRegCmd, kPipeId1);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa1).consumed_size,
              static_cast<uint32_t>(handshake.size() + 1 + 2));

    // Write the remaining 2 bytes of the handle.
    mem_ptr->Put(kCmdBufGpa1,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId1, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa1, kMaxBufs, {{kHandleGpa + 2, 2}}, 0);
    dev.Write(kPipeRegCmd, kPipeId1);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa1).consumed_size, 2U);
    ASSERT_EQ(gpu.opened_color_buffers.size(), 1U);
    EXPECT_EQ(gpu.opened_color_buffers[0], kColorBufferHandle);

    // Reading from pipe:refcount returns kPipeErrorAgain.
    mem_ptr->Put(kCmdBufGpa1,
                 PipeCommandHeader{.cmd = kPipeCmdRead, .id = kPipeId1, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa1, kMaxBufs, {{kReadGpa, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId1);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa1).status, kPipeErrorAgain);
    EXPECT_TRUE(gpu.closed_color_buffers.empty());

    // Open pipe 2 with an incomplete 2-byte handle to verify it does not trigger
    // ColorBufferLastRef.
    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdBufGpa2,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdBufGpa2, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeId2});
    dev.Write(kPipeRegCmd, kPipeId2);

    mem_ptr->Put(kCmdBufGpa2,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId2, .buffers_count = 2});
    SetupBuffers(mem_ptr, kCmdBufGpa2, kMaxBufs,
                 {{kHandshakeGpa, static_cast<uint32_t>(handshake.size() + 1)}, {kHandleGpa, 2}},
                 0);
    dev.Write(kPipeRegCmd, kPipeId2);

    mem_ptr->Put(kCmdBufGpa2, PipeCommandHeader{.cmd = kPipeCmdClose, .id = kPipeId2});
    dev.Write(kPipeRegCmd, kPipeId2);
    EXPECT_TRUE(gpu.closed_color_buffers.empty());

    // Closing pipe 1 notifies ColorBufferLastRef with kColorBufferHandle.
    mem_ptr->Put(kCmdBufGpa1, PipeCommandHeader{.cmd = kPipeCmdClose, .id = kPipeId1});
    dev.Write(kPipeRegCmd, kPipeId1);
    ASSERT_EQ(gpu.closed_color_buffers.size(), 1U);
    EXPECT_EQ(gpu.closed_color_buffers[0], kColorBufferHandle);
}

TEST(GoldfishPipeTest, StaleOpenglesCallbackAfterPipeCloseOrDeviceDestructionIsSafeNoOp) {
    TestGoldfishGpu gpu;
    std::atomic<int> irq_count{0};
    {
        auto mem = std::make_unique<FakeGuestMemory>(0x10000);
        FakeGuestMemory* mem_ptr = mem.get();
        GoldfishPipeDevice dev(std::move(mem), [&irq_count](bool /*high*/) { irq_count++; }, gpu);

        constexpr uint64_t kOpenBufGpa = 0x1000;
        constexpr uint64_t kCmdBufGpa = 0x2000;
        constexpr uint32_t kMaxBufs = 4;
        constexpr int32_t kPipeId = 30;

        dev.Write(kPipeRegOpenBufferHigh, 0);
        dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
        mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdBufGpa,
                                                    .rw_params_max_count = kMaxBufs});
        mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeId});
        dev.Write(kPipeRegCmd, kPipeId);

        const std::string handshake = "pipe:opengles";
        constexpr uint64_t kDataGpa = 0x3000;
        mem_ptr->Write(kDataGpa, handshake.c_str(), handshake.size() + 1);
        mem_ptr->Put(kCmdBufGpa,
                     PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
        SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs,
                     {{kDataGpa, static_cast<uint32_t>(handshake.size() + 1)}}, 0);
        dev.Write(kPipeRegCmd, kPipeId);
        ASSERT_TRUE(gpu.last_read_cb != nullptr);
    }

    // Invoking the host data callback after GoldfishPipeDevice destruction must be a safe no-op.
    const uint32_t data = 0x11223344;
    gpu.last_read_cb(reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    EXPECT_EQ(irq_count.load(), 0);
    EXPECT_EQ(gpu.closed_pipes.size(), 1U);
}

TEST(GoldfishPipeTest, ConcurrentHostWakeAndGuestDrain) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    std::atomic<bool> irq{false};

    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq.store(high); }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 40;

    dev.Write(0x24, 4);
    EXPECT_EQ(dev.Read(0x24), kGoldfishPipeDeviceVersion);

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);

    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdBufGpa,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);

    const std::string handshake = "pipe:opengles";
    constexpr uint64_t kDataGpa = 0x3000;
    mem_ptr->Write(kDataGpa, handshake.c_str(), handshake.size() + 1);
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs,
                 {{kDataGpa, static_cast<uint32_t>(handshake.size() + 1)}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);
    ASSERT_TRUE(gpu.last_read_cb != nullptr);

    std::atomic<bool> stop{false};
    std::thread producer([&]() {
        const uint8_t byte = 0xAA;
        while (!stop.load(std::memory_order_relaxed)) {
            gpu.last_read_cb(&byte, 1);
        }
    });

    for (int i = 0; i < 100; ++i) {
        mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdWakeOnRead, .id = kPipeId});
        dev.Write(kPipeRegCmd, kPipeId);
        (void)dev.Read(kPipeRegGetSignalled);
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();
}

TEST(GoldfishPipeTest, SynchronousHostCallbackDuringWriteDoesNotDeadlock) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    gpu.sync_echo_on_write = true;
    bool irq = false;

    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 50;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);

    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdBufGpa,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);

    // Complete handshake first.
    const std::string handshake = "pipe:opengles";
    constexpr uint64_t kHandshakeGpa = 0x3000;
    mem_ptr->Write(kHandshakeGpa, handshake.c_str(), handshake.size() + 1);
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs,
                 {{kHandshakeGpa, static_cast<uint32_t>(handshake.size() + 1)}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);

    // Arm WAKE_ON_READ, then issue a Write that synchronously invokes on_host_data -> SignalWake.
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdWakeOnRead, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_FALSE(irq);

    constexpr uint64_t kPayloadGpa = 0x3100;
    constexpr uint64_t kReplyGpa = 0x3200;
    const uint32_t payload = 0x13572468U;
    mem_ptr->Put(kPayloadGpa, payload);

    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdCall, .id = kPipeId, .buffers_count = 2});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kPayloadGpa, 4}, {kReplyGpa, 4}},
                 /*read_index=*/1);
    dev.Write(kPipeRegCmd, kPipeId);

    EXPECT_TRUE(irq);
    const auto hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.status, kPipeStatusOk);
    EXPECT_EQ(hdr.consumed_size, 8U);
    EXPECT_EQ(mem_ptr->Get<uint32_t>(kReplyGpa), payload);
}

TEST(GoldfishPipeTest, SnapshotSaveAndRestorePreservesAllPipesWithoutDrivingGpuSnapshot) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;

    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdGpaGlProc = 0x2000;
    constexpr uint64_t kCmdGpaRefcount = 0x2400;
    constexpr uint64_t kCmdGpaOpengles = 0x2800;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeGlProc = 11;
    constexpr int32_t kPipeRefcount = 12;
    constexpr int32_t kPipeOpengles = 13;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);

    // 1. Open and initialize pipe:GLProcessPipe (without reading reply yet).
    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdGpaGlProc,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdGpaGlProc, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeGlProc});
    dev.Write(kPipeRegCmd, kPipeGlProc);

    const std::string hs_glproc = "pipe:GLProcessPipe";
    constexpr uint64_t kDataGpa = 0x3000;
    mem_ptr->Write(kDataGpa, hs_glproc.c_str(), hs_glproc.size() + 1);
    mem_ptr->Put<int32_t>(kDataGpa + 64, 100);
    mem_ptr->Put(kCmdGpaGlProc,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeGlProc, .buffers_count = 2});
    SetupBuffers(mem_ptr, kCmdGpaGlProc, kMaxBufs,
                 {{kDataGpa, static_cast<uint32_t>(hs_glproc.size() + 1)}, {kDataGpa + 64, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeGlProc);
    ASSERT_EQ(gpu.created_puids.size(), 1U);

    // 2. Open and initialize pipe:refcount with handle 0x778899AA.
    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdGpaRefcount,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdGpaRefcount, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeRefcount});
    dev.Write(kPipeRegCmd, kPipeRefcount);

    const std::string hs_refcount = "pipe:refcount";
    mem_ptr->Write(kDataGpa + 128, hs_refcount.c_str(), hs_refcount.size() + 1);
    constexpr uint32_t kCbHandle = 0x778899AAU;
    mem_ptr->Put<uint32_t>(kDataGpa + 192, kCbHandle);
    mem_ptr->Put(kCmdGpaRefcount,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeRefcount, .buffers_count = 2});
    SetupBuffers(
            mem_ptr, kCmdGpaRefcount, kMaxBufs,
            {{kDataGpa + 128, static_cast<uint32_t>(hs_refcount.size() + 1)}, {kDataGpa + 192, 4}},
            0);
    dev.Write(kPipeRegCmd, kPipeRefcount);

    // 3. Open and initialize pipe:opengles, push unread host data into its queue.
    mem_ptr->Put(kOpenBufGpa, OpenCommandBuffer{.command_buffer_ptr = kCmdGpaOpengles,
                                                .rw_params_max_count = kMaxBufs});
    mem_ptr->Put(kCmdGpaOpengles, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = kPipeOpengles});
    dev.Write(kPipeRegCmd, kPipeOpengles);

    const std::string hs_opengles = "pipe:opengles";
    mem_ptr->Write(kDataGpa + 256, hs_opengles.c_str(), hs_opengles.size() + 1);
    mem_ptr->Put(kCmdGpaOpengles,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeOpengles, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdGpaOpengles, kMaxBufs,
                 {{kDataGpa + 256, static_cast<uint32_t>(hs_opengles.size() + 1)}}, 0);
    dev.Write(kPipeRegCmd, kPipeOpengles);
    ASSERT_NE(gpu.last_read_cb, nullptr);

    constexpr uint32_t kPendingReply = 0xFEEDFACEU;
    gpu.last_read_cb(reinterpret_cast<const uint8_t*>(&kPendingReply), sizeof(kPendingReply));

    // Save snapshot via C bridge functions. The GPU is quiesced, saved and resumed by its own
    // VMState sections, so the pipe must not call Quiesce(), Save(), or PostSave().
    IMmioDevice* mmio_core = &dev;
    QEMUFile qf;
    ASSERT_TRUE(GoldfishPipeCoreSave(mmio_core, "snap_测试_🚀", &qf));
    EXPECT_GT(qf.archive.Size(), 0U);
    EXPECT_TRUE(gpu.quiesced_snapshots.empty());
    EXPECT_TRUE(gpu.saved_snapshots.empty());
    EXPECT_TRUE(gpu.post_saved_snapshots.empty());

    // Load into a fresh GoldfishPipeDevice sharing the same guest memory. PreLoad(), Load(), and
    // PostLoad() of the GPU belong to its own sections too; the pipe only re-binds its pipes and
    // re-evaluates wanted_flags.
    ASSERT_TRUE(GoldfishPipeCorePreLoad(mmio_core, "snap_测试_🚀"));
    EXPECT_TRUE(gpu.pre_loaded_snapshots.empty());
    // PreLoad must not destroy process contexts or ColorBuffer refs on the old pipes.
    EXPECT_TRUE(gpu.destroyed_puids.empty());
    EXPECT_TRUE(gpu.closed_color_buffers.empty());

    ASSERT_TRUE(GoldfishPipeCoreLoad(mmio_core, "snap_测试_🚀", &qf));
    EXPECT_TRUE(gpu.loaded_snapshots.empty());
    EXPECT_TRUE(gpu.loaded_gpu_meta.empty());
    EXPECT_TRUE(gpu.post_loaded_snapshots.empty());
    ASSERT_TRUE(GoldfishPipeCorePostLoad(mmio_core, "snap_测试_🚀"));
    EXPECT_TRUE(gpu.post_loaded_snapshots.empty());

    ASSERT_EQ(gpu.bound_pipes.size(), 1U);
    EXPECT_EQ(gpu.bound_pipes[0], gpu.opened_pipes[0]);

    // Verify restored GLProcessPipe still delivers its 8-byte puid reply and destroys on close.
    constexpr uint64_t kReadGpa = 0x4000;
    mem_ptr->Put(kCmdGpaGlProc,
                 PipeCommandHeader{.cmd = kPipeCmdRead, .id = kPipeGlProc, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdGpaGlProc, kMaxBufs, {{kReadGpa, 8}}, 0);
    dev.Write(kPipeRegCmd, kPipeGlProc);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdGpaGlProc).consumed_size, 8U);
    EXPECT_EQ(mem_ptr->Get<uint64_t>(kReadGpa), gpu.created_puids[0]);

    mem_ptr->Put(kCmdGpaGlProc, PipeCommandHeader{.cmd = kPipeCmdClose, .id = kPipeGlProc});
    dev.Write(kPipeRegCmd, kPipeGlProc);
    EXPECT_EQ(gpu.destroyed_puids, std::vector<uint64_t>{gpu.created_puids[0]});

    // Verify restored refcount pipe closes its ColorBuffer handle on close.
    mem_ptr->Put(kCmdGpaRefcount, PipeCommandHeader{.cmd = kPipeCmdClose, .id = kPipeRefcount});
    dev.Write(kPipeRegCmd, kPipeRefcount);
    EXPECT_EQ(gpu.closed_color_buffers, std::vector<uint32_t>{kCbHandle});

    // Verify restored opengles pipe still has kPendingReply in its read queue and can receive new
    // host data via the rebound callback.
    mem_ptr->Put(kCmdGpaOpengles,
                 PipeCommandHeader{.cmd = kPipeCmdRead, .id = kPipeOpengles, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdGpaOpengles, kMaxBufs, {{kReadGpa + 16, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeOpengles);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdGpaOpengles).consumed_size, 4U);
    EXPECT_EQ(mem_ptr->Get<uint32_t>(kReadGpa + 16), kPendingReply);

    mem_ptr->Put(kCmdGpaOpengles,
                 PipeCommandHeader{.cmd = kPipeCmdWakeOnRead, .id = kPipeOpengles});
    dev.Write(kPipeRegCmd, kPipeOpengles);
    irq = false;
    constexpr uint32_t kPostRestoreReply = 0x12344321U;
    gpu.last_read_cb(reinterpret_cast<const uint8_t*>(&kPostRestoreReply),
                     sizeof(kPostRestoreReply));
    EXPECT_TRUE(irq);

    // Corrupt snapshot magic is rejected cleanly.
    QEMUFile bad_qf;
    const uint8_t bad_snapshot[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    bad_qf.archive.Write(bad_snapshot, sizeof(bad_snapshot));
    EXPECT_FALSE(GoldfishPipeCoreLoad(&dev, "snap_bad", &bad_qf));
}

// Opens `pipe_id` with its command buffer at `cmd_gpa` and completes the `handshake`
// (e.g. "pipe:opengles") through a single kPipeCmdWrite whose payload lives at `data_gpa`.
void OpenPipeWithHandshake(GoldfishPipeDevice& dev, FakeGuestMemory* mem, uint64_t open_buf_gpa,
                           uint64_t cmd_gpa, uint32_t max_bufs, int32_t pipe_id,
                           const std::string& handshake, uint64_t data_gpa) {
    mem->Put(open_buf_gpa,
             OpenCommandBuffer{.command_buffer_ptr = cmd_gpa, .rw_params_max_count = max_bufs});
    mem->Put(cmd_gpa, PipeCommandHeader{.cmd = kPipeCmdOpen, .id = pipe_id});
    dev.Write(kPipeRegCmd, pipe_id);
    ASSERT_EQ(mem->Get<PipeCommandHeader>(cmd_gpa).status, kPipeStatusOk);

    mem->Write(data_gpa, handshake.c_str(), handshake.size() + 1);
    mem->Put(cmd_gpa, PipeCommandHeader{.cmd = kPipeCmdWrite, .id = pipe_id, .buffers_count = 1});
    SetupBuffers(mem, cmd_gpa, max_bufs, {{data_gpa, static_cast<uint32_t>(handshake.size() + 1)}},
                 0);
    dev.Write(kPipeRegCmd, pipe_id);
    ASSERT_EQ(mem->Get<PipeCommandHeader>(cmd_gpa).consumed_size, handshake.size() + 1);
}

// A full render channel must not block the vCPU thread (which holds the BQL): the guest gets
// kPipeErrorAgain and is woken with kPipeWakeWrite once the renderer reports writability.
TEST(GoldfishPipeTest, OpenglesWriteReturnsAgainWhenChannelFullAndWakesOnWritable) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;
    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint64_t kDataGpa = 0x3000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 60;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);
    ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(dev, mem_ptr, kOpenBufGpa, kCmdBufGpa, kMaxBufs,
                                                  kPipeId, "pipe:opengles", kDataGpa));
    ASSERT_NE(gpu.last_writable_cb, nullptr);

    // The next host write finds the render channel full.
    gpu.unavailable_write_call = gpu.write_calls + 1;
    const uint32_t payload = 0xCAFEBABEU;
    mem_ptr->Put(kDataGpa + 64, payload);
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kDataGpa + 64, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);

    PipeCommandHeader hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.status, kPipeErrorAgain);
    EXPECT_EQ(hdr.consumed_size, 0U);
    EXPECT_TRUE(gpu.written_data.empty());

    // While blocked the pipe is not writable, so WAKE_ON_WRITE must not fire immediately.
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdPoll, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa).status & kPipePollOut, 0);

    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdWakeOnWrite, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_FALSE(irq);

    // The renderer drains the channel and reports writability.
    gpu.last_writable_cb();
    EXPECT_TRUE(irq);
    EXPECT_EQ(dev.Read(kPipeRegGetSignalled), 1U);
    EXPECT_FALSE(irq);
    const auto sig = mem_ptr->Get<SignalledPipeEntry>(kSignalBufGpa);
    EXPECT_EQ(sig.id, static_cast<uint32_t>(kPipeId));
    EXPECT_EQ(sig.flags, static_cast<uint32_t>(kPipeWakeWrite));

    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdPoll, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_NE(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa).status & kPipePollOut, 0);

    // The retried write goes through.
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kDataGpa + 64, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);
    hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.status, 4);
    EXPECT_EQ(hdr.consumed_size, 4U);
    ASSERT_EQ(gpu.written_data.size(), 4U);
    EXPECT_EQ(std::memcmp(gpu.written_data.data(), &payload, 4), 0);
}

// The lost-wakeup window: the renderer's one-shot writability report arrives
// *during* the write that fails with UNAVAILABLE. The pipe must not end up
// blocked afterwards (the report has already been spent and will not repeat),
// so the guest's next POLL / WAKE_ON_WRITE must see the pipe as writable.
TEST(GoldfishPipeTest, OpenglesWritableDuringFailedWriteIsNotLost) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;
    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint64_t kDataGpa = 0x3000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 61;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);
    ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(dev, mem_ptr, kOpenBufGpa, kCmdBufGpa, kMaxBufs,
                                                  kPipeId, "pipe:opengles", kDataGpa));
    ASSERT_NE(gpu.last_writable_cb, nullptr);

    gpu.unavailable_write_call = gpu.write_calls + 1;
    gpu.fire_writable_inside_unavailable_write = true;
    mem_ptr->Put(kDataGpa + 64, 0x11223344U);
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kDataGpa + 64, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa).status, kPipeErrorAgain);

    // The writability report was consumed during the write: the pipe must report
    // POLLOUT now, and WAKE_ON_WRITE must signal immediately rather than wait for
    // a second report that will never come.
    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdPoll, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_NE(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa).status & kPipePollOut, 0);

    mem_ptr->Put(kCmdBufGpa, PipeCommandHeader{.cmd = kPipeCmdWakeOnWrite, .id = kPipeId});
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_TRUE(irq);
    EXPECT_EQ(dev.Read(kPipeRegGetSignalled), 1U);
    const auto sig = mem_ptr->Get<SignalledPipeEntry>(kSignalBufGpa);
    EXPECT_EQ(sig.id, static_cast<uint32_t>(kPipeId));
    EXPECT_NE(sig.flags & kPipeWakeWrite, 0U);

    // And the retry succeeds.
    gpu.fire_writable_inside_unavailable_write = false;
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kDataGpa + 64, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa).status, 4);
}

// When the channel fills up part-way through a scatter-gather write, the guest must see the
// bytes already consumed (not AGAIN) so it retries only the remainder.
TEST(GoldfishPipeTest, OpenglesPartialWriteReportsConsumedBytesBeforeAgain) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    GoldfishPipeDevice dev(std::move(mem), [](bool /*high*/) {}, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kCmdBufGpa = 0x2000;
    constexpr uint64_t kDataGpa = 0x3000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeId = 61;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(dev, mem_ptr, kOpenBufGpa, kCmdBufGpa, kMaxBufs,
                                                  kPipeId, "pipe:opengles", kDataGpa));

    mem_ptr->Put<uint32_t>(kDataGpa + 64, 0x11111111U);
    mem_ptr->Put<uint32_t>(kDataGpa + 128, 0x22222222U);
    gpu.unavailable_write_call = gpu.write_calls + 2;  // first buffer ok, second AGAIN
    mem_ptr->Put(kCmdBufGpa,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeId, .buffers_count = 2});
    SetupBuffers(mem_ptr, kCmdBufGpa, kMaxBufs, {{kDataGpa + 64, 4}, {kDataGpa + 128, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeId);

    const PipeCommandHeader hdr = mem_ptr->Get<PipeCommandHeader>(kCmdBufGpa);
    EXPECT_EQ(hdr.status, 4);
    EXPECT_EQ(hdr.consumed_size, 4U);
    EXPECT_EQ(gpu.written_data, std::vector<uint8_t>({0x11, 0x11, 0x11, 0x11}));
}

class GoldfishPipeSnapshotBadInputTest : public ::testing::Test {
  protected:
    static constexpr uint64_t kOpenBufGpa = 0x1000;
    static constexpr uint64_t kSignalBufGpa = 0x1800;
    static constexpr uint64_t kCmdGpaGlProc = 0x2000;
    static constexpr uint64_t kCmdGpaRefcount = 0x2400;
    static constexpr uint64_t kCmdGpaOpengles = 0x2800;
    static constexpr uint64_t kDataGpa = 0x3000;
    static constexpr uint32_t kMaxBufs = 4;
    static constexpr int32_t kPipeGlProc = 11;
    static constexpr int32_t kPipeRefcount = 12;
    static constexpr int32_t kPipeOpengles = 13;
    static constexpr uint32_t kCbHandle = 0x778899AAU;

    void SetUp() override {
        auto mem = std::make_unique<FakeGuestMemory>(0x10000);
        mem_ = mem.get();
        dev_ = std::make_unique<GoldfishPipeDevice>(
                std::move(mem), [this](bool high) { irq_ = high; }, gpu_);
        dev_->Write(kPipeRegOpenBufferHigh, 0);
        dev_->Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
        dev_->Write(kPipeRegSignalBufferHigh, 0);
        dev_->Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
        dev_->Write(kPipeRegSignalBufferCount, 16);

        // GLProcessPipe with its confirmation int sent, reply unread.
        ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(*dev_, mem_, kOpenBufGpa, kCmdGpaGlProc,
                                                      kMaxBufs, kPipeGlProc, "pipe:GLProcessPipe",
                                                      kDataGpa));
        mem_->Put<int32_t>(kDataGpa + 64, 100);
        mem_->Put(kCmdGpaGlProc,
                  PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeGlProc, .buffers_count = 1});
        SetupBuffers(mem_, kCmdGpaGlProc, kMaxBufs, {{kDataGpa + 64, 4}}, 0);
        dev_->Write(kPipeRegCmd, kPipeGlProc);
        ASSERT_EQ(gpu_.created_puids.size(), 1U);

        // refcount pipe holding a ColorBuffer handle.
        ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(*dev_, mem_, kOpenBufGpa, kCmdGpaRefcount,
                                                      kMaxBufs, kPipeRefcount, "pipe:refcount",
                                                      kDataGpa + 128));
        mem_->Put<uint32_t>(kDataGpa + 192, kCbHandle);
        mem_->Put(kCmdGpaRefcount,
                  PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeRefcount, .buffers_count = 1});
        SetupBuffers(mem_, kCmdGpaRefcount, kMaxBufs, {{kDataGpa + 192, 4}}, 0);
        dev_->Write(kPipeRegCmd, kPipeRefcount);
        ASSERT_EQ(gpu_.opened_color_buffers.size(), 1U);

        // opengles pipe with unread host data queued.
        ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(*dev_, mem_, kOpenBufGpa, kCmdGpaOpengles,
                                                      kMaxBufs, kPipeOpengles, "pipe:opengles",
                                                      kDataGpa + 256));
        const uint32_t pending_reply = 0xFEEDFACEU;
        gpu_.last_read_cb(reinterpret_cast<const uint8_t*>(&pending_reply), sizeof(pending_reply));

        archive::DequeArchive saved;
        ASSERT_OK(dev_->Save("snap", saved));
        blob_.assign(saved.storage.begin(), saved.storage.end());
        ASSERT_OK(dev_->PreLoad("snap"));
    }

    TestGoldfishGpu gpu_;
    FakeGuestMemory* mem_ = nullptr;
    std::unique_ptr<GoldfishPipeDevice> dev_;
    std::vector<uint8_t> blob_;
    bool irq_ = false;
};

TEST_F(GoldfishPipeSnapshotBadInputTest, EveryTruncatedPrefixIsRejectedWithoutLeakingHandlers) {
    ASSERT_GT(blob_.size(), 8U);
    const size_t refs_before = gpu_.opened_color_buffers.size();
    for (size_t n = 0; n < blob_.size(); ++n) {
        EXPECT_THAT(LoadFromBytes(*dev_, "snap", blob_.data(), n),
                    StatusIs(absl::StatusCode::kDataLoss))
                << "prefix length " << n;
        // A refcount handler restored before a later failure must drop its ColorBuffer ref.
        EXPECT_EQ(gpu_.opened_color_buffers.size() - refs_before, gpu_.closed_color_buffers.size())
                << "prefix length " << n;
    }
    // The device is untouched by the failed loads and still accepts the full blob.
    EXPECT_TRUE(gpu_.bound_pipes.empty());
    ASSERT_OK(LoadFromBytes(*dev_, "snap", blob_));
    ASSERT_OK(dev_->PostLoad("snap"));
    EXPECT_EQ(gpu_.bound_pipes, std::vector<uint32_t>{gpu_.opened_pipes[0]});
    EXPECT_TRUE(gpu_.loaded_snapshots.empty());
}

TEST_F(GoldfishPipeSnapshotBadInputTest, HugePipeCountIsRejectedBeforeAllocating) {
    auto make_with_pipe_count = [](uint32_t pipe_count) {
        archive::DequeArchive w;
        w << uint32_t{0x47504950U} << uint32_t{1} << kOpenBufGpa << kSignalBufGpa << uint32_t{16}
          << kGoldfishPipeDeviceVersion << pipe_count;
        return w;
    };
    archive::DequeArchive huge = make_with_pipe_count(0xFFFFFFFFU);
    EXPECT_THAT(dev_->Load("snap", huge),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("pipe_count 4294967295 exceeds "
                                                                "65536")));

    // A plausible count that the remaining bytes cannot hold is rejected when the stream ends.
    archive::DequeArchive truncated = make_with_pipe_count(65535U);
    EXPECT_THAT(dev_->Load("snap", truncated),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("truncated")));
}

TEST_F(GoldfishPipeSnapshotBadInputTest, BadMagicVersionAndOversizedQueueAreRejected) {
    {
        archive::DequeArchive bad_magic;
        bad_magic << uint32_t{0x12345678U} << uint32_t{1};
        EXPECT_THAT(dev_->Load("snap", bad_magic),
                    StatusIs(absl::StatusCode::kDataLoss, HasSubstr("magic")));
    }
    {
        archive::DequeArchive bad_ver;
        bad_ver << uint32_t{0x47504950U} << uint32_t{2};
        EXPECT_THAT(dev_->Load("snap", bad_ver),
                    StatusIs(absl::StatusCode::kFailedPrecondition,
                             HasSubstr("version 2 is not supported")));
    }

    // An opengles queue larger than the whole snapshot budget is refused before allocation.
    archive::DequeArchive svc;
    svc << uint32_t{1} << uint32_t{0xFFFFFFFFU};
    archive::DequeArchive w;
    w << uint32_t{0x47504950U} << uint32_t{1} << uint64_t{0} << uint64_t{0} << uint32_t{0}
      << kGoldfishPipeDeviceVersion << uint32_t{1} << uint32_t{13} << kCmdGpaOpengles << kMaxBufs
      << uint32_t{0} << uint32_t{0} << uint8_t{1};
    const std::string hs = "pipe:opengles";
    w << static_cast<uint32_t>(hs.size());
    w.Write(hs.data(), hs.size());
    w << static_cast<uint32_t>(svc.Size());
    for (uint8_t b : svc.storage) {
        w << b;
    }
    EXPECT_THAT(dev_->Load("snap", w),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("queued_bytes 4294967295")));
}

TEST_F(GoldfishPipeSnapshotBadInputTest, OpenglesQueueLargerThanServiceStateIsRejectedBeforeAlloc) {
    // 64 MiB is within the queued-bytes budget but far beyond the service state
    // header; chunked reading hits EOF on the first 64 KiB read without
    // allocating the 64 MiB buffer or binding the pipe.
    archive::DequeArchive svc;
    svc << uint32_t{1} << uint32_t{64U << 20};
    archive::DequeArchive w;
    w << uint32_t{0x47504950U} << uint32_t{1} << uint64_t{0} << uint64_t{0} << uint32_t{0}
      << kGoldfishPipeDeviceVersion << uint32_t{1} << uint32_t{13} << kCmdGpaOpengles << kMaxBufs
      << uint32_t{0} << uint32_t{0} << uint8_t{1};
    const std::string hs = "pipe:opengles";
    w << static_cast<uint32_t>(hs.size());
    w.Write(hs.data(), hs.size());
    w << static_cast<uint32_t>(svc.Size());
    for (uint8_t b : svc.storage) {
        w << b;
    }
    EXPECT_THAT(dev_->Load("snap", w),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("opengles queued bytes")));
    EXPECT_TRUE(gpu_.bound_pipes.empty());
}

TEST_F(GoldfishPipeSnapshotBadInputTest, ServiceLengthMismatchClosesTheRestoredHandler) {
    // The service state is parsed successfully (4 queued bytes) but service_len
    // includes one extra trailing byte: the opengles channel bound during
    // restore must be closed again instead of leaking.
    archive::DequeArchive svc;
    svc << uint32_t{1} << uint32_t{4};
    const uint8_t payload[5] = {0xAB, 0xAB, 0xAB, 0xAB, 0xCD};
    svc.Write(payload, sizeof(payload));

    archive::DequeArchive w;
    w << uint32_t{0x47504950U} << uint32_t{1} << uint64_t{0} << uint64_t{0} << uint32_t{0}
      << kGoldfishPipeDeviceVersion << uint32_t{1} << uint32_t{13} << kCmdGpaOpengles << kMaxBufs
      << uint32_t{0} << uint32_t{0} << uint8_t{1};
    const std::string hs = "pipe:opengles";
    w << static_cast<uint32_t>(hs.size());
    w.Write(hs.data(), hs.size());
    w << static_cast<uint32_t>(svc.Size());
    for (uint8_t b : svc.storage) {
        w << b;
    }

    const size_t closed_before = gpu_.closed_pipes.size();
    EXPECT_THAT(dev_->Load("snap", w),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("consumed 6 of 7")));
    ASSERT_EQ(gpu_.bound_pipes.size(), 1U);
    EXPECT_EQ(gpu_.closed_pipes.size(), closed_before + 1);
}

TEST_F(GoldfishPipeSnapshotBadInputTest, DuplicateOpenglesStreamIdIsRejectedAndCleanedUp) {
    archive::DequeArchive w;
    w << uint32_t{0x47504950U} << uint32_t{1} << uint64_t{0} << uint64_t{0} << uint32_t{0}
      << kGoldfishPipeDeviceVersion << uint32_t{2};

    const std::string hs = "pipe:opengles";
    for (int i = 0; i < 2; ++i) {
        archive::DequeArchive svc;
        svc << uint32_t{1} << uint32_t{0};
        w << static_cast<uint32_t>(13 + i)
          << static_cast<uint64_t>(kCmdGpaOpengles + static_cast<uint64_t>(i) * 0x100) << kMaxBufs
          << uint32_t{0} << uint32_t{0} << uint8_t{1} << static_cast<uint32_t>(hs.size());
        w.Write(hs.data(), hs.size());
        w << static_cast<uint32_t>(svc.Size());
        for (uint8_t b : svc.storage) {
            w << b;
        }
    }

    const size_t closed_before = gpu_.closed_pipes.size();
    EXPECT_THAT(dev_->Load("snap", w),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("duplicate opengles stream_id 1")));
    ASSERT_EQ(gpu_.bound_pipes.size(), 1U);
    EXPECT_EQ(gpu_.closed_pipes.size(), closed_before + 1);
}

TEST(GoldfishPipeTest, SnapshotLoadAndPostLoadRaiseIrqForSignalledAndWantedFlags) {
    auto mem = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem_ptr = mem.get();
    TestGoldfishGpu gpu;
    bool irq = false;
    GoldfishPipeDevice dev(std::move(mem), [&irq](bool high) { irq = high; }, gpu);

    constexpr uint64_t kOpenBufGpa = 0x1000;
    constexpr uint64_t kSignalBufGpa = 0x1800;
    constexpr uint64_t kCmdGpaSignalled = 0x2000;
    constexpr uint64_t kCmdGpaWanted = 0x2400;
    constexpr uint64_t kDataGpa = 0x3000;
    constexpr uint32_t kMaxBufs = 4;
    constexpr int32_t kPipeSignalled = 21;
    constexpr int32_t kPipeWanted = 22;

    dev.Write(kPipeRegOpenBufferHigh, 0);
    dev.Write(kPipeRegOpenBuffer, static_cast<uint32_t>(kOpenBufGpa));
    dev.Write(kPipeRegSignalBufferHigh, 0);
    dev.Write(kPipeRegSignalBuffer, static_cast<uint32_t>(kSignalBufGpa));
    dev.Write(kPipeRegSignalBufferCount, 16);

    // Pipe 1: arm WAKE_ON_READ, push host data so signalled_flags == kPipeWakeRead.
    ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(dev, mem_ptr, kOpenBufGpa, kCmdGpaSignalled,
                                                  kMaxBufs, kPipeSignalled, "pipe:opengles",
                                                  kDataGpa));
    mem_ptr->Put(kCmdGpaSignalled,
                 PipeCommandHeader{.cmd = kPipeCmdWakeOnRead, .id = kPipeSignalled});
    dev.Write(kPipeRegCmd, kPipeSignalled);
    const uint32_t host_word = 0xABCD1234U;
    gpu.last_read_cb(reinterpret_cast<const uint8_t*>(&host_word), sizeof(host_word));
    EXPECT_TRUE(irq);

    // Pipe 2: block writes on an unavailable call, then arm WAKE_ON_WRITE so
    // wanted_flags == kPipeWakeWrite while signalled_flags == 0.
    ASSERT_NO_FATAL_FAILURE(OpenPipeWithHandshake(dev, mem_ptr, kOpenBufGpa, kCmdGpaWanted,
                                                  kMaxBufs, kPipeWanted, "pipe:opengles",
                                                  kDataGpa + 128));
    gpu.unavailable_write_call = gpu.write_calls + 1;
    mem_ptr->Put<uint32_t>(kDataGpa + 192, 0x55667788U);
    mem_ptr->Put(kCmdGpaWanted,
                 PipeCommandHeader{.cmd = kPipeCmdWrite, .id = kPipeWanted, .buffers_count = 1});
    SetupBuffers(mem_ptr, kCmdGpaWanted, kMaxBufs, {{kDataGpa + 192, 4}}, 0);
    dev.Write(kPipeRegCmd, kPipeWanted);
    EXPECT_EQ(mem_ptr->Get<PipeCommandHeader>(kCmdGpaWanted).status, kPipeErrorAgain);

    mem_ptr->Put(kCmdGpaWanted, PipeCommandHeader{.cmd = kPipeCmdWakeOnWrite, .id = kPipeWanted});
    dev.Write(kPipeRegCmd, kPipeWanted);

    archive::DequeArchive snap;
    ASSERT_OK(dev.Save("snap", snap));

    // Restore into a second device: Load() must raise IRQ for kPipeSignalled,
    // and PostLoad() must raise IRQ when kPipeWanted's wanted_flags matches OnPoll().
    auto mem2 = std::make_unique<FakeGuestMemory>(0x10000);
    FakeGuestMemory* mem2_ptr = mem2.get();
    bool irq2 = false;
    GoldfishPipeDevice dev2(std::move(mem2), [&irq2](bool high) { irq2 = high; }, gpu);
    ASSERT_OK(dev2.PreLoad("snap"));
    ASSERT_OK(dev2.Load("snap", snap));
    EXPECT_TRUE(irq2);

    // Drain kPipeSignalled so signalled_flags becomes 0 and IRQ lowers before PostLoad.
    EXPECT_EQ(dev2.Read(kPipeRegGetSignalled), 1U);
    EXPECT_FALSE(irq2);
    const auto sig1 = mem2_ptr->Get<SignalledPipeEntry>(kSignalBufGpa);
    EXPECT_EQ(sig1.id, static_cast<uint32_t>(kPipeSignalled));
    EXPECT_EQ(sig1.flags, static_cast<uint32_t>(kPipeWakeRead));

    // PostLoad re-evaluates wanted_flags against OnPoll() (which reports kPipePollOut)
    // and raises IRQ for kPipeWanted.
    ASSERT_OK(dev2.PostLoad("snap"));
    EXPECT_TRUE(irq2);
    EXPECT_EQ(dev2.Read(kPipeRegGetSignalled), 1U);
    const auto sig2 = mem2_ptr->Get<SignalledPipeEntry>(kSignalBufGpa);
    EXPECT_EQ(sig2.id, static_cast<uint32_t>(kPipeWanted));
    EXPECT_EQ(sig2.flags, static_cast<uint32_t>(kPipeWakeWrite));
}

}  // namespace
}  // namespace goldfish::legacy
