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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "render-utils/RenderChannel.h"
#include "render-utils/Renderer.h"
#include "render-utils/stream.h"

// The "pipe:opengles" render channels of the goldfish_pipe GPU service.
//
// Each guest OpenGL ES pipe maps to one gfxstream RenderChannel plus a host
// reader thread that drains host->guest data into the pipe's `on_read`
// callback. Guest->host writes go straight into the channel and never block:
// the caller runs on a vCPU thread holding the BQL, and the render thread
// that drains the channel may itself need the BQL, so a full channel is
// reported as UNAVAILABLE and the pipe's `on_writable` callback says when to
// retry.
//
// Lifecycle (same vocabulary as AsgBlobTable; the guest-visible id is the
// pipe's stream id):
//
//   Create -> Write ... -> Close                         (runtime)
//   Restore (goldfish_pipe, MIG_PRI_DEFAULT: callbacks)   (snapshot load,
//     -> LoadEntry (GoldfishGpu, MIG_PRI_LOW: channel)     two halves)
//
// Snapshot: PauseAll() parks every reader (waiting for an in-flight `on_read`
// to return) and WaitAllPaused() drains each channel's save stream, so no
// reader touches guest memory while QEMU saves RAM; SaveEntry() writes one
// channel; ResumeAll() un-parks the readers and starts the readers of channels
// that LoadEntry() created. StopAll() tears everything down.
//
// Lock order: OpenglesPipeTable::mu_ -> PipeChannel::mu_. Neither lock is held
// while `on_read`/`on_writable` run or while the RenderChannel is called.
//
// RenderChannel events are edge-triggered and one-shot (a wanted bit is
// reported once, then cleared). Every non-blocking read/write in this file
// therefore goes through one helper (TryThenArmAndRetry in the .cc): try, arm
// the wanted event on TryAgain, try once more. Do not call tryRead()/tryWrite()
// + setWantedEvents() by hand; the retry is what makes the callback wait safe.

namespace goldfish::legacy {

class PipeChannel {
  public:
    struct View {
        uint32_t id = 0;
        const gfxstream::RenderChannel* channel = nullptr;
        bool closing = false;
        bool paused_for_snapshot = false;
        bool reading_in_progress = false;
        bool reader_started = false;
    };

    // `paused` channels (created by a snapshot load) have no reader until
    // Resume() runs.
    PipeChannel(uint32_t id, gfxstream::RenderChannelPtr channel, bool paused);
    ~PipeChannel();

    PipeChannel(const PipeChannel&) = delete;
    PipeChannel& operator=(const PipeChannel&) = delete;

    uint32_t id() const { return id_; }
    gfxstream::RenderChannel* channel() const { return channel_.get(); }

    void SetCallbacks(OpenglesReadCallback on_read, OpenglesWritableCallback on_writable)
            ABSL_LOCKS_EXCLUDED(mu_);

    // Installs the channel event callback. Must run before the renderer's
    // render thread for this channel can run (i.e. right after
    // createRenderChannel(), and for loaded channels before
    // Renderer::resumeAll()): RenderChannel::setEventCallback is not
    // synchronized against the render thread's notifyStateChangeLocked().
    // Idempotent. Takes the shared_ptr because the callback keeps the channel
    // alive.
    static void InstallEventCallback(const std::shared_ptr<PipeChannel>& self)
            ABSL_LOCKS_EXCLUDED(self->mu_);

    // Installs the channel event callback (if not done yet) and starts the
    // reader thread. Must run before the channel is published (PIPE-RUNNING
    // invariant).
    static void StartReader(const std::shared_ptr<PipeChannel>& self)
            ABSL_LOCKS_EXCLUDED(self->mu_);

    // Never blocks. UNAVAILABLE when the channel is full (a CanWrite event is
    // requested and `on_writable` fires once it can accept data), INTERNAL
    // when the channel stopped.
    absl::Status TryWrite(const uint8_t* data, size_t size);

    // Marks the channel paused and waits until the reader is not inside
    // `on_read` any more.
    void PauseForSnapshot() ABSL_LOCKS_EXCLUDED(mu_);
    // Clears the pause, wakes the reader, and starts it if it never ran.
    static void Resume(const std::shared_ptr<PipeChannel>& self) ABSL_LOCKS_EXCLUDED(self->mu_);
    // Stops the channel and joins the reader. Idempotent. Safe to call from
    // inside `on_read` (the reader is detached instead of self-joined).
    void Stop() ABSL_LOCKS_EXCLUDED(mu_);

    void SaveTo(gfxstream::Stream* stream) { channel_->onSave(stream); }

    View Snapshot() const ABSL_LOCKS_EXCLUDED(mu_);

  private:
    void OnChannelEvent(gfxstream::RenderChannel::State state) ABSL_LOCKS_EXCLUDED(mu_);
    void ReaderLoop() ABSL_LOCKS_EXCLUDED(mu_);
    // Reads one buffer; returns false when the reader should go back to
    // waiting (nothing to read, paused, or the channel stopped).
    bool ReadOne(const OpenglesReadCallback& cb) ABSL_LOCKS_EXCLUDED(mu_);
    void SetReading(bool reading) ABSL_LOCKS_EXCLUDED(mu_);

