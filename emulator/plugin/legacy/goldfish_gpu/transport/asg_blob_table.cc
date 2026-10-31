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

#include "goldfish/legacy/goldfish_gpu/transport/asg_blob_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <unistd.h>
#endif

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

namespace goldfish::legacy {
namespace {

constexpr uint32_t kAsgWriteStepSize = 262144;

}  // namespace

// ---------------------------------------------------------------------------
// Storage

void PageAlignedDeleter::operator()(uint8_t* ptr) const {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

PageAlignedBuffer AllocatePageAlignedStorage(size_t size) {
    if (size == 0) {
        return nullptr;
    }
    size_t align = kAsgStorageAlignment;
#if !defined(_WIN32)
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 &&
        (static_cast<size_t>(page_size) & (static_cast<size_t>(page_size) - 1)) == 0) {
        align = std::max(align, static_cast<size_t>(page_size));
    }
#endif
    void* raw = nullptr;
#if defined(_WIN32)
    raw = _aligned_malloc(size, align);
#else
    if (posix_memalign(&raw, align, size) != 0) {
        raw = nullptr;
    }
#endif
    if (raw == nullptr) {
        return nullptr;
    }
    std::memset(raw, 0, size);
    return PageAlignedBuffer(static_cast<uint8_t*>(raw));
}

// ---------------------------------------------------------------------------
// AsgConsumerEntry

void* AsgConsumerEntry::GetConsumer() {
    absl::MutexLock lock(&mu);
    return consumer;
}

void AsgConsumerEntry::SetConsumer(void* value) {
    absl::MutexLock lock(&mu);
    consumer = value;
}

void* AsgConsumerEntry::TakeConsumerForDestroy() {
    absl::MutexLock lock(&mu);
    exiting = true;
    return std::exchange(consumer, nullptr);
}

gfxstream::AsgOnUnavailableReadStatus AsgConsumerEntry::OnUnavailableRead() {
    auto has_work = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
        return exiting || pause_pre_snapshot_requested || resume_post_snapshot_requested ||
               wakeup_requested;
    };
    absl::MutexLock lock(&mu);
    mu.Await(absl::Condition(&has_work));
    if (exiting) {
        return gfxstream::AsgOnUnavailableReadStatus::kExit;
    }
    if (pause_pre_snapshot_requested) {
        pause_pre_snapshot_requested = false;
        return gfxstream::AsgOnUnavailableReadStatus::kPauseForSnapshot;
    }
    if (resume_post_snapshot_requested) {
        resume_post_snapshot_requested = false;
        return gfxstream::AsgOnUnavailableReadStatus::kResumeAfterSnapshot;
    }
    wakeup_requested = false;
    return gfxstream::AsgOnUnavailableReadStatus::kContinue;
}

// ---------------------------------------------------------------------------
// AsgBlobTable

AsgBlobTable::AsgBlobTable(std::function<void*(uint64_t)> resolve_gpa)
    : resolve_gpa_(std::move(resolve_gpa)) {}

AsgBlobTable::~AsgBlobTable() = default;

