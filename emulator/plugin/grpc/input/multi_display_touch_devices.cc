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
#include "multi_display_touch_devices.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"

#include "android/emulation/control/ev_dev_event.h"
#include "android/status/status_macros.h"
#include "goldfish/display/display.h"

namespace goldfish::grpc::v2 {

using ::android::emulation::control::EvDevEvent;
using ::goldfish::display::DisplayId;
using ::goldfish::display::DisplayPtr;
using ::goldfish::display::IMultiDisplay;
using ::goldfish::display::SharedDisplay;

// The input device of one display, reached through a weak reference so that a
// removed display is inactive rather than kept alive.
class MultiDisplayTouchDevices::DisplaySink final : public TouchDeviceSink {
  public:
    DisplaySink(TouchDeviceId device, std::weak_ptr<ActiveChangedEventSource> events)
            : device_(device), events_(std::move(events)) {}

    void Bind(DisplayPtr display) {
        if (!display_.owner_before(display) && !display.owner_before(display_)) {
            return;
        }
        active_subscription_.reset();
        display_ = std::move(display);
        if (const SharedDisplay locked = display_.lock()) {
            const TouchDeviceId device = device_;
            std::weak_ptr<ActiveChangedEventSource> events = events_;
            active_subscription_ = locked->AddInputActiveListener([device, events](uint64_t) {
                if (auto sink = events.lock()) {
                    sink->FireEvent(device);
                }
            });
        }
    }

    bool IsActive() const override {
        const SharedDisplay display = display_.lock();
        return display != nullptr && display->IsInputActive();
    }

    uint64_t ActiveGeneration() const override {
        const SharedDisplay display = display_.lock();
        return display != nullptr ? display->InputActiveGeneration() : 0;
    }

    void WriteReport(absl::Span<const EvDevEvent> events) override {
        if (const SharedDisplay display = display_.lock()) {
            display->WriteEvDevReportOnLoop(events);
        }
    }

  private:
    const TouchDeviceId device_;
    std::weak_ptr<ActiveChangedEventSource> events_;
    DisplayPtr display_;
    ::goldfish::display::ScopedInputActiveListener active_subscription_;
};

MultiDisplayTouchDevices::MultiDisplayTouchDevices(IMultiDisplay& multi_display, bool has_hinge)
        : multi_display_(multi_display), has_hinge_(has_hinge) {}

MultiDisplayTouchDevices::~MultiDisplayTouchDevices() = default;

absl::StatusOr<TouchDeviceId> MultiDisplayTouchDevices::Resolve(int32_t display_id) {
    if (display_id < 0) {
        return absl::InvalidArgumentError(
                absl::StrCat("Failed to resolve touch device for display_id=", display_id,
                             ": display_id must be non-negative (>= 0)"));
    }
    ASSIGN_OR_RETURN(
            const SharedDisplay display,
            multi_display_.GetActiveDisplay(static_cast<DisplayId>(display_id), has_hinge_));
    return TouchDeviceId{display->Id()};
}

TouchDeviceSink* MultiDisplayTouchDevices::Find(TouchDeviceId device) {
    absl::StatusOr<DisplayPtr> display = multi_display_.GetDisplay(device);
    if (!display.ok() || display->expired()) {
        return nullptr;
    }
    std::unique_ptr<DisplaySink>& sink = sinks_[device];
    if (sink == nullptr) {
        sink = std::make_unique<DisplaySink>(device, active_changed_events());
    }
    sink->Bind(*std::move(display));
    return sink.get();
}

std::vector<TouchDeviceId> MultiDisplayTouchDevices::Devices() {
    std::vector<TouchDeviceId> devices;
    for (DisplayId id = 0; id < IMultiDisplay::kMaxDisplays; ++id) {
        absl::StatusOr<DisplayPtr> display = multi_display_.GetDisplay(id);
        if (display.ok() && !display->expired()) {
            devices.push_back(TouchDeviceId{id});
        }
    }
    return devices;
}

}  // namespace goldfish::grpc::v2
