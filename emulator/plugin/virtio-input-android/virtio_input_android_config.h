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
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct virtio_input_config;

/**
 * Returns the static virtio-input configuration table (terminated by an entry
 * with `select == 0`) advertised by `virtio-input-android`.
 */
const struct virtio_input_config* virtio_input_android_configs(void);

/** Returns the `EV_KEY` codes advertised by `virtio-input-android`. */
const unsigned short* virtio_input_android_ev_key_codes(size_t* count);

/** Returns the `EV_ABS` codes advertised by `virtio-input-android`. */
const unsigned short* virtio_input_android_ev_abs_codes(size_t* count);

/**
 * Looks up the `[min, max]` range configured for `axis` (`ABS_*`) in
 * `virtio_input_android_configs()`, converting little-endian config fields to
 * host order. Returns false if `axis` has no `VIRTIO_INPUT_CFG_ABS_INFO` entry.
 */
bool virtio_input_android_get_abs_range(uint8_t axis, int32_t* min_out, int32_t* max_out);

__END_DECLS
