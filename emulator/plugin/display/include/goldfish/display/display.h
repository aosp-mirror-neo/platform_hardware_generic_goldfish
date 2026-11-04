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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/base/unique_handle.h"
#include "goldfish/display/display_frames.h"
#include "goldfish/display/ev_dev_event.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/geometry.h"
#include "goldfish/eventing/event_sources.h"
#include "goldfish/perfetto/perfetto_categories.h"

extern "C" {
struct QemuConsole;
}

namespace goldfish::display {

using ::goldfish::async::EventLoop;

enum class PixelFormat : std::uint8_t {
    // Portable Network Graphics format
    // (https://en.wikipedia.org/wiki/Portable_Network_Graphics)
    kPng = 0,

    // Three-channel RGB color model supplemented with a fourth alpha
    // channel. https://en.wikipedia.org/wiki/RGBA_color_model
    // Each pixel consists of 4 bytes.
    kRgba8888,

    // Three-channel RGB color model, each pixel consists of 3 bytes
    kRgb888,
};

enum class Orientation : std::uint8_t {
    kPortrait,
    kLandscape,
    kSquare,
};

/**
 * @struct FrameInfo
 * @brief  Metadata returned by IDisplay::GetPixels describing the retrieved frame.
 */
struct FrameInfo {
    Dimensions dimensions{0, 0};
    PixelFormat format = PixelFormat::kRgba8888;
    ImageRotation rotation = ImageRotation::kRotation0;
    uint64_t sequence_number = 0;  ///< Monotonically increasing frame sequence number.
    absl::Time timestamp;          ///< Timestamp when the frame was captured.

    explicit FrameInfo(uint64_t seq = 0) : sequence_number(seq), timestamp(absl::Now()) {}
};

class IDisplay;
using DisplayPtr = std::weak_ptr<IDisplay>;
using SharedDisplay = std::shared_ptr<IDisplay>;

using InputActiveEventSource =
        ::android::base::eventing::WithCallbacks<::android::base::eventing::EventSource<
                uint64_t,
                ::android::base::eventing::HybridStoragePolicy<
                        uint64_t, 16,
                        std::shared_ptr<::android::base::eventing::EventListener<uint64_t>>>>>;

struct InputActiveListenerDeleter {
    struct Empty {};
    explicit InputActiveListenerDeleter(Empty) noexcept {}
    explicit InputActiveListenerDeleter(std::weak_ptr<InputActiveEventSource> source) noexcept
            : source(std::move(source)) {}

    void operator()(size_t id) const;

    std::weak_ptr<InputActiveEventSource> source;
};

/**
 * Move-only RAII subscription handle for an input active-generation listener on
 * `IDisplay`. Safe to destroy before or after the `IDisplay` is destroyed.
 */
using ScopedInputActiveListener =
        ::goldfish::base::UniqueHandle<size_t, 0, InputActiveListenerDeleter>;

/**
 * @class IDisplay
 * @brief Interface representing an Android display.
 *
 *  This interface provides methods to get display properties,
 *  acquire immutable SurfaceFrames, subscribe to FrameEvents via DisplayFrames,
 *  and send virtio input events.
 */
class IDisplay : public std::enable_shared_from_this<IDisplay> {
  public:
    using ScopedFrameListener = ::goldfish::display::ScopedFrameListener;
    using ScopedInputActiveListener = ::goldfish::display::ScopedInputActiveListener;

    explicit IDisplay(EventLoop* loop) : IDisplay(loop, 0, 0, 0) {}
    virtual ~IDisplay() { StopProducerCalls(); }

    /**
     * @brief Returns the orientation of the given dimensions.
     * @param w Width
     * @param h Height
     * @return The Orientation (Portrait, Landscape, or Square).
     */
    static Orientation GetOrientation(int w, int h);

    /**
     * @brief Returns the unique identifier of this display.
     * @return The display ID (uint8_t).
     */
    uint8_t Id() const { return display_id_; }

    /**
     * @brief Returns the dimensions of the display.
     * @return The display dimensions (Dimensions).
     */
    Dimensions GetDimensions() const {
        const absl::MutexLock lock(&dimension_mutex_);
        return dimensions_;
    }

    uint32_t Dpi() const { return dpi_; }

    uint32_t Flags() const { return flags_; }

    /**
     * @brief Actively disconnects the display from underlying network/looper endpoints.
     */
    virtual void Disconnect() {}

    /**
     * Calculates new dimensions to fit a box while preserving aspect ratio.
     * The box dimensions (desired_width, desired_height) are logical dimensions,
     * which means they can be rotated relative to the physical display dimensions.
     * The returned dimensions will match the orientation of the requested box.
     *
     * If either desired_width or desired_height is 0, the function returns {0, 0}.
     * The returned dimensions will never exceed the dimensions of the display.
     *
     * @param desired_width The maximum logical width of the bounding box.
     * @param desired_height The maximum logical height of the bounding box.
     * @return A std::pair<int, int> containing the new logical width and height.
     */
    virtual std::pair<int, int> ResizeKeepAspectRatio(int desired_width, int desired_height);

