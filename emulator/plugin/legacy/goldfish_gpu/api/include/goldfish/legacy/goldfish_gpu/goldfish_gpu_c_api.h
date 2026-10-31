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

/**
 * @file
 * @brief C ABI of the legacy GoldfishGpu lifecycle and snapshot hooks. Every function operates on
 * the process-wide `goldfish::legacy::GetGoldfishGpu()` instance and logs failures with LOG(ERROR).
 *
 * Save sequence (driven by a VM change-state handler and two standalone VMState sections, see
 * goldfish_gpu_low_level.c):
 *   0. vm_stop(RUN_STATE_SAVE_VM) notifies the change-state handler → `GoldfishGpuCoreQuiesce(NULL)`
 *      pauses the renderer before QEMU saves guest RAM.
 *   1. "goldfish_gpu_begin" (`MIG_PRI_IOMMU`, before goldfish_address_space / goldfish_sync /
 *      goldfish_pipe): `GoldfishGpuCorePreSave(snap_name)` ensures the renderer is paused, writes
 *      the gfxstream render/texture streams, and computes + caches the meta blob inside the GPU
 *      (gfxstream's `addressSpaceGraphicsConsumerSave()` must run before the ASG blob memory is
 *      snapshotted).
 *   2. "goldfish_gpu_end" (`MIG_PRI_LOW`, after goldfish_address_space, goldfish_sync, and
 *      goldfish_pipe): `GoldfishGpuCoreSave()` returns the cached blob, which is written as a be64
 *      length + bytes, and its `post_save` calls `GoldfishGpuCorePostSave()` to resume the
 *      renderer once `goldfish_pipe` has saved the opengles read queues.
 * Load sequence mirrors it: `GoldfishGpuCoreQuiesce(NULL)` from vm_stop(RUN_STATE_RESTORE_VM) before
 * RAM is overwritten, `GoldfishGpuCorePreLoad()` from the "goldfish_gpu_begin" section (drops the
 * live ASG consumers and render channels), `GoldfishGpuCoreLoad()` from the "goldfish_gpu_end"
 * section once goldfish_address_space has restored the ASG blobs and goldfish_pipe has staged the
 * opengles pipe callbacks, and `GoldfishGpuCorePostLoad()` from the "goldfish_gpu_end" section's
 * `post_load` to resume the renderer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Maximum size of a GoldfishGpu snapshot meta blob (256 MiB). It holds ASG consumer and
 * render-channel bookkeeping (<= 65536 entries each, a few hundred bytes per entry plus the bytes
 * buffered in each render channel) and the last posted frame (width x height x 4 bytes; an 8K
 * 7680x4320 frame is 127 MiB). `GoldfishGpuCoreSave()` refuses to produce a larger blob and
 * `GoldfishGpuCoreLoad()` / the VMState `get` reject one.
 */
#define GOLDFISH_GPU_SNAPSHOT_MAX_BYTES ((uint64_t)256U << 20)

/**
 * @brief Largest frame edge (pixels) the GPU ever hands to the post callback or stores in a
 * snapshot. 16384 covers 8K and foldable panels while keeping width * height * 4 far below any
 * integer limit. Consumers of `GoldfishPostCallback` may rely on both dimensions being <= this.
 */
#define GOLDFISH_GPU_MAX_FRAME_DIM 16384U

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GoldfishGpuFrameCallback)(void* opaque, uint32_t width, uint32_t height,
                                         const uint8_t* bgra_pixels);

/**
 * Starts the legacy GoldfishGpu renderer. `guest_api_level` is the AVD's Android API level and
 * must be non-zero.
 */
bool GoldfishGpuCoreStart(uint32_t width, uint32_t height, uint32_t guest_api_level,
                          const char* renderer_features, const char* snapshot_directory,
                          GoldfishGpuFrameCallback on_frame, void* opaque, char* err_buf,
                          size_t err_buf_size);
void GoldfishGpuCoreStop(void);
/**
 * Quiesces the renderer at QEMU exit without destroying it (see
 * `IGoldfishGpu::StopForProcessExit`).
 */
