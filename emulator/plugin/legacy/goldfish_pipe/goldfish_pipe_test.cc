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

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "goldfish/legacy/common/fake_guest_memory.h"
#include "goldfish/legacy/goldfish_gpu/fake_goldfish_gpu.h"

namespace goldfish::legacy {
namespace {

class TestGoldfishGpu : public FakeGoldfishGpu {
  public:
    uint64_t CreateProcessContext() override {
        created_puids.push_back(next_puid);
        return next_puid++;
    }

    void DestroyProcessContext(uint64_t puid) override { destroyed_puids.push_back(puid); }

    void ColorBufferLastRef(uint32_t handle) override { closed_color_buffers.push_back(handle); }

    absl::StatusOr<uint32_t> OpenOpenglesPipe(OpenglesReadCallback on_host_data) override {
        last_read_cb = std::move(on_host_data);
        uint32_t id = next_pipe_id++;
        opened_pipes.push_back(id);
        return id;
    }

    absl::Status WriteOpenglesPipe(uint32_t /*id*/, const uint8_t* data, size_t size) override {
        written_data.insert(written_data.end(), data, data + size);
        if (sync_echo_on_write && last_read_cb != nullptr) {
            last_read_cb(data, size);
        }
        return absl::OkStatus();
    }

    void CloseOpenglesPipe(uint32_t id) override { closed_pipes.push_back(id); }

    OpenglesReadCallback last_read_cb;
    std::vector<uint64_t> created_puids;
    std::vector<uint64_t> destroyed_puids;
    std::vector<uint32_t> closed_color_buffers;
    std::vector<uint32_t> opened_pipes;
    std::vector<uint32_t> closed_pipes;
    std::vector<uint8_t> written_data;
    uint64_t next_puid = 0x123456789ABCDEF0ULL;
    uint32_t next_pipe_id = 1;
    bool sync_echo_on_write = false;
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

    const uint32_t kColorBufferHandle = 0x42420007U;
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

}  // namespace
}  // namespace goldfish::legacy
