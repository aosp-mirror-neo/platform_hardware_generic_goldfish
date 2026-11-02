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
#pragma once

// Consumer-facing frame hub for a single display: retains the latest published
// frame, dispatches FrameEvents to subscribers, tracks consumer demand with a
// warm retention window, fences stale frames across producer resets, and
// manages sticky display removal.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/base/unique_handle.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/geometry.h"

namespace goldfish::display {

/**
 * Immutable handle to a single published display frame.
 *
 * Architectural Role:
 * `SurfaceFrame` is the consumer's handle to a captured `SlotLease`. Multiple
 * consumers (e.g., WebRTC video encoding, gRPC screenshot streaming) may hold
 * `std::shared_ptr<const SurfaceFrame>` (`FramePtr`) to the same frame and call
 * `ReadInto()` concurrently with different crop, rotation, scale, and format
 * parameters.
 *
 * Invariants:
 * - Frame pixel content is immutable for the lifetime of the `SurfaceFrame`.
 * - Each frame records the display's reset epoch at publication time; if the
 *   producer subsequently resets or the display is removed, `ReadInto()`
 *   returns `UNAVAILABLE`.
 *
 * Thread Safety:
 * `event()`, `dimensions()`, and `rotation()` are safe on any thread.
 * `ReadInto()` is thread-safe and may be called concurrently from any consumer
 * worker thread, but must never be called on the QEMU main loop or the display
 * event loop.
 *
 * Ownership & Lifetimes:
 * Shared across consumers via `FramePtr`. Dropping the final `FramePtr`
 * reference destroys the underlying `SlotLease` and returns the slot to the
 * producer's `SlotPool` on whichever thread drops the reference.
 */
class SurfaceFrame final {
  public:
    ~SurfaceFrame() = default;

    SurfaceFrame(const SurfaceFrame&) = delete;
    SurfaceFrame& operator=(const SurfaceFrame&) = delete;

    /** @return Metadata event associated with this frame. */
    const FrameEvent& event() const { return event_; }
    /** @return Native (unrotated) dimensions of this frame. */
    Dimensions dimensions() const { return event_.dimensions; }
    /** @return Counter-clockwise display rotation associated with this frame. */
    ImageRotation rotation() const { return event_.rotation; }

    /**
     * Executes the read pipeline (`crop -> rotate -> scale -> convert`) into
     * `dst`.
     *
     * Synchronous; must not be called on the QEMU main loop or the display
     * event loop. A blank frame fills `dst` with opaque black. Reading the same
     * frame multiple times produces identical output bytes.
     *
     * @param request Desired crop, rotation, output dimensions, and YUV matrix.
     * @param dst Caller-owned destination planes.
     * @return `OK` on success, `INVALID_ARGUMENT` if `request` or `dst` does
     *         not fit this frame, or `UNAVAILABLE` if the producer was reset or
     *         the display was removed after this frame was published.
     */
    absl::Status ReadInto(const ReadRequest& request, const ReadTarget& dst) const;

    /**
     * Resolves `request` for `format`, invokes `allocator(layout)` to allocate a
     * contiguous destination buffer of `layout.total_bytes` bytes, reads the
     * transformed pixels into it, and returns the resolved output `Dimensions`.
     *
     * If this frame was invalidated by a reset or `request` is invalid for
     * `dimensions()`, returns `UNAVAILABLE` or `INVALID_ARGUMENT` without
     * invoking `allocator`.
     */
    absl::StatusOr<Dimensions> ReadInto(
            const ReadRequest& request, ReadFormat format,
            absl::FunctionRef<absl::StatusOr<uint8_t*>(const ReadLayout& layout)> allocator) const;

    /**
     * Resolves `request` for `Target`'s format (deduced at compile time from
     * `allocator`'s return type `absl::StatusOr<Target>`), invokes
     * `allocator(layout)` to obtain the destination `Target` (`Rgba8888Target`,
     * `Rgb888Target`, or `Nv12Target`), reads the transformed pixels into it,
     * and returns the resolved output `Dimensions`.
     *
     * If this frame was invalidated by a reset or `request` is invalid for
     * `dimensions()`, returns `UNAVAILABLE` or `INVALID_ARGUMENT` without
     * invoking `allocator`.
     */
    template <typename AllocFn,
              typename StatusOrTarget = std::invoke_result_t<AllocFn, const ReadLayout&>,
              typename Target = typename StatusOrTarget::value_type,
              typename = std::enable_if_t<std::is_same_v<Target, Rgba8888Target> ||
                                          std::is_same_v<Target, Rgb888Target> ||
                                          std::is_same_v<Target, Nv12Target>>>
    absl::StatusOr<Dimensions> ReadInto(const ReadRequest& request, AllocFn&& allocator) const {
        if (absl::Status status = CheckAvailable(); !status.ok()) {
            return status;
        }
        return lease_->ReadInto(request, std::forward<AllocFn>(allocator));
    }

