/* Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Lazy loader for libgfxstream_backend.
 *
 * Neither qemu-system-* nor the goldfish plugin links libgfxstream_backend at
 * build time (no DT_NEEDED / LC_LOAD_DYLIB / import entry): loading it would
 * pull EGL, ANGLE and the Vulkan loader into every emulator process, including
 * ones that never instantiate a virtio-gpu-gfxstream device (-gpu off, or
 * -virtio-gpu-device rutabaga, whose librutabaga_ffi embeds its own copy of
 * gfxstream and must not coexist with this one in a process).  Instead
 * gfxstream_init() (gfxstream_renderer.c, from the device's realize() hook)
 * calls gfxstream_backend_load(), which opens the library once via GLib GModule
 * (G_MODULE_BIND_LOCAL, thread-safe via GOnce) and resolves every
 * stream_renderer_* entry point into a function table.  The library is never
 * unloaded.
 *
 * Search order: $ANDROID_EMULATOR_LIBRARY_DIR (set by the launcher), then the
 * directory of the plugin itself, then - only when the variable is unset - the
 * dynamic loader's default search path.
 */

#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifndef _WIN32
#include <sys/uio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct stream_renderer_param;
struct stream_renderer_resource_create_args;
struct stream_renderer_command;
struct stream_renderer_box;
struct stream_renderer_create_blob;
struct stream_renderer_handle;
struct stream_renderer_fence;
struct iovec;

/*
 * X-macro over the entry points: X(slot, return type, parameter list, argument list).
 * The slot name is the stream_renderer_* symbol with the prefix stripped.
 */
#define GFXSTREAM_BACKEND_FOR_EACH_FN(X)                                                           \
    X(init, int, (struct stream_renderer_param * params, uint64_t num_params),                     \
      (params, num_params))                                                                        \
    X(teardown, void, (void), ())                                                                  \
    X(resource_create, int,                                                                        \
      (struct stream_renderer_resource_create_args * args, struct iovec * iov, uint32_t num_iovs), \
      (args, iov, num_iovs))                                                                       \
    X(resource_unref, void, (uint32_t res_handle), (res_handle))                                   \
    X(context_create, int,                                                                         \
      (uint32_t ctx_id, uint32_t nlen, const char* name, uint32_t context_init),                   \
      (ctx_id, nlen, name, context_init))                                                          \
    X(context_destroy, void, (uint32_t handle), (handle))                                          \
    X(submit_cmd, int, (struct stream_renderer_command * cmd), (cmd))                              \
    X(transfer_read_iov, int,                                                                      \
      (uint32_t handle, uint32_t ctx_id, uint32_t level, uint32_t stride, uint32_t layer_stride,   \
       struct stream_renderer_box* box, uint64_t offset, struct iovec* iov, int iovec_cnt),        \
      (handle, ctx_id, level, stride, layer_stride, box, offset, iov, iovec_cnt))                  \
    X(transfer_write_iov, int,                                                                     \
      (uint32_t handle, uint32_t ctx_id, int level, uint32_t stride, uint32_t layer_stride,        \
       struct stream_renderer_box* box, uint64_t offset, struct iovec* iovec,                      \
       unsigned int iovec_cnt),                                                                    \
      (handle, ctx_id, level, stride, layer_stride, box, offset, iovec, iovec_cnt))                \
    X(get_cap_set, void, (uint32_t set, uint32_t* max_ver, uint32_t* max_size),                    \
      (set, max_ver, max_size))                                                                    \
    X(fill_caps, void, (uint32_t set, uint32_t version, void* caps), (set, version, caps))         \
    X(resource_attach_iov, int, (int res_handle, struct iovec* iov, int num_iovs),                 \
      (res_handle, iov, num_iovs))                                                                 \
    X(resource_detach_iov, void, (int res_handle, struct iovec** iov, int* num_iovs),              \
      (res_handle, iov, num_iovs))                                                                 \
    X(ctx_attach_resource, void, (int ctx_id, int res_handle), (ctx_id, res_handle))               \
    X(ctx_detach_resource, void, (int ctx_id, int res_handle), (ctx_id, res_handle))               \
    X(create_blob, int,                                                                            \
      (uint32_t ctx_id, uint32_t res_handle,                                                       \
       const struct stream_renderer_create_blob* create_blob, const struct iovec* iovecs,          \
       uint32_t num_iovs, const struct stream_renderer_handle* handle),                            \
      (ctx_id, res_handle, create_blob, iovecs, num_iovs, handle))                                 \
    X(resource_map, int, (uint32_t res_handle, void** hva_out, uint64_t* size_out),                \
      (res_handle, hva_out, size_out))                                                             \
    X(resource_unmap, int, (uint32_t res_handle), (res_handle))                                    \
    X(resource_map_info, int, (uint32_t res_handle, uint32_t* map_info), (res_handle, map_info))   \
    X(create_fence, int, (const struct stream_renderer_fence* fence), (fence))                     \
    X(snapshot, int, (const char* dir), (dir))                                                     \
    X(restore, int, (const char* dir), (dir))

/* One function pointer per entry point; see GFXSTREAM_BACKEND_FOR_EACH_FN. */
struct gfxstream_backend_fns {
#define GFXSTREAM_BACKEND_DECLARE_SLOT(slot, ret, params, args) ret(*slot) params;
    GFXSTREAM_BACKEND_FOR_EACH_FN(GFXSTREAM_BACKEND_DECLARE_SLOT)
#undef GFXSTREAM_BACKEND_DECLARE_SLOT
};

/*
 * Loads libgfxstream_backend (once per process, thread-safe via GOnce) and
 * resolves the function table.  Returns NULL on failure, stores a g_malloc()ed
 * human-readable reason in *error_msg (caller frees; may be NULL), and sets
 * *out_errno (may be NULL) to:
 *   - 0 on success
 *   - ENOENT if no candidate library file exists
 *   - EIO if a candidate library file exists on disk but fails to load
 *   - ENOSYS if the library loads but is missing required symbols
 * Repeated calls after a failure return the same failure without retrying.
 *
 * Search order: $ANDROID_EMULATOR_LIBRARY_DIR (set by the launcher to
 * <emulator>/lib/qemu), then the directory this plugin was loaded from, then
 * (only when $ANDROID_EMULATOR_LIBRARY_DIR is unset) the dynamic loader's
 * default search path (LD_LIBRARY_PATH / rpath / PATH).
 */
const struct gfxstream_backend_fns* gfxstream_backend_load(char** error_msg, int* out_errno);

/* The table if gfxstream_backend_load() already succeeded, NULL otherwise. */
const struct gfxstream_backend_fns* gfxstream_backend_fns(void);

#ifdef __cplusplus
}  // extern "C"
#endif