    /**
     * @brief Adds a listener for frame updates.
     * @return An RAII ScopedFrameListener that automatically unregisters when destroyed.
     */
    [[nodiscard]] ScopedFrameListener AddFrameListener(FrameListener listener) {
        return frames_ ? frames_->AddFrameListener(std::move(listener)) : ScopedFrameListener{};
    }

    /**
     * @brief Acquires the latest published frame in O(1) without blocking.
     */
    LatestFrame AcquireLatestFrame() const {
        return frames_ ? frames_->AcquireLatestFrame() : LatestFrame{};
    }

    /**
     * @brief Waits until the display is warm and has a non-null frame, or until deadline.
     */
    absl::StatusOr<FramePtr> WaitForFreshFrame(absl::Time deadline) const {
        if (!frames_) {
            return absl::UnavailableError("Display has no frame source");
        }
        return frames_->WaitForFreshFrame(deadline);
    }

    /**
     * @brief Waits until a frame with frame_id > after is published, or until deadline.
     */
    absl::StatusOr<FramePtr> WaitForFrame(absl::Time deadline, FrameId after) const {
        if (!frames_) {
            return absl::UnavailableError("Display has no frame source");
        }
        return frames_->WaitForFrame(deadline, after);
    }

    /**
     * @brief Returns the last assigned FrameId on this display (0 if none).
     */
    FrameId CurrentFrameId() const { return frames_ ? frames_->CurrentFrameId() : 0; }

    /**
     * @brief Permanently marks the display as removed, invalidates frames, and wakes waiters.
     */
    void Remove() {
        if (frames_) {
            frames_->Remove();
        }
    }

    /**
     * @brief Retrieves pixel data for the display into a caller-provided buffer.
     *
     * Waits for a fresh frame via WaitForFreshFrame (with fallback to AcquireLatestFrame on
     * deadline exceeded), validates the buffer size against LayoutFor, and reads via ReadInto.
     */
    virtual absl::StatusOr<FrameInfo> GetPixels(PixelFormat fmt, int width, int height,
                                                ImageRotation rotation, uint8_t* pixel,
                                                size_t* c_pixels) const;

    /**
     * Sends a raw evdev event to the proper display.
     */
    virtual void SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) = 0;

    /**
     * @brief Sends one evdev report: `events` followed by EV_SYN/SYN_REPORT.
     *
     * The guest's virtio input device accumulates events until SYN_REPORT, so a
     * report must reach it whole. The display posts the report as a single task
     * to the event loop that writes to the device: `events` in order, then the
     * sync. Events from other senders cannot land inside the report.
     *
     * @param events The report's events, without the trailing SYN_REPORT. If
     *        empty, only the sync is sent.
     */
    virtual void SendEvDevReport(EvDevEvents events) = 0;