  private:
    friend class DisplayFrames;

    SurfaceFrame(const FrameEvent& event, SlotLease lease,
                 std::shared_ptr<const std::atomic<uint64_t>> reset_epoch)
            : event_(event)
            , lease_(std::make_shared<const SlotLease>(std::move(lease)))
            , reset_epoch_(std::move(reset_epoch))
            , epoch_(reset_epoch_->load()) {}

    SurfaceFrame(const FrameEvent& event, std::shared_ptr<const SlotLease> lease,
                 std::shared_ptr<const std::atomic<uint64_t>> reset_epoch, uint64_t epoch)
            : event_(event)
            , lease_(std::move(lease))
            , reset_epoch_(std::move(reset_epoch))
            , epoch_(epoch) {}

    absl::Status CheckAvailable() const;

    const FrameEvent event_;
    const std::shared_ptr<const SlotLease> lease_;
    const std::shared_ptr<const std::atomic<uint64_t>> reset_epoch_;
    const uint64_t epoch_;
};

using FramePtr = std::shared_ptr<const SurfaceFrame>;

/**
 * Snapshot of a display's latest frame state returned by
 * `DisplayFrames::AcquireLatestFrame()`.
 */
struct LatestFrame {
    /** Latest published frame (may be blank); `nullptr` before the first frame or after removal. */
    FramePtr frame;
    /**
     * True if the producer is actively capturing for current demand and has
     * published at least one post-reset frame (or no-content confirmation).
     * When `false`, `frame` may be stale from an earlier warm period.
     */
    bool warm = false;
    /** Sticky flag indicating the display has been removed and will never publish again. */
    bool removed = false;
};

using FrameListener = std::function<void(const FrameEvent&)>;

class DisplayFrames;

/**
 * Deleter for `ScopedFrameListener` that unsubscribes the listener from its
 * `DisplayFrames` instance if the display still exists.
 */
struct FrameListenerDeleter {
    struct Empty {};
    explicit FrameListenerDeleter(Empty) noexcept {}
    explicit FrameListenerDeleter(std::weak_ptr<DisplayFrames> frames) noexcept
            : frames(std::move(frames)) {}

    void operator()(uint64_t id) const;

    std::weak_ptr<DisplayFrames> frames;
};

/**
 * Move-only RAII subscription handle for a `FrameListener`.
 *
 * Architectural Role:
 * Owns one active listener registration on a `DisplayFrames` instance. While
 * non-empty, it contributes one unit of consumer demand to keep the display's
 * producer warm.
 *
 * Thread Safety & Lifetimes:
 * Destroying or calling `reset()` unsubscribes the listener and blocks until
 * any in-progress callback on another thread finishes, guaranteeing no callback
 * runs after `reset()` returns. Calling `reset()` from inside the listener's
 * own callback is explicitly supported and does not deadlock. Never destroy or
 * reset a `ScopedFrameListener` while holding a mutex that its callback also
 * acquires.
 */
using ScopedFrameListener = goldfish::base::UniqueHandle<uint64_t, 0, FrameListenerDeleter>;

/**
 * Coordinates frame publication, consumer subscriptions, demand propagation,
 * and reset fencing for a single display.
 *
 * Architectural Role:
 * `DisplayFrames` is the `IFrameSink` that a display's `FrameProducer`
 * publishes into, and the backing implementation of `IDisplay`'s consumer frame
 * API.
 *
 * Concept & Data Flow:
 * - **Demand & Warmth (`kWarmTime`):** Subscribing via `AddFrameListener()` or
 *   blocking in `WaitForFreshFrame()` increments consumer demand and tells the
 *   producer `SetDemand(true)` on the display event loop. Once the producer
 *   publishes a frame (or `PublishNoContent()`), the display becomes `warm`.
 *   When demand drops to zero (e.g., between one-shot `getScreenshot` RPCs),
 *   `DisplayFrames` keeps the producer warm for `kWarmTime` (2 seconds) before
 *   calling `SetDemand(false)`, avoiding repeated cold-start latency for
 *   polling callers.
 * - **Catch-Up Events:** A listener added while the display is already `warm`
 *   immediately receives a catch-up `FrameEvent` for the current frame on the
 *   display loop. A listener added while warming up receives the frame that
 *   completes warm-up.
 * - **Reset Epoch Fencing:** `OnProducerReset()` clears warmth. If anything
 *   was published since the last reset, it also increments an atomic
 *   `reset_epoch_` shared with all published `SurfaceFrame`s, immediately
 *   making pre-reset frames return `UNAVAILABLE` on `ReadInto()`, and publishes
 *   a replacement blank frame; otherwise it publishes nothing.
 * - **Recapture After Reset:** While demand is held, the hub does not ask the
 *   producer for a new frame after `OnProducerReset()`; it only calls
 *   `IFrameProducer::RequestFrame()` when demand next rises from zero or a
 *   `WaitForFreshFrame()` caller starts waiting. Capturing again after a reset
 *   (e.g. by calling `RequestFrame()`) is the producer's or integrator's
 *   responsibility.
 *
 * Thread Safety:
 * Fully thread-safe. Listener callbacks, `IFrameProducer::SetDemand()`, and
 * `Remove()` execute on the display event loop (`loop_`). Internal locks are
 * never held while invoking listener callbacks, calling the producer, or
 * destroying a replaced `SlotLease`.
 */
class DisplayFrames final : public IFrameSink, public std::enable_shared_from_this<DisplayFrames> {
  public:
    /** Duration a display stays warm after its last consumer demand disappears. */
    static constexpr absl::Duration kWarmTime = absl::Seconds(2);