std::shared_ptr<AsgConsumerEntry> AsgBlobTable::Find(uint32_t handle) const {
    absl::MutexLock lock(&mu_);
    auto it = by_handle_.find(handle);
    return it == by_handle_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<AsgConsumerEntry>> AsgBlobTable::Registered() const {
    absl::MutexLock lock(&mu_);
    std::vector<std::shared_ptr<AsgConsumerEntry>> out;
    out.reserve(by_handle_.size());
    for (const auto& [handle, entry] : by_handle_) {
        out.push_back(entry);
    }
    return out;
}

std::vector<std::shared_ptr<AsgConsumerEntry>> AsgBlobTable::TakeRetired() {
    absl::MutexLock lock(&mu_);
    return std::exchange(retired_, {});
}

absl::StatusOr<AsgBlob> AsgBlobTable::Create(uint64_t size) {
    uint32_t handle = 0;
    {
        absl::MutexLock lock(&mu_);
        handle = next_handle_++;
    }
    return Restore(handle, size);
}

absl::StatusOr<AsgBlob> AsgBlobTable::Restore(uint32_t context_handle, uint64_t size) {
    if (context_handle == 0) {
        return absl::InvalidArgumentError("ASG blob handle 0 is reserved");
    }
    if (size <= kAsgRingRegionSize) {
        return absl::InvalidArgumentError(absl::StrCat("ASG blob size (", size,
                                                       ") must exceed ring region size (",
                                                       kAsgRingRegionSize, ")"));
    }
    auto entry = std::make_shared<AsgConsumerEntry>();
    entry->context_handle = context_handle;
    entry->size = size;
    entry->storage = AllocatePageAlignedStorage(static_cast<size_t>(size));
    if (!entry->storage) {
        return absl::ResourceExhaustedError("failed to allocate page-aligned ASG blob storage");
    }

    AsgBlob blob;
    blob.external_addr = entry->storage.get();
    blob.size = size;
    blob.context_handle = context_handle;
    {
        absl::MutexLock lock(&mu_);
        blob.blob_id = next_blob_id_++;
        entry->blob_id = blob.blob_id;
        next_handle_ = std::max(next_handle_, context_handle + 1);
        by_handle_[context_handle] = entry;
        by_blob_id_[blob.blob_id] = std::move(entry);
    }
    return blob;
}

absl::Status AsgBlobTable::Start(gfxstream::Renderer* renderer, const AsgBlob& blob) {
    std::shared_ptr<AsgConsumerEntry> entry = Find(blob.context_handle);
    if (!entry) {
        return absl::NotFoundError(
                absl::StrCat("ASG blob handle ", blob.context_handle, " not found"));
    }
    if (renderer == nullptr) {
        return absl::FailedPreconditionError("GoldfishGpu::StartAsg called before Start()");
    }
    if (entry->GetConsumer() != nullptr) {
        return absl::OkStatus();
    }
    void* consumer =
            renderer->addressSpaceGraphicsConsumerCreate(MakeCreateInfo(entry), /*loadStream=*/nullptr);
    if (consumer == nullptr) {
        return absl::InternalError("addressSpaceGraphicsConsumerCreate returned nullptr");
    }
    entry->SetConsumer(consumer);
    return absl::OkStatus();
}

absl::Status AsgBlobTable::Ping(const AsgBlob& blob) {
    std::shared_ptr<AsgConsumerEntry> entry = Find(blob.context_handle);
    if (!entry) {
        return absl::NotFoundError(
                absl::StrCat("ASG blob handle ", blob.context_handle, " not found"));
    }
    absl::MutexLock lock(&entry->mu);
    entry->wakeup_requested = true;
    return absl::OkStatus();
}

std::shared_ptr<AsgConsumerEntry> AsgBlobTable::Close(const AsgBlob& blob, bool retire) {
    std::shared_ptr<AsgConsumerEntry> entry;
    absl::MutexLock lock(&mu_);
    if (blob.blob_id != 0) {
        auto blob_it = by_blob_id_.find(blob.blob_id);
        if (blob_it != by_blob_id_.end()) {
            entry = std::move(blob_it->second);
            by_blob_id_.erase(blob_it);
        }
    }
    auto it = by_handle_.find(blob.context_handle);
    if (it != by_handle_.end() && (blob.blob_id == 0 || it->second->blob_id == blob.blob_id)) {
        if (!entry) {
            entry = std::move(it->second);
        }
        if (entry && entry->blob_id != 0) {
            by_blob_id_.erase(entry->blob_id);
        }
        by_handle_.erase(it);
    }
    if (entry && retire) {
        retired_.push_back(std::move(entry));
        return nullptr;
    }
    return entry;
}

absl::Status AsgBlobTable::LoadEntry(gfxstream::Renderer* renderer, uint32_t id,
                                     gfxstream::Stream* in) {
    std::shared_ptr<AsgConsumerEntry> entry = Find(id);
    if (!entry) {
        return absl::DataLossError(absl::StrCat("GoldfishGpu snapshot ASG entry refers to blob id ",
                                                id,
                                                " which goldfish_address_space did not restore"));
    }
    if (entry->GetConsumer() != nullptr) {
        return absl::DataLossError(
                absl::StrCat("GoldfishGpu snapshot ASG entry repeats blob id ", id));
    }
    void* consumer = renderer->addressSpaceGraphicsConsumerCreate(MakeCreateInfo(entry), in);
    entry->SetConsumer(consumer);
    if (consumer == nullptr) {
        return absl::DataLossError(
                absl::StrCat("addressSpaceGraphicsConsumerCreate failed on load for blob id ", id));
    }
    renderer->addressSpaceGraphicsConsumerRegisterPostLoadRenderThread(consumer);
    return absl::OkStatus();
}

void AsgBlobTable::PauseAll(gfxstream::Renderer& renderer) {
    for (const auto& entry : Registered()) {
        if (void* consumer = entry->GetConsumer(); consumer != nullptr) {
            renderer.addressSpaceGraphicsConsumerPreSave(consumer);
            absl::MutexLock lock(&entry->mu);
            entry->resume_post_snapshot_requested = false;
            entry->pause_pre_snapshot_requested = true;
        }
    }
}

void AsgBlobTable::WaitAllPaused(gfxstream::Renderer& renderer, gfxstream::Stream* sink) const {
    for (const auto& entry : Registered()) {
        if (void* consumer = entry->GetConsumer(); consumer != nullptr) {
            renderer.addressSpaceGraphicsConsumerSave(consumer, sink);
        }
    }
}

void AsgBlobTable::ResumeAll(gfxstream::Renderer& renderer) {
    for (const auto& entry : Registered()) {
        if (void* consumer = entry->GetConsumer(); consumer != nullptr) {
            {
                absl::MutexLock lock(&entry->mu);
                entry->pause_pre_snapshot_requested = false;
                entry->resume_post_snapshot_requested = true;
            }
            renderer.addressSpaceGraphicsConsumerPostSave(consumer);
        }
    }
}

void AsgBlobTable::ReloadRingConfigs(gfxstream::Renderer& renderer) {
    for (const auto& entry : Registered()) {
        if (void* consumer = entry->GetConsumer(); consumer != nullptr) {
            renderer.addressSpaceGraphicsConsumerReloadRingConfig(consumer);
        }
    }
}

std::vector<uint32_t> AsgBlobTable::SortedIds() const {
    std::vector<uint32_t> handles;
    for (const auto& entry : Registered()) {
        if (entry->GetConsumer() != nullptr) {
            handles.push_back(entry->context_handle);
        }
    }
    std::sort(handles.begin(), handles.end());
    return handles;
}

void AsgBlobTable::SaveEntry(gfxstream::Renderer* renderer, uint32_t id,
                             gfxstream::Stream* out) const {
    if (std::shared_ptr<AsgConsumerEntry> entry = Find(id)) {
        if (void* consumer = entry->GetConsumer(); consumer != nullptr) {
            renderer->addressSpaceGraphicsConsumerSave(consumer, out);
        }
    }
}

void AsgBlobTable::MarkAllExiting() {
    std::vector<std::shared_ptr<AsgConsumerEntry>> entries = Registered();
    {
        absl::MutexLock lock(&mu_);
        entries.insert(entries.end(), retired_.begin(), retired_.end());
    }
    for (const auto& entry : entries) {
        absl::MutexLock lock(&entry->mu);
        entry->exiting = true;
    }
}

void AsgBlobTable::StopAll(gfxstream::Renderer* renderer) {
    DestroyConsumers(renderer, TakeRetired());
    const std::vector<std::shared_ptr<AsgConsumerEntry>> live = Registered();
    DestroyConsumers(renderer, live);
    for (const auto& entry : live) {
        // The entry stays registered (goldfish_address_space may restore a
        // consumer for it); clear the flags for the next consumer.
        absl::MutexLock lock(&entry->mu);
        entry->exiting = false;
        entry->wakeup_requested = false;
        entry->pause_pre_snapshot_requested = false;
        entry->resume_post_snapshot_requested = false;
    }
}

void AsgBlobTable::DrainRetired(gfxstream::Renderer* renderer) {
    DestroyConsumers(renderer, TakeRetired());
}

std::vector<std::shared_ptr<AsgConsumerEntry>> AsgBlobTable::Reset() {
    absl::MutexLock lock(&mu_);
    std::vector<std::shared_ptr<AsgConsumerEntry>> out;
    for (auto& [handle, entry] : by_handle_) {
        out.push_back(std::move(entry));
    }
    by_handle_.clear();
    by_blob_id_.clear();
    for (auto& entry : retired_) {
        out.push_back(std::move(entry));
    }
    retired_.clear();
    next_handle_ = 1;
    return out;
}

void AsgBlobTable::DestroyConsumers(gfxstream::Renderer* renderer,
                                    const std::vector<std::shared_ptr<AsgConsumerEntry>>& entries) {
    for (const auto& entry : entries) {
        void* consumer = entry->TakeConsumerForDestroy();
        if (consumer != nullptr && renderer != nullptr) {
            renderer->addressSpaceGraphicsConsumerDestroy(consumer);
        }
    }
}

uint32_t AsgBlobTable::NextId() const {
    absl::MutexLock lock(&mu_);
    return next_handle_;
}

void AsgBlobTable::RaiseNextId(uint32_t value) {
    absl::MutexLock lock(&mu_);
    next_handle_ = std::max(next_handle_, value);
}

gfxstream::AsgConsumerCreateInfo AsgBlobTable::MakeCreateInfo(
        const std::shared_ptr<AsgConsumerEntry>& entry) {
    gfxstream::AsgConsumerCreateInfo create_info;
    create_info.version = 1;
    create_info.ring_storage = reinterpret_cast<char*>(entry->storage.get());
    create_info.buffer = reinterpret_cast<char*>(entry->storage.get() + kAsgRingRegionSize);
    create_info.buffer_size = static_cast<uint32_t>(entry->size - kAsgRingRegionSize);
    create_info.buffer_flush_interval = kAsgWriteStepSize;
    std::weak_ptr<AsgConsumerEntry> weak_entry = entry;
    create_info.callbacks.onUnavailableRead = [weak_entry]() {
        if (auto locked = weak_entry.lock()) {
            return locked->OnUnavailableRead();
        }
        return gfxstream::AsgOnUnavailableReadStatus::kExit;
    };
    create_info.callbacks.getPtr = [resolve = resolve_gpa_](uint64_t gpa) {
        return reinterpret_cast<char*>(resolve ? resolve(gpa) : nullptr);
    };
    return create_info;
}

AsgBlobTable::EntryView AsgBlobTable::MakeView(const std::shared_ptr<AsgConsumerEntry>& entry) {
    EntryView v;
    v.entry = entry.get();
    if (!entry) {
        return v;
    }
    v.context_handle = entry->context_handle;
    v.blob_id = entry->blob_id;
    v.size = entry->size;
    v.storage = entry->storage.get();
    absl::MutexLock lock(&entry->mu);
    v.consumer = entry->consumer;
    v.exiting = entry->exiting;
    v.wakeup_requested = entry->wakeup_requested;
    v.pause_pre_snapshot_requested = entry->pause_pre_snapshot_requested;
    v.resume_post_snapshot_requested = entry->resume_post_snapshot_requested;
    return v;
}

AsgBlobTable::View AsgBlobTable::Snapshot() const {
    View view;
    absl::MutexLock lock(&mu_);
    view.next_handle = next_handle_;
    view.next_blob_id = next_blob_id_;
    view.retired_count = retired_.size();
    view.registered.reserve(by_handle_.size());
    for (const auto& [handle, entry] : by_handle_) {
        view.registered.emplace_back(handle, MakeView(entry));
    }
    view.blobs.reserve(by_blob_id_.size());
    for (const auto& [blob_id, entry] : by_blob_id_) {
        view.blobs.emplace_back(blob_id, MakeView(entry));
    }
    return view;
}

}  // namespace goldfish::legacy
