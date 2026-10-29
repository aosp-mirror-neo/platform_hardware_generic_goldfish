// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "goldfish/input/virtio_input_android.h"

#include "virtio_input_android_config.h"
#include "virtio_input_android_internal.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/module.h"

#include "hw/virtio/virtio-pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-input.h"

#include "ui/console.h"

#include "standard-headers/linux/input.h"
// IWYU pragma: end_keep
// clang-format on

#include "android/base/logging/abseil_log_bridge.h"
#include "goldfish/base/array_size.h"

#define DEBUG 0
#if DEBUG
#define DD(fmt, ...) ALOGV(1, fmt, ##__VA_ARGS__);
#else
#define DD(fmt, ...) (void)0
#endif

#define VIRTIO_ID_NAME_ANDROID "virtio_input_multi_touch_"

// Parent types
#define TYPE_VIRTIO_INPUT_HID_PCI "virtio-input-hid-pci"

static const unsigned short keymap_button[INPUT_BUTTON__MAX] = {
    [INPUT_BUTTON_LEFT] = BTN_LEFT,
    [INPUT_BUTTON_RIGHT] = BTN_RIGHT,
    [INPUT_BUTTON_MIDDLE] = BTN_MIDDLE,
    [INPUT_BUTTON_WHEEL_UP] = BTN_GEAR_UP,
    [INPUT_BUTTON_WHEEL_DOWN] = BTN_GEAR_DOWN,
    [INPUT_BUTTON_SIDE] = BTN_SIDE,
    [INPUT_BUTTON_EXTRA] = BTN_EXTRA,
    [INPUT_BUTTON_TOUCH] = BTN_TOUCH,
};

static const unsigned short axismap_tch[INPUT_AXIS__MAX] = {
    [INPUT_AXIS_X] = ABS_MT_POSITION_X,
    [INPUT_AXIS_Y] = ABS_MT_POSITION_Y,
};

// --- debug functions ---
#define MAX_STRING_LENGTH 64
const char* get_button_name(unsigned short button_index) {
    switch (button_index) {
    case INPUT_BUTTON_LEFT:
        return "INPUT_BUTTON_LEFT";
    case INPUT_BUTTON_RIGHT:
        return "INPUT_BUTTON_RIGHT";
    case INPUT_BUTTON_MIDDLE:
        return "INPUT_BUTTON_MIDDLE";
    case INPUT_BUTTON_WHEEL_UP:
        return "INPUT_BUTTON_WHEEL_UP";
    case INPUT_BUTTON_WHEEL_DOWN:
        return "INPUT_BUTTON_WHEEL_DOWN";
    case INPUT_BUTTON_SIDE:
        return "INPUT_BUTTON_SIDE";
    case INPUT_BUTTON_EXTRA:
        return "INPUT_BUTTON_EXTRA";
    case INPUT_BUTTON_TOUCH:
        return "INPUT_BUTTON_TOUCH";
    default:
        return "UNKNOWN_BUTTON_INDEX";
    }
}

static const char* get_abs_axis_name(unsigned short axis_code) {
    static char buffer[MAX_STRING_LENGTH];

    switch (axis_code) {
    case ABS_X:
        return "ABS_X";
    case ABS_Y:
        return "ABS_Y";
    case ABS_MT_POSITION_X:
        return "ABS_MT_POSITION_X";
    case ABS_MT_POSITION_Y:
        return "ABS_MT_POSITION_Y";
    default:
        snprintf(buffer, MAX_STRING_LENGTH, "UNKNOWN_ABS_AXIS (0x%04x)", axis_code);
        return buffer;
    }
}

// -- device configuration --
static struct virtio_input_config virtio_input_android_template[] = {
    {
        .select = VIRTIO_INPUT_CFG_ID_NAME,
        .size = sizeof(VIRTIO_ID_NAME_ANDROID),
        .u.string = VIRTIO_ID_NAME_ANDROID,
    },
    {.select = VIRTIO_INPUT_CFG_ID_DEVIDS,
     .size = sizeof(struct virtio_input_devids),
     .u.ids =
             {
                 .bustype = const_le16(BUS_VIRTUAL),
                 .vendor = const_le16(0),
                 .product = const_le16(0),
                 .version = const_le16(0),
             }},
    {/* end of list */},
};

