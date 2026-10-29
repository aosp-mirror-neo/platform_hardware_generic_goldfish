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

#include <memory>

#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/display/pixman_display.h"
#include "goldfish/display/virtio_input_tracker.h"

extern "C" {
#include "pixman.h"
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"

using VirtIOInputHID = struct VirtIOInputHID;
}
#include "virtio_bridge.h"

namespace goldfish::display {

class QemuDisplay : public PixmanDisplay {
    struct Private {};

  public:
    static absl::StatusOr<std::shared_ptr<QemuDisplay>> Create(EventLoop* loop,
                                                               EventLoop* qemu_loop,
                                                               QemuConsole* con, DisplaySurface* ds,
                                                               int index);
    QemuDisplay(Private, EventLoop* loop, EventLoop* qemu_loop, QemuConsole* con,
                DisplaySurface* ds, int index, ::VirtIOInputHID* vhid);
    ~QemuDisplay() override;

    bool IsActive() const;
    void SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) override;
    void SendEvDevReport(EvDevEvents events) override;
    void WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) override;
    bool IsInputActive() const override;
    uint64_t InputActiveGeneration() const override;

    void SetOwnedSurface(DisplaySurface* surface);

    QemuConsole* GetConsole() const override { return console_; }

    void OnSurfaceSwitch(DisplaySurface* surface);
    void OnGuestFrame(DisplaySurface* surface);
    void OnDeviceReset();

  protected:
    // QEMU loop: the legacy source's hooks. Refresh transfers the scanout
    // into the console surface.
    void RefreshSource() override;
    // (Un)registers the DisplayChangeListener.
    void OnSourceDemandChanged(bool warm) override;
    IFrameSource::Start CaptureSource(LegacySurfaceSource& source) override;

  private:

    // The console's surface, or the last one QEMU gave us.
    DisplaySurface* LegacySurface() const;

    template <typename Sink>
    friend void AbslStringify(Sink&, const QemuDisplay&);
    QemuConsole* console_;
    EventLoop* qemu_loop_;
    ::VirtIOInputHID* vhid_ = nullptr;
    VirtioInputActiveTracker input_active_tracker_;
    absl::Mutex send_lock_;
    std::unique_ptr<DisplayChangeListener> dcl_;
    DisplaySurface* owned_surface_{nullptr};
    DisplaySurface* current_surface_{nullptr};
    Dimensions current_size_{0, 0};
    bool placeholder_shown_{false};
};

template <typename Sink>
void AbslStringify(Sink& sink, const QemuDisplay& display) {
    absl::Format(&sink, "QemuDisplay: %s, con: %p", display.String(), display.console_);
}

}  // namespace goldfish::display