    const uint32_t id_;
    const gfxstream::RenderChannelPtr channel_;
    mutable absl::Mutex mu_;
    OpenglesReadCallback on_read_ ABSL_GUARDED_BY(mu_);
    OpenglesWritableCallback on_writable_ ABSL_GUARDED_BY(mu_);
    bool has_event_ ABSL_GUARDED_BY(mu_) = false;
    bool paused_for_snapshot_ ABSL_GUARDED_BY(mu_) = false;
    bool reading_in_progress_ ABSL_GUARDED_BY(mu_) = false;
    bool closing_ ABSL_GUARDED_BY(mu_) = false;
    // Set once InstallEventCallback() has run.
    bool event_callback_installed_ ABSL_GUARDED_BY(mu_) = false;
    // Set once StartReader() has run; `reader_thread_` itself is only touched
    // by the (externally serialized) starter and joiner.
    bool reader_started_ ABSL_GUARDED_BY(mu_) = false;
    std::thread reader_thread_;
};

class OpenglesPipeTable {
  public:
    struct View {
        uint32_t next_id = 1;
        std::vector<PipeChannel::View> pipes;  // in map order
    };

    OpenglesPipeTable() = default;
    ~OpenglesPipeTable();

    // --- Lifecycle -----------------------------------------------------------

    // Creates a render channel with a fresh id and its reader, then publishes it.
    absl::StatusOr<uint32_t> Create(gfxstream::Renderer* renderer, OpenglesReadCallback on_read,
                                    OpenglesWritableCallback on_writable) ABSL_LOCKS_EXCLUDED(mu_);
    // Load, device half: goldfish_pipe re-announces a saved `id` with its
    // callbacks. Attaches them if the channel exists; otherwise (goldfish_pipe
    // loads at MIG_PRI_DEFAULT, before LoadEntry() runs at MIG_PRI_LOW) they
    // are staged until LoadEntry() creates the channel. INVALID_ARGUMENT for
    // id 0.
    absl::Status Restore(uint32_t id, OpenglesReadCallback on_read,
                         OpenglesWritableCallback on_writable) ABSL_LOCKS_EXCLUDED(mu_);
    // Load, GPU half: re-creates channel `id` from `in`, paused and without a
    // reader until ResumeAll(), and attaches the callbacks staged by
    // Restore(). DATA_LOSS when `id` is already present or the renderer refuses.
    absl::Status LoadEntry(gfxstream::Renderer* renderer, uint32_t id, gfxstream::Stream* in)
            ABSL_LOCKS_EXCLUDED(mu_);
    // DATA_LOSS if any Restore() was not matched by a LoadEntry().
    absl::Status VerifyNoPendingRestores() const ABSL_LOCKS_EXCLUDED(mu_);
    // Never blocks; see PipeChannel::TryWrite().
    absl::Status Write(uint32_t id, const uint8_t* data, size_t size) ABSL_LOCKS_EXCLUDED(mu_);
    // Stops and drops channel `id` (and any staged Restore() for it).
    void Close(uint32_t id) ABSL_LOCKS_EXCLUDED(mu_);

    // --- Snapshot ------------------------------------------------------------

    // Quiesce: parks every reader (waits for an in-flight `on_read`).
    void PauseAll() ABSL_LOCKS_EXCLUDED(mu_);
    // Quiesce: drains every channel's save stream into `sink`, which returns
    // once the render side has parked. `renderer` is unused here (the channel
    // saves itself) and accepted for symmetry with AsgBlobTable.
    void WaitAllPaused(gfxstream::Renderer& renderer, gfxstream::Stream* sink) const
            ABSL_LOCKS_EXCLUDED(mu_);
    // PostSave / PostLoad / failed snapshot: un-parks the readers and starts
    // the readers of channels created by LoadEntry().
    void ResumeAll() ABSL_LOCKS_EXCLUDED(mu_);
    // PreLoad: drops and stops every channel and any staged restores;
    // keeps `next_id`.
    void StopAll() ABSL_LOCKS_EXCLUDED(mu_);
    // Stop: StopAll() plus `next_id` back to 1.
    void Reset() ABSL_LOCKS_EXCLUDED(mu_);

    // Ids in ascending order (the snapshot order).
    std::vector<uint32_t> SortedIds() const ABSL_LOCKS_EXCLUDED(mu_);
    // Writes channel `id`'s gfxstream state; no-op for an unknown id.
    // `renderer` is unused (see WaitAllPaused).
    void SaveEntry(gfxstream::Renderer* renderer, uint32_t id, gfxstream::Stream* out) const
            ABSL_LOCKS_EXCLUDED(mu_);

    uint32_t NextId() const ABSL_LOCKS_EXCLUDED(mu_);
    void RaiseNextId(uint32_t value) ABSL_LOCKS_EXCLUDED(mu_);
    size_t size() const ABSL_LOCKS_EXCLUDED(mu_);
    View Snapshot() const ABSL_LOCKS_EXCLUDED(mu_);

  private:
    struct PendingRestore {
        OpenglesReadCallback on_read;
        OpenglesWritableCallback on_writable;
    };

    std::shared_ptr<PipeChannel> Find(uint32_t id) const ABSL_LOCKS_EXCLUDED(mu_);
    std::vector<std::shared_ptr<PipeChannel>> All() const ABSL_LOCKS_EXCLUDED(mu_);

    mutable absl::Mutex mu_;
    uint32_t next_id_ ABSL_GUARDED_BY(mu_) = 1;
    std::unordered_map<uint32_t, std::shared_ptr<PipeChannel>> pipes_ ABSL_GUARDED_BY(mu_);
    std::unordered_map<uint32_t, PendingRestore> pending_restores_ ABSL_GUARDED_BY(mu_);
};

}  // namespace goldfish::legacy
