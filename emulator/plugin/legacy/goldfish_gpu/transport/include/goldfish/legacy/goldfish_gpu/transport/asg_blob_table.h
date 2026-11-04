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
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"
#include "render-utils/Renderer.h"
#include "render-utils/address_space_graphics_types.h"
#include "render-utils/stream.h"

// Address Space Graphics (ASG) blobs and their gfxstream consumers.
//
// goldfish_address_space hands each guest graphics context a host memory blob
// (ring + buffer) that gfxstream drains on its own render thread ("the
// consumer"). The table owns the blob storage, the handle/blob-id allocation
// and the consumer lifecycle across snapshots.
//
// Lifecycle (same vocabulary as OpenglesPipeTable; the guest-visible id is the
// `context_handle`):
//
//   Create -> Start (consumer created) -> Ping ... -> Close     (runtime)
//   Restore (goldfish_address_space, MIG_PRI_DEFAULT: storage)  (snapshot load,
//     -> LoadEntry (GoldfishGpu, MIG_PRI_LOW: consumer)           two halves)
//
// Snapshot: PauseAll() asks every consumer to park and WaitAllPaused() drains
// each consumer's save stream, which returns once its render thread has
// parked; SaveEntry() writes one consumer; ResumeAll() un-parks; StopAll()
// destroys the consumers but keeps the entries for the load.
//
// Two ids identify a blob: the `context_handle` the guest sees (restored
// verbatim from a snapshot) and a process-unique `blob_id`, so that a stale
// AsgBlob from before a same-session reload cannot destroy the blob that
// replaced it.
//
// Threading: Create/Restore/Start/Ping/Close run on vCPU threads with the BQL
// held; Close must therefore never wait for a render thread (the caller
// destroys the returned entry, or retires it while the GPU is quiesced). The
// snapshot helpers run on the QEMU main thread. OnUnavailableRead() runs on
// the consumer's render thread.
//
// Lock order: AsgBlobTable::mu_ -> AsgConsumerEntry::mu. Neither is held while
// the renderer is called.

namespace goldfish::legacy {

struct PageAlignedDeleter {
    void operator()(uint8_t* ptr) const;
};
using PageAlignedBuffer = std::unique_ptr<uint8_t, PageAlignedDeleter>;
// Zeroed storage aligned to max(16 KiB, page size); null when `size` is 0 or
// the allocation fails.
PageAlignedBuffer AllocatePageAlignedStorage(size_t size);

inline constexpr size_t kAsgRingRegionSize = gfxstream::kAsgConsumerRingStorageSize;
inline constexpr size_t kAsgStorageAlignment = 16384;

struct AsgConsumerEntry {
    uint32_t context_handle = 0;
    uint32_t blob_id = 0;
    uint64_t size = 0;
    PageAlignedBuffer storage;

    absl::Mutex mu;
    // The gfxstream ASG consumer. Guarded so that the RCU-deferred Close()
    // (BQL held) and a BQL-dropped PreLoad/Stop cannot both observe a live
    // consumer and destroy it twice: destroyers claim it with
    // TakeConsumerForDestroy(), readers use GetConsumer().
    void* consumer ABSL_GUARDED_BY(mu) = nullptr;
    bool wakeup_requested ABSL_GUARDED_BY(mu) = false;
    bool pause_pre_snapshot_requested ABSL_GUARDED_BY(mu) = false;
    bool resume_post_snapshot_requested ABSL_GUARDED_BY(mu) = false;
    bool exiting ABSL_GUARDED_BY(mu) = false;

    void* GetConsumer() ABSL_LOCKS_EXCLUDED(mu);
    void SetConsumer(void* value) ABSL_LOCKS_EXCLUDED(mu);
    // Marks the entry as exiting and returns the consumer (null if already
    // claimed), transferring the responsibility to destroy it to the caller.
    void* TakeConsumerForDestroy() ABSL_LOCKS_EXCLUDED(mu);
    // gfxstream's onUnavailableRead callback: blocks the render thread until
    // there is something to do. Priority: exit > pause > resume > wake-up.
    gfxstream::AsgOnUnavailableReadStatus OnUnavailableRead() ABSL_LOCKS_EXCLUDED(mu);
};

class AsgBlobTable {
  public:
    struct EntryView {
        const void* entry = nullptr;  // identity of the AsgConsumerEntry, never dereferenced
        uint32_t context_handle = 0;
        uint32_t blob_id = 0;
        uint64_t size = 0;
        const uint8_t* storage = nullptr;
        const void* consumer = nullptr;
        bool exiting = false;
        bool wakeup_requested = false;
        bool pause_pre_snapshot_requested = false;
        bool resume_post_snapshot_requested = false;
    };
    struct View {
        uint32_t next_handle = 1;
        uint32_t next_blob_id = 1;
        // {map key, entry} for the handle table and the blob table.
        std::vector<std::pair<uint32_t, EntryView>> registered;
        std::vector<std::pair<uint32_t, EntryView>> blobs;
        size_t retired_count = 0;
    };

    // `resolve_gpa` backs gfxstream's getPtr callback (guest physical address
    // -> host pointer); it may be invoked from render threads at any time.
    explicit AsgBlobTable(std::function<void*(uint64_t)> resolve_gpa);
    ~AsgBlobTable();

    // --- Lifecycle -----------------------------------------------------------

