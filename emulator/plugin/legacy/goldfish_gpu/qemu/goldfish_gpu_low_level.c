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
 * @brief QEMU VMState shim for the legacy GoldfishGpu (gfxstream renderer behind goldfish_pipe).
 *
 * The GoldfishGpu is not a QOM device, so its snapshot state is coordinated by a VM change-state
 * handler (`qemu_add_vm_change_state_handler`) and two standalone VMState sections registered with
 * `vmstate_register_with_alias_id(NULL, ...)`:
 *
 *  0. `goldfish_gpu_vm_state_change` (`qemu_add_vm_change_state_handler`): when `vm_stop()` stops
 *     the vCPUs (`!running` for `RUN_STATE_SAVE_VM`, `RUN_STATE_RESTORE_VM`,
 *     `RUN_STATE_FINISH_MIGRATE`, `RUN_STATE_SHUTDOWN`, or `RUN_STATE_PAUSED`), it calls
 *     `GoldfishGpuCoreQuiesce(NULL)` (`IGoldfishGpu::Quiesce`), which pauses and drains all ASG
 *     consumers, render channels, and pipe reader threads and flushes active ColorBuffers *before*
 *     QEMU starts its RAM save pass (`qemu_savevm_state()`). When the VM resumes (`running == true`)
 *     it calls `GoldfishGpuCoreOnSnapshotFinished()` synchronously, which un-quiesces the GPU if
 *     no snapshot section did (plain pause/continue, aborted save/load) and is a no-op otherwise.
 *     `snapshot_finished_bh` covers the remaining case: a save/load that failed while the VM
 *     stays stopped (progress handler, see below).
 *  1. `"goldfish_gpu_begin"` (`MIG_PRI_IOMMU`) only carries a version marker. Its `pre_save`
 *     (`GoldfishGpuCorePreSave`) ensures the renderer is paused (idempotent if the VM change-state
 *     handler already quiesced it), writes the gfxstream render/texture streams, and computes +
 *     caches the meta blob, because gfxstream's `addressSpaceGraphicsConsumerSave()` must run
 *     before goldfish_address_space snapshots the ASG blob memory; its `pre_load`
 *     (`GoldfishGpuCorePreLoad`) drops the live ASG consumers and render channels before
 *     goldfish_address_space restores the blobs.
 *  2. `"goldfish_gpu_end"` (`MIG_PRI_LOW`) carries the meta blob and runs after all
 *     `MIG_PRI_DEFAULT` devices (`goldfish_address_space`, `goldfish_sync`, `goldfish_pipe`). On
 *     save, its `post_save` (`GoldfishGpuCorePostSave`) resumes the renderer once `goldfish_pipe`
 *     has saved the opengles read queues. On load, it recreates the ASG consumers (using the blobs
 *     restored by `goldfish_address_space`) and render channels (attaching the pending callbacks
 *     bound by `goldfish_pipe::Load`), and its `post_load` (`GoldfishGpuCorePostLoad`) resumes the
 *     renderer. Should an intermediate section fail, the snapshot-progress handler below resumes
 *     the renderer instead.
 *
 * Ordering evidence (third_party/qemu/migration/savevm.c):
 *  - `savevm_state_handler_insert()` keeps `savevm_state.handlers` sorted by descending
 *    `save_state_priority()` (`vmsd->priority`, `MIG_PRI_DEFAULT` when 0): a new entry is inserted
 *    before the head of the next lower priority group, i.e. appended to the end of its own group.
 *    `vmstate_register_with_alias_id()` calls it for every registration, whether `obj` is NULL
 *    or a device, so section order = (priority desc, registration order).
 *    MIG_PRI_IOMMU ("goldfish_gpu_begin") > MIG_PRI_DEFAULT (goldfish_address_space PCI,
 *    goldfish_sync, goldfish_pipe) > MIG_PRI_LOW ("goldfish_gpu_end").
 *  - `qemu_savevm_state_complete_precopy_non_iterable()` saves the non-iterable sections by
 *    walking that list, and `vmstate_save_state_v()` (migration/vmstate.c) runs `pre_save`,
 *    the fields and `post_save` of one section before moving to the next.
 *  - `qemu_loadvm_state_main()` loads sections in stream order (`qemu_loadvm_section_start_full()`
 *    per section), and `vmstate_load_state()` runs `pre_load`, the fields and `post_load` of one
 *    section before the next, so the load order equals the save order.
 */

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu_c_api.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "migration/qemu-file-types.h"
#include "migration/snapshot.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "google/system/aemu_func_defs.h"
// IWYU pragma: end_keep
// clang-format on