    /**
     * Creates a `DisplayFrames` instance bound to `loop`.
     *
     * @param loop Display event loop used to serialize listener callbacks and
     *        producer demand transitions; must outlive this object.
     * @param display_id Display identifier stamped into every `FrameEvent`.
     * @param configured_size Fallback dimensions used for blank frames before
     *        any real frame has been published.
     */
    static std::shared_ptr<DisplayFrames> Create(async::EventLoop* loop, uint8_t display_id,
                                                 Dimensions configured_size);
    ~DisplayFrames() override;

    DisplayFrames(const DisplayFrames&) = delete;
    DisplayFrames& operator=(const DisplayFrames&) = delete;

    /**
     * Binds or replaces the `IFrameProducer` that receives demand notifications.
     *
     * If a previous producer was capturing, `SetDemand(false)` is posted for it
     * on the display loop before starting the new producer. Held weakly because
     * the owning display manages both objects' lifetimes. Callable from any
     * thread.
     */
    void SetProducer(std::weak_ptr<IFrameProducer> producer);

    /**
     * Updates the fallback dimensions used when generating a blank frame before
     * the producer has published any frame.
     */
    void SetConfiguredSize(Dimensions size);

    /**
     * Updates the display's counter-clockwise rotation.
     *
     * Stamps `rotation` onto all subsequently published frames. If `rotation`
     * differs from the current rotation and a frame has already been published,
     * replaces the latest `SurfaceFrame` with one carrying the updated
     * `rotation` (sharing the underlying `SlotLease` and preserving `frame_id`)
     * and dispatches a `FrameEvent` to active listeners on the display loop.
     * Listeners that subscribed after that frame was published and never
     * received it (no catch-up, because the display was not warm) are skipped;
     * they get the next published frame.
     */
    void SetRotation(ImageRotation rotation);

    /**
     * @return Current counter-clockwise rotation of the display.
     */
    ImageRotation GetRotation() const;

    /**
     * Subscribes `listener` to receive `FrameEvent` notifications on the
     * display event loop.
     *
     * Creates one unit of consumer demand until the returned
     * `ScopedFrameListener` is reset or destroyed. Never invokes `listener`
     * inline. If the display is already warm, a catch-up event for the current
     * frame is queued for `listener`. Returns an empty `ScopedFrameListener` if
     * the display has already been removed.
     */
    [[nodiscard]] ScopedFrameListener AddFrameListener(FrameListener listener);

    /**
     * Returns a non-blocking O(1) snapshot of the latest published frame,
     * warmth state, and removal flag without creating demand or triggering a
     * capture.
     */
    LatestFrame AcquireLatestFrame() const;

    /**
     * Waits until the display is warm and returns the latest `SurfaceFrame`,
     * creating one unit of consumer demand while waiting.
     *
     * Blocking; must not be called on the display event loop or QEMU main loop.
     *
     * @param deadline Expiration time for the wait.
     * @return Non-null `FramePtr` (which may be blank), `NOT_FOUND` if the
     *         display is removed, or `DEADLINE_EXCEEDED` on timeout.
     */
    absl::StatusOr<FramePtr> WaitForFreshFrame(absl::Time deadline) const;

    /**
     * Waits until a frame with `frame_id > after` is published, without
     * creating consumer demand.
     *
     * Blocking; must not be called on the display event loop or QEMU main loop.
     *
     * @param deadline Expiration time for the wait.
     * @param after Frame sequence number that the returned frame must exceed.
     * @return Non-null `FramePtr` with `frame_id > after`, `NOT_FOUND` if the
     *         display is removed, or `DEADLINE_EXCEEDED` on timeout.
     */
    absl::StatusOr<FramePtr> WaitForFrame(absl::Time deadline, FrameId after) const;

