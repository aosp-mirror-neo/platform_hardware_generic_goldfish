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

#include "goldfish/legacy/goldfish_gpu/transport/opengles_pipe_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

namespace goldfish::legacy {
namespace {

using State = gfxstream::RenderChannel::State;
using IoResult = gfxstream::RenderChannel::IoResult;

bool HasBit(State state, State bit) {
    return (static_cast<int>(state) & static_cast<int>(bit)) != 0;
}

// The one correct way to do a non-blocking I/O attempt against an
// edge-triggered RenderChannel (same rule as epoll EPOLLET + EAGAIN):
//
//   1. try the operation;
//   2. on TryAgain, arm `wanted` so the channel reports the next transition;
//   3. try once more, because the transition may already have happened
//      between (1) and (2) — the channel delivers such an already-satisfied
//      event synchronously inside setWantedEvents() and then clears it, so a
//      caller that did not look again would wait for a callback that already
//      fired.
//
// Returns the result of the last attempt. Only after a final TryAgain is the
// caller entitled to wait for the event callback.
template <typename Attempt>
IoResult TryThenArmAndRetry(gfxstream::RenderChannel& channel, State wanted,
                            const Attempt& attempt) {
    IoResult res = attempt();
    if (res == IoResult::TryAgain) {
        channel.setWantedEvents(wanted);
        res = attempt();
    }
    return res;
}

}  // namespace

// ---------------------------------------------------------------------------
// PipeChannel

PipeChannel::PipeChannel(uint32_t id, gfxstream::RenderChannelPtr channel, bool paused)
    : id_(id), channel_(std::move(channel)) {
    absl::MutexLock lock(&mu_);
    paused_for_snapshot_ = paused;
}

PipeChannel::~PipeChannel() { Stop(); }

void PipeChannel::SetCallbacks(OpenglesReadCallback on_read, OpenglesWritableCallback on_writable) {
    absl::MutexLock lock(&mu_);
    on_read_ = std::move(on_read);
    on_writable_ = std::move(on_writable);
}

void PipeChannel::InstallEventCallback(const std::shared_ptr<PipeChannel>& self) {
    {
        absl::MutexLock lock(&self->mu_);
        if (self->event_callback_installed_) {
            return;
        }
        self->event_callback_installed_ = true;
    }
    std::weak_ptr<PipeChannel> weak = self;
    self->channel_->setEventCallback([weak](State state) {
        if (auto locked = weak.lock()) {
            locked->OnChannelEvent(state);
        }
    });
}

void PipeChannel::StartReader(const std::shared_ptr<PipeChannel>& self) {
    InstallEventCallback(self);
    self->channel_->setWantedEvents(State::CanRead);
    {
        absl::MutexLock lock(&self->mu_);
        self->reader_started_ = true;
    }
    self->reader_thread_ = std::thread([self]() { self->ReaderLoop(); });
}

void PipeChannel::OnChannelEvent(State state) {
    OpenglesWritableCallback on_writable;
    {
        absl::MutexLock lock(&mu_);
        if (HasBit(state, State::CanRead)) {
            has_event_ = true;
        }
        if (HasBit(state, State::Stopped)) {
            closing_ = true;
        }
        if (HasBit(state, State::CanWrite)) {
            on_writable = on_writable_;
        }
    }
    // Run the guest wake-up outside the lock: the consumer may call back
    // into TryWrite() / SetCallbacks().
    if (on_writable) {
        on_writable();
    }
}

absl::Status PipeChannel::TryWrite(const uint8_t* data, size_t size) {
    // Keep CanRead wanted alongside CanWrite: setWantedEvents() ORs into the
    // wanted set, and the channel clears a bit once it has delivered it.
    constexpr auto kWanted =
            static_cast<State>(static_cast<int>(State::CanRead) | static_cast<int>(State::CanWrite));
    const IoResult res = TryThenArmAndRetry(*channel_, kWanted, [&]() {
        return channel_->tryWrite(gfxstream::RenderChannel::Buffer(
                reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size));
    });
    if (res == IoResult::Ok) {
        return absl::OkStatus();
    }
    if (res == IoResult::TryAgain) {
        return absl::UnavailableError(absl::StrCat("OpenGLES pipe channel ", id_,
                                                   " is full; retry the ", size,
                                                   "-byte write after the writable callback fires"));
    }
    return absl::InternalError(absl::StrCat("RenderChannel::tryWrite failed for pipe channel ",
                                            id_, " (", size, " bytes): channel stopped"));
}

void PipeChannel::PauseForSnapshot() {
    auto not_reading = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        return !reading_in_progress_;
    };
    absl::MutexLock lock(&mu_);
    paused_for_snapshot_ = true;
    mu_.Await(absl::Condition(&not_reading));
}