void GoldfishGpuCoreStopForProcessExit(void);
void GoldfishGpuCoreFlush(void);

/**
 * Registers the standalone "goldfish_gpu_begin" and "goldfish_gpu_end" VMState sections that
 * snapshot the legacy GoldfishGpu, and the snapshot-progress handler that resumes the renderer
 * when a save/load finishes without reaching the matching post hook. Must be called from
 * goldfish_pipe's realize right after `GoldfishGpuCoreStart()` succeeded. Takes the BQL if the
 * caller does not hold it. Idempotent. Implemented in `goldfish_gpu_low_level.c` (`:impl`).
 */
void goldfish_gpu_vmstate_register(void);
/** Undoes `goldfish_gpu_vmstate_register()`. Safe to call when nothing is registered. */
void goldfish_gpu_vmstate_unregister(void);

/**
 * Pauses the renderer, the ASG consumers and the opengles reader threads so that none of them
 * touches guest RAM any more. Called from the VM change-state handler when the VM stops
 * (RUN_STATE_SAVE_VM / RESTORE_VM / FINISH_MIGRATE / SHUTDOWN / PAUSED, with `snapshot_name == NULL`)
 * before QEMU's RAM pass. Idempotent. Undone by `GoldfishGpuCorePostSave()`/`PostLoad()`, or by
 * `GoldfishGpuCoreOnSnapshotFinished()` when no snapshot operation follows (plain pause/continue,
 * failed save, cancelled load).
 */
bool GoldfishGpuCoreQuiesce(const char* snapshot_name);
/**
 * Ensures the GPU is quiesced, writes the gfxstream render/texture streams under the configured
 * snapshot directory, and builds + caches the meta blob inside the GPU before
 * `goldfish_address_space` snapshots the ASG ring storage. Called from the "goldfish_gpu_begin"
 * VMState section's `pre_save`.
 */
bool GoldfishGpuCorePreSave(const char* snapshot_name);
/**
 * Returns the serialized GPU meta blob cached by `GoldfishGpuCorePreSave()`. Fails (and logs) when
 * `GoldfishGpuCorePreSave()` has not completed for this snapshot: the blob must be built before
 * `goldfish_address_space` saves the ASG ring memory, so `Save` never builds it on its own.
 * `*out_data` is owned by the caller and released with `GoldfishGpuCoreFreeBuffer()`; it is NULL
 * with `*out_size == 0` when the renderer is not running.
 */
bool GoldfishGpuCoreSave(const char* snapshot_name, uint8_t** out_data, size_t* out_size);
void GoldfishGpuCoreFreeBuffer(uint8_t* data);
/** Resumes everything paused by `GoldfishGpuCoreQuiesce()`. */
bool GoldfishGpuCorePostSave(const char* snapshot_name);
/** Stops the reader threads and destroys the live ASG consumers / render channels. Idempotent. */
bool GoldfishGpuCorePreLoad(const char* snapshot_name);
/**
 * Restores the GPU state from a blob written by `GoldfishGpuCoreSave()`. Requires the ASG blobs
 * to have been restored (`IGoldfishGpu::RestoreAsgBlob()`) by goldfish_address_space first.
 */
bool GoldfishGpuCoreLoad(const char* snapshot_name, const uint8_t* data, size_t size);
/** Resumes the renderer after a load; the opengles pipes must have been re-bound before. */
bool GoldfishGpuCorePostLoad(const char* snapshot_name);
/**
 * Called when QEMU reports that a snapshot save or load finished (completed or failed). Resumes
 * the renderer if a Quiesce/PreLoad was never matched by PostSave/PostLoad because another VMState
 * section failed. Must not be called with the BQL held: it may wait for renderer threads.
 */
void GoldfishGpuCoreOnSnapshotFinished(void);
/** Largest blob `GoldfishGpuCoreSave()` produces and `GoldfishGpuCoreLoad()` accepts. */
size_t GoldfishGpuCoreMaxSnapshotSize(void);

#ifdef __cplusplus
}
#endif
