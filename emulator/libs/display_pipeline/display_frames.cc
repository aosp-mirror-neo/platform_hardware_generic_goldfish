// Copyright 2025 The Android Open Source Project
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
#include "goldfish/display/display_frames.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/perfetto/perfetto_categories.h"

namespace goldfish::display {

// One listener. Callbacks run on the display loop only, so at most one runs at
// a time.
class DisplayFrames::Listener {
  public:
    // @param subscribed_after The latest frame id when it subscribed; it only
    //        gets newer events, plus catch-ups.
    Listener(uint64_t id, FrameListener callback, FrameId subscribed_after)
            : id_(id), callback_(std::move(callback)), subscribed_after_(subscribed_after) {}

    uint64_t Id() const { return id_; }

    // Display loop only. Calls the listener unless it is inactive or has
    // already seen `event` (or a newer one) with the same rotation. A catch-up
    // re-delivers the current frame's event to listeners that haven't seen it.
    void Deliver(const FrameEvent& event, bool catch_up) ABSL_LOCKS_EXCLUDED(listener_mutex_) {
        {
            absl::MutexLock lock(&listener_mutex_);
            if (!active_ || event.frame_id < last_delivered_) return;
            if (event.frame_id == last_delivered_ && last_rotation_ == event.rotation) return;
            if (!catch_up && event.frame_id <= subscribed_after_ &&
                event.frame_id != last_delivered_) {
                return;
            }
            last_delivered_ = event.frame_id;
            last_rotation_ = event.rotation;
            running_on_ = std::this_thread::get_id();
        }
        callback_(event);
        absl::MutexLock lock(&listener_mutex_);
        running_on_ = std::thread::id();
    }

    // Stops all future calls and waits for a running one, unless it is
    // running on this thread (the listener unsubscribing itself).
    void Deactivate() ABSL_LOCKS_EXCLUDED(listener_mutex_) {
        absl::MutexLock lock(&listener_mutex_);
        active_ = false;
        const std::thread::id self = std::this_thread::get_id();
        auto idle = [this, self]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(listener_mutex_) {
            return running_on_ == std::thread::id() || running_on_ == self;
        };
        listener_mutex_.Await(absl::Condition(&idle));
    }

  private:
    const uint64_t id_;
    const FrameListener callback_;
    const FrameId subscribed_after_;
    absl::Mutex listener_mutex_;
    bool active_ ABSL_GUARDED_BY(listener_mutex_) = true;
    FrameId last_delivered_ ABSL_GUARDED_BY(listener_mutex_) = 0;
    std::optional<ImageRotation> last_rotation_ ABSL_GUARDED_BY(listener_mutex_);
    std::thread::id running_on_ ABSL_GUARDED_BY(listener_mutex_);  // Default: none running.
};

absl::Status SurfaceFrame::CheckAvailable() const {
    if (reset_epoch_->load() != epoch_) {
        return absl::UnavailableError("The display was reset; this frame is gone");
    }
    return absl::OkStatus();
}

absl::Status SurfaceFrame::ReadInto(const ReadRequest& request, const ReadTarget& dst) const {
    if (absl::Status status = CheckAvailable(); !status.ok()) {
        return status;
    }
    TRACE_EVENT("display", "SurfaceFrame::ReadInto", "seq", event_.frame_id);
    return lease_->ReadInto(request, dst);
}

absl::StatusOr<Dimensions> SurfaceFrame::ReadInto(
        const ReadRequest& request, ReadFormat format,
        absl::FunctionRef<absl::StatusOr<uint8_t*>(const ReadLayout& layout)> allocator) const {
    if (absl::Status status = CheckAvailable(); !status.ok()) {
        return status;
    }
    TRACE_EVENT("display", "SurfaceFrame::ReadInto", "seq", event_.frame_id);
    return lease_->ReadInto(request, format, allocator);
}

void FrameListenerDeleter::operator()(uint64_t id) const {
    if (const std::shared_ptr<DisplayFrames> target = frames.lock()) {
        target->RemoveListener(id);
    }
}

std::shared_ptr<DisplayFrames> DisplayFrames::Create(async::EventLoop* loop, uint8_t display_id,
                                                     Dimensions configured_size) {
    return std::shared_ptr<DisplayFrames>(new DisplayFrames(loop, display_id, configured_size));
}

DisplayFrames::DisplayFrames(async::EventLoop* loop, uint8_t display_id, Dimensions configured_size)
        : loop_(loop)
        , display_id_(display_id)
        , reset_epoch_(std::make_shared<std::atomic<uint64_t>>(0))
        , configured_size_(configured_size)
        , cool_down_timer_(loop_->CreateTimer([this] {
            CoolDown();
            return true;
        })) {}

DisplayFrames::~DisplayFrames() {
    reset_epoch_->fetch_add(1);
}

void DisplayFrames::SetProducer(std::weak_ptr<IFrameProducer> producer) {
    std::weak_ptr<IFrameProducer> old;
    bool stop_old = false;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_) return;
        old = std::exchange(producer_, std::move(producer));
        stop_old = std::exchange(capturing_, false);
        warm_ = false;
        needs_frame_on_demand_ = false;
        ++cool_down_generation_;
        if (demand_ > 0) post_update_demand_ = true;
    }
    if (stop_old) {
        // SetDemand runs on the display loop only. Post before PostPendingTasks
        // so the old producer is stopped before the new one is started.
        absl::Status status = loop_->Post([old = std::move(old)] {
            if (std::shared_ptr<IFrameProducer> producer = old.lock()) producer->SetDemand(false);
        });
        if (!status.ok()) LOG(WARNING) << "Cannot stop the old producer: " << status;
    }
    PostPendingTasks();
}

