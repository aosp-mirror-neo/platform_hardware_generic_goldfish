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
 * @file address_space_low_level.c
 * @brief QEMU PCI QOM shim for the legacy `goldfish_address_space` device (`607D:F153`).
 *
 * Implements the thin C11 QEMU hardware glue layer for `/dev/goldfish_address_space`:
 * - **PCI Identity**: Vendor `0x607D`, Device `0xF153`, Revision `1`, Class `PCI_CLASS_OTHERS`,
 *   INTx Pin B (`GOLDFISH_ADDRESS_SPACE_PCI_INTERRUPT_PIN`).
 * - **BAR 0 (Control MMIO)**: 4 KiB 32-bit little-endian MMIO region forwarding 4-byte aligned
 *   register reads and writes to the QEMU-independent C++ core (`GoldfishMmioCore`).
 * - **BAR 1 (Shared Memory Window)**: 64-bit memory container (`16 GiB` on x86_64/Linux/Windows,
 *   `8 GiB` on Apple Silicon arm64) initialized via `memory_region_init` with zero upfront host
 *   RAM. Host-backed RAM subregions (`memory_region_init_ram_ptr`) are dynamically mapped and
 *   unmapped on demand via `GoldfishBar1Ops`.
 * - **RCU-Deferred Retirement**: Each mapped BAR 1 slice is wrapped in a refcounted QOM object
 *   (`Bar1Subregion`). Unmapping deletes the subregion and drops its QOM reference, deferring
 *   `bar1_subregion_finalize` (and the caller's `retire_cb` that frees host/gfxstream memory) until
 *   QEMU's RCU grace period / Bottom Half retires all FlatViews and RAMBlock references.
 * - **Snapshot Blocker**: Registers a QEMU migration blocker in v1 (`b/573994361`).
 */

#include "goldfish/legacy/goldfish_address_space_low_level.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qemu/rcu.h"
#include "qom/object.h"
#include "system/memory.h"
#include "hw/core/qdev.h"
#include "hw/core/resettable.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "migration/blocker.h"
#include "google/drivers/hw/acpi/goldfish_defs.h"
#include "google/system/aemu_func_defs.h"
// IWYU pragma: end_keep
// clang-format on

#include "goldfish/legacy/common/c/device_env.h"
#include "goldfish/legacy/common/c/mmio_core.h"
#include "goldfish_address_space.h"

#if defined(__APPLE__) && defined(__arm64__)
/** @brief Fallback BAR 1 container size on Apple Silicon HVF hosts (8 GiB). */
#define GOLDFISH_AS_DEFAULT_BAR1_SIZE (8ULL * 1024ULL * 1024ULL * 1024ULL)
#else
/** @brief Fallback BAR 1 container size on x86_64 Linux/Windows hosts (16 GiB). */
#define GOLDFISH_AS_DEFAULT_BAR1_SIZE (16ULL * 1024ULL * 1024ULL * 1024ULL)
#endif

/**
 * @brief Refcounted QOM wrapper owning one host-backed RAM subregion inside PCI BAR 1.
 *
 * Embedding `MemoryRegion` inside a dedicated `TYPE_OBJECT` subclass ties the backing buffer's
 * lifetime to QEMU's RCU FlatView reference count: `bar1_subregion_finalize` only runs after all
 * vCPU threads and RCU readers can no longer observe the removed `MemoryRegion`.
 */
typedef struct Bar1Subregion {
    Object parent_obj;                /**< Base QOM object providing reference counting. */
    MemoryRegion mr;                  /**< QEMU RAM-pointer `MemoryRegion` child object. */
    void (*retire_cb)(void*);         /**< Optional callback run on QOM finalize after RCU. */
    void* retire_opaque;              /**< Opaque context passed to `retire_cb`. */
    QTAILQ_ENTRY(Bar1Subregion) next; /**< Intrusive link in `GoldfishAddressSpacePciState`. */
    uint64_t offset;                  /**< Byte offset of this subregion relative to BAR 1 base. */
    uint64_t size;                    /**< Length of the mapped host buffer in bytes. */
} Bar1Subregion;

/** @brief QOM type name for the refcounted BAR 1 RAM subregion node. */
#define TYPE_GOLDFISH_BAR1_SUBREGION "goldfish-bar1-subregion"
DECLARE_INSTANCE_CHECKER(Bar1Subregion, GOLDFISH_BAR1_SUBREGION, TYPE_GOLDFISH_BAR1_SUBREGION)

/**
 * @brief QEMU `PCIDevice` instance state for `goldfish_address_space` (`607D:F153`).
 */
typedef struct GoldfishAddressSpacePciState {
    PCIDevice pci_dev;                       /**< Parent QEMU PCI device state. */
    MemoryRegion bar0_mr;                    /**< BAR 0: 4 KiB 32-bit MMIO control registers. */
    MemoryRegion bar1_mr;                    /**< BAR 1: 64-bit container window for subregions. */
    QTAILQ_HEAD(, Bar1Subregion) subregions; /**< Live BAR 1 RAM subregions, guarded by the BQL. */
    Error* migration_blocker;                /**< Registered blocker preventing v1 VM snapshots. */
    struct GoldfishMmioCore core;            /**< C++ `GoldfishAddressSpaceDevice` vtable. */
    uint64_t bar1_size;                      /**< Total byte capacity of the BAR 1 window. */
} GoldfishAddressSpacePciState;

/** @brief QOM type name (`"goldfish_address_space"`) matching QEMU board definitions. */
#define TYPE_GOLDFISH_ADDRESS_SPACE GOLDFISH_ADDRESS_SPACE_NAME
DECLARE_INSTANCE_CHECKER(GoldfishAddressSpacePciState, GOLDFISH_ADDRESS_SPACE_PCI,
                         TYPE_GOLDFISH_ADDRESS_SPACE)

/** @brief Saved parent `ResettablePhases` chained during `goldfish_address_space_reset_hold`. */
static ResettablePhases s_parent_reset_phases;

/**
 * @brief Reads `size` bytes from guest physical memory `[gpa, gpa + size)` into `dst`.
 *
 * Loops over `address_space_map` / `address_space_unmap` to handle buffers spanning guest page or
 * RAM-block boundaries. Reads are staged into a temporary buffer (256-byte stack buffer for small
 * structs like the 40-byte `AddressSpacePing`, heap buffer otherwise) so that any unmapped page
 * midway through `[gpa, gpa + size)` aborts without leaving partial bytes in `dst`.
 *
 * @param opaque Unused device pointer from `GoldfishGuestMemoryOps`.
 * @param gpa Guest physical start address to read from.
 * @param dst Destination host buffer of at least `size` bytes.
 * @param size Number of bytes to read (`0` succeeds immediately).
 * @return `true` if all `size` bytes were mapped and copied into `dst`; `false` on overflow or
 *         unmapped guest address.
 */
static bool goldfish_as_guest_mem_read(void* opaque, uint64_t gpa, void* dst, size_t size) {
    if (size == 0U) {
        return true;
    }
    (void)opaque;
    /* Reject null destination or 64-bit GPA wrap-around before touching QEMU's AddressSpace. */
    if (dst == NULL || gpa + size < gpa) {
        return false;
    }

    AddressSpace* as = get_qemu_system_address_space();
    assert(as != NULL);

    /* Stage the entire read so dst is untouched if a later page in the range fails to map. */
    uint8_t stack_buf[256];
    uint8_t* staging = (size <= sizeof(stack_buf)) ? stack_buf : (uint8_t*)g_malloc(size);

    uint64_t cur_gpa = gpa;
    size_t offset = 0U;
    size_t remaining = size;
    while (remaining > 0U) {
        hwaddr plen = (hwaddr)remaining;
        void* ptr = address_space_map(as, (hwaddr)cur_gpa, &plen, false, MEMTXATTRS_UNSPECIFIED);
        if (ptr == NULL || plen == 0U) {
            if (ptr != NULL) {
                address_space_unmap(as, ptr, plen, false, 0U);
            }
            if (staging != stack_buf) {
                g_free(staging);
            }
            return false;
        }
        /* address_space_map may clamp plen at a RAMBlock boundary; copy that chunk and advance. */
        const size_t chunk = (plen < (hwaddr)remaining) ? (size_t)plen : remaining;
        memcpy(staging + offset, ptr, chunk);
        address_space_unmap(as, ptr, plen, false, (hwaddr)chunk);
        cur_gpa += chunk;
        offset += chunk;
        remaining -= chunk;
    }

    memcpy(dst, staging, size);
    if (staging != stack_buf) {
        g_free(staging);
    }
    return true;
}

/**
 * @brief Writes `size` bytes from host buffer `src` into guest physical memory `[gpa, gpa + size)`.
 *
 * Iterates with `address_space_map(..., is_write=true, ...)` across page or RAM-block boundaries,
 * dirtying each written chunk via `address_space_unmap`.
 *
 * @param opaque Unused device pointer from `GoldfishGuestMemoryOps`.
 * @param gpa Guest physical start address to write to.
 * @param src Source host buffer containing `size` bytes.
 * @param size Number of bytes to write (`0` succeeds immediately).
 * @return `true` if all `size` bytes were written to guest RAM; `false` on overflow or unmapped
 *         guest address.
 */
static bool goldfish_as_guest_mem_write(void* opaque, uint64_t gpa, const void* src, size_t size) {
    if (size == 0U) {
        return true;
    }
    (void)opaque;
    /* Reject null source or 64-bit GPA wrap-around. */
    if (src == NULL || gpa + size < gpa) {
        return false;
    }

    AddressSpace* as = get_qemu_system_address_space();
    assert(as != NULL);

    const uint8_t* src_bytes = (const uint8_t*)src;
    uint64_t cur_gpa = gpa;
    size_t offset = 0U;
    size_t remaining = size;
    while (remaining > 0U) {
        hwaddr plen = (hwaddr)remaining;
        void* ptr = address_space_map(as, (hwaddr)cur_gpa, &plen, true, MEMTXATTRS_UNSPECIFIED);
        if (ptr == NULL || plen == 0U) {
            if (ptr != NULL) {
                address_space_unmap(as, ptr, plen, true, 0U);
            }
            return false;
        }
        const size_t chunk = (plen < (hwaddr)remaining) ? (size_t)plen : remaining;
        memcpy(ptr, src_bytes + offset, chunk);
        address_space_unmap(as, ptr, plen, true, (hwaddr)chunk);
        cur_gpa += chunk;
        offset += chunk;
        remaining -= chunk;
    }

    return true;
}

/**
 * @brief QOM `instance_finalize` hook for `Bar1Subregion`, invoked after RCU retirement.
 *
 * Runs once `memory_region_del_subregion` has removed the subregion from BAR 1 and QEMU's RCU
 * reclaim / Bottom Half has released the last FlatView reference to `node->mr`. Unparents the
 * embedded `MemoryRegion` and invokes `retire_cb` with the BQL held so the core can safely destroy
 * the underlying host buffer or gfxstream `RingBlob`.
 *
 * @param obj QOM `Object*` pointing to the dying `Bar1Subregion`.
 */
static void bar1_subregion_finalize(Object* obj) {
    Bar1Subregion* node = GOLDFISH_BAR1_SUBREGION(obj);
    object_unparent(OBJECT(&node->mr));
    if (node->retire_cb != NULL) {
        void (*cb)(void*) = node->retire_cb;
        void* cb_opaque = node->retire_opaque;
        node->retire_cb = NULL;
        node->retire_opaque = NULL;
        /* RCU finalizers may run on QEMU's RCU call_rcu thread without the BQL; acquire it first
         * because retire callbacks may call into IGoldfishGraphicsBridge::CloseAsg. */
        const bool need_bql = !bql_locked();
        if (need_bql) {
            bql_lock();
        }
        cb(cb_opaque);
        if (need_bql) {
            bql_unlock();
        }
    }
}

/**
 * @brief Drives the PCI INTx interrupt line (Pin B) for `goldfish_address_space`.
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @param level `true` to assert INTx (`1`), `false` to deassert (`0`).
 */
static void goldfish_as_irq_set_level(void* opaque, bool level) {
    assert(opaque != NULL);
    GoldfishAddressSpacePciState* s = (GoldfishAddressSpacePciState*)opaque;
    pci_set_irq(&s->pci_dev, level ? 1 : 0);
}

/**
 * @brief Queries the guest physical base address currently assigned to PCI BAR 1 by guest firmware
 *        or kernel PCI enumeration.
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @return Guest physical base address of BAR 1, or `0` if BAR 1 is currently unmapped
 *         (`PCI_BAR_UNMAPPED`).
 */
static uint64_t goldfish_as_bar1_guest_phys_base(void* opaque) {
    assert(opaque != NULL);
    GoldfishAddressSpacePciState* s = (GoldfishAddressSpacePciState*)opaque;
    const pcibus_t addr = s->pci_dev.io_regions[GOLDFISH_ADDRESS_SPACE_AREA_BAR].addr;
    if (addr == PCI_BAR_UNMAPPED) {
        return 0ULL;
    }
    return (uint64_t)addr;
}

/**
 * @brief Maps a host-page-aligned host memory buffer into PCI BAR 1 as a RAM subregion.
 *
 * Enforces strict host page alignment on `offset`, `size`, and `host_ptr` (4 KiB on x86_64, 16 KiB
 * on Apple Silicon HVF where misaligned hypervisor memslots abort the VM), verifies bounds and
 * non-overlap under the BQL, and attaches a refcounted `Bar1Subregion` via
 * `memory_region_init_ram_ptr` + `memory_region_add_subregion`.
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @param offset Byte offset within BAR 1 (`[0, bar1_size)`), multiple of `host_page_size`.
 * @param host_ptr Host virtual address backing the mapping, aligned to `host_page_size`.
 * @param size Mapping length in bytes (`> 0`), multiple of `host_page_size`.
 * @return `true` if the subregion was added to BAR 1; `false` on invalid alignment, out-of-bounds
 *         range, or overlap with an existing subregion.
 */
static bool goldfish_as_bar1_map(void* opaque, uint64_t offset, void* host_ptr, uint64_t size) {
    assert(opaque != NULL);
    if (host_ptr == NULL || size == 0ULL) {
        return false;
    }

    GoldfishAddressSpacePciState* s = (GoldfishAddressSpacePciState*)opaque;
    const uint64_t host_page = GoldfishAddressSpaceHostPageSize();
    /* Hypervisor memory slots (especially Apple Silicon HVF 16 KiB pages) require GPA offset,
     * length, and HVA to all be multiples of the host page size. */
    if (host_page == 0ULL || (offset % host_page) != 0ULL || (size % host_page) != 0ULL ||
        (((uintptr_t)host_ptr) % (uintptr_t)host_page) != 0U) {
        return false;
    }
    if (offset > s->bar1_size || size > s->bar1_size - offset) {
        return false;
    }

    const bool need_bql = !bql_locked();
    if (need_bql) {
        bql_lock();
    }

    /* Reject overlapping subregions so every mapping is uniquely addressable by start offset. */
    Bar1Subregion* existing = NULL;
    QTAILQ_FOREACH(existing, &s->subregions, next) {
        if (offset < existing->offset + existing->size && existing->offset < offset + size) {
            if (need_bql) {
                bql_unlock();
            }
            return false;
        }
    }

    Bar1Subregion* node = GOLDFISH_BAR1_SUBREGION(object_new(TYPE_GOLDFISH_BAR1_SUBREGION));
    node->offset = offset;
    node->size = size;
    node->retire_cb = NULL;
    node->retire_opaque = NULL;
    memory_region_init_ram_ptr(&node->mr, OBJECT(node), "goldfish-as-sub", size, host_ptr);
    memory_region_add_subregion(&s->bar1_mr, (hwaddr)offset, &node->mr);
    QTAILQ_INSERT_TAIL(&s->subregions, node, next);

    if (need_bql) {
        bql_unlock();
    }
    return true;
}

/**
 * @brief Removes the BAR 1 subregion at `offset` and schedules `cb` once RCU retires its FlatView.
 *
 * Detaches `node->mr` from `bar1_mr` immediately under the BQL so new guest accesses stop seeing
 * the mapping, then drops the list's QOM reference via `object_unref`. When the last RCU FlatView
 * reference drops, `bar1_subregion_finalize` invokes `cb(cb_opaque)`. If no subregion exists at
 * `offset` (or `opaque` is `NULL`), `cb` is invoked immediately so callers never leak resources.
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @param offset Exact BAR 1 byte offset previously passed to `goldfish_as_bar1_map`.
 * @param cb Optional retirement callback invoked after RCU grace period completion.
 * @param cb_opaque Opaque user pointer forwarded to `cb`.
 */
static void goldfish_as_bar1_unmap_and_retire(void* opaque, uint64_t offset, void (*cb)(void*),
                                              void* cb_opaque) {
    assert(opaque != NULL);

    const bool need_bql = !bql_locked();
    if (need_bql) {
        bql_lock();
    }

    GoldfishAddressSpacePciState* s = (GoldfishAddressSpacePciState*)opaque;
    Bar1Subregion* node = NULL;
    Bar1Subregion* tmp = NULL;
    QTAILQ_FOREACH_SAFE(node, &s->subregions, next, tmp) {
        if (node->offset == offset) {
            node->retire_cb = cb;
            node->retire_opaque = cb_opaque;
            /* Unlink from BAR 1 first; object_unref triggers bar1_subregion_finalize once RCU
             * readers release the old FlatView. */
            memory_region_del_subregion(&s->bar1_mr, &node->mr);
            QTAILQ_REMOVE(&s->subregions, node, next);
            object_unref(OBJECT(node));
            if (need_bql) {
                bql_unlock();
            }
            return;
        }
    }

    if (need_bql) {
        bql_unlock();
    }
    if (cb != NULL) {
        cb(cb_opaque);
    }
}

/**
 * @brief Removes the BAR 1 subregion at `offset` without a retirement callback.
 *
 * Used when the caller either retains ownership of the backing memory or knows it outlives RCU.
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @param offset Exact BAR 1 byte offset previously passed to `goldfish_as_bar1_map`.
 */
static void goldfish_as_bar1_unmap(void* opaque, uint64_t offset) {
    goldfish_as_bar1_unmap_and_retire(opaque, offset, NULL, NULL);
}

/**
 * @brief Executes `fn(fn_opaque)` while holding QEMU's Big QEMU Lock (BQL).
 *
 * Acquires the BQL first if the calling thread (such as a gfxstream Vulkan/GL decoder thread) does
 * not already hold it, making guest-block validation, BAR 1 mapping, and bookkeeping atomic with
 * respect to vCPU MMIO handlers, `Reset()`, and device teardown.
 *
 * @warning Deliberately ignores and never dereferences `opaque`, because background gfxstream
 *          threads may invoke `run_locked` after the `PCIDevice` has been unrealized.
 *
 * @param opaque Unused `GoldfishBar1Ops` pointer (must NOT be dereferenced).
 * @param fn Callback function to invoke under the BQL.
 * @param fn_opaque Opaque argument forwarded to `fn`.
 */
static void goldfish_as_bar1_run_locked(void* opaque, void (*fn)(void*), void* fn_opaque) {
    (void)opaque;
    assert(fn != NULL);
    const bool need_bql = !bql_locked();
    if (need_bql) {
        bql_lock();
    }
    fn(fn_opaque);
    if (need_bql) {
        bql_unlock();
    }
}

/**
 * @brief Force-unmaps any remaining BAR 1 subregions during PCI reset or unrealize.
 *
 * Normally the C++ core (`Reset()` / `destroy()`) has already unmapped every subregion; this acts
 * as a defensive safety net before `bar1_mr` is torn down.
 *
 * @param s Owning `GoldfishAddressSpacePciState` (caller must hold the BQL).
 */
static void goldfish_as_unmap_all_subregions(GoldfishAddressSpacePciState* s) {
    Bar1Subregion* node = NULL;
    Bar1Subregion* tmp = NULL;
    QTAILQ_FOREACH_SAFE(node, &s->subregions, next, tmp) {
        memory_region_del_subregion(&s->bar1_mr, &node->mr);
        QTAILQ_REMOVE(&s->subregions, node, next);
        object_unref(OBJECT(node));
    }
}

/**
 * @brief QEMU `MemoryRegionOps::read` callback for BAR 0 control registers (`0x00..0x30`).
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @param offset 4-byte aligned byte offset within BAR 0.
 * @param size Access width in bytes (enforced to `4` by `goldfish_address_space_bar0_ops`).
 * @return Zero-extended 32-bit register value returned by the C++ core.
 */
static uint64_t goldfish_address_space_bar0_read(void* opaque, hwaddr offset, unsigned size) {
    (void)size;
    GoldfishAddressSpacePciState* s = (GoldfishAddressSpacePciState*)opaque;
    assert(s != NULL);
    assert(s->core.read != NULL);
    return (uint64_t)s->core.read(s->core.core, (uint32_t)offset);
}

/**
 * @brief QEMU `MemoryRegionOps::write` callback for BAR 0 control registers (`0x00..0x30`).
 *
 * @param opaque Pointer to `GoldfishAddressSpacePciState`.
 * @param offset 4-byte aligned byte offset within BAR 0.
 * @param val 32-bit value written by the guest vCPU.
 * @param size Access width in bytes (enforced to `4` by `goldfish_address_space_bar0_ops`).
 */
static void goldfish_address_space_bar0_write(void* opaque, hwaddr offset, uint64_t val,
                                              unsigned size) {
    (void)size;
    GoldfishAddressSpacePciState* s = (GoldfishAddressSpacePciState*)opaque;
    assert(s != NULL);
    assert(s->core.write != NULL);
    s->core.write(s->core.core, (uint32_t)offset, (uint32_t)val);
}

/** @brief MMIO dispatch table for the 4 KiB BAR 0 control register window. */
static const MemoryRegionOps goldfish_address_space_bar0_ops = {
    .read = goldfish_address_space_bar0_read,
    .write = goldfish_address_space_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/**
 * @brief `PCIDeviceClass::realize` handler initializing BAR 0, BAR 1, migration blocker, and C++
 *        core.
 *
 * Sets up the 4 KiB 32-bit MMIO control BAR (BAR 0) and the 16 GiB / 8 GiB 64-bit container memory
 * window (BAR 1) without allocating upfront host RAM, then instantiates the C++ core via
 * `GoldfishAddressSpaceCoreCreate`.
 *
 * @param pci_dev QEMU `PCIDevice` being realized.
 * @param errp QEMU error out-parameter populated if migration blocker registration fails.
 */
static void goldfish_address_space_realize(PCIDevice* pci_dev, Error** errp) {
    GoldfishAddressSpacePciState* s = GOLDFISH_ADDRESS_SPACE_PCI(pci_dev);
    QTAILQ_INIT(&s->subregions);

    error_setg(&s->migration_blocker, "%s does not support migration/snapshots in v1",
               TYPE_GOLDFISH_ADDRESS_SPACE);
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }

    pci_config_set_interrupt_pin(pci_dev->config, GOLDFISH_ADDRESS_SPACE_PCI_INTERRUPT_PIN);

    /* BAR 0: 4 KiB 32-bit MMIO control registers. */
    memory_region_init_io(&s->bar0_mr, OBJECT(s), &goldfish_address_space_bar0_ops, s,
                          GOLDFISH_ADDRESS_SPACE_CONTROL_NAME, GOLDFISH_ADDRESS_SPACE_CONTROL_SIZE);
    pci_register_bar(pci_dev, GOLDFISH_ADDRESS_SPACE_CONTROL_BAR, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar0_mr);

    const uint64_t macro_bar1_size = (uint64_t)GOLDFISH_ADDRESS_SPACE_AREA_SIZE;
    s->bar1_size = (macro_bar1_size != 0ULL) ? macro_bar1_size : GOLDFISH_AS_DEFAULT_BAR1_SIZE;

    /* BAR 1: pure 64-bit container MemoryRegion (never memory_region_init_ram, which would commit
     * 16 GiB of host RAM upfront). Subregions are mapped dynamically via goldfish_as_bar1_map. */
    memory_region_init(&s->bar1_mr, OBJECT(s), GOLDFISH_ADDRESS_SPACE_AREA_NAME, s->bar1_size);
    pci_register_bar(pci_dev, GOLDFISH_ADDRESS_SPACE_AREA_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar1_mr);

    struct GoldfishDeviceEnv env;
    memset(&env, 0, sizeof(env));
    env.memory.opaque = s;
    env.memory.read = &goldfish_as_guest_mem_read;
    env.memory.write = &goldfish_as_guest_mem_write;
    env.irq.opaque = s;
    env.irq.set_level = &goldfish_as_irq_set_level;
    env.bar1.opaque = s;
    env.bar1.guest_phys_base = &goldfish_as_bar1_guest_phys_base;
    env.bar1.map = &goldfish_as_bar1_map;
    env.bar1.unmap = &goldfish_as_bar1_unmap;
    env.bar1.unmap_and_retire = &goldfish_as_bar1_unmap_and_retire;
    env.bar1.run_locked = &goldfish_as_bar1_run_locked;
    env.bar1.size = s->bar1_size;
    env.bar1.host_page_size = GoldfishAddressSpaceHostPageSize();

    s->core = GoldfishAddressSpaceCoreCreate(&env);
    add_deletable_object(OBJECT(pci_dev));
}

/**
 * @brief `PCIDeviceClass::exit` (unrealize) handler tearing down the C++ core, BAR 1 subregions,
 *        and migration blocker.
 *
 * Destroys the C++ core first (which unmaps subregions and schedules RCU retirement callbacks
 * before releasing host/gfxstream memory), then cleans up any residual subregions and removes the
 * migration blocker.
 *
 * @param pci_dev QEMU `PCIDevice` being unrealized.
 */
static void goldfish_address_space_exit(PCIDevice* pci_dev) {
    GoldfishAddressSpacePciState* s = GOLDFISH_ADDRESS_SPACE_PCI(pci_dev);
    assert(s->core.destroy != NULL);
    s->core.destroy(s->core.core);
    s->core.core = NULL;
    goldfish_as_unmap_all_subregions(s);
    if (s->migration_blocker != NULL) {
        migrate_del_blocker(&s->migration_blocker);
    }
}

/**
 * @brief `ResettableClass::phases.hold` handler invoked on machine or PCI bus reset.
 *
 * Chains the parent `PCIDevice` reset phase first, resets the C++ core (which tears down all
 * active handles and shared pools), and ensures no BAR 1 subregions remain mapped.
 *
 * @param obj QOM `Object*` for `GoldfishAddressSpacePciState`.
 * @param type QEMU reset trigger type.
 */
static void goldfish_address_space_reset_hold(Object* obj, ResetType type) {
    if (s_parent_reset_phases.hold != NULL) {
        s_parent_reset_phases.hold(obj, type);
    }
    GoldfishAddressSpacePciState* s = GOLDFISH_ADDRESS_SPACE_PCI(obj);
    assert(s->core.reset != NULL);
    s->core.reset(s->core.core);
    goldfish_as_unmap_all_subregions(s);
}

/**
 * @brief QOM `class_init` callback configuring PCI IDs (`607D:F153`), lifecycle hooks, and reset
 *        chaining for `goldfish_address_space`.
 *
 * @param klass QOM `ObjectClass*` being initialized.
 * @param data Unused type registration user data.
 */
static void goldfish_address_space_class_init(ObjectClass* klass, const void* data) {
    (void)data;
    DeviceClass* dc = DEVICE_CLASS(klass);
    PCIDeviceClass* pc = PCI_DEVICE_CLASS(klass);
    ResettableClass* rc = RESETTABLE_CLASS(klass);

    pc->realize = goldfish_address_space_realize;
    pc->exit = goldfish_address_space_exit;
    pc->vendor_id = GOLDFISH_ADDRESS_SPACE_PCI_VENDOR_ID;
    pc->device_id = GOLDFISH_ADDRESS_SPACE_PCI_DEVICE_ID;
    pc->revision = GOLDFISH_ADDRESS_SPACE_PCI_REVISION;
    pc->class_id = PCI_CLASS_OTHERS;

    dc->desc = "goldfish address space";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    resettable_class_set_parent_phases(rc, NULL, goldfish_address_space_reset_hold, NULL,
                                       &s_parent_reset_phases);
}

/** @brief QOM `TypeInfo` descriptor for `Bar1Subregion`. */
static const TypeInfo goldfish_bar1_subregion_info = {
    .name = TYPE_GOLDFISH_BAR1_SUBREGION,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(Bar1Subregion),
    .instance_finalize = bar1_subregion_finalize,
};

/** @brief QOM `TypeInfo` descriptor for the `goldfish_address_space` conventional PCI device. */
static const TypeInfo goldfish_address_space_info = {
    .name = TYPE_GOLDFISH_ADDRESS_SPACE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GoldfishAddressSpacePciState),
    .class_init = goldfish_address_space_class_init,
    .interfaces =
            (const InterfaceInfo[]){
                {INTERFACE_CONVENTIONAL_PCI_DEVICE},
                {},
            },
};

void goldfish_address_space_register_types(void) {
    type_register_static(&goldfish_bar1_subregion_info);
    type_register_static(&goldfish_address_space_info);
}