static void virtio_input_extend_config(VirtIOInput* vinput, const unsigned short* map,
                                       size_t mapsize, uint8_t select, uint8_t subsel) {
    virtio_input_config ext;
    int i, bit, byte, bmax = 0;

    memset(&ext, 0, sizeof(ext));
    for (i = 0; i < mapsize; i++) {
        bit = map[i];
        if (!bit) {
            continue;
        }
        byte = bit / 8;
        bit = bit % 8;
        ext.u.bitmap[byte] |= (1 << bit);
        if (bmax < byte + 1) {
            bmax = byte + 1;
        }
    }
    ext.select = select;
    ext.subsel = subsel;
    ext.size = bmax;
    virtio_input_add_config(vinput, &ext);
}
/**
 * @brief Sends a virtio-input event to the guest.
 *
 * @param vinput The VirtIOInput device.
 * @param type The event type (e.g., `EV_ABS`, `EV_KEY`, `EV_SYN`).  These are standard Linux input
 * event types.
 * @param code The event code (e.g., `ABS_X`, `BTN_LEFT`, `SYN_REPORT`).  These are standard Linux
 * input event codes, specific to the `type`.
 * @param value The event value.  The meaning of this value depends on the `type` and `code`.
 */
static inline void send_event(VirtIOInput* vinput, uint16_t type, uint16_t code, uint32_t value) {
    virtio_input_event event = {
        .type = cpu_to_le16(type), .code = cpu_to_le16(code), .value = cpu_to_le32(value)};
    DD("Sending generic event (%d, %d, %d)", type, code, value);
    virtio_input_send(vinput, &event);
}

/**
 * @brief Simulates a multi-touch "pointer down" event (finger press).
 *
 * @param dev The device state (`VirtIOInputAndroidHID`).
 * @param tracking_id A unique identifier for the touch point (finger).
 * @param x The X-coordinate of the touch.
 * @param y The Y-coordinate of the touch.
 * @param pressure The pressure of the touch (0 to `MTS_PRESSURE_RANGE_MAX`).
 *
 * This function emulates a finger touching the screen.  It sends a sequence of
 * `virtio_input_event` structures to the guest to represent the touch.  The
 * events are sent using the `send_event` helper function.  The `tracking_id`
 * is used to distinguish between multiple simultaneous touches.
 *
 * The function also updates the internal `tracked_pointers` array in the
 * `VirtIOInputAndroidHID` structure to keep track of the touch state.
 */