/** @brief Value written by the "goldfish_gpu_begin" marker field. */
#define GOLDFISH_GPU_BEGIN_MARKER 1U

/** @brief Opaque passed to both VMState sections and the progress handler. */
struct GoldfishGpuVmState {
    /** Bottom half resuming the renderer once QEMU reports a save/load as finished. */
    QEMUBH* snapshot_finished_bh;
    /** Registration returned by `add_snapshot_progress_handler()`, or NULL. */
    QemuSnapshotProgressEntry* snapshot_progress_entry;
    /** Registration returned by `qemu_add_vm_change_state_handler()`, or NULL. */
    VMChangeStateEntry* vm_state_entry;
    bool registered; /**< True while the VMState sections are registered. */
};

static struct GoldfishGpuVmState g_goldfish_gpu_vmstate;

/*
 * BQL handling
 * ------------
 * Most GPU hooks wait for renderer threads (quiesce drains them, PreLoad/PostSave destroy ASG
 * consumers, Load/PostLoad create channels and remap rings), and those threads themselves take the
 * BQL through RunUnderBql for goldfish_address_space BAR 1 mappings. Holding the BQL across such a
 * wait would deadlock, so it is dropped around every hook that can wait:
 *
 *   hook               drops BQL   why
 *   vm_state_change    yes         Quiesce() waits for render threads to park
 *   begin.pre_save     yes         PreSave() quiesces if the VM was not stopped first
 *   end.put            NO          Save() only returns the blob cached by pre_save (no renderer call)
 *   end.post_save      yes         PostSave() drains retired ASG consumers (thread joins)
 *   begin.pre_load     yes         PreLoad() stops consumers/channels (thread joins)
 *   end.get            yes         Load() creates consumers that map rings via RunUnderBql
 *   end.post_load      yes         PostLoad() remaps rings and replays the last frame
 *   snapshot_finished  yes         OnSnapshotFinished() == resume path of post_save/post_load
 *
 * Consequence: while QEMU is walking `savevm_state.handlers`, other BQL users (gRPC/UI callbacks
 * via RunUnderBql, HAL plugs, audio) can run between an already-saved section and one not yet
 * saved. This is the same exposure the device-only code had (goldfish_address_space and
 * goldfish_pipe drop the BQL in their own hooks for the same reason); the vCPUs are stopped, the
 * GPU is quiesced before the first section, and the goldfish devices' guest-visible state is only
 * mutated by vCPU-driven MMIO, so the window does not reach the state those sections serialize.
 * Anything added to these hooks that does not wait on a renderer thread should keep the BQL.
 */
static bool goldfish_gpu_unlock_bql(void) {
    const bool unlock_bql = bql_locked();
    if (unlock_bql) {
        bql_unlock();
    }
    return unlock_bql;
}

static void goldfish_gpu_relock_bql(bool unlocked) {
    if (unlocked) {
        bql_lock();
    }
}

/* ---- "goldfish_gpu_begin" -------------------------------------------------------------------- */

static int goldfish_gpu_begin_pre_save(void* opaque) {
    (void)opaque;
    const char* snap_name = qemu_snapshot_in_flight_name();
    const bool unlocked = goldfish_gpu_unlock_bql();
    const bool ok = GoldfishGpuCorePreSave(snap_name);
    goldfish_gpu_relock_bql(unlocked);
    return ok ? 0 : -1;
}

static int goldfish_gpu_begin_pre_load(void* opaque) {
    (void)opaque;
    const char* snap_name = qemu_snapshot_in_flight_name();
    const bool unlocked = goldfish_gpu_unlock_bql();
    const bool ok = GoldfishGpuCorePreLoad(snap_name);
    goldfish_gpu_relock_bql(unlocked);
    return ok ? 0 : -1;
}

static int goldfish_gpu_begin_marker_put(QEMUFile* f, void* pv, size_t size,
                                         const VMStateField* field, JSONWriter* vmdesc) {
    (void)pv;
    (void)size;
    (void)field;
    (void)vmdesc;
    qemu_put_be32(f, GOLDFISH_GPU_BEGIN_MARKER);
    return 0;
}

