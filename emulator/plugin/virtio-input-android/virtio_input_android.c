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

#define VIRTIO_ID_NAME_ANDROID "virtio_input_multi_touch_"

// Parent types
#define TYPE_VIRTIO_INPUT_HID_PCI "virtio-input-hid-pci"

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

static const QemuInputHandler virtio_android_handler_template = {
    // All touch, stylus, and mouse input is injected directly as raw evdev
    // reports via virtio_input_send_evdev; QEMU console input routing is unused.
    .name = VIRTIO_ID_NAME_ANDROID,
    .mask = 0,
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
