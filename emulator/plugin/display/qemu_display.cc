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

#include "qemu_display.h"

#include <cstdint>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "goldfish/display/input_handler.h"
#include "goldfish/display/multi_display_callbacks.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "pixman.h"
#include "qom/object.h"
}
#include "virtio_bridge.h"
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::display {

namespace {

constexpr DisplayChangeListenerOps kDclOps = {
    .dpy_name = "grpc-display",
    .dpy_gfx_update = grpc_dpy_gfx_update,
    .dpy_gfx_switch = grpc_dpy_gfx_switch,
};

constexpr DisplayChangeListenerOps kDclSwitchOnlyOps = {
    .dpy_name = "grpc-display",
    .dpy_gfx_update = nullptr,
    .dpy_gfx_switch = grpc_dpy_gfx_switch,
};

}  // namespace

absl::StatusOr<std::shared_ptr<QemuDisplay>> QemuDisplay::Create(EventLoop* loop,
                                                                 EventLoop* qemu_loop,
                                                                 QemuConsole* con,
                                                                 DisplaySurface* ds, int index) {
    if (!con) {
        return absl::InvalidArgumentError(absl::StrCat("Display: ", index, " has nullptr console"));
    }

    // note, the input 1 to 5 are taken by virtual displays
    // and external display offset starts from 6, so we need
    // to add 5 to account for the offset
    constexpr int kExternalDisplayInputOffset = 5;
    const char* gpu = "gpu0";
    // Head will normally be 0 if this is the default console.
    uint32_t head = qemu_console_get_head(con);
    if (head != 0) {
        head += kExternalDisplayInputOffset;
    }
    VirtioDeviceInfo device_info{.display = gpu, .head = head};
    Object* objs = object_resolve_path_component(object_get_root(), "machine");
    if (!object_child_foreach_recursive(objs, ::find_virtio_device, &device_info)) {
        return absl::NotFoundError(
                absl::StrCat("Unable to find a virtio device for head: ", device_info.head,
                             " attached to display: ", device_info.display));
    }

    auto display = std::make_shared<QemuDisplay>(Private(), loop, qemu_loop, con, ds, index,
                                                 device_info.vhid);
    VLOG(1) << "Connecting: " << *display << ", to: " << device_info;
    return display;
}

QemuDisplay::QemuDisplay(Private, EventLoop* loop, EventLoop* qemu_loop, QemuConsole* con,
                         DisplaySurface* ds, int index, ::VirtIOInputHID* vhid)
        : PixmanDisplay(loop, /*executor=*/qemu_loop, index, ds ? ds->image : nullptr)
        , console_(con)
        , qemu_loop_(qemu_loop)
        , vhid_(vhid)
        , input_active_tracker_(vhid, input_active_events())
        , dcl_(std::make_unique<DisplayChangeListener>(DisplayChangeListener{
              .ops = &kDclOps,
              .con = con,
          }))
        , current_surface_(ds)
        , placeholder_shown_(ds == nullptr || surface_is_placeholder(ds)) {
    if (ds != nullptr && !surface_is_placeholder(ds) && ds->image != nullptr) {
        current_size_ = {
            .width = static_cast<uint32_t>(surface_width(ds)),
            .height = static_cast<uint32_t>(surface_height(ds)),
        };
        if (current_size_.width > 0 && current_size_.height > 0) {
            legacy_->Resize(current_size_);
        }
    }
}

QemuDisplay::~QemuDisplay() {
    input_active_tracker_.Reset();
    ShutdownProducer();  // Its hooks use our state.
    // Release ownership so we can unregister and delete on the QEMU event loop.
    auto cleanup = [dcl = std::move(dcl_)]() {
        if (dcl->ds != nullptr) {
            unregister_displaychangelistener(dcl.get());
        }
    };

    if (qemu_loop_->IsOnLoopThread()) {
        cleanup();
    } else if (auto status = qemu_loop_->Post(std::move(cleanup)); !status.ok()) {
        LOG(WARNING) << "Failed to post display unregister task to QEMU event loop: " << status
                     << ". The DisplayChangeListener could not be cleanly unregistered, which may "
                        "cause use-after-free or memory corruption during QEMU shutdown.";
    }

    if (owned_surface_) {
        qemu_free_displaysurface(owned_surface_);
    }
}

bool QemuDisplay::IsActive() const {
    return frame_producer_->warm();
}

void QemuDisplay::OnSurfaceSwitch(DisplaySurface* surface) {
    current_surface_ = surface;
    if (surface == nullptr || surface_is_placeholder(surface)) {
        if (!placeholder_shown_) {
            placeholder_shown_ = true;
            current_size_ = {0, 0};
            const absl::MutexLock lock(&pixman_mutex_);
            source_image_.reset();
            legacy_->Reset();
            frame_producer_->Reset();
        }
        frame_producer_->NoContent();
        return;
    }
    placeholder_shown_ = false;
    const Dimensions size{
        .width = static_cast<uint32_t>(surface_width(surface)),
        .height = static_cast<uint32_t>(surface_height(surface)),
    };
    SetDimensions(size);
    {
        const absl::MutexLock lock(&pixman_mutex_);
        if (surface->image != nullptr) {
            source_image_ = SharePixmanImage(surface->image);
        } else {
            source_image_.reset();
        }
        if (size != current_size_) {
            current_size_ = size;
            if (size.width > 0 && size.height > 0) {
                legacy_->Resize(size);
            }
        }
    }
}

void QemuDisplay::OnGuestFrame(DisplaySurface* surface) {
    if (surface == nullptr || surface->image == nullptr) {
        return;
    }
    current_surface_ = surface;
    const Dimensions size{
        .width = static_cast<uint32_t>(surface_width(surface)),
        .height = static_cast<uint32_t>(surface_height(surface)),
    };
    SetDimensions(size);
    {
        const absl::MutexLock lock(&pixman_mutex_);
        source_image_ = SharePixmanImage(surface->image);
        if (size != current_size_ && size.width > 0 && size.height > 0) {
            current_size_ = size;
            legacy_->Resize(size);
        }
    }
    if (pixman_image_get_data(surface->image) != nullptr) {
        // Captures now, or once the capture or refresh in progress ends (I5).
        frame_producer_->Invalidate();
    }
}

void QemuDisplay::OnDeviceReset() {
    current_size_ = {0, 0};
    {
        const absl::MutexLock lock(&pixman_mutex_);
        legacy_->Reset();
    }
    frame_producer_->Reset();
    frame_producer_->RequestFrame();
}

void QemuDisplay::OnSourceDemandChanged(bool warm) {
    if (warm) {
        VLOG(1) << *this << " Executing register_displaychangelistener on qemu_loop_";
        if (dcl_->ds == nullptr) {
            // Suppress dpy_gfx_update during registration so QEMU does not push
            // stale/blank con->surface pixels before graphic_hw_update transfers
            // the latest GPU scanout.
            dcl_->ops = &kDclSwitchOnlyOps;
            register_displaychangelistener(dcl_.get());
            dcl_->ops = &kDclOps;
        }
    } else {
        VLOG(1) << *this << " Executing unregister_displaychangelistener on qemu_loop_";
        if (dcl_->ds != nullptr) {
            unregister_displaychangelistener(dcl_.get());
        }
    }
}

DisplaySurface* QemuDisplay::LegacySurface() const {
    DisplaySurface* surface = qemu_console_surface(console_);
    return surface != nullptr ? surface : current_surface_;
}

void QemuDisplay::RefreshSource() {
    DisplaySurface* surface = LegacySurface();
    if (surface == nullptr || surface_is_placeholder(surface)) {
        return;
    }
    // Reads the current scanout back into the surface. Its dpy_gfx_update
    // re-enters OnGuestFrame, which only marks the producer dirty (I5).
    graphic_hw_update(console_);
}

IFrameSource::Start QemuDisplay::CaptureSource(LegacySurfaceSource& source) {
    DisplaySurface* surface = LegacySurface();
    if (surface == nullptr || surface_is_placeholder(surface)) {
        return IFrameSource::Start::kNoContent;
    }
    SetDimensions({
        .width = static_cast<uint32_t>(surface_width(surface)),
        .height = static_cast<uint32_t>(surface_height(surface)),
    });
    const absl::MutexLock lock(&pixman_mutex_);
    ::pixman_image_t* img = surface->image ? surface->image : source_image_.get();
    if (img == nullptr || pixman_image_get_data(img) == nullptr) {
        return IFrameSource::Start::kNoContent;
    }
    return source.CaptureFrom(SurfaceSourceFromPixman(img));
}

void QemuDisplay::SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) {
    const absl::MutexLock lock(send_lock_);
    VLOG(2) << *this << ", SendEvDevEvent(" << type << ", " << code << ", " << value << ")";
    qemu_loop_
            ->Post([vhid = vhid_, type, code, value] {
                virtio_input_send_evdev(vhid, type, code, value);
            })
            .IgnoreError();
}

void QemuDisplay::SendEvDevReport(EvDevEvents events) {
    const absl::MutexLock lock(send_lock_);
    if (!vhid_) return;
    VLOG(2) << *this << ", SendEvDevReport(" << events.size() << " events)";
    qemu_loop_
            ->Post([vhid = vhid_, report = std::move(events)] {
                InputHandler::SendEvDevReport(vhid, report);
            })
            .IgnoreError();
}

void QemuDisplay::WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) {
    DCHECK(qemu_loop_->IsOnLoopThread());
    InputHandler::SendEvDevReport(vhid_, events);
}

bool QemuDisplay::IsInputActive() const {
    DCHECK(qemu_loop_->IsOnLoopThread());
    return virtio_input_hid_is_active(vhid_);
}

uint64_t QemuDisplay::InputActiveGeneration() const {
    DCHECK(qemu_loop_->IsOnLoopThread());
    return input_active_tracker_.GenerationOnLoop();
}

void QemuDisplay::SetOwnedSurface(DisplaySurface* surface) {
    if (owned_surface_) {
        qemu_free_displaysurface(owned_surface_);
    }
    owned_surface_ = surface;
}

}  // namespace goldfish::display