static int goldfish_gpu_begin_marker_get(QEMUFile* f, void* pv, size_t size,
                                         const VMStateField* field) {
    (void)pv;
    (void)size;
    (void)field;
    const uint32_t marker = qemu_get_be32(f);
    if (marker != GOLDFISH_GPU_BEGIN_MARKER) {
        error_report("goldfish_gpu_begin: unexpected marker %" PRIu32 " (expected %u)", marker,
                     GOLDFISH_GPU_BEGIN_MARKER);
        return -1;
    }
    return 0;
}

/*
 * A local VMStateInfo instead of VMSTATE_UINT32: that macro embeds the address of QEMU's exported
 * `vmstate_info_uint32`, which cannot live in a `static const` initializer on Windows (the
 * plugin imports it from the executable), see goldfish_address_space_low_level.c.
 */
static const VMStateInfo goldfish_gpu_begin_marker_info = {
    .name = "goldfish_gpu_begin_marker",
    .get = goldfish_gpu_begin_marker_get,
    .put = goldfish_gpu_begin_marker_put,
};

static const VMStateDescription goldfish_gpu_begin_vmsd = {
    .name = "goldfish_gpu_begin",
    .version_id = 1,
    .minimum_version_id = 1,
    // "Must happen before PCI devices": ahead of goldfish_address_space (PCI, MIG_PRI_DEFAULT),
    // goldfish_sync (sysbus, MIG_PRI_DEFAULT), and goldfish_pipe (sysbus, MIG_PRI_DEFAULT).
    .priority = MIG_PRI_IOMMU,
    .pre_save = goldfish_gpu_begin_pre_save,
    .pre_load = goldfish_gpu_begin_pre_load,
    .fields =
            (const VMStateField[]){
                {
                    .name = "marker",
                    .version_id = 1,
                    .size = 0,
                    .info = &goldfish_gpu_begin_marker_info,
                    .flags = VMS_SINGLE,
                    .offset = 0,
                },
                VMSTATE_END_OF_LIST(),
            },
};

/* ---- "goldfish_gpu_end" ---------------------------------------------------------------------- */

static int goldfish_gpu_end_put(QEMUFile* f, void* pv, size_t size, const VMStateField* field,
                                JSONWriter* vmdesc) {
    (void)pv;
    (void)size;
    (void)field;
    (void)vmdesc;
    const char* snap_name = qemu_snapshot_in_flight_name();
    uint8_t* data = NULL;
    size_t data_size = 0;
    // Returns the blob cached by goldfish_gpu_begin_pre_save(); it never calls into the renderer
    // or waits for a renderer thread, so the BQL stays held (see "BQL handling" above).
    const bool ok = GoldfishGpuCoreSave(snap_name, &data, &data_size);
    if (!ok) {
        return -1;
    }
    // Refuse to write a blob that goldfish_gpu_end_get() would reject.
    if ((uint64_t)data_size > GOLDFISH_GPU_SNAPSHOT_MAX_BYTES) {
        error_report("goldfish_gpu_end: snapshot blob of %" PRIu64 " bytes exceeds the %" PRIu64
                     " byte load limit",
                     (uint64_t)data_size, (uint64_t)GOLDFISH_GPU_SNAPSHOT_MAX_BYTES);
        GoldfishGpuCoreFreeBuffer(data);
        return -1;
    }
    qemu_put_be64(f, (uint64_t)data_size);
    if (data_size > 0 && data != NULL) {
        qemu_put_buffer(f, data, data_size);
    }
    GoldfishGpuCoreFreeBuffer(data);
    return 0;
}

static int goldfish_gpu_end_get(QEMUFile* f, void* pv, size_t size, const VMStateField* field) {
    (void)pv;
    (void)size;
    (void)field;
    const char* snap_name = qemu_snapshot_in_flight_name();
    const uint64_t data_size = qemu_get_be64(f);
    // GOLDFISH_GPU_SNAPSHOT_MAX_BYTES (goldfish_gpu_c_api.h) bounds the ASG/render-channel
    // bookkeeping plus the last posted frame (256 MiB).
    if (data_size > GOLDFISH_GPU_SNAPSHOT_MAX_BYTES ||
        data_size > (uint64_t)GoldfishGpuCoreMaxSnapshotSize()) {
        error_report("goldfish_gpu_end: snapshot blob size %" PRIu64 " exceeds %" PRIu64, data_size,
                     (uint64_t)GOLDFISH_GPU_SNAPSHOT_MAX_BYTES);
        return -1;
    }
    uint8_t* data = NULL;
    if (data_size > 0) {
        data = (uint8_t*)g_try_malloc((size_t)data_size);
        if (data == NULL) {
            error_report("goldfish_gpu_end: failed to allocate %" PRIu64
                         " bytes for the snapshot blob",
                         data_size);
            return -1;
        }
        if (qemu_get_buffer(f, data, (size_t)data_size) != (size_t)data_size) {
            error_report("goldfish_gpu_end: snapshot blob truncated before %" PRIu64 " bytes",
                         data_size);
            g_free(data);
            return -1;
        }
    }
    const bool unlocked = goldfish_gpu_unlock_bql();
    const bool ok = GoldfishGpuCoreLoad(snap_name, data, (size_t)data_size);
    goldfish_gpu_relock_bql(unlocked);
    g_free(data);
    return ok ? 0 : -1;
}

