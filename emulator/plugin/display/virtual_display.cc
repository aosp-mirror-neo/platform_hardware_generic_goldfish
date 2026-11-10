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

#include "goldfish/display/virtual_display.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/avd/avd_info.h"
#include "goldfish/devices/multidisplay/multidisplay_device.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/cpu_slot_storage.h"
#include "goldfish/display/frame_producer.h"
#include "goldfish/display/frame_source.h"
#include "goldfish/display/input_handler.h"
#include "goldfish/display/legacy_surface_source.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qapi/error.h"
#include "qom/object.h"
}
#include "virtio_bridge.h"
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::display {

absl::StatusOr<std::shared_ptr<VirtualDisplay>> VirtualDisplay::Create(
        EventLoop* loop, EventLoop* qloop, uint8_t id, uint32_t width, uint32_t height,
        uint32_t dpi, uint32_t flags) {
    if (id == 0 || id >= IMultiDisplay::kMaxDisplays) {
        return absl::InvalidArgumentError(
                absl::StrCat("Invalid virtual display id: ", static_cast<int>(id), " (expected 1..",
                             IMultiDisplay::kMaxDisplays - 1, ")"));
    }
    if (width == 0 || height == 0 || width > IMultiDisplay::kMaxDisplayDimension ||
        height > IMultiDisplay::kMaxDisplayDimension) {
        return absl::InvalidArgumentError(
                absl::StrCat("Invalid virtual display dimensions: ", width, "x", height,
                             " (expected 1..", IMultiDisplay::kMaxDisplayDimension, ")"));
    }
    const char* gpu = "gpu0";
    // Head will normally be > 0 for virtual console
    const uint32_t head = id;
    VirtioDeviceInfo device_info{.display = gpu, .head = head};
    Object* objs = object_resolve_path_component(object_get_root(), "machine");
    if (!object_child_foreach_recursive(objs, ::find_virtio_device, &device_info)) {
        return absl::NotFoundError(
                absl::StrCat("Unable to find a virtio device for head: ", device_info.head,
                             " attached to display: ", device_info.display));
    }
    VLOG(1) << "display id " << static_cast<int>(head) << " vhid " << device_info.vhid;
    return std::make_shared<VirtualDisplay>(Private(), loop, qloop, id, width, height, dpi, flags,
                                            device_info.vhid);
}

VirtualDisplay::VirtualDisplay(Private, EventLoop* loop, EventLoop* qloop, uint8_t id,
                               uint32_t width, uint32_t height, uint32_t dpi, uint32_t flags,
                               ::VirtIOInputHID* vhid)
        : IDisplay(loop, id, width, height, dpi, flags)
        , frame_producer_(
                  std::make_shared<FrameProducer>(qloop != nullptr ? qloop : loop, frames()))
        , legacy_(LegacySurfaceSource::Create(
                  frame_producer_,
                  LegacySurfaceSource::Hooks{.capture =
                                                     [this](LegacySurfaceSource& source) {
                                                         return CaptureSurface(source);
                                                     }},
                  std::make_unique<CpuSlotStorage>()))
        , qemu_loop_(qloop)
        , vhid_(vhid)
        , input_active_tracker_(vhid, input_active_events()) {
    frame_producer_->SetSource(legacy_);
    legacy_->Resize({.width = width, .height = height});

    // Initialize our dummy framebuffer with a solid color (e.g., White RGBA)
    // In reality, this memory would be mapped to a virtio-gpu guest buffer.
    const size_t buffer_size = static_cast<size_t>(width) * height * 4;
    {
        const absl::MutexLock lock(&buffer_lock_);
        frame_buffer_.resize(buffer_size, 0xFF);
    }
    auto* avd = avd::GetNullableAvd();
    if (auto device = avd ? avd->GetActiveMultiDisplayDevice() : nullptr) {
        device->SendAddDisplay(id, width, height, dpi, flags);
    } else {
        LOG(WARNING) << "Dropped multidisplay message: Guest not connected.";
    }
    SimulateGuestFrameUpdate();
    if (loop != nullptr) {
        poll_timer_ = loop->CreateTimer([this] {
            if (!frame_producer_->warm() || disconnected_.load(std::memory_order_acquire)) {
                return false;
            }
            OnFrameRequested();
            return true;
        });
    }
}

void VirtualDisplay::Disconnect() {
    if (!disconnected_.exchange(true, std::memory_order_acq_rel)) {
        poll_timer_.reset();
        auto* avd = avd::GetNullableAvd();
        if (auto device = avd ? avd->GetActiveMultiDisplayDevice() : nullptr) {
            device->SendDelDisplay(display_id_);
        } else {
            LOG(WARNING) << "Dropped multidisplay message: Guest not connected.";
        }
    }
}

VirtualDisplay::~VirtualDisplay() {
    // WARNING: Do NOT trigger active network operations (like SendDelDisplay)
    // from this C++ destructor. Lingering std::shared_ptr references held by UI
    // windows or background queues can cause this destructor to run asynchronously
    // on non-network threads or post-unrealize when the AVD universe is offline.
    // Intentional network teardown must execute explicitly via Disconnect() instead.
    input_active_tracker_.Reset();
    StopProducerCalls();  // OnDemand uses poll_timer_.
    frame_producer_->Shutdown();
    poll_timer_.reset();
}

std::pair<int, int> VirtualDisplay::ResizeKeepAspectRatio(int /*desired_width*/,
                                                          int /*desired_height*/) {
    return {GetDimensions().width, GetDimensions().height};
}

void VirtualDisplay::OnDemand(bool warm) {
    warm = warm && !disconnected_.load(std::memory_order_acquire);
    if (warm) {
        if (!poll_timer_ && loop() != nullptr) {
            poll_timer_ = loop()->CreateTimer([this] {
                if (!frame_producer_->warm() || disconnected_.load(std::memory_order_acquire)) {
                    return false;
                }
                OnFrameRequested();
                return true;
            });
        }
        if (poll_timer_) {
            poll_timer_->Schedule(absl::Milliseconds(50), absl::Milliseconds(50));
        }
    } else {
        poll_timer_.reset();
    }
    frame_producer_->SetDemand(warm);
}

void VirtualDisplay::OnFrameRequested() {
    if (disconnected_.load(std::memory_order_acquire)) {
        return;
    }
    frame_producer_->RequestFrame();
}

IFrameSource::Start VirtualDisplay::CaptureSurface(LegacySurfaceSource& source) {
    const absl::MutexLock lock(&buffer_lock_);
    const Dimensions dims = GetDimensions();
    if (dims.width == 0 || dims.height == 0 || frame_buffer_.empty()) {
        return IFrameSource::Start::kNoContent;
    }
    auto* avd = avd::GetNullableAvd();
    auto device = avd ? avd->GetActiveMultiDisplayDevice() : nullptr;
    const uint32_t cb_handle = device ? device->GetDisplayColorBuffer(this->Id()) : 0;
    if (cb_handle > 0) {
        device->ReadDisplayColorBuffer(cb_handle, frame_buffer_.data());
    }
    return source.CaptureFrom(SurfaceSource{
        .data = frame_buffer_.data(),
        .dimensions = dims,
        .stride = static_cast<size_t>(dims.width) * 4,
        .pixman_format = PIXMAN_x8b8g8r8,
    });
}

void VirtualDisplay::SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) {
    const absl::MutexLock lock(&send_lock_);
    if (!vhid_) return;
    qemu_loop_
            ->Post([vhid = vhid_, type, code, value] {
                virtio_input_send_evdev(vhid, type, code, value);
            })
            .IgnoreError();
}

void VirtualDisplay::SendEvDevReport(EvDevEvents events) {
    const absl::MutexLock lock(&send_lock_);
    if (!vhid_) return;
    qemu_loop_
            ->Post([vhid = vhid_, report = std::move(events)] {
                InputHandler::SendEvDevReport(vhid, report);
            })
            .IgnoreError();
}

void VirtualDisplay::WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) {
    DCHECK(qemu_loop_->IsOnLoopThread());
    InputHandler::SendEvDevReport(vhid_, events);
}

bool VirtualDisplay::IsInputActive() const {
    DCHECK(qemu_loop_->IsOnLoopThread());
    return virtio_input_hid_is_active(vhid_);
}

uint64_t VirtualDisplay::InputActiveGeneration() const {
    DCHECK(qemu_loop_->IsOnLoopThread());
    return input_active_tracker_.GenerationOnLoop();
}

void VirtualDisplay::SimulateGuestFrameUpdate() {
    OnFrameRequested();
}

}  // namespace goldfish::display