void DisplayFrames::SetConfiguredSize(Dimensions size) {
    absl::MutexLock lock(&frames_mutex_);
    configured_size_ = size;
}

void DisplayFrames::SetRotation(ImageRotation rotation) {
    FramePtr replaced;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_ || rotation_ == rotation) return;
        rotation_ = rotation;
        if (latest_ != nullptr) {
            FrameEvent event = latest_->event();
            event.rotation = rotation;
            EnqueueLocked({.event = event});
            replaced = std::exchange(latest_,
                                     FramePtr(new SurfaceFrame(event, latest_->lease_, reset_epoch_,
                                                               latest_->epoch_)));
        }
    }
    replaced.reset();
    PostPendingTasks();
}

ImageRotation DisplayFrames::GetRotation() const {
    absl::MutexLock lock(&frames_mutex_);
    return rotation_;
}

ScopedFrameListener DisplayFrames::AddFrameListener(FrameListener listener) {
    uint64_t id = 0;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_) return {};
        id = ++last_listener_id_;
        listeners_.push_back(std::make_shared<Listener>(id, std::move(listener), last_frame_id_));
        AddDemandLocked(1);
        if (warm_ && latest_ != nullptr) {
            EnqueueLocked({.type = QueueEntry::Type::kCatchUp, .event = latest_->event()});
        }
    }
    PostPendingTasks();
    return ScopedFrameListener(id, FrameListenerDeleter(weak_from_this()));
}

void DisplayFrames::RemoveListener(uint64_t id) {
    std::shared_ptr<Listener> listener;
    {
        absl::MutexLock lock(&frames_mutex_);
        auto it = std::find_if(listeners_.begin(), listeners_.end(),
                               [id](const std::shared_ptr<Listener>& l) { return l->Id() == id; });
        if (it == listeners_.end()) return;  // Released by Remove.
        listener = std::move(*it);
        listeners_.erase(it);
        AddDemandLocked(-1);
    }
    PostPendingTasks();
    listener->Deactivate();
}

LatestFrame DisplayFrames::AcquireLatestFrame() const {
    absl::MutexLock lock(&frames_mutex_);
    return LatestFrame{.frame = latest_, .warm = warm_, .removed = removed_};
}

absl::StatusOr<FramePtr> DisplayFrames::WaitForFreshFrame(absl::Time deadline) const {
    auto ready = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(frames_mutex_) {
        return removed_ || (warm_ && latest_ != nullptr);
    };
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_) return absl::NotFoundError("The display was removed");
        if (ready()) return FramePtr(latest_);
        AddDemandLocked(1);
        if (capturing_ && needs_frame_on_demand_) post_update_demand_ = true;
    }
    PostPendingTasks();  // Before blocking: it tells the producer.

    absl::StatusOr<FramePtr> result;
    {
        absl::MutexLock lock(&frames_mutex_);
        frames_mutex_.AwaitWithDeadline(absl::Condition(&ready), deadline);
        AddDemandLocked(-1);
        if (removed_) {
            result = absl::NotFoundError("The display was removed");
        } else if (!ready()) {
            result = absl::DeadlineExceededError("The display did not warm up in time");
        } else {
            result = FramePtr(latest_);
        }
    }
    PostPendingTasks();
    return result;
}