    /**
     * @brief Writes one evdev report, `events` followed by EV_SYN/SYN_REPORT,
     * synchronously.
     *
     * Unlike SendEvDevReport, nothing is posted: the report reaches the device
     * before this returns, in order with everything else the caller writes
     * from the loop. Must be called on the QEMU event loop thread.
     *
     * @param events The report's events, without the trailing SYN_REPORT. If
     *        empty, only the sync is written.
     */
    virtual void WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) = 0;

    /**
     * @brief Whether the guest driver of this display's input device is active,
     * so a written report would reach it. The device discards events while its
     * driver is inactive.
     *
     * Must be called on the QEMU event loop thread.
     */
    virtual bool IsInputActive() const = 0;

    /**
     * @brief A counter that changes whenever the guest driver of this
     * display's input device goes active or inactive: driver ready, driver or
     * device reset, re-probe, snapshot load. A change means the guest forgot
     * the device's touch contacts. 0 if the display has no input device.
     *
     * Must be called on the QEMU event loop thread.
     */
    virtual uint64_t InputActiveGeneration() const = 0;

    /**
     * @brief Subscribes `listener` to push notifications fired whenever this
     * display's input device advances its active generation (on a vCPU/QEMU
     * thread under the BQL).
     *
     * Must be non-blocking and must not reset its own
     * `ScopedInputActiveListener` from inside `listener`. Thread-safe; the
     * returned handle may outlive the `IDisplay`.
     */
    [[nodiscard]] ScopedInputActiveListener AddInputActiveListener(
            InputActiveEventSource::EventCallback listener);

    virtual QemuConsole* GetConsole() const { return nullptr; }

    void SetDimensions(Dimensions dim) {
        {
            const absl::MutexLock lock(&dimension_mutex_);
            dimensions_ = dim;
        }
        if (frames_ && dim.width > 0 && dim.height > 0) {
            frames_->SetConfiguredSize(dim);
        }
    }

    void SetDimensions(uint32_t width, uint32_t height) {
        SetDimensions({.width = width, .height = height});
    }

    /**
     * @brief Updates the display's counter-clockwise rotation and notifies active frame listeners.
     */
    void SetRotation(ImageRotation rotation) {
        if (frames_) {
            frames_->SetRotation(rotation);
        }
    }

    /**
     * @brief Returns the current counter-clockwise rotation of the display.
     */
    ImageRotation GetRotation() const {
        return frames_ ? frames_->GetRotation() : ImageRotation::kRotation0;
    }

    template <typename Sink>
    friend void AbslStringify(Sink& sink, const IDisplay* display) {
        absl::Format(&sink, "%s", display ? display->String().c_str() : "<none>");
    }

  protected:
    struct LogicalFit {
        int width;
        int height;
        bool swapped;
    };

    LogicalFit CalculateLogicalFit(int desired_width, int desired_height) const;

    virtual std::string String() const;

    IDisplay(EventLoop* loop, uint8_t id, uint32_t width, uint32_t height, uint32_t dpi = 0,
             uint32_t flags = 0)
            : dpi_(dpi)
            , flags_(flags)
            , display_id_(id)
            , loop_(loop)
            , dimensions_({.width = width, .height = height})
            , frames_(loop ? DisplayFrames::Create(loop, id, dimensions_) : nullptr)
            , producer_(std::make_shared<Producer>(this)) {
        if (frames_) {
            frames_->SetProducer(producer_);
        }
    }

    EventLoop* loop() const { return loop_; }
    // The sink the display's producer publishes into. Null without a loop.
    const std::shared_ptr<DisplayFrames>& frames() const { return frames_; }
    // Forwards IFrameProducer calls to OnDemand and OnFrameRequested.
    std::weak_ptr<IFrameProducer> weak_producer() const { return producer_; }
    const std::shared_ptr<InputActiveEventSource>& input_active_events() const {
        return input_active_events_;
    }
    void NotifyInputActiveChanged(uint64_t generation) {
        input_active_events_->FireEvent(generation);
    }

    // IFrameProducer::SetDemand and RequestFrame, as hooks. Called on the
    // threads IFrameProducer allows, never on a display being destroyed once
    // StopProducerCalls has returned.
    virtual void OnDemand(bool /*warm*/) {}
    virtual void OnFrameRequested() {}

    // Waits for a hook call in progress and stops new ones. Idempotent.
    // ~IDisplay calls it, but only after derived members are destroyed, so
    // the most-derived destructor of a display that overrides a hook must
    // call it first.
    void StopProducerCalls() { producer_->Detach(); }

    uint32_t dpi_{0};
    uint32_t flags_{0};
    const uint8_t display_id_;

  private:
    friend class DisplayTestPeer;

    // The display's IFrameProducer. A display owned by a shared_ptr is kept alive
    // for the duration of each call, and a call that starts once its destruction
    // has begun does nothing. A display never owned by a shared_ptr (a test
    // fixture) is called directly, and StopProducerCalls waits for calls in
    // progress.
    class Producer final : public IFrameProducer {
      public:
        explicit Producer(IDisplay* owner) : owner_(owner) {}

        // No call reaches the display after this returns. Idempotent.
        void Detach() ABSL_LOCKS_EXCLUDED(owner_mutex_) {
            const absl::MutexLock owner_lock(&owner_mutex_);
            owner_ = nullptr;
            owner_mutex_.Await(
                    absl::Condition(+[](int* calls) { return *calls == 0; }, &unowned_calls_));
        }

        void SetDemand(bool warm) override {
            Call([warm](IDisplay& display) { display.OnDemand(warm); });
        }
        void RequestFrame() override {
            Call([](IDisplay& display) { display.OnFrameRequested(); });
        }

      private:
        template <typename Hook>
        void Call(Hook hook) ABSL_LOCKS_EXCLUDED(owner_mutex_) {
            std::shared_ptr<IDisplay> owned;
            IDisplay* unowned = nullptr;
            {
                const absl::MutexLock owner_lock(&owner_mutex_);
                if (owner_ == nullptr) return;
                const std::weak_ptr<IDisplay> weak = owner_->weak_from_this();
                if (IsEmpty(weak)) {
                    unowned = owner_;
                    ++unowned_calls_;
                } else {
                    // Null once the last owner is gone, before any destructor runs.
                    owned = weak.lock();
                    if (owned == nullptr) return;
                }
            }
            if (owned != nullptr) {
                hook(*owned);
                return;  // If the display's owners let go meanwhile, it dies here.
            }
            hook(*unowned);
            const absl::MutexLock owner_lock(&owner_mutex_);
            --unowned_calls_;
        }

        // True if `weak` never referred to an object (not merely expired).
        static bool IsEmpty(const std::weak_ptr<IDisplay>& weak) {
            const std::weak_ptr<IDisplay> empty;
            return !weak.owner_before(empty) && !empty.owner_before(weak);
        }

        absl::Mutex owner_mutex_;
        IDisplay* owner_ ABSL_GUARDED_BY(owner_mutex_);
        int unowned_calls_ ABSL_GUARDED_BY(owner_mutex_) = 0;
    };

    EventLoop* loop_ = nullptr;
    Dimensions dimensions_ ABSL_GUARDED_BY(dimension_mutex_);
    mutable absl::Mutex dimension_mutex_;
    std::shared_ptr<DisplayFrames> frames_;
    std::shared_ptr<Producer> producer_;
    std::shared_ptr<InputActiveEventSource> input_active_events_ =
            std::make_shared<InputActiveEventSource>();
};

}  // namespace goldfish::display
