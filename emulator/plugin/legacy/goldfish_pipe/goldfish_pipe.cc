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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "goldfish/archive/deque_archive.h"
#include "goldfish/archive/qemu_file_reader.h"
#include "goldfish/archive/qemu_file_writer.h"
#include "goldfish/legacy/common/adapters.h"
#include "goldfish/socket_buffer.h"

namespace goldfish::legacy {
namespace {

constexpr uint32_t kPipeSnapshotMagic = 0x47504950U;  // "GPIP"
constexpr uint32_t kPipeSnapshotVersion = 1U;
constexpr size_t kIoChunkSize = size_t{64} * 1024;

// Largest per-pipe snapshot entry excluding the opengles read queue; see the
// GOLDFISH_PIPE_SNAPSHOT_* derivation in goldfish_pipe.h (32-bit varints take at
// most 5 bytes; 64-bit varints take at most 10 bytes).
constexpr size_t kVarint32MaxBytes = 5;
constexpr size_t kVarint64MaxBytes = 10;
constexpr size_t kPipeEntryFixedBytes =
        kVarint32MaxBytes + kVarint64MaxBytes + 3 * kVarint32MaxBytes + 1 +
        kVarint32MaxBytes + kMaxHandshakeLength + kVarint32MaxBytes;
constexpr size_t kGlProcessPipeMaxReplyBytes = 64;
constexpr size_t kGlProcessPipeStateBytes =
        1 + 1 + kVarint64MaxBytes + kVarint32MaxBytes + sizeof(int32_t) +
        2 * kVarint32MaxBytes + kGlProcessPipeMaxReplyBytes;
constexpr size_t kRefcountPipeStateBytes = 1 + 2 * kVarint32MaxBytes + sizeof(uint32_t);
constexpr size_t kOpenglesPipeStateFixedBytes = kVarint32MaxBytes + kVarint64MaxBytes;
static_assert(kPipeEntryFixedBytes + kGlProcessPipeStateBytes <= kMaxSnapshotPipeEntryBytes);
static_assert(kPipeEntryFixedBytes + kRefcountPipeStateBytes <= kMaxSnapshotPipeEntryBytes);
static_assert(kPipeEntryFixedBytes + kOpenglesPipeStateFixedBytes <= kMaxSnapshotPipeEntryBytes);
// magic, version, open_buf, signal_buf, signal_size, dev_ver, pipe_count.
static_assert(GOLDFISH_PIPE_SNAPSHOT_HEADER_BYTES == 5 * kVarint32MaxBytes + 2 * kVarint64MaxBytes);

absl::Status ExceedsError(std::string_view field, uint64_t value, uint64_t limit) {
    return absl::DataLossError(absl::StrCat(field, " ", value, " exceeds ", limit));
}

template <typename T>
absl::Status ReadField(archive::IReader& r, T& dst, std::string_view context) {
    absl::Status st = archive::ReadValue(r, dst);
    if (!st.ok()) {
        return absl::DataLossError(
                absl::StrCat("snapshot stream truncated or corrupt while reading ", context, ": ",
                             st.message()));
    }
    return absl::OkStatus();
}

void WriteBytes(archive::IWriter& w, const void* src, size_t size) {
    if (size > 0 && src != nullptr) {
        w.Write(src, size);
    }
}

absl::Status ReadBytes(archive::IReader& r, void* dst, size_t size, std::string_view context) {
    if (size == 0) {
        return absl::OkStatus();
    }
    absl::Status st = r.Read(dst, size);
    if (!st.ok()) {
        return absl::DataLossError(
                absl::StrCat("snapshot stream truncated while reading ", context, ": ",
                             st.message()));
    }
    return absl::OkStatus();
}

absl::Status ReadIntoSocketBuffer(archive::IReader& r, size_t size, SocketBuffer* out,
                                  std::string_view context) {
    uint8_t chunk[kIoChunkSize];
    size_t remaining = size;
    while (remaining > 0) {
        const size_t n = std::min(remaining, sizeof(chunk));
        absl::Status st = ReadBytes(r, chunk, n, context);
        if (!st.ok()) {
            return st;
        }
        (void)out->Append(chunk, n);
        remaining -= n;
    }
    return absl::OkStatus();
}

absl::Status ReadIntoDequeArchive(archive::IReader& r, size_t size, archive::DequeArchive* out,
                                  std::string_view context) {
    uint8_t chunk[kIoChunkSize];
    size_t remaining = size;
    while (remaining > 0) {
        const size_t n = std::min(remaining, sizeof(chunk));
        absl::Status st = ReadBytes(r, chunk, n, context);
        if (!st.ok()) {
            return st;
        }
        out->Write(chunk, n);
        remaining -= n;
    }
    return absl::OkStatus();
}

void WriteDequeArchive(archive::IWriter& w, const archive::DequeArchive& src) {
    uint8_t chunk[kIoChunkSize];
    auto it = src.storage.begin();
    size_t remaining = src.storage.size();
    while (remaining > 0) {
        const size_t n = std::min(remaining, sizeof(chunk));
        const auto next = std::next(it, static_cast<std::ptrdiff_t>(n));
        std::copy(it, next, chunk);
        w.Write(chunk, n);
        it = next;
        remaining -= n;
    }
}

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