static void _mts_pointer_down(DeviceState* dev, int tracking_id, int x, int y, int pressure) {
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(dev);
    VirtIOInput* vinput = VIRTIO_INPUT(dev);

    /* Get first available slot for the new pointer. */
    const int slot_index = tracking_id;

    /* Make sure there is a place for the pointer. */
    if (slot_index >= 0) {
        /* Initialize pointer's entry. */
        vahid->tracked_pointers[slot_index].tracking_id = tracking_id;
        vahid->tracked_pointers[slot_index].x = x;
        vahid->tracked_pointers[slot_index].y = y;
        vahid->tracked_pointers[slot_index].pressure = pressure;

        /* Send events indicating a "pointer down" to the EventHub */
        /* Make sure that correct slot is selected. */
        if (slot_index != vahid->current_slot) {
            send_event(vinput, EV_ABS, ABS_MT_SLOT, slot_index);
            vahid->current_slot = slot_index;
        }

        send_event(vinput, EV_ABS, ABS_MT_TRACKING_ID, slot_index);
        send_event(vinput, EV_ABS, ABS_MT_POSITION_X, x);
        send_event(vinput, EV_ABS, ABS_MT_POSITION_Y, y);
        send_event(vinput, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        send_event(vinput, EV_ABS, ABS_MT_PRESSURE, pressure);
        send_event(vinput, EV_ABS, ABS_MT_ORIENTATION, 0);
        send_event(vinput, EV_ABS, ABS_MT_TOUCH_MAJOR, MTS_TOUCH_AXIS_DEFAULT);
        send_event(vinput, EV_ABS, ABS_MT_TOUCH_MINOR, MTS_TOUCH_AXIS_DEFAULT);

        DD("Delivered touchdown for pointer: %d at (%d, %d)", slot_index, x, y);
    }
}

/**
 * @brief Simulates a multi-touch "pointer up" event (finger release).
 *
 * @param dev The device state (`VirtIOInputAndroidHID`).
 * @param slot_index The index of the touch slot (finger) to release.
 *
 * This function emulates a finger lifting from the screen. It sends a sequence of
 * `virtio_input_event` structures to the guest.
 *
 * The function also updates the `tracked_pointers` array in the
 * `VirtIOInputAndroidHID` structure to reflect that the pointer is no longer
 * active. The `current_slot` is set to -1 to indicate that no slot is
 * currently selected.
 */
static void _mts_pointer_up(DeviceState* dev, int slot_index) {
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(dev);
    VirtIOInput* vinput = VIRTIO_INPUT(dev);

    /* Make sure that correct slot is selected. */
    if (slot_index != vahid->current_slot) {
        send_event(vinput, EV_ABS, ABS_MT_SLOT, slot_index);
    }
    /* Send event indicating "pointer up" to the EventHub. */
    send_event(vinput, EV_ABS, ABS_MT_PRESSURE, 0);
    send_event(vinput, EV_ABS, ABS_MT_TRACKING_ID, MTS_POINTER_UP);

    /* Update MTS descriptor, removing the tracked pointer. */
    vahid->tracked_pointers[slot_index].tracking_id = MTS_POINTER_UP;
    vahid->tracked_pointers[slot_index].pressure = 0;

    /* Since current slot is no longer tracked, make sure we will do a "select"
     * next time we send events to the EventHub. */
    vahid->current_slot = -1;
}

/**
 * @brief Simulates a multi-touch "pointer move" event (finger drag).
 *
 * @param dev The device state (`VirtIOInputAndroidHID`).
 * @param slot_index The index of the touch slot (finger) to move.
 * @param x The new X-coordinate of the touch.
 * @param y The new Y-coordinate of the touch.
 * @param pressure The (potentially new) pressure of the touch.
 *
 * This function emulates a finger moving across the screen. It sends
 * `virtio_input_event` structures to the guest to update the position (and
 * optionally pressure) of the touch point.
 *
 * The function updates the `tracked_pointers` array in the
 * `VirtIOInputAndroidHID` structure with the new position and pressure. The
 * function *only* sends events for values that have actually changed,
 * optimizing for efficiency.
 */
static void _mts_pointer_move(DeviceState* dev, int slot_index, int x, int y, int pressure) {
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(dev);
    VirtIOInput* vinput = VIRTIO_INPUT(dev);
    MTSPointerState* ptr_state = &vahid->tracked_pointers[slot_index];

    DD("Moving %d -> (%d, %d) (state: %s)", slot_index, x, y,
       pressure == MTS_PRESSURE_RANGE_MAX ? "down" : "up");

    if (ptr_state->x == x && ptr_state->y == y) {
        DD("Coordinates have not changed.");
        /* Coordinates didn't change. Bail out. */
        return;
    }

    /* Make sure that the right slot is selected. */
    if (slot_index != vahid->current_slot) {
        send_event(vinput, EV_ABS, ABS_MT_SLOT, slot_index);
        vahid->current_slot = slot_index;
    }
    if (ptr_state->x != x) {
        send_event(vinput, EV_ABS, ABS_MT_POSITION_X, x);
        ptr_state->x = x;
    }
    if (ptr_state->y != y) {
        send_event(vinput, EV_ABS, ABS_MT_POSITION_Y, y);
        ptr_state->y = y;
    }
    if (ptr_state->pressure != pressure) {
        send_event(vinput, EV_ABS, ABS_MT_PRESSURE, pressure);
        ptr_state->pressure = pressure;
    }
}

/**
 * @brief Handles input events received from QEMU's input subsystem.
 *
 * @param dev The device state (`VirtIOInputAndroidHID`).
 * @param src The QemuConsole that generated the event.
 * @param evt The input event (`InputEvent`).
 *
 * This function is the core event handler for the `virtio-input-android` device.  It
 * receives `InputEvent` structures from QEMU's input subsystem and translates them
 * into appropriate `virtio_input_event` structures that are sent to the Android guest.
 *
 * This handler effectively acts as a translator between QEMU's generic input
 * event representation and the specific multi-touch events expected by the
 * Android guest.
 */
static void virtio_input_handle_event(DeviceState* dev, QemuConsole* src, InputEvent* evt) {
    VirtIOInputHID* vhid = VIRTIO_INPUT_HID(dev);
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(dev);
    VirtIOInput* vinput = VIRTIO_INPUT(dev);
    virtio_input_event event;
    InputMoveEvent* move;
    InputBtnEvent* btn;
    InputMultiTouchEvent* mtt;

    DD("virtio_input_handle_event: for virtio_input_multi_touch_%d %s (%d)", vahid->device_id,
       vhid->display, vhid->head);
    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY:
        ALOGE("Keyboard events cannot be handled by this device (%s:%d), dropping event",
              vhid->display, vhid->head);
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        // This android device only understands touch events, so we will be translating this
        // button event into a touch "event"
        if ((btn->button == INPUT_BUTTON_WHEEL_UP || btn->button == INPUT_BUTTON_WHEEL_DOWN) &&
            btn->down) {
            ALOGW("We do not yet translate wheel events to touch events, ignoring.");
        } else if (keymap_button[btn->button]) {
            DD("Handling button event: %s tracked as: %d (%s)", get_button_name(btn->button),
               btn->button, btn->down ? "down" : "up");
            int x = vahid->tracked_pointers[btn->button].x;
            int y = vahid->tracked_pointers[btn->button].y;
            if (btn->down) {
                vahid->keymap_button_down[btn->button] = true;
                _mts_pointer_down(dev, btn->button, x, y, MTS_PRESSURE_RANGE_MAX);
            } else {
                vahid->keymap_button_down[btn->button] = false;
                _mts_pointer_up(dev, btn->button);
            }
        } else {
            if (btn->down) {
                ALOGW("unmapped button: %d [%s], ignoring.", btn->button,
                      InputButton_str(btn->button));
            }
        }
        break;
    case INPUT_EVENT_KIND_REL:
        ALOGW("This handler cannot handle RELATIVE mouse events.");
        break;
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        DD("Handling INPUT_EVENT_KIND_ABS: %s", get_abs_axis_name(move->axis));
        // We now will translate this to and individual touch move for every button
        for (int i = 0; i < INPUT_BUTTON__MAX; i++) {
            int x = vahid->tracked_pointers[i].x;
            int y = vahid->tracked_pointers[i].y;
            if (move->axis == INPUT_AXIS_X) {
                x = move->value;
            } else if (move->axis == INPUT_AXIS_Y) {
                y = move->value;
            }
            // Send updated location.
            if (vahid->keymap_button_down[i]) {
                _mts_pointer_move(dev, i, x, y, MTS_PRESSURE_RANGE_MAX);
            }
            if (move->axis == INPUT_AXIS_X) {
                vahid->tracked_pointers[i].x = move->value;
            } else if (move->axis == INPUT_AXIS_Y) {
                vahid->tracked_pointers[i].y = move->value;
            }
        }
        break;
    case INPUT_EVENT_KIND_MTT:
        mtt = evt->u.mtt.data;
        if (mtt->type == INPUT_MULTI_TOUCH_TYPE_DATA) {
            DD("Handling INPUT_MULTI_TOUCH_TYPE_DATA, axis: %s", get_abs_axis_name(mtt->axis));
            event.type = cpu_to_le16(EV_ABS);
            event.code = cpu_to_le16(axismap_tch[mtt->axis]);
            event.value = cpu_to_le32(mtt->value);
            virtio_input_send(vinput, &event);
        } else {
            event.type = cpu_to_le16(EV_ABS);
            event.code = cpu_to_le16(ABS_MT_SLOT);
            event.value = cpu_to_le32(mtt->slot);
            virtio_input_send(vinput, &event);
            event.type = cpu_to_le16(EV_ABS);
            event.code = cpu_to_le16(ABS_MT_TRACKING_ID);
            event.value = cpu_to_le32(mtt->tracking_id);
            virtio_input_send(vinput, &event);
        }
        break;
    default:
        break;
    }
}