void PipeChannel::Resume(const std::shared_ptr<PipeChannel>& self) {
    bool reader_started = false;
    {
        absl::MutexLock lock(&self->mu_);
        self->paused_for_snapshot_ = false;
        self->has_event_ = true;
        reader_started = self->reader_started_;
    }
    if (!reader_started) {
        StartReader(self);
    }
}

void PipeChannel::Stop() {
    {
        absl::MutexLock lock(&mu_);
        closing_ = true;
    }
    if (channel_) {
        channel_->stop();
    }
    if (reader_thread_.joinable()) {
        if (reader_thread_.get_id() == std::this_thread::get_id()) {
            // Stop() reached from inside `on_read` (the guest closed the pipe
            // while data was being delivered): a self-join would throw.
            // Detach instead; the shared_ptr captured by the reader thread
            // keeps *this alive until ReaderLoop() observes `closing_` and
            // returns.
            reader_thread_.detach();
        } else {
            reader_thread_.join();
        }
    }
}

PipeChannel::View PipeChannel::Snapshot() const {
    absl::MutexLock lock(&mu_);
    return View{.id = id_,
                .channel = channel_.get(),
                .closing = closing_,
                .paused_for_snapshot = paused_for_snapshot_,
                .reading_in_progress = reading_in_progress_,
                .reader_started = reader_started_};
}

void PipeChannel::SetReading(bool reading) {
    absl::MutexLock lock(&mu_);
    reading_in_progress_ = reading;
}

void PipeChannel::ReaderLoop() {
    while (true) {
        OpenglesReadCallback cb;
        {
            auto should_wake = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
                return closing_ || (!paused_for_snapshot_ && has_event_);
            };
            absl::MutexLock lock(&mu_);
            mu_.Await(absl::Condition(&should_wake));
            if (closing_) {
                return;
            }
            has_event_ = false;
            cb = on_read_;
        }
        while (ReadOne(cb)) {
        }
    }
}