    void Save(archive::IWriter& out, uint64_t* queued_bytes) const override {
        *queued_bytes = 0;
        out << static_cast<uint8_t>(initialized_ ? 1 : 0)
            << static_cast<uint8_t>(failed_ ? 1 : 0) << puid_
            << static_cast<uint32_t>(confirm_bytes_);
        WriteBytes(out, confirm_buf_, sizeof(confirm_buf_));
        out << static_cast<uint32_t>(reply_pos_) << static_cast<uint32_t>(reply_.size());
        WriteBytes(out, reply_.data(), reply_.size());
    }

    absl::Status Load(archive::IReader& in) override {
        uint8_t init = 0;
        uint8_t failed = 0;
        uint64_t puid = 0;
        uint32_t confirm_bytes = 0;
        uint8_t confirm_buf[sizeof(confirm_buf_)] = {};
        uint32_t reply_pos = 0;
        uint32_t reply_size = 0;
        absl::Status st = ReadField(in, init, "GLProcessPipe state");
        if (st.ok()) st = ReadField(in, failed, "GLProcessPipe state");
        if (st.ok()) st = ReadField(in, puid, "GLProcessPipe state");
        if (st.ok()) st = ReadField(in, confirm_bytes, "GLProcessPipe state");
        if (st.ok()) st = ReadBytes(in, confirm_buf, sizeof(confirm_buf), "GLProcessPipe state");
        if (st.ok()) st = ReadField(in, reply_pos, "GLProcessPipe state");
        if (st.ok()) st = ReadField(in, reply_size, "GLProcessPipe state");
        if (!st.ok()) {
            return st;
        }
        if (confirm_bytes > sizeof(confirm_buf_)) {
            return ExceedsError("GLProcessPipe confirm_bytes", confirm_bytes, sizeof(confirm_buf_));
        }
        if (reply_size > kGlProcessPipeMaxReplyBytes) {
            return ExceedsError("GLProcessPipe reply_size", reply_size,
                                kGlProcessPipeMaxReplyBytes);
        }
        if (reply_pos > reply_size) {
            return ExceedsError("GLProcessPipe reply_pos", reply_pos, reply_size);
        }
        std::vector<uint8_t> reply(reply_size);
        st = ReadBytes(in, reply.data(), reply_size, "GLProcessPipe reply");
        if (!st.ok()) {
            return st;
        }
        puid_ = puid;
        std::memcpy(confirm_buf_, confirm_buf, sizeof(confirm_buf_));
        reply_ = std::move(reply);
        initialized_ = (init != 0);
        failed_ = (failed != 0);
        confirm_bytes_ = confirm_bytes;
        reply_pos_ = reply_pos;
        return absl::OkStatus();
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
                gpu_.ColorBufferRef(handle_);
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

    void Save(archive::IWriter& out, uint64_t* queued_bytes) const override {
        *queued_bytes = 0;
        out << static_cast<uint8_t>(has_handle_ ? 1 : 0) << handle_
            << static_cast<uint32_t>(buf_.size());
        WriteBytes(out, buf_.data(), buf_.size());
    }

    absl::Status Load(archive::IReader& in) override {
        uint8_t has_handle = 0;
        uint32_t handle = 0;
        uint32_t buf_size = 0;
        absl::Status st = ReadField(in, has_handle, "refcount state");
        if (st.ok()) st = ReadField(in, handle, "refcount state");
        if (st.ok()) st = ReadField(in, buf_size, "refcount state");
        if (!st.ok()) {
            return st;
        }
        if (buf_size > sizeof(uint32_t)) {
            return ExceedsError("refcount buf_size", buf_size, sizeof(uint32_t));
        }
        std::vector<uint8_t> buf(buf_size);
        st = ReadBytes(in, buf.data(), buf_size, "refcount handle bytes");
        if (!st.ok()) {
            return st;
        }
        buf_ = std::move(buf);
        has_handle_ = (has_handle != 0);
        handle_ = handle;
        if (has_handle_) {
            gpu_.ColorBufferRef(handle_);
        }
        return absl::OkStatus();
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
        absl::StatusOr<uint32_t> stream_id =
                gpu.CreateOpenglesPipe(MakeReadCallback(state), MakeWritableCallback(state));
        if (!stream_id.ok()) {
            LOG(ERROR) << "goldfish_pipe: CreateOpenglesPipe failed: " << stream_id.status();
            return nullptr;
        }
        return std::unique_ptr<OpenglesPipeService>(
                new OpenglesPipeService(gpu, *stream_id, std::move(state)));
    }

    // `queued_budget` is the number of host->guest bytes the snapshot may still
    // hold across all pipes; it is decremented by this pipe's queue.
    static absl::StatusOr<std::unique_ptr<OpenglesPipeService>> Restore(
            IGoldfishGpu& gpu, PipeWakeCallback signal_wake, archive::IReader& in,
            uint64_t* queued_budget) {
        uint32_t stream_id = 0;
        uint32_t q_size = 0;
        absl::Status st = ReadField(in, stream_id, "opengles state");
        if (st.ok()) st = ReadField(in, q_size, "opengles state");
        if (!st.ok()) {
            return st;
        }
        if (q_size > *queued_budget) {
            return absl::DataLossError(absl::StrCat("opengles queued_bytes ", q_size,
                                                    " exceeds the remaining ", *queued_budget,
                                                    " of ", kMaxSnapshotQueuedBytes));
        }
        auto state = std::make_shared<SharedState>(std::move(signal_wake));
        {
            absl::MutexLock lock(&state->mu);
            st = ReadIntoSocketBuffer(in, q_size, &state->read_queue, "opengles queued bytes");
            if (!st.ok()) {
                return st;
            }
        }
        *queued_budget -= q_size;

        st = gpu.RestoreOpenglesPipe(stream_id, MakeReadCallback(state),
                                     MakeWritableCallback(state));
        if (!st.ok()) {
            return absl::Status(st.code(), absl::StrCat("RestoreOpenglesPipe(stream_id=", stream_id,
                                                        ") failed: ", st.message()));
        }
        return std::unique_ptr<OpenglesPipeService>(
                new OpenglesPipeService(gpu, stream_id, std::move(state)));
    }

    // Runs on a vCPU thread holding the BQL, so it must never block: when the
    // render channel is full `WriteOpenglesPipe` returns UNAVAILABLE and the
    // guest gets `kPipeErrorAgain` (like qemu2's `OpenglEsPipe::onGuestSend`
    // returning `PIPE_ERROR_AGAIN`). The renderer later reports writability
    // through `on_writable`, which wakes the guest with `kPipeWakeWrite`.
    //
    // The renderer reports writability only once per full write, and it may
    // do so *during* `WriteOpenglesPipe` (from the render thread, or even
    // synchronously while the channel re-arms its CanWrite interest). So after
    // a full write we check whether that one-shot report already arrived: if
    // it did, the guest is not blocked and simply retries; only if it did not
    // do we mark the pipe blocked and wait for it.
    int32_t OnWrite(const uint8_t* data, size_t size, size_t* consumed) override {
        if (size == 0) {
            *consumed = 0;
            return kPipeStatusOk;
        }
        uint64_t writable_seq_before = 0;
        {
            absl::MutexLock lock(&state_->mu);
            writable_seq_before = state_->writable_seq;
        }
        auto status = gpu_.WriteOpenglesPipe(stream_id_, data, size);
        if (absl::IsUnavailable(status)) {
            absl::MutexLock lock(&state_->mu);
            const bool woke_during_write = state_->writable_seq != writable_seq_before;
            if (!woke_during_write) {
                state_->write_blocked = true;
            }
            *consumed = 0;
            return kPipeErrorAgain;
        }
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

    // Reports `kPipePollOut` only while the last write did not hit a full render
    // channel; once `on_writable` fires the flag is cleared again. This keeps
    // `kPipeCmdWakeOnWrite` from signalling immediately after an AGAIN.
    uint32_t OnPoll() const override {
        absl::MutexLock lock(&state_->mu);
        if (state_->closed) {
            return kPipePollHup;
        }
        uint32_t flags = state_->write_blocked ? 0 : kPipePollOut;
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

    void Save(archive::IWriter& out, uint64_t* queued_bytes) const override {
        out << stream_id_;
        absl::MutexLock lock(&state_->mu);
        *queued_bytes = state_->read_queue.Size();
        state_->read_queue.SaveToSnapshot(out);
    }

    absl::Status Load(archive::IReader& /*in*/) override {
        return absl::InternalError("opengles pipes are restored through Restore()");
    }

  private:
    struct SharedState {
        explicit SharedState(PipeWakeCallback cb) : signal_wake(std::move(cb)) {}

        SocketBuffer read_queue ABSL_GUARDED_BY(mu);
        PipeWakeCallback signal_wake ABSL_GUARDED_BY(mu);
        bool closed ABSL_GUARDED_BY(mu) = false;
        // Set when the last guest write returned kPipeErrorAgain; cleared by
        // the renderer's on_writable callback.
        bool write_blocked ABSL_GUARDED_BY(mu) = false;
        // Incremented by every on_writable callback, so OnWrite can tell
        // whether the renderer's one-shot writability report landed while the
        // write was in progress.
        uint64_t writable_seq ABSL_GUARDED_BY(mu) = 0;
        mutable absl::Mutex mu;
    };

    static OpenglesReadCallback MakeReadCallback(const std::shared_ptr<SharedState>& state) {
        std::weak_ptr<SharedState> weak_state = state;
        return [weak_state](const uint8_t* data, size_t size) {
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
        };
    }

    static OpenglesWritableCallback MakeWritableCallback(
            const std::shared_ptr<SharedState>& state) {
        std::weak_ptr<SharedState> weak_state = state;
        return [weak_state]() {
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
                locked_state->write_blocked = false;
                ++locked_state->writable_seq;
                wake_cb = locked_state->signal_wake;
            }
            if (wake_cb != nullptr) {
                wake_cb(kPipeWakeWrite);
            }
        };
    }

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

void GoldfishPipeDevice::SignalWakeWithGeneration(int32_t pipe_id, uint64_t generation,
                                                  uint32_t wake_flags) ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    auto it = pipes_.find(pipe_id);
    if (it == pipes_.end() || it->second.generation != generation) {
        return;
    }
    const uint32_t matched = it->second.wanted_flags & wake_flags;
    if (matched != 0) {
        it->second.signalled_flags |= matched;
        it->second.wanted_flags &= ~matched;
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
            set_irq_(false);
        }
        for (auto& [id, slot] : pipes) {
            if (slot.handler != nullptr) {
                slot.handler->OnClose();
            }
        }
        return version;
    }
    case kPipeRegGetSignalled: {
        absl::MutexLock lock(&mu_);
        bool lower_irq = false;
        const uint32_t count = DrainSignalledPipesLocked(&lower_irq);
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
            if (*out_raise_irq) {
                set_irq_(true);
            }
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
            if (*out_raise_irq) {
                set_irq_(true);
            }
            return;
        }

        switch (hdr.cmd) {
        case kPipeCmdOpen:
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            if (*out_raise_irq) {
                set_irq_(true);
            }
            return;
        case kPipeCmdClose:
            closed_handler = std::move(slot.handler);
            hdr.status = kPipeStatusOk;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            pipes_.erase(it);
            *out_raise_irq = HasSignalledPipesLocked();
            if (*out_raise_irq) {
                set_irq_(true);
            }
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
                if (*out_raise_irq) {
                    set_irq_(true);
                }
                return;
            }
            break;
        case kPipeCmdDmaHostMap:
        case kPipeCmdDmaHostUnmap:
            // DMA is not used in API 36.
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            if (*out_raise_irq) {
                set_irq_(true);
            }
            return;
        default:
            LOG(WARNING) << "goldfish_pipe: unknown command " << hdr.cmd;
            hdr.status = kPipeErrorInval;
            memory_->Write(cmd_gpa, &hdr, sizeof(hdr));
            *out_raise_irq = HasSignalledPipesLocked();
            if (*out_raise_irq) {
                set_irq_(true);
            }
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
        if (*out_raise_irq) {
            set_irq_(true);
        }
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
        if (*out_raise_irq) {
            set_irq_(true);
        }
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
    uint64_t total_size = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (sizes[i] > kMaxPipeBufferSize || total_size + sizes[i] > kMaxPipeBufferSize) {
            LOG(WARNING) << "goldfish_pipe: buffer size " << sizes[i]
                         << " (cumulative=" << (total_size + sizes[i]) << ") exceeds limit ("
                         << kMaxPipeBufferSize << ")";
            return false;
        }
        total_size += sizes[i];
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

    std::vector<uint8_t> data;
    for (const auto& b : bufs) {
        if (b.size == 0) {
            continue;
        }
        for (size_t b_offset = 0; b_offset < b.size;) {
            const size_t chunk_len = std::min<size_t>(b.size - b_offset, kIoChunkSize);
            if (data.size() < chunk_len) {
                data.resize(chunk_len);
            }
            if (!memory_->Read(b.gpa + b_offset, data.data(), chunk_len)) {
                return (*total_consumed > 0) ? kPipeStatusOk : kPipeErrorIo;
            }
            b_offset += chunk_len;

            size_t offset = 0;
            if (!handshake_done) {
                while (offset < chunk_len) {
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
                if (offset == chunk_len) {
                    continue;
                }
            }

            size_t consumed = 0;
            const int32_t st =
                    handler->OnWrite(data.data() + offset, chunk_len - offset, &consumed);
            *total_consumed += static_cast<uint32_t>(consumed);
            if (st != kPipeStatusOk) {
                return (*total_consumed > 0) ? kPipeStatusOk : st;
            }
            if (consumed < chunk_len - offset) {
                return kPipeStatusOk;
            }
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
    std::vector<uint8_t> data;
    for (const auto& b : bufs) {
        if (b.size == 0) {
            continue;
        }
        for (size_t b_offset = 0; b_offset < b.size;) {
            const size_t chunk_len = std::min<size_t>(b.size - b_offset, kIoChunkSize);
            if (data.size() < chunk_len) {
                data.resize(chunk_len);
            }
            size_t produced = 0;
            const int32_t st = handler->OnRead(data.data(), chunk_len, &produced);
            if (produced > 0) {
                if (!memory_->Write(b.gpa + b_offset, data.data(), produced)) {
                    return (*total_produced > 0) ? kPipeStatusOk : kPipeErrorIo;
                }
                *total_produced += static_cast<uint32_t>(produced);
                b_offset += produced;
            }
            if (st != kPipeStatusOk) {
                return (*total_produced > 0) ? kPipeStatusOk : st;
            }
            if (produced < chunk_len) {
                return (*total_produced > 0) ? kPipeStatusOk : kPipeErrorAgain;
            }
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

absl::StatusOr<std::shared_ptr<IPipeService>> GoldfishPipeDevice::RestoreServiceHandler(
        int32_t pipe_id, uint64_t generation, std::string_view service_name, archive::IReader& in,
        uint64_t* queued_budget) ABSL_LOCKS_EXCLUDED(mu_) {
    constexpr std::string_view kPipePrefix = "pipe:";
    if (service_name.substr(0, kPipePrefix.size()) != kPipePrefix) {
        return absl::DataLossError(absl::StrCat("pipe ", pipe_id, " handshake '", service_name,
                                                "' does not start with 'pipe:'"));
    }
    std::string_view service = service_name.substr(kPipePrefix.size());

    if (service == "GLProcessPipe") {
        auto svc = std::make_shared<GlProcessPipeService>(gpu_);
        absl::Status st = svc->Load(in);
        if (!st.ok()) {
            return st;
        }
        return std::shared_ptr<IPipeService>(std::move(svc));
    }
    if (service == "refcount") {
        auto svc = std::make_shared<RefcountPipeService>(gpu_);
        absl::Status st = svc->Load(in);
        if (!st.ok()) {
            return st;
        }
        return std::shared_ptr<IPipeService>(std::move(svc));
    }
    if (service == "opengles" || service.substr(0, 9) == "opengles:") {
        std::weak_ptr<WakerState> weak_waker = waker_state_;
        absl::StatusOr<std::unique_ptr<OpenglesPipeService>> svc = OpenglesPipeService::Restore(
                gpu_,
                [weak_waker, pipe_id, generation](uint32_t flags) {
                    if (auto waker = weak_waker.lock()) {
                        absl::MutexLock lock(&waker->mu);
                        if (waker->dev != nullptr) {
                            waker->dev->SignalWakeWithGeneration(pipe_id, generation, flags);
                        }
                    }
                },
                in, queued_budget);
        if (!svc.ok()) {
            return svc.status();
        }
        return std::shared_ptr<IPipeService>(std::move(*svc));
    }
    return absl::DataLossError(
            absl::StrCat("pipe ", pipe_id, " names unknown service '", service_name, "'"));
}

absl::Status GoldfishPipeDevice::Save(std::string_view /*snapshot_name*/, archive::IWriter& out)
        ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    if (pipes_.size() > kMaxSnapshotPipeCount) {
        return absl::ResourceExhaustedError(absl::StrCat("pipe_count ", pipes_.size(), " exceeds ",
                                                         kMaxSnapshotPipeCount,
                                                         "; the snapshot could not be loaded"));
    }
    out << kPipeSnapshotMagic << kPipeSnapshotVersion << open_buffer_addr_ << signal_buffer_addr_
        << signal_buffer_size_ << version_ << static_cast<uint32_t>(pipes_.size());
    uint64_t total_queued = 0;
    for (const auto& [id, slot] : pipes_) {
        out << static_cast<uint32_t>(slot.id) << slot.command_buffer_gpa
            << slot.rw_params_max_count << slot.wanted_flags << slot.signalled_flags
            << static_cast<uint8_t>(slot.handshake_done ? 1 : 0)
            << static_cast<uint32_t>(slot.handshake_buf.size());
        WriteBytes(out, slot.handshake_buf.data(), slot.handshake_buf.size());
        archive::DequeArchive svc;
        if (slot.handler != nullptr) {
            uint64_t queued = 0;
            slot.handler->Save(svc, &queued);
            total_queued += queued;
            if (total_queued > kMaxSnapshotQueuedBytes) {
                return absl::ResourceExhaustedError(absl::StrCat(
                        "opengles queued_bytes ", total_queued, " (through pipe ", id, ") exceeds ",
                        kMaxSnapshotQueuedBytes, "; the snapshot could not be loaded"));
            }
        }
        out << static_cast<uint32_t>(svc.Size());
        WriteDequeArchive(out, svc);
    }
    return absl::OkStatus();
}

absl::Status GoldfishPipeDevice::PreLoad(std::string_view /*snapshot_name*/)
        ABSL_LOCKS_EXCLUDED(mu_) {
    // The GoldfishGpu's PreLoad already ran from the "goldfish_gpu_begin" section and dropped
    // every render channel, so the old handlers are simply forgotten here.
    {
        absl::MutexLock lock(&mu_);
        pipes_.clear();
    }
    set_irq_(false);
    return absl::OkStatus();
}

absl::Status GoldfishPipeDevice::ParseSnapshot(archive::IReader& in, RestoredState* out)
        ABSL_LOCKS_EXCLUDED(mu_) {
    uint32_t magic = 0;
    uint32_t version = 0;
    absl::Status st = ReadField(in, magic, "goldfish_pipe snapshot magic/version");
    if (st.ok()) st = ReadField(in, version, "goldfish_pipe snapshot magic/version");
    if (!st.ok()) {
        return st;
    }
    if (magic != kPipeSnapshotMagic) {
        return absl::DataLossError(absl::StrCat("goldfish_pipe snapshot magic 0x", absl::Hex(magic),
                                                " != expected 0x", absl::Hex(kPipeSnapshotMagic)));
    }
    if (version != kPipeSnapshotVersion) {
        return absl::FailedPreconditionError(absl::StrCat("goldfish_pipe snapshot version ",
                                                          version, " is not supported (expected ",
                                                          kPipeSnapshotVersion, ")"));
    }

    st = ReadField(in, out->open_buffer_addr, "goldfish_pipe snapshot header");
    if (st.ok()) st = ReadField(in, out->signal_buffer_addr, "goldfish_pipe snapshot header");
    if (st.ok()) st = ReadField(in, out->signal_buffer_size, "goldfish_pipe snapshot header");
    if (st.ok()) st = ReadField(in, out->version, "goldfish_pipe snapshot header");
    if (!st.ok()) {
        return st;
    }

    uint32_t pipe_count = 0;
    st = ReadField(in, pipe_count, "pipe_count");
    if (!st.ok()) {
        return st;
    }
    if (pipe_count > kMaxSnapshotPipeCount) {
        return ExceedsError("pipe_count", pipe_count, kMaxSnapshotPipeCount);
    }

    uint64_t queued_budget = kMaxSnapshotQueuedBytes;
    std::set<uint32_t> seen_opengles_streams;
    for (uint32_t i = 0; i < pipe_count; ++i) {
        uint32_t raw_id = 0;
        uint64_t cmd_gpa = 0;
        uint32_t rw_max = 0;
        uint32_t wanted = 0;
        uint32_t signalled = 0;
        uint8_t hs_done = 0;
        uint32_t hs_len = 0;
        const std::string entry_ctx = absl::StrCat("pipe entry ", i, " of ", pipe_count);
        st = ReadField(in, raw_id, entry_ctx);
        if (st.ok()) st = ReadField(in, cmd_gpa, entry_ctx);
        if (st.ok()) st = ReadField(in, rw_max, entry_ctx);
        if (st.ok()) st = ReadField(in, wanted, entry_ctx);
        if (st.ok()) st = ReadField(in, signalled, entry_ctx);
        if (st.ok()) st = ReadField(in, hs_done, entry_ctx);
        if (st.ok()) st = ReadField(in, hs_len, entry_ctx);
        if (!st.ok()) {
            return st;
        }
        const int32_t pipe_id = static_cast<int32_t>(raw_id);
        if (rw_max == 0 || rw_max > kMaxPipeBuffersCount) {
            return absl::DataLossError(absl::StrCat("pipe ", pipe_id, " rw_params_max_count ",
                                                    rw_max, " is outside [1, ",
                                                    kMaxPipeBuffersCount, "]"));
        }
        if (hs_len > kMaxHandshakeLength) {
            return ExceedsError(absl::StrCat("pipe ", pipe_id, " handshake_len"), hs_len,
                                kMaxHandshakeLength);
        }
        if (out->pipes.count(pipe_id) != 0) {
            return absl::DataLossError(
                    absl::StrCat("pipe id ", pipe_id, " appears twice in the snapshot"));
        }
        std::string hs_buf(hs_len, '\0');
        st = ReadBytes(in, hs_buf.data(), hs_len, absl::StrCat("pipe ", pipe_id, " handshake"));
        if (!st.ok()) {
            return st;
        }
        uint32_t svc_len = 0;
        st = ReadField(in, svc_len, absl::StrCat("pipe ", pipe_id, " service_len"));
        if (!st.ok()) {
            return st;
        }
        if (svc_len > queued_budget + kMaxSnapshotPipeEntryBytes) {
            return ExceedsError(absl::StrCat("pipe ", pipe_id, " service_len"), svc_len,
                                queued_budget + kMaxSnapshotPipeEntryBytes);
        }
        archive::DequeArchive svc_in;
        st = ReadIntoDequeArchive(in, svc_len, &svc_in,
                                  absl::StrCat("pipe ", pipe_id, " service state"));
        if (!st.ok()) {
            return st;
        }

        uint64_t generation = 0;
        {
            absl::MutexLock lock(&mu_);
            generation = next_generation_++;
        }

        std::shared_ptr<IPipeService> handler;
        if (hs_done != 0) {
            if (hs_buf == "pipe:opengles" || hs_buf.substr(0, 14) == "pipe:opengles:") {
                archive::DequeArchive peek = svc_in;
                uint32_t stream_id = 0;
                if (archive::ReadValue(peek, stream_id).ok() &&
                    !seen_opengles_streams.insert(stream_id).second) {
                    return absl::DataLossError(absl::StrCat("pipe ", pipe_id, " service '", hs_buf,
                                                            "': duplicate opengles stream_id ",
                                                            stream_id));
                }
            }
            absl::StatusOr<std::shared_ptr<IPipeService>> restored =
                    RestoreServiceHandler(pipe_id, generation, hs_buf, svc_in, &queued_budget);
            if (!restored.ok()) {
                return absl::Status(restored.status().code(),
                                    absl::StrCat("pipe ", pipe_id, " service '", hs_buf,
                                                 "': ", restored.status().message()));
            }
            if (!svc_in.Empty()) {
                // The handler already holds GPU resources (a ColorBuffer ref or
                // a render channel); release them since Load() only closes the
                // handlers recorded in out->pipes.
                (*restored)->OnClose();
                return absl::DataLossError(
                        absl::StrCat("pipe ", pipe_id, " service '", hs_buf, "' consumed ",
                                     svc_len - svc_in.Size(), " of ", svc_len,
                                     " service_len bytes"));
            }
            handler = std::move(*restored);
        }

        PipeSlot slot;
        slot.id = pipe_id;
        slot.generation = generation;
        slot.command_buffer_gpa = cmd_gpa;
        slot.rw_params_max_count = rw_max;
        slot.wanted_flags = wanted;
        slot.signalled_flags = signalled;
        slot.handshake_done = (hs_done != 0);
        slot.handshake_buf = std::move(hs_buf);
        slot.handler = std::move(handler);
        out->pipes.emplace(pipe_id, std::move(slot));
    }
    return absl::OkStatus();
}

absl::Status GoldfishPipeDevice::Load(std::string_view /*snapshot_name*/, archive::IReader& in)
        ABSL_LOCKS_EXCLUDED(mu_) {
    RestoredState restored;
    absl::Status st = ParseSnapshot(in, &restored);
    if (!st.ok()) {
        // Release GPU resources (ColorBuffer refs, render channels) taken by the
        // handlers that were restored before the failure.
        for (auto& [id, slot] : restored.pipes) {
            if (slot.handler != nullptr) {
                slot.handler->OnClose();
            }
        }
        return st;
    }

    {
        absl::MutexLock lock(&mu_);
        open_buffer_addr_ = restored.open_buffer_addr;
        signal_buffer_addr_ = restored.signal_buffer_addr;
        signal_buffer_size_ = restored.signal_buffer_size;
        version_ = restored.version;
        pipes_ = std::move(restored.pipes);
        set_irq_(HasSignalledPipesLocked());
    }
    return absl::OkStatus();
}

absl::Status GoldfishPipeDevice::PostLoad(std::string_view /*snapshot_name*/)
        ABSL_LOCKS_EXCLUDED(mu_) {
    // Re-evaluate wanted_flags against restored service poll state.
    std::vector<std::pair<int32_t, std::shared_ptr<IPipeService>>> handlers;
    {
        absl::MutexLock lock(&mu_);
        for (const auto& [id, slot] : pipes_) {
            if (slot.wanted_flags != 0 && slot.handler != nullptr) {
                handlers.emplace_back(id, slot.handler);
            }
        }
    }

    std::vector<std::pair<int32_t, uint32_t>> poll_masks;
    poll_masks.reserve(handlers.size());
    for (const auto& [id, handler] : handlers) {
        poll_masks.emplace_back(id, handler->OnPoll());
    }

    {
        absl::MutexLock lock(&mu_);
        for (const auto& [id, mask] : poll_masks) {
            auto it = pipes_.find(id);
            if (it == pipes_.end()) {
                continue;
            }
            PipeSlot& slot = it->second;
            if ((slot.wanted_flags & kPipeWakeRead) != 0 &&
                (mask & (kPipePollIn | kPipePollHup)) != 0) {
                slot.signalled_flags |= kPipeWakeRead;
                slot.wanted_flags &= ~kPipeWakeRead;
            }
            if ((slot.wanted_flags & kPipeWakeWrite) != 0 &&
                (mask & (kPipePollOut | kPipePollHup)) != 0) {
                slot.signalled_flags |= kPipeWakeWrite;
                slot.wanted_flags &= ~kPipeWakeWrite;
            }
        }
        if (HasSignalledPipesLocked()) {
            set_irq_(true);
        }
    }
    return absl::OkStatus();
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
        set_irq_(false);
    }
    for (auto& [id, slot] : pipes) {
        if (slot.handler != nullptr) {
            slot.handler->OnClose();
        }
    }
}

}  // namespace goldfish::legacy

extern "C" GoldfishMmioCore GoldfishPipeCoreCreate(const GoldfishDeviceEnv* env) {
    DCHECK(env != nullptr);
    auto dev = std::make_unique<goldfish::legacy::GoldfishPipeDevice>(
            goldfish::legacy::WrapGuestMemory(env->memory), goldfish::legacy::WrapIrq(env->irq),
            goldfish::legacy::GetGoldfishGpu());
    return goldfish::legacy::ToMmioCore(std::move(dev));
}

extern "C" bool GoldfishPipeCoreSave(void* core, const char* snapshot_name, QEMUFile* f) {
    if (core == nullptr || f == nullptr) {
        return false;
    }
    auto* dev = static_cast<goldfish::legacy::GoldfishPipeDevice*>(
            static_cast<goldfish::legacy::IMmioDevice*>(core));
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    goldfish::archive::QEMUFileWriter writer(f);
    absl::Status st = dev->Save(name, writer);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_pipe: Save failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishPipeCorePreLoad(void* core, const char* snapshot_name) {
    if (core == nullptr) {
        return false;
    }
    auto* dev = static_cast<goldfish::legacy::GoldfishPipeDevice*>(
            static_cast<goldfish::legacy::IMmioDevice*>(core));
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = dev->PreLoad(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_pipe: PreLoad failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishPipeCoreLoad(void* core, const char* snapshot_name, QEMUFile* f) {
    if (core == nullptr || f == nullptr) {
        return false;
    }
    auto* dev = static_cast<goldfish::legacy::GoldfishPipeDevice*>(
            static_cast<goldfish::legacy::IMmioDevice*>(core));
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    goldfish::archive::QEMUFileReader reader(f);
    absl::Status st = dev->Load(name, reader);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_pipe: Load failed: " << st;
        return false;
    }
    return true;
}

extern "C" bool GoldfishPipeCorePostLoad(void* core, const char* snapshot_name) {
    if (core == nullptr) {
        return false;
    }
    auto* dev = static_cast<goldfish::legacy::GoldfishPipeDevice*>(
            static_cast<goldfish::legacy::IMmioDevice*>(core));
    const std::string_view name = (snapshot_name != nullptr) ? snapshot_name : "";
    absl::Status st = dev->PostLoad(name);
    if (!st.ok()) {
        LOG(ERROR) << "goldfish_pipe: PostLoad failed: " << st;
        return false;
    }
    return true;
}