    // Allocates a fresh handle; otherwise like Restore().
    absl::StatusOr<AsgBlob> Create(uint64_t size) ABSL_LOCKS_EXCLUDED(mu_);
    // Load, device half (also used by Create): registers blob storage under
    // `context_handle` (0 is reserved; `size` must exceed the ring region).
    // Replaces any entry with the same handle.
    absl::StatusOr<AsgBlob> Restore(uint32_t context_handle, uint64_t size)
            ABSL_LOCKS_EXCLUDED(mu_);
    // Load, GPU half: re-creates the consumer of `id` from `in`. DATA_LOSS when
    // the handle was not restored, already has a consumer, or gfxstream refuses.
    absl::Status LoadEntry(gfxstream::Renderer* renderer, uint32_t id, gfxstream::Stream* in)
            ABSL_LOCKS_EXCLUDED(mu_);
    // Creates the consumer (no-op if it exists). NOT_FOUND for an unknown
    // handle, FAILED_PRECONDITION without a renderer.
    absl::Status Start(gfxstream::Renderer* renderer, const AsgBlob& blob) ABSL_LOCKS_EXCLUDED(mu_);
    absl::Status Ping(const AsgBlob& blob) ABSL_LOCKS_EXCLUDED(mu_);
    // Never blocks. Drops the blob from the table (matching by blob_id when non-zero). With
    // `retire` the entry joins the retired list for DrainRetired() and null is
    // returned; otherwise the entry is returned for DestroyConsumers().
    std::shared_ptr<AsgConsumerEntry> Close(const AsgBlob& blob, bool retire)
            ABSL_LOCKS_EXCLUDED(mu_);

    // --- Snapshot ------------------------------------------------------------

    // Quiesce: asks every consumer to park (gfxstream + entry flag).
    void PauseAll(gfxstream::Renderer& renderer) ABSL_LOCKS_EXCLUDED(mu_);
    // Quiesce: drives addressSpaceGraphicsConsumerSave() into `sink` for every
    // consumer, which blocks until its render thread has parked.
    void WaitAllPaused(gfxstream::Renderer& renderer, gfxstream::Stream* sink) const
            ABSL_LOCKS_EXCLUDED(mu_);
    // PostSave / failed snapshot: un-parks every consumer.
    void ResumeAll(gfxstream::Renderer& renderer) ABSL_LOCKS_EXCLUDED(mu_);
    // PostLoad: consumers re-read their ring configuration from the blob.
    void ReloadRingConfigs(gfxstream::Renderer& renderer) ABSL_LOCKS_EXCLUDED(mu_);
    // Handles that currently have a consumer, ascending (the snapshot order).
    std::vector<uint32_t> SortedIds() const ABSL_LOCKS_EXCLUDED(mu_);
    // Writes consumer `id`'s gfxstream state; no-op for an unknown id or an
    // entry without a consumer.
    void SaveEntry(gfxstream::Renderer* renderer, uint32_t id, gfxstream::Stream* out) const
            ABSL_LOCKS_EXCLUDED(mu_);

    // PreLoad step 1: flag every live and retired consumer as exiting so its
    // render thread leaves OnUnavailableRead() without decoding anything.
    void MarkAllExiting() ABSL_LOCKS_EXCLUDED(mu_);
    // PreLoad step 2 (after the caller un-parked the render threads): destroys
    // the retired and live consumers; the live entries stay registered (the
    // address space device may restore a consumer for them) with clean flags.
    void StopAll(gfxstream::Renderer* renderer) ABSL_LOCKS_EXCLUDED(mu_);
    // Destroys the consumers retired while the GPU was quiesced.
    void DrainRetired(gfxstream::Renderer* renderer) ABSL_LOCKS_EXCLUDED(mu_);
    // Stop: drops every entry and resets the counters. The caller destroys
    // the returned entries (DestroyConsumers) once the render threads are
    // un-parked.
    std::vector<std::shared_ptr<AsgConsumerEntry>> Reset() ABSL_LOCKS_EXCLUDED(mu_);
    // Claims and destroys the consumer of every entry (no-op for entries whose
    // consumer was already claimed). Render threads must not be parked.
    static void DestroyConsumers(gfxstream::Renderer* renderer,
                                 const std::vector<std::shared_ptr<AsgConsumerEntry>>& entries);

    uint32_t NextId() const ABSL_LOCKS_EXCLUDED(mu_);
    void RaiseNextId(uint32_t value) ABSL_LOCKS_EXCLUDED(mu_);
    View Snapshot() const ABSL_LOCKS_EXCLUDED(mu_);

  private:
    std::shared_ptr<AsgConsumerEntry> Find(uint32_t handle) const ABSL_LOCKS_EXCLUDED(mu_);
    std::vector<std::shared_ptr<AsgConsumerEntry>> Registered() const ABSL_LOCKS_EXCLUDED(mu_);
    std::vector<std::shared_ptr<AsgConsumerEntry>> TakeRetired() ABSL_LOCKS_EXCLUDED(mu_);
    gfxstream::AsgConsumerCreateInfo MakeCreateInfo(const std::shared_ptr<AsgConsumerEntry>& entry);
    static EntryView MakeView(const std::shared_ptr<AsgConsumerEntry>& entry);

    const std::function<void*(uint64_t)> resolve_gpa_;
    mutable absl::Mutex mu_;
    uint32_t next_handle_ ABSL_GUARDED_BY(mu_) = 1;
    uint32_t next_blob_id_ ABSL_GUARDED_BY(mu_) = 1;
    std::unordered_map<uint32_t, std::shared_ptr<AsgConsumerEntry>> by_handle_ ABSL_GUARDED_BY(mu_);
    std::unordered_map<uint32_t, std::shared_ptr<AsgConsumerEntry>> by_blob_id_
            ABSL_GUARDED_BY(mu_);
    // Closed while the GPU was quiesced; destroyed by DrainRetired().
    std::vector<std::shared_ptr<AsgConsumerEntry>> retired_ ABSL_GUARDED_BY(mu_);
};

}  // namespace goldfish::legacy