static int goldfish_gpu_end_post_save(void* opaque) {
    (void)opaque;
    const char* snap_name = qemu_snapshot_in_flight_name();
    const bool unlocked = goldfish_gpu_unlock_bql();
    const bool ok = GoldfishGpuCorePostSave(snap_name);
    goldfish_gpu_relock_bql(unlocked);
    return ok ? 0 : -1;
}

static int goldfish_gpu_end_post_load(void* opaque, int version_id) {
    (void)opaque;
    (void)version_id;
    const char* snap_name = qemu_snapshot_in_flight_name();
    const bool unlocked = goldfish_gpu_unlock_bql();
    const bool ok = GoldfishGpuCorePostLoad(snap_name);
    goldfish_gpu_relock_bql(unlocked);
    return ok ? 0 : -1;
}

static const VMStateInfo goldfish_gpu_end_info = {
    .name = "goldfish_gpu_end_blob",
    .get = goldfish_gpu_end_get,
    .put = goldfish_gpu_end_put,
};

static const VMStateDescription goldfish_gpu_end_vmsd = {
    .name = "goldfish_gpu_end",
    .version_id = 1,
    .minimum_version_id = 1,
    // Lower priority than the MIG_PRI_DEFAULT devices (goldfish_address_space, goldfish_sync,
    // goldfish_pipe), so this section runs after all of them and resumes the GPU in its own
    // post_save/post_load.
    .priority = MIG_PRI_LOW,
    .post_save = goldfish_gpu_end_post_save,
    .post_load = goldfish_gpu_end_post_load,
    .fields =
            (const VMStateField[]){
                {
                    .name = "state",
                    .version_id = 1,
                    .size = 0,
                    .info = &goldfish_gpu_end_info,
                    .flags = VMS_SINGLE,
                    .offset = 0,
                },
                VMSTATE_END_OF_LIST(),
            },
};

/* ---- snapshot progress ----------------------------------------------------------------------- */

/*
 * Runs on the main loop with the BQL held. `GoldfishGpuCoreOnSnapshotFinished()` may wait for
 * renderer threads that themselves need the BQL, so the BQL is dropped around it.
 */
static void goldfish_gpu_snapshot_finished_bh(void* opaque) {
    struct GoldfishGpuVmState* s = (struct GoldfishGpuVmState*)opaque;
    if (!s->registered) {
        return;
    }
    const bool unlocked = goldfish_gpu_unlock_bql();
    GoldfishGpuCoreOnSnapshotFinished();
    goldfish_gpu_relock_bql(unlocked);
}

/*
 * QEMU reports COMPLETED or FAILED exactly once per save_snapshot()/load_snapshot(), even when a
 * VMState section between "goldfish_gpu_begin" and "goldfish_gpu_end" failed and
 * "goldfish_gpu_end"'s post_save/post_load (which resume the renderer) therefore never ran. The
 * handler runs synchronously inside the snapshot code with the BQL held and must not block or
 * release the BQL (QEMU is iterating its handler list), so the renderer resume is deferred to a
 * bottom half.
 */
static void goldfish_gpu_on_snapshot_progress(void* opaque, const QemuSnapshotProgress* progress) {
    struct GoldfishGpuVmState* s = (struct GoldfishGpuVmState*)opaque;
    if (progress == NULL || s->snapshot_finished_bh == NULL) {
        return;
    }
    if (progress->phase == QEMU_SNAPSHOT_PHASE_COMPLETED ||
        progress->phase == QEMU_SNAPSHOT_PHASE_FAILED) {
        qemu_bh_schedule(s->snapshot_finished_bh);
    }
}

