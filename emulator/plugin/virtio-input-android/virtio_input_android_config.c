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
#include "virtio_input_android_config.h"

#include "virtio_input_android_internal.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "hw/virtio/virtio-input.h"
#include "ui/console.h"
#include "standard-headers/linux/input.h"
// IWYU pragma: end_keep
// clang-format on

#include "goldfish/base/array_size.h"

// The EV_SW bitmap: SW_LID (fold/unfold) plus the wired headset jack switches.
enum {
    kEvSwBits = (1 << SW_LID) | (1 << SW_HEADPHONE_INSERT) | (1 << SW_MICROPHONE_INSERT),
};

static const struct virtio_input_config kVirtioAndroidConfig[] = {
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_X,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.min = const_le32(INPUT_EVENT_ABS_MIN),
        .u.abs.max = const_le32(INPUT_EVENT_ABS_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_Y,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.min = const_le32(INPUT_EVENT_ABS_MIN),
        .u.abs.max = const_le32(INPUT_EVENT_ABS_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_Z,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.min = const_le32(INPUT_EVENT_ABS_MIN),
        .u.abs.max = const_le32(1),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_SLOT,
        .size = sizeof(virtio_input_absinfo),
        // SLOT max value seems to be TRACKING_ID-1
        .u.abs.max = const_le32(MTS_POINTERS_NUM),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_TOUCH_MAJOR,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.max = const_le32(MTS_TOUCH_AXIS_RANGE_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_TOUCH_MINOR,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.max = const_le32(MTS_TOUCH_AXIS_RANGE_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_ORIENTATION,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.min = const_le32(MTS_ORIENTATION_RANGE_MIN),
        .u.abs.max = const_le32(MTS_ORIENTATION_RANGE_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_TILT_X,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.min = const_le32(MTS_TILT_RANGE_MIN),
        .u.abs.max = const_le32(MTS_TILT_RANGE_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_TILT_Y,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.min = const_le32(MTS_TILT_RANGE_MIN),
        .u.abs.max = const_le32(MTS_TILT_RANGE_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_POSITION_X,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.max = const_le32(INPUT_EVENT_ABS_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_POSITION_Y,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.max = const_le32(INPUT_EVENT_ABS_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_TOOL_TYPE,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.max = const_le32(MT_TOOL_MAX),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_TRACKING_ID,
        .size = sizeof(virtio_input_absinfo),
        // TRACKING_ID max value seems to be 0xFFFF
        .u.abs.max = const_le32(MTS_POINTERS_NUM + 1),
    },
    {
        .select = VIRTIO_INPUT_CFG_ABS_INFO,
        .subsel = ABS_MT_PRESSURE,
        .size = sizeof(virtio_input_absinfo),
        .u.abs.max = const_le32(MTS_PRESSURE_RANGE_MAX),
    },
    {
        // Fold/unfold and wired headset jack switches (EV_SW).
        .select = VIRTIO_INPUT_CFG_EV_BITS,
        .subsel = EV_SW,
        .size = 1,
        .u.bitmap =
                {
                    kEvSwBits,
                },
    },
    {/* end of list */},
};

static const unsigned short kEvKeyCodes[] = {BTN_TOOL_RUBBER, BTN_STYLUS};

static const unsigned short kEvAbsCodes[] = {ABS_X,
                                             ABS_Y,
                                             ABS_Z,
                                             ABS_TILT_X,
                                             ABS_TILT_Y,
                                             ABS_MT_SLOT,
                                             ABS_MT_TOUCH_MAJOR,
                                             ABS_MT_TOUCH_MINOR,
                                             ABS_MT_ORIENTATION,
                                             ABS_MT_POSITION_X,
                                             ABS_MT_POSITION_Y,
                                             ABS_MT_TOOL_TYPE,
                                             ABS_MT_TRACKING_ID,
                                             ABS_MT_PRESSURE};

const struct virtio_input_config* virtio_input_android_configs(void) {
    return kVirtioAndroidConfig;
}

const unsigned short* virtio_input_android_ev_key_codes(size_t* count) {
    if (count != NULL) {
        *count = ARRAY_SIZE(kEvKeyCodes);
    }
    return kEvKeyCodes;
}

const unsigned short* virtio_input_android_ev_abs_codes(size_t* count) {
    if (count != NULL) {
        *count = ARRAY_SIZE(kEvAbsCodes);
    }
    return kEvAbsCodes;
}

uint8_t virtio_input_android_ev_sw_bitmap(void) {
    for (size_t i = 0; kVirtioAndroidConfig[i].select != 0; ++i) {
        if (kVirtioAndroidConfig[i].select == VIRTIO_INPUT_CFG_EV_BITS &&
            kVirtioAndroidConfig[i].subsel == EV_SW && kVirtioAndroidConfig[i].size > 0) {
            return kVirtioAndroidConfig[i].u.bitmap[0];
        }
    }
    return 0;
}

bool virtio_input_android_get_abs_range(uint8_t axis, int32_t* min_out, int32_t* max_out) {
    for (size_t i = 0; kVirtioAndroidConfig[i].select != 0; ++i) {
        if (kVirtioAndroidConfig[i].select == VIRTIO_INPUT_CFG_ABS_INFO &&
            kVirtioAndroidConfig[i].subsel == axis) {
            if (min_out != NULL) {
                *min_out = (int32_t)le32_to_cpu(kVirtioAndroidConfig[i].u.abs.min);
            }
            if (max_out != NULL) {
                *max_out = (int32_t)le32_to_cpu(kVirtioAndroidConfig[i].u.abs.max);
            }
            return true;
        }
    }
    return false;
}