absl::StatusOr<FramePtr> DisplayFrames::WaitForFrame(absl::Time deadline, FrameId after) const {
    absl::MutexLock lock(&frames_mutex_);
    auto ready = [this, after]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(frames_mutex_) {
        return removed_ || (latest_ != nullptr && latest_->event().frame_id > after);
    };
    if (!frames_mutex_.AwaitWithDeadline(absl::Condition(&ready), deadline)) {
        return absl::DeadlineExceededError("No newer frame in time");
    }
    if (removed_) return absl::NotFoundError("The display was removed");
    return FramePtr(latest_);
}

FrameId DisplayFrames::CurrentFrameId() const {
    absl::MutexLock lock(&frames_mutex_);
    return last_frame_id_;
}

void DisplayFrames::Remove() {
    FramePtr latest;  // Destroyed after the lock is released.
    std::weak_ptr<IFrameProducer> producer;
    bool stop_producer = false;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_) return;
        removed_ = true;
        reset_epoch_->fetch_add(1);  // Every frame reads UNAVAILABLE from now on.
        latest = std::move(latest_);
        producer = std::exchange(producer_, {});
        stop_producer = std::exchange(capturing_, false);
        warm_ = false;
        needs_frame_on_demand_ = false;
        ++cool_down_generation_;
        EnqueueLocked({.event = {.frame_id = ++last_frame_id_,
                                 .rotation = rotation_,
                                 .display_id = display_id_}});
        EnqueueLocked({.type = QueueEntry::Type::kReleaseListeners});
    }
    if (stop_producer) {
        if (std::shared_ptr<IFrameProducer> live = producer.lock()) live->SetDemand(false);
    }
    latest.reset();
    DispatchQueue();
}

void DisplayFrames::PublishFrame(SlotLease lease, absl::Time capture_time) {
    FramePtr replaced;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (!removed_) {  // Else the lease is destroyed on return, without the lock.
            replaced = PublishLocked(std::move(lease), /*blank=*/false, capture_time);
            published_since_reset_ = true;
            needs_frame_on_demand_ = false;
            if (capturing_) warm_ = true;
        }
    }
    replaced.reset();
    PostPendingTasks();
}

void DisplayFrames::OnProducerReset() {
    FramePtr replaced;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_) return;
        warm_ = false;
        if (capturing_) needs_frame_on_demand_ = true;
        if (!std::exchange(published_since_reset_, false)) return;  // Idempotent.
        reset_epoch_->fetch_add(1);
        const Dimensions size = latest_ != nullptr ? latest_->dimensions() : configured_size_;
        replaced = PublishLocked(SlotLease::Blank(size), /*blank=*/true, absl::InfinitePast());
    }
    replaced.reset();
    PostPendingTasks();
}

void DisplayFrames::PublishNoContent() {
    FramePtr replaced;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_) return;
        const bool blank_since_reset = latest_ != nullptr && latest_->event().blank &&
                                       latest_->epoch_ == reset_epoch_->load();
        const bool completes_warm_up = capturing_ && !warm_;
        if (!blank_since_reset) {
            const Dimensions size = latest_ != nullptr ? latest_->dimensions() : configured_size_;
            replaced = PublishLocked(SlotLease::Blank(size), /*blank=*/true, absl::InfinitePast());
        } else if (completes_warm_up) {
            // Listeners added while warming up start from the current frame.
            EnqueueLocked({.type = QueueEntry::Type::kCatchUp, .event = latest_->event()});
        }
        needs_frame_on_demand_ = false;
        if (capturing_) warm_ = true;
    }
    replaced.reset();
    PostPendingTasks();
}

FramePtr DisplayFrames::PublishLocked(SlotLease lease, bool blank, absl::Time capture_time) {
    const FrameEvent event{
        .capture_time = capture_time,
        .frame_id = ++last_frame_id_,
        .dimensions = lease.dimensions(),
        .rotation = rotation_,
        .display_id = display_id_,
        .blank = blank,
    };
    TRACE_EVENT_INSTANT("display", "IDisplay::FrameReceived",
                        perfetto::Flow::ProcessScoped(event.frame_id), "seq", event.frame_id,
                        "display_id", display_id_, "blank", event.blank);
    EnqueueLocked({.event = event});
    return std::exchange(latest_,
                         FramePtr(new SurfaceFrame(event, std::move(lease), reset_epoch_)));
}

