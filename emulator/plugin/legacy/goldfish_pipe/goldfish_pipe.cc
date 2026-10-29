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

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"

#include "goldfish/legacy/common/adapters.h"
#include "goldfish/socket_buffer.h"

namespace goldfish::legacy {
namespace {

// ============================================================================
// Pipe service: "pipe:GLProcessPipe"
// ============================================================================
class GlProcessPipeService final : public IPipeService {
  public:
    explicit GlProcessPipeService(IGoldfishGpu& gpu) : gpu_(gpu) {}

    int32_t OnWrite(const uint8_t* data, size_t size, size_t* consumed) override {
        *consumed = 0;
        if (failed_) {
            return kPipeErrorIo;
        }
        if (size == 0) {
            return kPipeStatusOk;
        }
        if (confirm_bytes_ < sizeof(confirm_buf_)) {
            const size_t take = std::min<size_t>(size, sizeof(confirm_buf_) - confirm_bytes_);
            std::memcpy(confirm_buf_ + confirm_bytes_, data, take);
            confirm_bytes_ += take;
            *consumed = size;

            if (confirm_bytes_ == sizeof(confirm_buf_)) {
                int32_t confirm_val = 0;
                std::memcpy(&confirm_val, confirm_buf_, sizeof(confirm_val));
                if (confirm_val != kConfirmInt) {
                    LOG(WARNING) << "GLProcessPipe: unexpected confirmation int " << confirm_val
                                 << " (expected " << kConfirmInt << ")";
                    failed_ = true;
                    *consumed = 0;
                    return kPipeErrorIo;
                }
                initialized_ = true;
                puid_ = gpu_.CreateProcessContext();
                const auto* bytes = reinterpret_cast<const uint8_t*>(&puid_);
                reply_.assign(bytes, bytes + sizeof(puid_));
            }
            return kPipeStatusOk;
        }
        *consumed = size;
        return kPipeStatusOk;
    }

    int32_t OnRead(uint8_t* data, size_t size, size_t* produced) override {
        *produced = 0;
        if (failed_) {
            return kPipeErrorIo;
        }
        if (reply_pos_ >= reply_.size()) {
            return kPipeErrorAgain;
        }
        const size_t avail = reply_.size() - reply_pos_;
        const size_t n = std::min(size, avail);
        std::memcpy(data, reply_.data() + reply_pos_, n);
        reply_pos_ += n;
        *produced = n;
        return kPipeStatusOk;
    }

    uint32_t OnPoll() const override {
        if (failed_) {
            return kPipePollHup;
        }
        uint32_t flags = kPipePollOut;
        if (reply_pos_ < reply_.size()) {
            flags |= kPipePollIn;
        }
        return flags;
    }

    void OnClose() override {
        if (initialized_ && puid_ != 0) {
            gpu_.DestroyProcessContext(puid_);
            puid_ = 0;
        }
    }

  private:
    static constexpr int32_t kConfirmInt = 100;

    IGoldfishGpu& gpu_;
    std::vector<uint8_t> reply_;
    uint64_t puid_ = 0;
    size_t confirm_bytes_ = 0;
    size_t reply_pos_ = 0;
    uint8_t confirm_buf_[sizeof(int32_t)]{};
    bool initialized_ = false;
    bool failed_ = false;
};

// ============================================================================
// Pipe service: "pipe:refcount"
// ============================================================================
class RefcountPipeService final : public IPipeService {
  public:
    explicit RefcountPipeService(IGoldfishGpu& gpu) : gpu_(gpu) {}

    int32_t OnWrite(const uint8_t* data, size_t size, size_t* consumed) override {
        if (buf_.size() < sizeof(uint32_t)) {
            const size_t take = std::min<size_t>(size, sizeof(uint32_t) - buf_.size());
            buf_.insert(buf_.end(), data, data + take);
            if (buf_.size() == sizeof(uint32_t)) {
                std::memcpy(&handle_, buf_.data(), sizeof(uint32_t));
                has_handle_ = true;
            }
        }
        *consumed = size;
        return kPipeStatusOk;
    }

    int32_t OnRead(uint8_t* /*data*/, size_t /*size*/, size_t* produced) override {
        *produced = 0;
        return kPipeErrorAgain;
    }

