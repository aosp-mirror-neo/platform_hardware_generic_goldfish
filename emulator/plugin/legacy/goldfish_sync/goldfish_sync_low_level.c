// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

/**
 * @file
 * @brief Thin QEMU QOM `SysBusDevice` shim for the legacy Goldfish Sync device (`GFSH0006`).
 */

#include "goldfish/legacy/goldfish_sync_low_level.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "migration/blocker.h"
#include "system/memory.h"
#include "google/drivers/hw/acpi/goldfish_defs.h"
#include "google/system/aemu_func_defs.h"
// IWYU pragma: end_keep
// clang-format on

#include "goldfish/legacy/common/c/device_env.h"
#include "goldfish/legacy/common/c/mmio_core.h"
#include "goldfish_sync.h"

/** @brief QOM type name for the Goldfish Sync `SysBusDevice`. */
#define TYPE_GOLDFISH_SYNC "goldfish_sync"

/** @brief Type-checked QOM cast macro for `GoldfishSyncState`. */
#define GOLDFISH_SYNC(obj) OBJECT_CHECK(struct GoldfishSyncState, (obj), TYPE_GOLDFISH_SYNC)

/**
 * @brief QOM instance state for the `"goldfish_sync"` `SysBusDevice`.
 */
struct GoldfishSyncState {
    SysBusDevice parent_obj; /**< Base QEMU `SysBusDevice` instance. */

    MemoryRegion iomem;       /**< 8 KiB (`0x2000`) MMIO region mapped at `0xff014000` on x86_64. */
    qemu_irq irq;             /**< Outbound system-bus interrupt line (GSI 21 on x86_64). */
    QEMUBH* irq_bh;           /**< Bottom half for delivering cross-thread IRQ updates under BQL. */
    Error* migration_blocker; /**< Registered live-migration / snapshot blocker for v1. */
    struct GoldfishMmioCore core; /**< Opaque handle and dispatch table for C++ core logic. */
    int irq_level;                /**< Latest interrupt level (0 or 1), accessed atomically. */
};

static bool goldfish_sync_guest_mem_read(void* opaque, uint64_t gpa, void* dst, size_t size) {
    (void)opaque;
    if (size == 0) {
        return true;
    }
    if (dst == NULL || gpa + size < gpa) {
        return false;
    }

    AddressSpace* as = get_qemu_system_address_space();
    assert(as != NULL);

    uint8_t stack_buf[256];
    uint8_t* staging = (size <= sizeof(stack_buf)) ? stack_buf : (uint8_t*)g_malloc(size);
    size_t offset = 0;

    while (offset < size) {
        hwaddr plen = (hwaddr)(size - offset);
        void* ptr =
                address_space_map(as, (hwaddr)(gpa + offset), &plen, false, MEMTXATTRS_UNSPECIFIED);
        if (ptr == NULL || plen == 0) {
            if (ptr != NULL) {
                address_space_unmap(as, ptr, plen, false, 0);
            }
            if (staging != stack_buf) {
                g_free(staging);
            }
            return false;
        }
        size_t chunk = (plen < (hwaddr)(size - offset)) ? (size_t)plen : (size - offset);
        memcpy(staging + offset, ptr, chunk);
        address_space_unmap(as, ptr, plen, false, (hwaddr)chunk);
        offset += chunk;
    }

    memcpy(dst, staging, size);
    if (staging != stack_buf) {
        g_free(staging);
    }
    return true;
}

static bool goldfish_sync_guest_mem_write(void* opaque, uint64_t gpa, const void* src,
                                          size_t size) {
    (void)opaque;
    if (size == 0) {
        return true;
    }
    if (src == NULL || gpa + size < gpa) {
        return false;
    }

    AddressSpace* as = get_qemu_system_address_space();
    assert(as != NULL);

    const uint8_t* bytes = (const uint8_t*)src;
    size_t offset = 0;

    while (offset < size) {
        hwaddr plen = (hwaddr)(size - offset);
        void* ptr =
                address_space_map(as, (hwaddr)(gpa + offset), &plen, true, MEMTXATTRS_UNSPECIFIED);
        if (ptr == NULL || plen == 0) {
            if (ptr != NULL) {
                address_space_unmap(as, ptr, plen, true, 0);
            }
            return false;
        }
        size_t chunk = (plen < (hwaddr)(size - offset)) ? (size_t)plen : (size - offset);
        memcpy(ptr, bytes + offset, chunk);
        address_space_unmap(as, ptr, plen, true, (hwaddr)chunk);
        offset += chunk;
    }

    return true;
}