/*
 * Mirrors virtio_gpu_gfxstream_vm_state_change(): when vm_stop() freezes the vCPUs before saving
 * or restoring a snapshot (or pausing/shutting down the VM), quiesce all ASG consumers, render
 * channels and pipe reader threads before QEMU starts iterating guest RAM. When the VM resumes
 * (`running == true`), un-quiesce synchronously so a plain pause/continue or an aborted snapshot
 * that never reached "goldfish_gpu_end"'s post_save/post_load has the renderer running again
 * before the first vCPU instruction executes (a bottom half would run after the vCPUs, leaving a
 * window in which guest GL traffic stalls on parked threads and a newly created ASG consumer picks
 * up a stale resume flag). GoldfishGpuCoreOnSnapshotFinished() is a no-op when a snapshot section
 * already resumed the GPU.
 */
static void goldfish_gpu_vm_state_change(void* opaque, bool running, RunState state) {
    struct GoldfishGpuVmState* s = (struct GoldfishGpuVmState*)opaque;
    if (!s->registered) {
        return;
    }
    if (!running) {
        if (state == RUN_STATE_SAVE_VM || state == RUN_STATE_RESTORE_VM ||
            state == RUN_STATE_FINISH_MIGRATE || state == RUN_STATE_SHUTDOWN ||
            state == RUN_STATE_PAUSED) {
            const bool unlocked = goldfish_gpu_unlock_bql();
            (void)GoldfishGpuCoreQuiesce(NULL);
            goldfish_gpu_relock_bql(unlocked);
        }
        return;
    }
    const bool unlocked = goldfish_gpu_unlock_bql();
    GoldfishGpuCoreOnSnapshotFinished();
    goldfish_gpu_relock_bql(unlocked);
}

/* ---- registration ---------------------------------------------------------------------------- */

void goldfish_gpu_vmstate_register(void) {
    struct GoldfishGpuVmState* s = &g_goldfish_gpu_vmstate;
    if (s->registered) {
        return;
    }
    // vmstate_register_with_alias_id() and add_snapshot_progress_handler() need the BQL.
    const bool lock_bql = !bql_locked();
    if (lock_bql) {
        bql_lock();
    }
    Error* err = NULL;
    if (vmstate_register_with_alias_id(NULL, 0, &goldfish_gpu_begin_vmsd, s, -1, 0, &err) < 0) {
        error_reportf_err(err, "goldfish_gpu: failed to register the begin VMState section: ");
        if (lock_bql) {
            bql_unlock();
        }
        return;
    }
    if (vmstate_register_with_alias_id(NULL, 0, &goldfish_gpu_end_vmsd, s, -1, 0, &err) < 0) {
        error_reportf_err(err, "goldfish_gpu: failed to register the end VMState section: ");
        vmstate_unregister(NULL, &goldfish_gpu_begin_vmsd, s);
        if (lock_bql) {
            bql_unlock();
        }
        return;
    }
    s->snapshot_finished_bh = qemu_bh_new(goldfish_gpu_snapshot_finished_bh, s);
    s->snapshot_progress_entry =
            add_snapshot_progress_handler(goldfish_gpu_on_snapshot_progress, s);
    s->vm_state_entry = qemu_add_vm_change_state_handler(goldfish_gpu_vm_state_change, s);
    s->registered = true;
    if (lock_bql) {
        bql_unlock();
    }
}

void goldfish_gpu_vmstate_unregister(void) {
    struct GoldfishGpuVmState* s = &g_goldfish_gpu_vmstate;
    if (!s->registered) {
        return;
    }
    const bool lock_bql = !bql_locked();
    if (lock_bql) {
        bql_lock();
    }
    s->registered = false;
    if (s->vm_state_entry != NULL) {
        qemu_del_vm_change_state_handler(s->vm_state_entry);
        s->vm_state_entry = NULL;
    }
    if (s->snapshot_progress_entry != NULL) {
        del_snapshot_progress_handler(s->snapshot_progress_entry);
        s->snapshot_progress_entry = NULL;
    }
    if (s->snapshot_finished_bh != NULL) {
        qemu_bh_cancel(s->snapshot_finished_bh);
        qemu_bh_delete(s->snapshot_finished_bh);
        s->snapshot_finished_bh = NULL;
    }
    vmstate_unregister(NULL, &goldfish_gpu_end_vmsd, s);
    vmstate_unregister(NULL, &goldfish_gpu_begin_vmsd, s);
    if (lock_bql) {
        bql_unlock();
    }
}
