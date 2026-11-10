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
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Single interrupt line control table provided by a C11 QOM shim.
 *
 * Implemented with `qemu_set_irq` for SysBus devices (`goldfish_pipe`, `goldfish_sync`, which
 * defer cross-thread updates to a `QEMUBH` when `!bql_locked()`) and `pci_set_irq` (INTx pin B)
 * for `goldfish_address_space`.
 */
struct GoldfishIrqOps {
    void* opaque; /**< Shim-owned device pointer passed to `set_level`. */

    /** @brief Drives the device interrupt line high (`true`) or low (`false`). */
    void (*set_level)(void* opaque, bool level);
};

#ifdef __cplusplus
}
#endif