static void virtio_input_handle_sync(DeviceState* dev) {
    VirtIOInput* vinput = VIRTIO_INPUT(dev);
    virtio_input_event event = {
        .type = cpu_to_le16(EV_SYN),
        .code = cpu_to_le16(SYN_REPORT),
        .value = 0,
    };

    virtio_input_send(vinput, &event);
}

static const QemuInputHandler virtio_android_handler_template = {
    .name = VIRTIO_ID_NAME_ANDROID,
    .mask = INPUT_EVENT_MASK_MTT | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = virtio_input_handle_event,
    .sync = virtio_input_handle_sync,
};

static void virtio_android_init(Object* obj) {
    VirtIOInputHID* vhid = VIRTIO_INPUT_HID(obj);
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(obj);
    VirtIOInput* vinput = VIRTIO_INPUT(obj);

    // Android expects device names of the form "virtio_input_multi_touch_%d", where %d is a unique
    // device ID. Ideally, we would use device properties (like 'display' and 'head') to generate
    // this ID. However, those properties are not available during object initialization.
    // They become available only later, during device realization.
    //
    // Because HID devices require their handler and configuration to be set up during
    // *initialization*, we cannot wait for realization.  Therefore, we use a statically
    // incrementing `device_id` that corresponds to the order in which the devices are added in
    // QEMU.  For example:
    //
    //   - The first `-device virtio-input-android-pci` will get device_id 1, resulting in:
    //     - Handler name:     "virtio_input_multi_touch_1"
    //     - Device config name: "virtio_input_multi_touch_1"
    //   - The second `-device virtio-input-android-pci` will get device_id 2, and so on.
    //
    // This approach guarantees unique names within a single QEMU instance, but it relies on the
    // order of device addition in the QEMU command line.
    static int device_id = 0;

    // Make sure we use a new device id for incoming registration.
    device_id++;

    if (device_id > VIRTIO_INPUT_MAX_NUM) {
        ALOGW("You are adding more than %d devices, android will likely not "
              "recognize " VIRTIO_ID_NAME_ANDROID "_%d",
              VIRTIO_INPUT_MAX_NUM, device_id);
    }

    //  Create a named input handler, based on the display head
    QemuInputHandler* handler = g_new0(QemuInputHandler, 1);
    *handler = virtio_android_handler_template;
    handler->name = g_strdup_printf(VIRTIO_ID_NAME_ANDROID "%d", device_id);
    vhid->handler = handler;

    // Create the virtio-input configuration for this device_id
    vahid->virtio_input_android_device_config =
            g_new0(struct virtio_input_config, ARRAY_SIZE(virtio_input_android_template));
    memcpy(vahid->virtio_input_android_device_config, virtio_input_android_template,
           sizeof(virtio_input_android_template));

    // Set the proper de vice name.
    int bytes_written = snprintf(vahid->virtio_input_android_device_config[0].u.string,
                                 sizeof(vahid->virtio_input_android_device_config[0].u.string),
                                 VIRTIO_ID_NAME_ANDROID "%d", device_id);
    vahid->virtio_input_android_device_config[0].size = bytes_written;
    assert(bytes_written > 0);
    vahid->device_id = device_id;

    // And register it with the proper configuration
    virtio_input_init_config(vinput, vahid->virtio_input_android_device_config);
    size_t ev_key_count = 0;
    const unsigned short* ev_key_codes = virtio_input_android_ev_key_codes(&ev_key_count);
    virtio_input_extend_config(vinput, ev_key_codes, ev_key_count, VIRTIO_INPUT_CFG_EV_BITS,
                               EV_KEY);
    size_t ev_abs_count = 0;
    const unsigned short* ev_abs_codes = virtio_input_android_ev_abs_codes(&ev_abs_count);
    virtio_input_extend_config(vinput, ev_abs_codes, ev_abs_count, VIRTIO_INPUT_CFG_EV_BITS,
                               EV_ABS);

    const struct virtio_input_config* configs = virtio_input_android_configs();
    for (int i = 0; configs[i].select; i++) {
        virtio_input_config cfg = configs[i];
        virtio_input_add_config(vinput, &cfg);
    }

    vahid->current_slot = 0;
    for (int i = 0; i < MTS_POINTERS_NUM; i++) {
        memset(&vahid->tracked_pointers[i], 0, sizeof(MTSPointerState));
    }
    object_property_add_uint64_ptr(obj, VIRTIO_INPUT_ANDROID_ACTIVE_GENERATION,
                                   &vahid->active_generation, OBJ_PROP_FLAG_READ);
}