    /**
     * @return Most recently assigned `FrameId`, or `0` before any frame is published.
     */
    FrameId CurrentFrameId() const;

    /**
     * Permanently marks the display as removed.
     *
     * Must be called on the display event loop (and not from inside a frame
     * listener callback). Stops and drops the producer, fences all previously
     * published frames so they return `UNAVAILABLE` on `ReadInto()`, delivers a
     * final `FrameEvent` with `dimensions == {0, 0}` to all active listeners,
     * deactivates all listeners, and wakes any blocked `WaitForFreshFrame` /
     * `WaitForFrame` callers with `NOT_FOUND`. Idempotent.
     */
    void Remove();

    // IFrameSink implementation.
    void PublishFrame(SlotLease lease, absl::Time capture_time) override;
    void OnProducerReset() override;
    void PublishNoContent() override;

  private:
    friend struct FrameListenerDeleter;
    class Listener;

    struct QueueEntry {
        enum class Type { kEvent, kCatchUp, kReleaseListeners };
        Type type = Type::kEvent;
        FrameEvent event;
    };

    DisplayFrames(async::EventLoop* loop, uint8_t display_id, Dimensions configured_size);

    // Publishes a new latest frame; returns the replaced one, to be destroyed
    // after the lock is released.
    FramePtr PublishLocked(SlotLease lease, bool blank, absl::Time capture_time)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(frames_mutex_);
    void EnqueueLocked(QueueEntry entry) ABSL_EXCLUSIVE_LOCKS_REQUIRED(frames_mutex_);
    // Adds or removes one unit of demand.
    void AddDemandLocked(int delta) const ABSL_EXCLUSIVE_LOCKS_REQUIRED(frames_mutex_);
    // Posts the tasks that EnqueueLocked and AddDemandLocked asked for.
    void PostPendingTasks() const ABSL_LOCKS_EXCLUDED(frames_mutex_);
    void RemoveListener(uint64_t id) ABSL_LOCKS_EXCLUDED(frames_mutex_);

    // Display loop tasks.
    void DispatchQueue() ABSL_LOCKS_EXCLUDED(frames_mutex_);
    void UpdateDemand() ABSL_LOCKS_EXCLUDED(frames_mutex_);
    void CoolDown() ABSL_LOCKS_EXCLUDED(frames_mutex_);

    async::EventLoop* const loop_;
    const uint8_t display_id_;
    // Read by frames, which may outlive this object. Written under frames_mutex_.
    const std::shared_ptr<std::atomic<uint64_t>> reset_epoch_;

    mutable absl::Mutex frames_mutex_;
    Dimensions configured_size_ ABSL_GUARDED_BY(frames_mutex_);
    ImageRotation rotation_ ABSL_GUARDED_BY(frames_mutex_) = ImageRotation::kRotation0;
    std::weak_ptr<IFrameProducer> producer_ ABSL_GUARDED_BY(frames_mutex_);
    FramePtr latest_ ABSL_GUARDED_BY(frames_mutex_);
    FrameId last_frame_id_ ABSL_GUARDED_BY(frames_mutex_) = 0;
    uint64_t last_listener_id_ ABSL_GUARDED_BY(frames_mutex_) = 0;
    bool published_since_reset_ ABSL_GUARDED_BY(frames_mutex_) = false;
    bool removed_ ABSL_GUARDED_BY(frames_mutex_) = false;
    // Demand: listeners plus WaitForFreshFrame calls.
    mutable int demand_ ABSL_GUARDED_BY(frames_mutex_) = 0;
    bool capturing_ ABSL_GUARDED_BY(frames_mutex_) = false;  // The producer has SetDemand(true).
    bool warm_ ABSL_GUARDED_BY(frames_mutex_) = false;
    bool needs_frame_on_demand_ ABSL_GUARDED_BY(frames_mutex_) = false;
    std::vector<std::shared_ptr<Listener>> listeners_ ABSL_GUARDED_BY(frames_mutex_);
    std::deque<QueueEntry> queue_ ABSL_GUARDED_BY(frames_mutex_);
    bool dispatching_ ABSL_GUARDED_BY(frames_mutex_) = false;
    // Tasks to post once the lock is released.
    mutable bool post_dispatch_ ABSL_GUARDED_BY(frames_mutex_) = false;
    mutable bool post_update_demand_ ABSL_GUARDED_BY(frames_mutex_) = false;
    uint64_t cool_down_generation_ ABSL_GUARDED_BY(frames_mutex_) = 0;
    uint64_t armed_cool_down_generation_ ABSL_GUARDED_BY(frames_mutex_) = 0;

    // Display loop only.
    async::EventLoop::ScopedTimer cool_down_timer_;
};

}  // namespace goldfish::display