bool PipeChannel::ReadOne(const OpenglesReadCallback& cb) {
    {
        absl::MutexLock lock(&mu_);
        if (closing_ || paused_for_snapshot_) {
            reading_in_progress_ = false;
            return false;
        }
        reading_in_progress_ = true;
    }
    gfxstream::RenderChannel::Buffer buf;
    const IoResult res = TryThenArmAndRetry(*channel_, State::CanRead,
                                            [&]() { return channel_->tryRead(&buf); });
    if (res == IoResult::Ok && !buf.empty()) {
        if (cb) {
            cb(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
        }
        SetReading(false);
        return true;
    }
    if (res == IoResult::TryAgain) {
        SetReading(false);
        return false;
    }
    // Error (channel stopped) or an empty Ok buffer: stop reading for good.
    absl::MutexLock lock(&mu_);
    reading_in_progress_ = false;
    closing_ = true;
    return false;
}

// ---------------------------------------------------------------------------
// OpenglesPipeTable

OpenglesPipeTable::~OpenglesPipeTable() { StopAll(); }

std::shared_ptr<PipeChannel> OpenglesPipeTable::Find(uint32_t id) const {
    absl::MutexLock lock(&mu_);
    auto it = pipes_.find(id);
    return it == pipes_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<PipeChannel>> OpenglesPipeTable::All() const {
    absl::MutexLock lock(&mu_);
    std::vector<std::shared_ptr<PipeChannel>> out;
    out.reserve(pipes_.size());
    for (const auto& [id, pipe] : pipes_) {
        out.push_back(pipe);
    }
    return out;
}

absl::StatusOr<uint32_t> OpenglesPipeTable::Create(gfxstream::Renderer* renderer,
                                                   OpenglesReadCallback on_read,
                                                   OpenglesWritableCallback on_writable) {
    gfxstream::RenderChannelPtr channel = renderer->createRenderChannel(nullptr, 0);
    if (!channel) {
        return absl::InternalError("Renderer::createRenderChannel returned nullptr");
    }
    uint32_t id = 0;
    {
        absl::MutexLock lock(&mu_);
        id = next_id_++;
    }
    auto pipe = std::make_shared<PipeChannel>(id, std::move(channel), /*paused=*/false);
    pipe->SetCallbacks(std::move(on_read), std::move(on_writable));
    // Start the reader before publishing so that a registered pipe always
    // has a running reader thread (PIPE-RUNNING invariant).
    PipeChannel::StartReader(pipe);
    absl::MutexLock lock(&mu_);
    pipes_[id] = std::move(pipe);
    return id;
}

absl::Status OpenglesPipeTable::Restore(uint32_t id, OpenglesReadCallback on_read,
                                        OpenglesWritableCallback on_writable) {
    if (id == 0) {
        return absl::InvalidArgumentError("OpenGLES pipe channel id 0 is reserved");
    }
    if (std::shared_ptr<PipeChannel> pipe = Find(id)) {
        pipe->SetCallbacks(std::move(on_read), std::move(on_writable));
        return absl::OkStatus();
    }
    absl::MutexLock lock(&mu_);
    if (pending_restores_.find(id) != pending_restores_.end()) {
        return absl::AlreadyExistsError(
                absl::StrCat("OpenGLES pipe channel id ", id, " restored twice in snapshot"));
    }
    pending_restores_[id] =
            PendingRestore{.on_read = std::move(on_read), .on_writable = std::move(on_writable)};
    return absl::OkStatus();
}

absl::Status OpenglesPipeTable::LoadEntry(gfxstream::Renderer* renderer, uint32_t id,
                                          gfxstream::Stream* in) {
    {
        absl::MutexLock lock(&mu_);
        if (pipes_.find(id) != pipes_.end()) {
            return absl::DataLossError(
                    absl::StrCat("GoldfishGpu snapshot pipe entry repeats pipe id ", id));
        }
    }
    gfxstream::RenderChannelPtr channel = renderer->createRenderChannel(in, 0);
    if (!channel) {
        return absl::DataLossError(
                absl::StrCat("createRenderChannel failed on load for pipe ", id));
    }
    auto pipe = std::make_shared<PipeChannel>(id, std::move(channel), /*paused=*/true);
    // Install the event callback now, while the renderer is still paused: once
    // Renderer::resumeAll() runs, the restored render thread may fire channel
    // events at any time, and setEventCallback() races with that.
    PipeChannel::InstallEventCallback(pipe);
    PendingRestore pending;
    bool has_pending = false;
    {
        absl::MutexLock lock(&mu_);
        if (auto it = pending_restores_.find(id); it != pending_restores_.end()) {
            pending = std::move(it->second);
            pending_restores_.erase(it);
            has_pending = true;
        }
        pipes_[id] = pipe;
        next_id_ = std::max(next_id_, id + 1);
    }
    if (has_pending) {
        pipe->SetCallbacks(std::move(pending.on_read), std::move(pending.on_writable));
    }
    return absl::OkStatus();
}

absl::Status OpenglesPipeTable::VerifyNoPendingRestores() const {
    absl::MutexLock lock(&mu_);
    if (pending_restores_.empty()) {
        return absl::OkStatus();
    }
    uint32_t min_id = pending_restores_.begin()->first;
    for (const auto& [id, _] : pending_restores_) {
        min_id = std::min(min_id, id);
    }
    return absl::DataLossError(
            absl::StrCat("OpenGLES pipe channel ", min_id,
                         " restored by goldfish_pipe was not loaded by the GoldfishGpu snapshot"));
}

absl::Status OpenglesPipeTable::Write(uint32_t id, const uint8_t* data, size_t size) {
    std::shared_ptr<PipeChannel> pipe = Find(id);
    if (!pipe) {
        return absl::NotFoundError(absl::StrCat("OpenGLES pipe channel ", id, " not found"));
    }
    return pipe->TryWrite(data, size);
}

void OpenglesPipeTable::Close(uint32_t id) {
    std::shared_ptr<PipeChannel> pipe;
    {
        absl::MutexLock lock(&mu_);
        pending_restores_.erase(id);
        auto it = pipes_.find(id);
        if (it != pipes_.end()) {
            pipe = std::move(it->second);
            pipes_.erase(it);
        }
    }
    if (pipe) {
        pipe->Stop();
    }
}

void OpenglesPipeTable::PauseAll() {
    for (const auto& pipe : All()) {
        pipe->PauseForSnapshot();
    }
}

void OpenglesPipeTable::WaitAllPaused(gfxstream::Renderer& /*renderer*/,
                                      gfxstream::Stream* sink) const {
    for (const auto& pipe : All()) {
        pipe->SaveTo(sink);
    }
}

void OpenglesPipeTable::ResumeAll() {
    for (const auto& pipe : All()) {
        PipeChannel::Resume(pipe);
    }
}

void OpenglesPipeTable::StopAll() {
    std::vector<std::shared_ptr<PipeChannel>> pipes;
    {
        absl::MutexLock lock(&mu_);
        pending_restores_.clear();
        for (auto& [id, pipe] : pipes_) {
            pipes.push_back(std::move(pipe));
        }
        pipes_.clear();
    }
    for (const auto& pipe : pipes) {
        pipe->Stop();
    }
}

void OpenglesPipeTable::Reset() {
    StopAll();
    absl::MutexLock lock(&mu_);
    next_id_ = 1;
}

std::vector<uint32_t> OpenglesPipeTable::SortedIds() const {
    std::vector<uint32_t> ids;
    {
        absl::MutexLock lock(&mu_);
        ids.reserve(pipes_.size());
        for (const auto& [id, pipe] : pipes_) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void OpenglesPipeTable::SaveEntry(gfxstream::Renderer* /*renderer*/, uint32_t id,
                                  gfxstream::Stream* out) const {
    if (std::shared_ptr<PipeChannel> pipe = Find(id)) {
        pipe->SaveTo(out);
    }
}

uint32_t OpenglesPipeTable::NextId() const {
    absl::MutexLock lock(&mu_);
    return next_id_;
}

void OpenglesPipeTable::RaiseNextId(uint32_t value) {
    absl::MutexLock lock(&mu_);
    next_id_ = std::max(next_id_, value);
}

size_t OpenglesPipeTable::size() const {
    absl::MutexLock lock(&mu_);
    return pipes_.size();
}

OpenglesPipeTable::View OpenglesPipeTable::Snapshot() const {
    View view;
    std::vector<std::shared_ptr<PipeChannel>> pipes;
    {
        absl::MutexLock lock(&mu_);
        view.next_id = next_id_;
        pipes.reserve(pipes_.size());
        for (const auto& [id, pipe] : pipes_) {
            pipes.push_back(pipe);
        }
    }
    for (const auto& pipe : pipes) {
        view.pipes.push_back(pipe->Snapshot());
    }
    return view;
}

}  // namespace goldfish::legacy