void DisplayFrames::EnqueueLocked(QueueEntry entry) {
    queue_.push_back(std::move(entry));
    if (!dispatching_) post_dispatch_ = true;
}

void DisplayFrames::AddDemandLocked(int delta) const {
    const bool had_demand = demand_ > 0;
    demand_ += delta;
    if (had_demand != (demand_ > 0)) post_update_demand_ = true;
}

void DisplayFrames::PostPendingTasks() const {
    bool dispatch;
    bool update_demand;
    {
        absl::MutexLock lock(&frames_mutex_);
        dispatch = std::exchange(post_dispatch_, false);
        update_demand = std::exchange(post_update_demand_, false);
    }
    // const: tasks only hold a weak reference.
    const std::weak_ptr<DisplayFrames> weak =
            std::const_pointer_cast<DisplayFrames>(shared_from_this());
    if (update_demand) {
        absl::Status status = loop_->Post([weak] {
            if (std::shared_ptr<DisplayFrames> self = weak.lock()) self->UpdateDemand();
        });
        if (!status.ok()) LOG(WARNING) << "Cannot update demand: " << status;
    }
    if (dispatch) {
        absl::Status status = loop_->Post([weak] {
            if (std::shared_ptr<DisplayFrames> self = weak.lock()) self->DispatchQueue();
        });
        if (!status.ok()) LOG(WARNING) << "Cannot deliver frame events: " << status;
    }
}

void DisplayFrames::DispatchQueue() {
    {
        absl::MutexLock lock(&frames_mutex_);
        if (dispatching_) return;  // That call delivers ours too, in order.
        dispatching_ = true;
    }
    for (;;) {
        QueueEntry entry;
        std::vector<std::shared_ptr<Listener>> listeners;
        {
            absl::MutexLock lock(&frames_mutex_);
            if (queue_.empty()) {
                dispatching_ = false;
                post_dispatch_ = false;
                return;
            }
            entry = std::move(queue_.front());
            queue_.pop_front();
            if (entry.type == QueueEntry::Type::kReleaseListeners) {
                listeners = std::move(listeners_);
                listeners_.clear();
            } else {
                listeners = listeners_;
            }
        }
        for (const std::shared_ptr<Listener>& listener : listeners) {
            if (entry.type == QueueEntry::Type::kReleaseListeners) {
                listener->Deactivate();
            } else {
                listener->Deliver(entry.event, entry.type == QueueEntry::Type::kCatchUp);
            }
        }
    }
}

void DisplayFrames::UpdateDemand() {
    std::weak_ptr<IFrameProducer> producer;
    bool start = false;
    bool request_frame = false;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_ || producer_.expired()) return;
        producer = producer_;
        ++cool_down_generation_;
        if (demand_ > 0) {
            if (!capturing_) {
                capturing_ = true;
                warm_ = false;
                needs_frame_on_demand_ = false;
                start = true;
            } else if (needs_frame_on_demand_) {
                needs_frame_on_demand_ = false;
                request_frame = true;
            }
        } else if (capturing_) {
            armed_cool_down_generation_ = cool_down_generation_;
            cool_down_timer_->Schedule(kWarmTime);
            return;
        }
    }
    if (start) {
        if (std::shared_ptr<IFrameProducer> live = producer.lock()) live->SetDemand(true);
    } else if (request_frame) {
        if (std::shared_ptr<IFrameProducer> live = producer.lock()) live->RequestFrame();
    }
}

void DisplayFrames::CoolDown() {
    std::weak_ptr<IFrameProducer> producer;
    {
        absl::MutexLock lock(&frames_mutex_);
        if (removed_ || demand_ > 0 || !capturing_ || producer_.expired() ||
            cool_down_generation_ != armed_cool_down_generation_) {
            return;
        }
        capturing_ = false;
        warm_ = false;
        producer = producer_;
    }
    if (std::shared_ptr<IFrameProducer> live = producer.lock()) live->SetDemand(false);
}

}  // namespace goldfish::display
