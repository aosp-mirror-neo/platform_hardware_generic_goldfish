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

// clang-format off
#include <time.h>
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "pixman.h"
#include "qapi/error.h"
#include "hw/virtio/virtio-input.h"
#include "virtio_bridge.h"
#include "goldfish/input/virtio_input_android.h"
// clang-format on

// A set of stubs for qemu methods we do not have.
QemuConsole* qemu_console_lookup_default() {
    return NULL;
}
uint32_t qemu_console_get_head(QemuConsole* con) {
    (void)con;
    return 0;
}

DisplaySurface* qemu_console_surface(QemuConsole* con) {
    (void)con;
    return NULL;
}

int qemu_console_get_index(QemuConsole* con) {
    (void)con;
    return 0;
}

Object* object_property_get_link(Object* obj, const char* name, Error** errp) {
    (void)obj;
    (void)name;
    (void)errp;
    return NULL;
}

Object* object_get_root(void) {
    return NULL;
}
Object* object_dynamic_cast_assert(Object* obj, const char* type_name, const char* file, int line,
                                   const char* func) {
    (void)obj;
    (void)type_name;
    (void)file;
    (void)line;
    (void)func;
    return NULL;
}

Object* object_dynamic_cast(Object* obj, const char* type_name) {
    (void)obj;
    (void)type_name;
    return NULL;
}

static VirtIOInputHID s_fake_vhid = {
    .display = "gpu0",
    .head = 0,
};

static int g_find_virtio_device_result = 1;
static bool g_find_virtio_device_null_vhid = false;

void SetTestFindVirtioDeviceResult(int result) {
    g_find_virtio_device_result = result;
}

// Makes a successful device lookup report no device, as a display without one.
void SetTestFindVirtioDeviceNullVhid(bool null_vhid) {
    g_find_virtio_device_null_vhid = null_vhid;
}

// Sets whether the guest driver of the fake virtio input device is active.
void SetTestVirtioInputActive(bool active) {
    s_fake_vhid.parent_obj.active = active;
}

static uint64_t s_fake_active_generation = 0;
static uint64_t s_object_property_get_uint_calls = 0;

// Sets the fake virtio input device's QOM active-generation property without
// firing a push event (e.g., to simulate state prior to display construction).
void SetTestVirtioInputInitialActiveGeneration(uint64_t generation) {
    s_fake_active_generation = generation;
}

// Simulates virtio_android_change_active: updates QOM active-generation and
// fires the push notification.
void SetTestVirtioInputActiveGeneration(uint64_t generation) {
    s_fake_active_generation = generation;
    virtio_input_android_notify_active_changed(&s_fake_vhid, generation);
}

// Fires an active-generation push notification for a distinct VirtIOInputHID
// instance not bound to the display under test.
void NotifyTestUnrelatedVirtioInputActiveGeneration(uint64_t generation) {
    static VirtIOInputHID s_unrelated_vhid = {
        .display = "gpu0",
        .head = 99,
    };
    virtio_input_android_notify_active_changed(&s_unrelated_vhid, generation);
}

void ResetTestObjectPropertyGetUintCalls(void) {
    s_object_property_get_uint_calls = 0;
}

uint64_t GetTestObjectPropertyGetUintCalls(void) {
    return s_object_property_get_uint_calls;
}

// Reads only the fake device's active generation.
uint64_t object_property_get_uint(Object* obj, const char* name, Error** errp) {
    (void)errp;
    ++s_object_property_get_uint_calls;
    if (obj == OBJECT(&s_fake_vhid) && strcmp(name, VIRTIO_INPUT_ANDROID_ACTIVE_GENERATION) == 0) {
        return s_fake_active_generation;
    }
    return 0;
}

int object_child_foreach_recursive(Object* obj, int (*fn)(Object* child, void* opaque),
                                   void* opaque) {
    (void)obj;
    (void)fn;
    if (g_find_virtio_device_result && opaque) {
        VirtioDeviceInfo* info = (VirtioDeviceInfo*)opaque;
        info->vhid = g_find_virtio_device_null_vhid ? NULL : &s_fake_vhid;
    }
    return g_find_virtio_device_result;
}

Object* object_resolve_path_component(Object* parent, const char* part) {
    (void)parent;
    (void)part;
    return NULL;
}

Object* object_resolve_path_type(const char* path, const char* type_name,
                                 bool* ambiguous) {  // NOLINT(readability-non-const-parameter)
    (void)path;
    (void)type_name;
    (void)ambiguous;
    return NULL;
}

void qemu_input_event_send_key_number(QemuConsole* src, int num,  // NOLINT
                                      bool down) {
    (void)src;
    (void)num;
    (void)down;
}

void console_handle_mouse_event(QemuConsole* dcl, int dx, int dy, int dz,  // NOLINT
                                int button_state) {
    (void)dcl;
    (void)dx;
    (void)dy;
    (void)dz;
    (void)button_state;
}

typedef void (*VirtioInputSendHook)(uint16_t type, uint16_t code, uint32_t value);
static VirtioInputSendHook g_virtio_input_send_hook = NULL;

void SetTestVirtioInputSendHook(VirtioInputSendHook hook) {
    g_virtio_input_send_hook = hook;
}

void virtio_input_send(VirtIOInput* vinput, virtio_input_event* event) {
    (void)vinput;
    if (g_virtio_input_send_hook) {
        g_virtio_input_send_hook(le16_to_cpu(event->type), le16_to_cpu(event->code),
                                 le32_to_cpu(event->value));
    }
}

void qemu_free_displaysurface(DisplaySurface* surface) {
    (void)surface;
}

DisplaySurface* qemu_create_displaysurface(int width, int height) {
    (void)width;
    (void)height;
    return NULL;
}

int dpy_set_ui_info(QemuConsole* con, QemuUIInfo* info, bool delay) {
    (void)con;
    (void)info;
    (void)delay;
    return 0;
}

typedef void (*RegisterDclHook)(DisplayChangeListener* dcl);
typedef void (*GraphicHwUpdateHook)(QemuConsole* con);
static RegisterDclHook g_register_dcl_hook = NULL;
static GraphicHwUpdateHook g_graphic_hw_update_hook = NULL;

void SetTestRegisterDclHook(RegisterDclHook hook) {
    g_register_dcl_hook = hook;
}

void SetTestGraphicHwUpdateHook(GraphicHwUpdateHook hook) {
    g_graphic_hw_update_hook = hook;
}

void unregister_displaychangelistener(DisplayChangeListener* dcl) {
    if (dcl) {
        dcl->ds = NULL;
    }
}
void register_displaychangelistener(DisplayChangeListener* dcl) {
    if (dcl) {
        dcl->ds = (DisplayState*)0x1;
        if (g_register_dcl_hook) {
            g_register_dcl_hook(dcl);
        }
    }
}
void graphic_hw_update(QemuConsole* con) {
    if (g_graphic_hw_update_hook) {
        g_graphic_hw_update_hook(con);
    }
}

__attribute__((weak)) void grpc_dpy_gfx_switch(  // NOLINT(readability-identifier-naming)
        struct DisplayChangeListener* dcl, struct DisplaySurface* new_surface) {
    (void)dcl;
    (void)new_surface;
}

__attribute__((weak)) void grpc_dpy_gfx_update(  // NOLINT(readability-identifier-naming)
        struct DisplayChangeListener* dcl, int x, int y, int w, int h) {
    (void)dcl;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