    uint32_t OnPoll() const override { return kPipePollOut; }

    void OnClose() override {
        if (has_handle_) {
            gpu_.ColorBufferLastRef(handle_);
            has_handle_ = false;
        }
    }

  private:
    IGoldfishGpu& gpu_;
    std::vector<uint8_t> buf_;
    uint32_t handle_ = 0;
    bool has_handle_ = false;
};

// ============================================================================
// Pipe service: "pipe:opengles"
// ============================================================================
class OpenglesPipeService final : public IPipeService {
  public:
    static std::unique_ptr<OpenglesPipeService> Create(IGoldfishGpu& gpu,
                                                       PipeWakeCallback signal_wake) {
        auto state = std::make_shared<SharedState>(std::move(signal_wake));
        std::weak_ptr<SharedState> weak_state = state;

        absl::StatusOr<uint32_t> stream_id =
                gpu.OpenOpenglesPipe([weak_state](const uint8_t* data, size_t size) {
                    if (data == nullptr || size == 0) {
                        return;
                    }
                    auto locked_state = weak_state.lock();
                    if (locked_state == nullptr) {
                        return;
                    }
                    PipeWakeCallback wake_cb;
                    {
                        absl::MutexLock lock(&locked_state->mu);
                        if (locked_state->closed) {
                            return;
                        }
                        (void)locked_state->read_queue.Append(data, size);
                        wake_cb = locked_state->signal_wake;
                    }
                    if (wake_cb != nullptr) {
                        wake_cb(kPipeWakeRead);
                    }
                });
        if (!stream_id.ok()) {
            LOG(ERROR) << "goldfish_pipe: OpenOpenglesPipe failed: " << stream_id.status();
            return nullptr;
        }
        return std::unique_ptr<OpenglesPipeService>(
                new OpenglesPipeService(gpu, *stream_id, std::move(state)));
    }

    int32_t OnWrite(const uint8_t* data, size_t size, size_t* consumed) override {
        if (size == 0) {
            *consumed = 0;
            return kPipeStatusOk;
        }
        auto status = gpu_.WriteOpenglesPipe(stream_id_, data, size);
        if (!status.ok()) {
            LOG(ERROR) << "goldfish_pipe: WriteOpenglesPipe(stream_id=" << stream_id_
                       << ", size=" << size << ") failed: " << status;
            *consumed = 0;
            return kPipeErrorIo;
        }
        *consumed = size;
        return kPipeStatusOk;
    }

    int32_t OnRead(uint8_t* data, size_t size, size_t* produced) override {
        absl::MutexLock lock(&state_->mu);
        if (state_->closed) {
            *produced = 0;
            return kPipeErrorIo;
        }
        if (state_->read_queue.Size() == 0) {
            *produced = 0;
            return kPipeErrorAgain;
        }
        size_t total = 0;
        while (total < size && state_->read_queue.Size() > 0) {
            const auto [chunk, chunk_size] = state_->read_queue.Peek();
            const size_t n = std::min(size - total, chunk_size);
            std::memcpy(data + total, chunk, n);
            (void)state_->read_queue.Consume(n);
            total += n;
        }
        *produced = total;
        return kPipeStatusOk;
    }

    uint32_t OnPoll() const override {
        absl::MutexLock lock(&state_->mu);
        if (state_->closed) {
            return kPipePollHup;
        }
        uint32_t flags = kPipePollOut;
        if (state_->read_queue.Size() > 0) {
            flags |= kPipePollIn;
        }
        return flags;
    }

    void OnClose() override {
        {
            absl::MutexLock lock(&state_->mu);
            if (state_->closed) {
                return;
            }
            state_->closed = true;
            state_->signal_wake = nullptr;
            state_->read_queue.Clear(true);
        }
        gpu_.CloseOpenglesPipe(stream_id_);
    }

  private:
    struct SharedState {
        explicit SharedState(PipeWakeCallback cb) : signal_wake(std::move(cb)) {}

        SocketBuffer read_queue ABSL_GUARDED_BY(mu);
        PipeWakeCallback signal_wake ABSL_GUARDED_BY(mu);
        bool closed ABSL_GUARDED_BY(mu) = false;
        mutable absl::Mutex mu;
    };

