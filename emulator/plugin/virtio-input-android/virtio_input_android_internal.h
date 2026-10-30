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
 *   This device is designed to handle the following input event types:
 *     - Multi-touch events (`MT_...`)
 *     - `EV_SW` (e.g., fold/unfold events)
 *   It registers with QEMU to handle these event masks:
 *     - `INPUT_EVENT_MASK_MTT`  (Touch events)
 *     - `INPUT_EVENT_MASK_ABS`  (Absolute mouse events - *not* relative)
 *     - `INPUT_EVENT_MASK_BTN`  (Mouse buttons)
 *
 * **Mouse Event Translation:**
 *   By default, the device translates mouse events into touch events.
 *
 * **Limitations:**
 *   - **Wheel Events:**  Wheel events are *not* currently handled, as there's no
 *     established mechanism to simulate wheel events as a sequence of touch events.
 *
 * **Display and Head Routing:**
 *  During device registration, you must specify the display and head the device
 *  is attached to. QEMU uses this information to route input events to the
 *  correct handler.
 *  Example:
 *  ```bash
 *    qemu-system-x86_64 ... -device virtio-input-android-pci,display=gpu0,head=0 ...
 *  ```
 *   This command connects the device to display `gpu0` and head `0`.  Mouse clicks
 *   on a VNC display connected to `gpu0`, head `0` will be routed to this device.
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
 * @brief Maximum number of simultaneous touch pointers supported.
 *
 *  This represents the maximum number of fingers or styluses that can be
 *  simultaneously tracked by the multi-touch emulation.  It's currently set to INPUT_BUTTON__MAX,
 *  so we can translate button clicks to finger tracking when needed.
 */
#define MTS_POINTERS_NUM INPUT_BUTTON__MAX

/**
 * @brief Indicates that a pointer is not currently tracked (finger/stylus is up).
 *
 *  This value signifies that a particular pointer slot is not in use, meaning
 *  the corresponding finger or stylus is not touching the screen.
 */
#define MTS_POINTER_UP -1

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

/**
 * @brief Default value for TOUCH_MAJOR and TOUCH_MINOR.
 *
 * This is the default size of touch events that will be send to the device.
 */
#define MTS_TOUCH_AXIS_DEFAULT 0x500

#define TYPE_VIRTIO_INPUT_ANDROID_HID "virtio-input-android"
#define TYPE_VIRTIO_INPUT_ANDROID_HID_PCI "virtio-input-android-pci"

OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputAndroidHID, VIRTIO_INPUT_ANDROID_HID);
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputAndroidHIDPCI, VIRTIO_INPUT_ANDROID_HID_PCI)

/**
 * @struct MTSPointerState
 * @brief Represents the state of a single multi-touch pointer (finger or stylus).
 *
 * This structure holds the data for a single touch point, including its
 * tracking ID, position, pressure, and the dimensions of the touch ellipse.
 */
typedef struct MTSPointerState {
    /**
     * @brief Tracking ID assigned to the pointer.
     *
     *  This ID is used by applications emulating multi-touch to uniquely
     *  identify and track individual touch points.
     */
    int tracking_id;
    /**
     * @brief X-coordinate of the pointer.
     *
     * The horizontal position of the touch point on the screen.
     */
    int x;
    /**
     * @brief Y-coordinate of the pointer.
     *
     * The vertical position of the touch point on the screen.
     */
    int y;
    /**
     * @brief Current pressure value.
     *
     * Represents the pressure applied by the finger or stylus, ranging from 0
     * (no pressure) to #MTS_PRESSURE_RANGE_MAX.
     */
    int pressure;
    /**
     * @brief Major axis of the touch ellipse.
     *
     *  The length of the major axis of the ellipse representing the area of contact.
     */
    int touch_major;
    /**
     * @brief Minor axis of the touch ellipse.
     *
     * The length of the minor axis of the ellipse representing the area of contact.
     */
    int touch_minor;
} MTSPointerState;

/**
 * @struct VirtIOInputAndroidHID
 * @brief Represents the state of the virtio-input-android device (HID variant).
 * @extends VirtIOInputHID
 *
 * This structure holds the complete state of the virtio-input-android device,
 * including the state of multiple touch pointers, button states, and device configuration.
 */
struct VirtIOInputAndroidHID {
    /*< private >*/
    VirtIOInputHID parent_obj;

    /*< public >*/
    /**
     * @brief Index of the currently selected multi-touch slot.
     *
     *  This variable tracks the last pointer for which `ABS_MT_SLOT` was sent.
     *  A value of -1 indicates that no slot selection has been made yet.
     */
    int current_slot;

    /**
     * @brief Array indicating the state of mouse buttons.
     *
     * A map to tell us if any given button is currently down.
     */
    bool keymap_button_down[INPUT_BUTTON__MAX];

    /**
     * @brief Array of multi-touch pointer states.
     *
     *  This array stores the state of each tracked pointer (finger or stylus),
     *  up to #INPUT_BUTTON__MAX
     */
    MTSPointerState tracked_pointers[INPUT_BUTTON__MAX];

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