static void virtio_android_fini(Object* obj) {
    VirtIOInputHID* vhid = VIRTIO_INPUT_HID(obj);
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(obj);
    g_free((void*)vhid->handler->name);
    g_free((void*)vhid->handler);
    g_free((void*)vahid->virtio_input_android_device_config);
}

/* The change_active of VirtIOInputHID, which (de)activates the input handler. */
static void (*virtio_android_parent_change_active)(VirtIOInput* vinput);

/**
 * @brief Advances the active generation, lets VirtIOInputHID handle the
 * change, and notifies host active-generation listeners.
 *
 * Called on a vCPU thread, under the BQL, when the guest driver becomes
 * ready, the driver or device resets, or the device loads from a snapshot.
 */
static void virtio_android_change_active(VirtIOInput* vinput) {
    VirtIOInputAndroidHID* vahid = VIRTIO_INPUT_ANDROID_HID(vinput);

    const uint64_t generation = ++vahid->active_generation;
    virtio_android_parent_change_active(vinput);
    virtio_input_android_notify_active_changed(VIRTIO_INPUT_HID(vinput), generation);
}

static void virtio_android_class_init(ObjectClass* klass, const void* data) {
    VirtIOInputClass* vic = VIRTIO_INPUT_CLASS(klass);

    virtio_android_parent_change_active = vic->change_active;
    vic->change_active = virtio_android_change_active;
}