    OpenglesPipeService(IGoldfishGpu& gpu, uint32_t stream_id, std::shared_ptr<SharedState> state)
            : state_(std::move(state)), gpu_(gpu), stream_id_(stream_id) {}

    const std::shared_ptr<SharedState> state_;
    IGoldfishGpu& gpu_;
    const uint32_t stream_id_;
};

}  // namespace

// ============================================================================
// GoldfishPipeDevice
// ============================================================================

GoldfishPipeDevice::GoldfishPipeDevice(std::unique_ptr<IGuestMemory> memory, SetIrqCallback set_irq,
                                       IGoldfishGpu& gpu)
        : memory_(std::move(memory))
        , waker_state_(std::make_shared<WakerState>())
        , set_irq_(std::move(set_irq))
        , gpu_(gpu) {
    DCHECK(memory_ != nullptr);
    DCHECK(set_irq_ != nullptr);
    absl::MutexLock lock(&waker_state_->mu);
    waker_state_->dev = this;
}

GoldfishPipeDevice::~GoldfishPipeDevice() {
    {
        absl::MutexLock lock(&waker_state_->mu);
        waker_state_->dev = nullptr;
    }
    std::map<int32_t, PipeSlot> pipes;
    {
        absl::MutexLock lock(&mu_);
        pipes = std::move(pipes_);
    }
    for (auto& [id, slot] : pipes) {
        if (slot.handler != nullptr) {
            slot.handler->OnClose();
        }
    }
}

void GoldfishPipeDevice::SignalWake(int32_t pipe_id, uint32_t wake_flags) ABSL_LOCKS_EXCLUDED(mu_) {
    bool raise_irq = false;
    {
        absl::MutexLock lock(&mu_);
        auto it = pipes_.find(pipe_id);
        if (it == pipes_.end()) {
            return;
        }
        const uint32_t matched = it->second.wanted_flags & wake_flags;
        if (matched != 0) {
            it->second.signalled_flags |= matched;
            it->second.wanted_flags &= ~matched;
            raise_irq = true;
        }
    }
    if (raise_irq) {
        set_irq_(true);
    }
}

void GoldfishPipeDevice::SignalWakeWithGeneration(int32_t pipe_id, uint64_t generation,
                                                  uint32_t wake_flags) ABSL_LOCKS_EXCLUDED(mu_) {
    bool raise_irq = false;
    {
        absl::MutexLock lock(&mu_);
        auto it = pipes_.find(pipe_id);
        if (it == pipes_.end() || it->second.generation != generation) {
            return;
        }
        const uint32_t matched = it->second.wanted_flags & wake_flags;
        if (matched != 0) {
            it->second.signalled_flags |= matched;
            it->second.wanted_flags &= ~matched;
            raise_irq = true;
        }
    }
    if (raise_irq) {
        set_irq_(true);
    }
}

uint32_t GoldfishPipeDevice::Read(uint32_t offset) ABSL_LOCKS_EXCLUDED(mu_) {
    switch (offset) {
    case kPipeRegVersion: {
        std::map<int32_t, PipeSlot> pipes;
        uint32_t version = kGoldfishPipeDeviceVersion;
        {
            absl::MutexLock lock(&mu_);
            pipes = std::move(pipes_);
            version = version_;
        }
        for (auto& [id, slot] : pipes) {
            if (slot.handler != nullptr) {
                slot.handler->OnClose();
            }
        }
        set_irq_(false);
        return version;
    }
    case kPipeRegGetSignalled: {
        bool lower_irq = false;
        uint32_t count = 0;
        {
            absl::MutexLock lock(&mu_);
            count = DrainSignalledPipesLocked(&lower_irq);
        }
        if (lower_irq) {
            set_irq_(false);
        }
        return count;
    }
    default:
        LOG(WARNING) << "goldfish_pipe: unexpected read at 0x" << std::hex << offset;
        return 0;
    }
}

void GoldfishPipeDevice::Write(uint32_t offset, uint32_t value) ABSL_LOCKS_EXCLUDED(mu_) {
    if (offset == kPipeRegCmd) {
        bool raise_irq = false;
        ExecutePipeCommand(static_cast<int32_t>(value), &raise_irq);
        if (raise_irq) {
            set_irq_(true);
        }
        return;
    }

    absl::MutexLock lock(&mu_);
    switch (offset) {
    case kPipeRegSignalBufferHigh:
        signal_buffer_addr_ = (signal_buffer_addr_ & 0x00000000FFFFFFFFULL) |
                              (static_cast<uint64_t>(value) << 32);
        break;
    case kPipeRegSignalBuffer:
        signal_buffer_addr_ =
                (signal_buffer_addr_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    case kPipeRegSignalBufferCount:
        signal_buffer_size_ = value;
        break;
    case kPipeRegOpenBufferHigh:
        open_buffer_addr_ =
                (open_buffer_addr_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case kPipeRegOpenBuffer:
        open_buffer_addr_ =
                (open_buffer_addr_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    case kPipeRegVersion:
        version_ = kGoldfishPipeDeviceVersion;
        break;
    default:
        LOG(WARNING) << "goldfish_pipe: unexpected write at 0x" << std::hex << offset;
        break;
    }
}

bool GoldfishPipeDevice::HasSignalledPipesLocked() const {
    for (const auto& [id, slot] : pipes_) {
        if (slot.signalled_flags != 0) {
            return true;
        }
    }
    return false;
}

uint32_t GoldfishPipeDevice::DrainSignalledPipesLocked(bool* out_lower_irq) {
    *out_lower_irq = false;
    if (signal_buffer_addr_ == 0 || signal_buffer_size_ == 0) {
        *out_lower_irq = true;
        return 0;
    }

    uint32_t written = 0;
    bool more_remaining = false;
    for (auto& [id, slot] : pipes_) {
        if (slot.signalled_flags == 0) {
            continue;
        }
        if (written >= signal_buffer_size_) {
            more_remaining = true;
            break;
        }
        SignalledPipeEntry entry{
            .id = static_cast<uint32_t>(id),
            .flags = slot.signalled_flags,
        };
        const uint64_t dst = signal_buffer_addr_ + written * sizeof(SignalledPipeEntry);
        if (!memory_->Write(dst, &entry, sizeof(entry))) {
            break;
        }
        slot.signalled_flags = 0;
        ++written;
    }
    if (!more_remaining) {
        *out_lower_irq = true;
    }
    return written;
}

void GoldfishPipeDevice::ExecutePipeCommand(int32_t pipe_id, bool* out_raise_irq)
        ABSL_LOCKS_EXCLUDED(mu_) {
    *out_raise_irq = false;

    uint64_t generation = 0;
    uint64_t cmd_gpa = 0;
    PipeCommandHeader hdr{};
    std::shared_ptr<IPipeService> handler;
    std::shared_ptr<IPipeService> closed_handler;
    std::vector<GuestIoBuffer> bufs;
    uint32_t read_index = 0;

    {
        absl::MutexLock lock(&mu_);
        auto it = pipes_.find(pipe_id);
        if (it == pipes_.end()) {
            if (open_buffer_addr_ == 0) {
                LOG(WARNING) << "goldfish_pipe: command on unknown pipe " << pipe_id;
                return;
            }
            OpenCommandBuffer open_buf{};
            if (!memory_->Read(open_buffer_addr_, &open_buf, sizeof(open_buf)) ||
                open_buf.command_buffer_ptr == 0 || open_buf.rw_params_max_count == 0 ||
                open_buf.rw_params_max_count > kMaxPipeBuffersCount) {
                LOG(WARNING) << "goldfish_pipe: failed to read valid OpenCommandBuffer for pipe "
                             << pipe_id << " (open_buffer_addr=0x" << std::hex << open_buffer_addr_
                             << ", command_buffer_ptr=0x" << open_buf.command_buffer_ptr << std::dec
                             << ", rw_params_max_count=" << open_buf.rw_params_max_count
                             << ", limit=" << kMaxPipeBuffersCount << ")";
                if (open_buf.command_buffer_ptr != 0) {
                    PipeCommandHeader err_hdr{};
                    if (memory_->Read(open_buf.command_buffer_ptr, &err_hdr, sizeof(err_hdr))) {
                        err_hdr.status = kPipeErrorInval;
                        memory_->Write(open_buf.command_buffer_ptr, &err_hdr, sizeof(err_hdr));
                    }
                }
                return;
            }
            if (!memory_->Read(open_buf.command_buffer_ptr, &hdr, sizeof(hdr))) {
                LOG(WARNING) << "goldfish_pipe: failed to read PipeCommandHeader at 0x" << std::hex
                             << open_buf.command_buffer_ptr;
                return;
            }
            if (hdr.cmd != kPipeCmdOpen || hdr.id != pipe_id) {
                hdr.status = kPipeErrorInval;
                memory_->Write(open_buf.command_buffer_ptr, &hdr, sizeof(hdr));
                return;
            }
            PipeSlot slot;
            slot.id = pipe_id;
            slot.generation = next_generation_++;
            slot.command_buffer_gpa = open_buf.command_buffer_ptr;
            slot.rw_params_max_count = open_buf.rw_params_max_count;
            pipes_.emplace(pipe_id, std::move(slot));
            hdr.status = kPipeStatusOk;
            hdr.consumed_size = 0;
            memory_->Write(open_buf.command_buffer_ptr, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            return;
        }

        PipeSlot& slot = it->second;
        generation = slot.generation;
        cmd_gpa = slot.command_buffer_gpa;
        handler = slot.handler;

        if (!memory_->Read(cmd_gpa, &hdr, sizeof(hdr))) {
            LOG(WARNING) << "goldfish_pipe: failed to read PipeCommandHeader at 0x" << std::hex
                         << cmd_gpa;
            return;
        }
        if (hdr.id != pipe_id) {
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            return;
        }

        switch (hdr.cmd) {
        case kPipeCmdOpen:
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            return;
        case kPipeCmdClose:
            closed_handler = std::move(slot.handler);
            hdr.status = kPipeStatusOk;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            pipes_.erase(it);
            *out_raise_irq = HasSignalledPipesLocked();
            break;
        case kPipeCmdPoll:
        case kPipeCmdWakeOnDoneIo:
            slot.wanted_flags |= (kPipeWakeRead | kPipeWakeWrite);
            break;
        case kPipeCmdWakeOnWrite:
            slot.wanted_flags |= kPipeWakeWrite;
            break;
        case kPipeCmdWakeOnRead:
            slot.wanted_flags |= kPipeWakeRead;
            break;
        case kPipeCmdWrite:
        case kPipeCmdRead:
        case kPipeCmdCall:
            if (!ReadIoBuffersLocked(slot, hdr, &bufs, &read_index) ||
                (hdr.cmd == kPipeCmdCall && read_index > bufs.size())) {
                hdr.status = kPipeErrorInval;
                hdr.consumed_size = 0;
                memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
                *out_raise_irq = HasSignalledPipesLocked();
                return;
            }
            break;
        case kPipeCmdDmaHostMap:
        case kPipeCmdDmaHostUnmap:
            // DMA is not used in API 36.
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            return;
        default:
            LOG(WARNING) << "goldfish_pipe: unknown command " << hdr.cmd;
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            return;
        }
    }

    if (hdr.cmd == kPipeCmdClose) {
        if (closed_handler != nullptr) {
            closed_handler->OnClose();
        }
        return;
    }

    // Execute service/GPU operations outside mu_ to prevent lock inversion or re-entrant deadlock
    // when IGoldfishGpu callbacks invoke SignalWakeWithGeneration.
    switch (hdr.cmd) {
    case kPipeCmdPoll:
    case kPipeCmdWakeOnWrite:
    case kPipeCmdWakeOnRead:
    case kPipeCmdWakeOnDoneIo: {
        const uint32_t mask = (handler != nullptr) ? handler->OnPoll() : kPipePollOut;
        absl::MutexLock lock(&mu_);
        auto it = pipes_.find(pipe_id);
        if (it != pipes_.end() && it->second.generation == generation) {
            PipeSlot& slot = it->second;
            switch (hdr.cmd) {
            case kPipeCmdPoll:
                if ((mask & kPipePollHup) != 0) {
                    slot.wanted_flags &= ~(kPipeWakeRead | kPipeWakeWrite);
                } else {
                    if ((mask & kPipePollIn) != 0) {
                        slot.wanted_flags &= ~kPipeWakeRead;
                    }
                    if ((mask & kPipePollOut) != 0) {
                        slot.wanted_flags &= ~kPipeWakeWrite;
                    }
                }
                hdr.status = static_cast<int32_t>(mask);
                break;
            case kPipeCmdWakeOnWrite:
                if (handler == nullptr || (mask & (kPipePollOut | kPipePollHup)) != 0) {
                    slot.signalled_flags |= kPipeWakeWrite;
                    slot.wanted_flags &= ~kPipeWakeWrite;
                }
                hdr.status = kPipeStatusOk;
                break;
            case kPipeCmdWakeOnRead:
                if (handler != nullptr && (mask & (kPipePollIn | kPipePollHup)) != 0) {
                    slot.signalled_flags |= kPipeWakeRead;
                    slot.wanted_flags &= ~kPipeWakeRead;
                }
                hdr.status = kPipeStatusOk;
                break;
            case kPipeCmdWakeOnDoneIo:
                if ((mask & (kPipePollIn | kPipePollHup)) != 0) {
                    slot.signalled_flags |= kPipeWakeRead;
                    slot.wanted_flags &= ~kPipeWakeRead;
                }
                if ((mask & (kPipePollOut | kPipePollHup)) != 0) {
                    slot.signalled_flags |= kPipeWakeWrite;
                    slot.wanted_flags &= ~kPipeWakeWrite;
                }
                hdr.status = kPipeStatusOk;
                break;
            default:
                break;
            }
        }
        memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
        *out_raise_irq = HasSignalledPipesLocked();
        return;
    }
    case kPipeCmdWrite: {
        uint32_t consumed = 0;
        const int32_t status = DoWrite(pipe_id, generation, bufs, &consumed);
        hdr.consumed_size = consumed;
        hdr.status = (consumed > 0) ? static_cast<int32_t>(consumed) : status;
        break;
    }
    case kPipeCmdRead: {
        uint32_t produced = 0;
        const int32_t status = DoRead(pipe_id, generation, bufs, &produced);
        hdr.consumed_size = produced;
        hdr.status = (produced > 0) ? static_cast<int32_t>(produced) : status;
        break;
    }
    case kPipeCmdCall: {
        std::vector<GuestIoBuffer> write_bufs(bufs.begin(), bufs.begin() + read_index);
        std::vector<GuestIoBuffer> read_bufs(bufs.begin() + read_index, bufs.end());

        uint32_t written = 0;
        int32_t last_error = kPipeStatusOk;
        bool write_complete = true;
        if (!write_bufs.empty()) {
            uint64_t expected_write = 0;
            for (const auto& b : write_bufs) {
                expected_write += b.size;
            }
            const int32_t write_st = DoWrite(pipe_id, generation, write_bufs, &written);
            if (write_st < 0) {
                last_error = write_st;
            }
            if (written < expected_write || write_st < 0) {
                write_complete = false;
            }
        }
        uint32_t read_bytes = 0;
        if (write_complete && !read_bufs.empty()) {
            const int32_t read_st = DoRead(pipe_id, generation, read_bufs, &read_bytes);
            if (read_st < 0) {
                last_error = read_st;
            }
        }
        const uint32_t total_consumed = written + read_bytes;
        hdr.consumed_size = total_consumed;
        hdr.status = (total_consumed > 0) ? kPipeStatusOk : last_error;
        break;
    }
    default:
        break;
    }

    memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
    {
        absl::MutexLock lock(&mu_);
        *out_raise_irq = HasSignalledPipesLocked();
    }
}

bool GoldfishPipeDevice::ReadIoBuffersLocked(const PipeSlot& slot, const PipeCommandHeader& hdr,
                                             std::vector<GuestIoBuffer>* out,
                                             uint32_t* out_read_index) {
    const uint32_t max_cnt = slot.rw_params_max_count;
    const uint32_t cnt = hdr.buffers_count;
    if (cnt == 0 || cnt > max_cnt || max_cnt > kMaxPipeBuffersCount) {
        return false;
    }
    const uint64_t ptrs_gpa = slot.command_buffer_gpa + sizeof(PipeCommandHeader);
    const uint64_t sizes_gpa = ptrs_gpa + static_cast<uint64_t>(max_cnt) * sizeof(uint64_t);
    const uint64_t read_index_gpa = sizes_gpa + static_cast<uint64_t>(max_cnt) * sizeof(uint32_t);

    std::vector<uint64_t> ptrs(cnt);
    std::vector<uint32_t> sizes(cnt);
    if (!memory_->Read(ptrs_gpa, ptrs.data(), cnt * sizeof(uint64_t)) ||
        !memory_->Read(sizes_gpa, sizes.data(), cnt * sizeof(uint32_t))) {
        return false;
    }

    uint32_t read_idx = 0;
    if (hdr.cmd == kPipeCmdCall) {
        if (!memory_->Read(read_index_gpa, &read_idx, sizeof(read_idx))) {
            return false;
        }
    }
    *out_read_index = read_idx;

    out->resize(cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        if (sizes[i] > kMaxPipeBufferSize) {
            LOG(WARNING) << "goldfish_pipe: buffer size " << sizes[i] << " exceeds limit ("
                         << kMaxPipeBufferSize << ")";
            return false;
        }
        (*out)[i] = GuestIoBuffer{.gpa = ptrs[i], .size = sizes[i]};
    }
    return true;
}

int32_t GoldfishPipeDevice::DoWrite(int32_t pipe_id, uint64_t generation,
                                    const std::vector<GuestIoBuffer>& bufs,
                                    uint32_t* total_consumed) ABSL_LOCKS_EXCLUDED(mu_) {
    *total_consumed = 0;

    bool handshake_done = false;
    std::string handshake_buf;
    std::shared_ptr<IPipeService> handler;
    {
        absl::MutexLock lock(&mu_);
        auto it = pipes_.find(pipe_id);
        if (it == pipes_.end() || it->second.generation != generation) {
            return kPipeErrorInval;
        }
        handshake_done = it->second.handshake_done;
        handshake_buf = it->second.handshake_buf;
        handler = it->second.handler;
    }

    for (const auto& b : bufs) {
        if (b.size == 0) {
            continue;
        }
        std::vector<uint8_t> data(b.size);
        if (!memory_->Read(b.gpa, data.data(), b.size)) {
            return (*total_consumed > 0) ? kPipeStatusOk : kPipeErrorIo;
        }

        size_t offset = 0;
        if (!handshake_done) {
            while (offset < data.size()) {
                const char c = static_cast<char>(data[offset++]);
                if (c == '\0') {
                    handshake_done = true;
                    break;
                }
                if (handshake_buf.size() >= kMaxHandshakeLength) {
                    absl::MutexLock lock(&mu_);
                    auto it = pipes_.find(pipe_id);
                    if (it != pipes_.end() && it->second.generation == generation) {
                        it->second.handshake_buf.clear();
                    }
                    return kPipeErrorInval;
                }
                handshake_buf.push_back(c);
            }
            if (!handshake_done) {
                absl::MutexLock lock(&mu_);
                auto it = pipes_.find(pipe_id);
                if (it == pipes_.end() || it->second.generation != generation) {
                    return kPipeErrorIo;
                }
                it->second.handshake_buf = handshake_buf;
                *total_consumed += static_cast<uint32_t>(offset);
                continue;
            }
            handler = CreateServiceHandler(pipe_id, generation, handshake_buf);
            if (handler == nullptr) {
                absl::MutexLock lock(&mu_);
                auto it = pipes_.find(pipe_id);
                if (it != pipes_.end() && it->second.generation == generation) {
                    it->second.handshake_done = false;
                    it->second.handshake_buf.clear();
                }
                return kPipeErrorInval;
            }
            std::shared_ptr<IPipeService> stale_handler;
            {
                absl::MutexLock lock(&mu_);
                auto it = pipes_.find(pipe_id);
                if (it == pipes_.end() || it->second.generation != generation) {
                    stale_handler = std::move(handler);
                } else {
                    it->second.handshake_done = true;
                    it->second.handshake_buf = handshake_buf;
                    it->second.handler = handler;
                }
            }
            if (stale_handler != nullptr) {
                stale_handler->OnClose();
                return kPipeErrorIo;
            }
            *total_consumed += static_cast<uint32_t>(offset);
            if (offset == data.size()) {
                continue;
            }
        }

        size_t consumed = 0;
        const int32_t st = handler->OnWrite(data.data() + offset, data.size() - offset, &consumed);
        *total_consumed += static_cast<uint32_t>(consumed);
        if (st != kPipeStatusOk) {
            return (*total_consumed > 0) ? kPipeStatusOk : st;
        }
        if (consumed < data.size() - offset) {
            break;
        }
    }
    return kPipeStatusOk;
}

int32_t GoldfishPipeDevice::DoRead(int32_t pipe_id, uint64_t generation,
                                   const std::vector<GuestIoBuffer>& bufs, uint32_t* total_produced)
        ABSL_LOCKS_EXCLUDED(mu_) {
    *total_produced = 0;
    std::shared_ptr<IPipeService> handler;
    {
        absl::MutexLock lock(&mu_);
        auto it = pipes_.find(pipe_id);
        if (it == pipes_.end() || it->second.generation != generation ||
            !it->second.handshake_done || it->second.handler == nullptr) {
            return kPipeErrorInval;
        }
        handler = it->second.handler;
    }
    for (const auto& b : bufs) {
        if (b.size == 0) {
            continue;
        }
        std::vector<uint8_t> data(b.size);
        size_t produced = 0;
        const int32_t st = handler->OnRead(data.data(), b.size, &produced);
        if (produced > 0) {
            if (!memory_->Write(b.gpa, data.data(), produced)) {
                return (*total_produced > 0) ? kPipeStatusOk : kPipeErrorIo;
            }
            *total_produced += static_cast<uint32_t>(produced);
        }
        if (st != kPipeStatusOk) {
            return (*total_produced > 0) ? kPipeStatusOk : st;
        }
        if (produced < b.size) {
            break;
        }
    }
    return (*total_produced > 0) ? kPipeStatusOk : kPipeErrorAgain;
}

std::shared_ptr<IPipeService> GoldfishPipeDevice::CreateServiceHandler(
        int32_t pipe_id, uint64_t generation, std::string_view service_name)
        ABSL_LOCKS_EXCLUDED(mu_) {
    constexpr std::string_view kPipePrefix = "pipe:";
    if (service_name.substr(0, kPipePrefix.size()) != kPipePrefix) {
        LOG(WARNING) << "goldfish_pipe: invalid service handshake '" << service_name << "'";
        return nullptr;
    }
    std::string_view service = service_name.substr(kPipePrefix.size());

    LOG(INFO) << "goldfish_pipe: opening service '" << service << "' on pipe " << pipe_id;

    if (service == "GLProcessPipe") {
        return std::make_shared<GlProcessPipeService>(gpu_);
    }
    if (service == "refcount") {
        return std::make_shared<RefcountPipeService>(gpu_);
    }
    if (service == "opengles" || service.substr(0, 9) == "opengles:") {
        std::weak_ptr<WakerState> weak_waker = waker_state_;
        return OpenglesPipeService::Create(gpu_, [weak_waker, pipe_id, generation](uint32_t flags) {
            if (auto waker = weak_waker.lock()) {
                absl::MutexLock lock(&waker->mu);
                if (waker->dev != nullptr) {
                    waker->dev->SignalWakeWithGeneration(pipe_id, generation, flags);
                }
            }
        });
    }

    LOG(WARNING) << "goldfish_pipe: unsupported service '" << service_name << "'";
    return nullptr;
}

void GoldfishPipeDevice::Reset() ABSL_LOCKS_EXCLUDED(mu_) {
    std::map<int32_t, PipeSlot> pipes;
    {
        absl::MutexLock lock(&mu_);
        pipes = std::move(pipes_);
        open_buffer_addr_ = 0;
        signal_buffer_addr_ = 0;
        signal_buffer_size_ = 0;
        version_ = kGoldfishPipeDeviceVersion;
    }
    for (auto& [id, slot] : pipes) {
        if (slot.handler != nullptr) {
            slot.handler->OnClose();
        }
    }
    set_irq_(false);
}

}  // namespace goldfish::legacy

extern "C" GoldfishMmioCore GoldfishPipeCoreCreate(const GoldfishDeviceEnv* env) {
    DCHECK(env != nullptr);
    auto dev = std::make_unique<goldfish::legacy::GoldfishPipeDevice>(
            goldfish::legacy::WrapGuestMemory(env->memory), goldfish::legacy::WrapIrq(env->irq),
            goldfish::legacy::GetGoldfishGpu());
    return goldfish::legacy::ToMmioCore(std::move(dev));
}