static void goldfish_sync_irq_bh(void* opaque) {
    struct GoldfishSyncState* s = (struct GoldfishSyncState*)opaque;
    qemu_set_irq(s->irq, qatomic_read(&s->irq_level));
}

static void goldfish_sync_irq_set_level(void* opaque, bool level) {
    struct GoldfishSyncState* s = (struct GoldfishSyncState*)opaque;
    const int target_level = level ? 1 : 0;
    qatomic_set(&s->irq_level, target_level);
    if (bql_locked()) {
        qemu_set_irq(s->irq, target_level);
    } else {
        qemu_bh_schedule(s->irq_bh);
    }
}

static uint64_t goldfish_sync_mmio_read(void* opaque, hwaddr offset, unsigned size) {
    (void)size;
    struct GoldfishSyncState* s = (struct GoldfishSyncState*)opaque;
    assert(s->core.read != NULL);
    return s->core.read(s->core.core, (uint32_t)offset);
}

static void goldfish_sync_mmio_write(void* opaque, hwaddr offset, uint64_t val, unsigned size) {
    (void)size;
    struct GoldfishSyncState* s = (struct GoldfishSyncState*)opaque;
    assert(s->core.write != NULL);
    s->core.write(s->core.core, (uint32_t)offset, (uint32_t)val);
}

static const MemoryRegionOps goldfish_sync_iomem_ops = {
    .read = goldfish_sync_mmio_read,
    .write = goldfish_sync_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void goldfish_sync_realize(DeviceState* dev, Error** errp) {
    SysBusDevice* sbdev = SYS_BUS_DEVICE(dev);
    struct GoldfishSyncState* s = GOLDFISH_SYNC(dev);

    error_setg(&s->migration_blocker, "%s does not support migration/snapshots in v1",
               TYPE_GOLDFISH_SYNC);
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &goldfish_sync_iomem_ops, s, TYPE_GOLDFISH_SYNC,
                          GOLDFISH_SYNC_IOMEM_SIZE);
    sysbus_init_mmio(sbdev, &s->iomem);
    sysbus_init_irq(sbdev, &s->irq);
    s->irq_level = 0;
    s->irq_bh = qemu_bh_new(goldfish_sync_irq_bh, s);

    struct GoldfishDeviceEnv env;
    memset(&env, 0, sizeof(env));
    env.memory.opaque = s;
    env.memory.read = goldfish_sync_guest_mem_read;
    env.memory.write = goldfish_sync_guest_mem_write;
    env.irq.opaque = s;
    env.irq.set_level = goldfish_sync_irq_set_level;

    s->core = GoldfishSyncCoreCreate(&env);
    add_deletable_object(OBJECT(dev));
}

static void goldfish_sync_unrealize(DeviceState* dev) {
    struct GoldfishSyncState* s = GOLDFISH_SYNC(dev);
    assert(s->core.destroy != NULL);
    s->core.destroy(s->core.core);
    memset(&s->core, 0, sizeof(s->core));
    if (s->irq_bh != NULL) {
        qemu_bh_delete(s->irq_bh);
        s->irq_bh = NULL;
    }
    if (s->migration_blocker != NULL) {
        migrate_del_blocker(&s->migration_blocker);
    }
}

static void goldfish_sync_reset_hold(Object* obj, ResetType type) {
    (void)type;
    struct GoldfishSyncState* s = GOLDFISH_SYNC(obj);
    assert(s->core.reset != NULL);
    s->core.reset(s->core.core);
}

static void goldfish_sync_class_init(ObjectClass* klass, const void* data) {
    (void)data;
    DeviceClass* dc = DEVICE_CLASS(klass);
    ResettableClass* rc = RESETTABLE_CLASS(klass);

    dc->realize = goldfish_sync_realize;
    dc->unrealize = goldfish_sync_unrealize;
    dc->desc = "goldfish sync";
    rc->phases.hold = goldfish_sync_reset_hold;
}

static const TypeInfo goldfish_sync_info = {
    .name = TYPE_GOLDFISH_SYNC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct GoldfishSyncState),
    .class_init = goldfish_sync_class_init,
};

void goldfish_sync_register_types(void) {
    type_register_static(&goldfish_sync_info);
}