static const TypeInfo virtio_android_info = {
    .name = TYPE_VIRTIO_INPUT_ANDROID_HID,
    .parent = TYPE_VIRTIO_INPUT_HID,
    .instance_size = sizeof(VirtIOInputAndroidHID),
    .instance_init = virtio_android_init,
    .instance_finalize = virtio_android_fini,
    .class_init = virtio_android_class_init,
};

/* ----------------------------------------------------------------- */

static void virtio_input_android_pci_class_init(ObjectClass* klass, const void* data) {
    PCIDeviceClass* pcidev_k = PCI_DEVICE_CLASS(klass);
    pcidev_k->class_id = PCI_CLASS_INPUT_OTHER;
}

static void virtio_input_android_initfn(Object* obj) {
    VirtIOInputAndroidHIDPCI* dev = VIRTIO_INPUT_ANDROID_HID_PCI(obj);
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev), TYPE_VIRTIO_INPUT_ANDROID_HID);
}

static const VirtioPCIDeviceTypeInfo virtio_android_pci_info = {
    .generic_name = TYPE_VIRTIO_INPUT_ANDROID_PCI,
    .parent = TYPE_VIRTIO_INPUT_HID_PCI,
    .class_init = virtio_input_android_pci_class_init,
    .instance_size = sizeof(VirtIOInputAndroidHIDPCI),
    .instance_init = virtio_input_android_initfn,
};

/* ----------------------------------------------------------------- */

void virtio_input_android_register_types(void) {
    type_register_static(&virtio_android_info);
    virtio_pci_types_register(&virtio_android_pci_info);
}
