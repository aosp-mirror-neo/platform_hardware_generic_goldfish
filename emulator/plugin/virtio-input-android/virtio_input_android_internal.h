// Copyright (C) 2025 The Android Open Source Project
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
#ifndef VIRTIO_INPUT_ANDROID_H
#define VIRTIO_INPUT_ANDROID_H

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/virtio/virtio-input.h"
#include "hw/virtio/virtio-pci.h"
// IWYU pragma: end_keep
// clang-format on

/**
 * @defgroup virtio-input-android Virtio Input Android Driver
 * @brief This driver provides a virtio-based input device tailored for Android guests.
 *
 * @details Android phone images (API 35 and later) expect specific virtio input devices.
 * This driver implements the `virtio-input-android` device, which emulates multi-touch
 * and other input events for Android guests running in QEMU.  The device is designed to
 * interact with Android's input system, using configuration files (IDC) located on the
 * guest system.
 *
 * **Device Configuration (Guest):**
 *
 *   The Android guest uses Input Device Configuration (.idc) files to define the
 *   behavior of input devices.  You can find the relevant IDC files for this driver
 *   in the guest at:
 *   - `device/generic/goldfish/input/virtio_input_multi_touch_1.idc`
 *   - `device/generic/goldfish/input/virtio_input_multi_touch_2.idc`
 *   - ...etc.
 *
 *   These IDC files generally contain properties like:
 *   ```
 *   device.internal = 1
 *   touch.deviceType = touchScreen
 *   touch.orientationAware = 1
 *   cursor.mode = navigation
 *   cursor.orientationAware = 1
 *   ```
 *
 * **Device Mapping (Guest):**
 *
 *  The devices typically correspond to displays as follows:
 *  - `virtio_input_multi_touch_0` <-- Touch events for display 0
 *  - `virtio_input_multi_touch_1` <-- Touch events for display 1
 *  - `virtio_input_multi_touch_2` <-- Touch events for display 2
 *   ...
 *  - `virtio_input_multi_touch_11` <-- Touch events for display 11
 *
 * **QEMU Device Addition:**
 *
 *   Add the device in QEMU using the `-device` option:
 *   ```bash
 *   qemu-system-x86_64 ... -device virtio-input-android ...
 *   ```
 *   Each added device is assigned a sequential name:
 *   - The first device:  `virtio-input-android_1`
 *   - The second device: `virtio-input-android_2`
 *   - ...and so on.
 *
 * **Supported Events:**
 *   All touch, stylus, mouse-as-touch, and `EV_SW` (e.g., fold/unfold) events
 *   are injected directly by the host `InputDispatcher` as raw evdev reports via
 *   `virtio_input_send_evdev`. The QEMU `QemuInputHandler` required by
 *   `VirtIOInputHID` realization is registered with `.mask = 0` because QEMU
 *   console input routing is unused.
 *
 * **Display and Head Association:**
 *   During device registration, specify the display and head the device is
 *   attached to so the host can bind the touch device to its target display:
 *   ```bash
 *     qemu-system-x86_64 ... -device virtio-input-android-pci,display=gpu0,head=0 ...
 *   ```
 *
 */

/**
 * @brief String identifier for the virtio-input device (Android HID).
 *
 *  Use this with `-device` in QEMU.  Example: `-device virtio-input-android`
 */
#define TYPE_VIRTIO_INPUT_ANDROID_HID "virtio-input-android"

/**
 * @brief String identifier for the virtio-input device (Android PCI).
 *
 * Use this with `-device` in QEMU, usually for PCI-based configurations.
 * Example: `-device virtio-input-android-pci,display=gpu0,head=0`
 */
#define TYPE_VIRTIO_INPUT_ANDROID_PCI "virtio-input-android-pci"

/**
 * @brief The maximum number of supported virtio-input devices.
 *
 *  This limits the number of `virtio-input-android` devices that can be added to a QEMU instance.
 */
#define VIRTIO_INPUT_MAX_NUM 11

/**
 * @brief Maximum `ABS_MT_SLOT` index advertised to the guest (`0..10`).
 */
#define MTS_POINTERS_NUM 10

/**
 * @brief Maximum value for the major and minor axis of a touch.
 *
 * Represents the maximum dimension (in arbitrary units) that the touch ellipse
 * can have along its major and minor axes.
 */
#define MTS_TOUCH_AXIS_RANGE_MAX 0x7fffffff

/**
 * @brief Minimum and maximum values for touch orientation (representing [-90, 90] degrees).
 *
 * Defines the degree bounds representing the orientation of the touch/stylus
 * ellipse, corresponding to [-90, 90] degrees.
 */
#define MTS_ORIENTATION_RANGE_MIN -90
#define MTS_ORIENTATION_RANGE_MAX 90

/**
 * @brief Minimum and maximum values for stylus tilt X/Y (representing [-90, 90] degrees).
 */
#define MTS_TILT_RANGE_MIN -90
#define MTS_TILT_RANGE_MAX 90

/**
 * @brief Maximum value for touch pressure (1024 levels).
 *
 * Specifies the maximum pressure value that the device can report, representing
 * 1024 distinct levels of pressure sensitivity.
 */
#define MTS_PRESSURE_RANGE_MAX 0x400

#define TYPE_VIRTIO_INPUT_ANDROID_HID_PCI "virtio-input-android-pci"

OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputAndroidHID, VIRTIO_INPUT_ANDROID_HID);
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputAndroidHIDPCI, VIRTIO_INPUT_ANDROID_HID_PCI)

/**
 * @struct VirtIOInputAndroidHID
 * @brief Represents the state of the virtio-input-android device (HID variant).
 * @extends VirtIOInputHID
 *
 * This structure holds the state and configuration of the virtio-input-android device.
 */
struct VirtIOInputAndroidHID {
    /*< private >*/
    VirtIOInputHID parent_obj;

    /*< public >*/
    /**
     * @brief Pointer to the virtio-input device configuration.
     *
     * The input configuration that will be reported to the guest.
     */
    struct virtio_input_config* virtio_input_android_device_config;

    /**
     * @brief The device ID number.
     *
     * In `adb getevent`, this will show up as part of the device name:
     * `virtio_input_multi_touch_{device_id}`
     * The name that will be reported is virtio_input_multi_touch_%d, device_id
     */
    int device_id;

    /**
     * @brief Changes each time the guest driver goes active or inactive.
     *
     * Advanced in change_active, under the BQL, published via
     * virtio_input_android_notify_active_changed(), and readable as the QOM
     * property #VIRTIO_INPUT_ANDROID_ACTIVE_GENERATION. A change means the
     * guest forgot its touch contacts.
     */
    uint64_t active_generation;
};

/**
 * @struct VirtIOInputAndroidHIDPCI
 * @brief Represents the state of the virtio-input-android device (PCI variant).
 * @extends VirtIOPCIProxy
 *
 * This structure combines the PCI proxy with the HID-specific state.
 */
struct VirtIOInputAndroidHIDPCI {
    /*< private >*/
    VirtIOPCIProxy parent_obj;
    /*< public >*/

    /**
     * @brief The core HID device state.
     *
     * An instance of #VirtIOInputAndroidHID, holding the state specific to the
     * HID functionality.
     */
    VirtIOInputAndroidHID vdev;
};

#endif /* VIRTIO_INPUT_ANDROID_H */
