// Copyright (C) 2026 The Android Open Source Project
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
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"

#include "android/emulation/control/ev_dev_event.h"
#include "contact_table.h"
#include "goldfish/base/unique_handle.h"
#include "goldfish/eventing/event_sources.h"

namespace goldfish::grpc::v2 {

/**
 * @brief The guest touch input device of one physical display.
 *
 * ### Thread Safety:
 * Called only on the QEMU event loop.
 */
class TouchDeviceSink {
  public:
    virtual ~TouchDeviceSink() = default;

    /** @brief Whether the guest driver is active, so a written report reaches it. */
    virtual bool IsActive() const = 0;

    /**
     * @brief A counter that changes whenever the guest driver goes active or
     * inactive: driver ready, driver or device reset, re-probe, snapshot load.
     * A change means the guest forgot the device's contacts.
     */
    virtual uint64_t ActiveGeneration() const = 0;

    /**
     * @brief Writes `events` followed by `SYN_REPORT` to the device,
     * synchronously and as one report.
     */
    virtual void WriteReport(
            absl::Span<const ::android::emulation::control::EvDevEvent> events) = 0;
};

using ActiveChangedEventSource =
        ::android::base::eventing::WithCallbacks<::android::base::eventing::EventSource<
                TouchDeviceId,
                ::android::base::eventing::HybridStoragePolicy<
                        TouchDeviceId, 16,
                        std::shared_ptr<::android::base::eventing::EventListener<TouchDeviceId>>>>>;

struct ActiveChangedListenerDeleter {
    struct Empty {};
    explicit ActiveChangedListenerDeleter(Empty = {}) noexcept {}
    explicit ActiveChangedListenerDeleter(std::weak_ptr<ActiveChangedEventSource> source) noexcept
            : source(std::move(source)) {}

    void operator()(size_t id) const;

    std::weak_ptr<ActiveChangedEventSource> source;
};

/**
 * Move-only RAII subscription handle for a `TouchDevices` active-generation
 * listener. Safe to destroy before or after `TouchDevices` is destroyed.
 */
using ScopedActiveChangedListener =
        ::goldfish::base::UniqueHandle<size_t, 0, ActiveChangedListenerDeleter>;

/**
 * @brief Maps displays to the touch devices that receive their input.
 *
 * ### Thread Safety:
 * `Resolve()` and `Find()` are called only on the QEMU event loop.
 * `AddActiveChangedListener()` is thread-safe, and registered callbacks may
 * fire on a vCPU/QEMU thread when a bound touch device's active generation
 * changes.
 *
 * ### Ownership:
 * A sink returned by `Find()` stays valid for as long as the caller uses it;
 * sinks are installed per display when the display is created.
 */
class TouchDevices {
  public:
    virtual ~TouchDevices() = default;

    /**
     * @brief Maps a requested display to the physical device that receives its
     * touches; a foldable may remap display 0 to 1 depending on its posture.
     * Called once when a contact is created.
     */
    virtual absl::StatusOr<TouchDeviceId> Resolve(int32_t display_id) = 0;

    /** @return The sink of `device`, or `nullptr` if there is none. */
    virtual TouchDeviceSink* Find(TouchDeviceId device) = 0;

    /**
     * @return The IDs of all existing touch devices, whether or not any session
     *     has touched them.
     */
    virtual std::vector<TouchDeviceId> Devices() = 0;

    /**
     * @brief Subscribes `listener` to push notifications fired whenever a bound
     * touch device advances its active generation.
     *
     * Must be non-blocking and must not reset its own
     * `ScopedActiveChangedListener` from inside `listener`. Thread-safe; the
     * returned handle may outlive `TouchDevices`.
     */
    [[nodiscard]] ScopedActiveChangedListener AddActiveChangedListener(
            ActiveChangedEventSource::EventCallback listener);

  protected:
    const std::shared_ptr<ActiveChangedEventSource>& active_changed_events() const {
        return active_changed_events_;
    }
    void NotifyActiveChanged(TouchDeviceId device) { active_changed_events_->FireEvent(device); }

  private:
    std::shared_ptr<ActiveChangedEventSource> active_changed_events_ =
            std::make_shared<ActiveChangedEventSource>();
};

}  // namespace goldfish::grpc::v2
