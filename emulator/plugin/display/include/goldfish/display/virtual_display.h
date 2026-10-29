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

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/display/display.h"
#include "goldfish/display/frame_producer.h"
#include "goldfish/display/frame_source.h"
#include "goldfish/display/legacy_surface_source.h"
#include "goldfish/display/virtio_input_tracker.h"

extern "C" {
#include "pixman.h"
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"

using VirtIOInputHID = struct VirtIOInputHID;
}

namespace goldfish::display {

class VirtualDisplay : public IDisplay {
    struct Private {};

  public:
    static absl::StatusOr<std::shared_ptr<VirtualDisplay>> Create(EventLoop* loop, EventLoop* qloop,
                                                                  uint8_t id, uint32_t width,
                                                                  uint32_t height, uint32_t dpi,
                                                                  uint32_t flags);
    VirtualDisplay(Private, EventLoop* loop, EventLoop* qloop, uint8_t id, uint32_t width,
                   uint32_t height, uint32_t dpi, uint32_t flags, ::VirtIOInputHID* vhid);

    ~VirtualDisplay() override;

    std::pair<int, int> ResizeKeepAspectRatio(int desired_width, int desired_height) override;

    void SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) override;
    void SendEvDevReport(EvDevEvents events) override;
    void WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) override;
    bool IsInputActive() const override;
    uint64_t InputActiveGeneration() const override;

    void Disconnect() override;

    void SimulateGuestFrameUpdate();

  protected:
    void OnDemand(bool warm) override;
    void OnFrameRequested() override;

  private:

    IFrameSource::Start CaptureSurface(LegacySurfaceSource& source);

    std::atomic<bool> disconnected_{false};
    mutable absl::Mutex buffer_lock_;
    std::vector<uint8_t> frame_buffer_ ABSL_GUARDED_BY(buffer_lock_);
    // Declared before legacy_, which sends it re-capture requests.
    const std::shared_ptr<FrameProducer> frame_producer_;
    const std::shared_ptr<LegacySurfaceSource> legacy_;
    std::shared_ptr<EventLoop::Timer> poll_timer_;

    EventLoop* qemu_loop_;
    ::VirtIOInputHID* vhid_;
    VirtioInputActiveTracker input_active_tracker_;
    absl::Mutex send_lock_;
};

}  // namespace goldfish::display
