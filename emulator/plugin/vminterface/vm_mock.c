// Copyright 2025 The Android Open Source Project
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
#include "emulator/plugin/vminterface/test/vm_mock.h"

#include <assert.h>
#include <stdbool.h>

// Global variables for mock functions
static RunState runstate_get_value = RUN_STATE_DEBUG;
static ShutdownCause mock_shutdown_cause = SHUTDOWN_CAUSE_NONE;
static bool mock_iothread_locked = false;
static bool mock_snapshot_op_ok = false;
static MockSnapshotOpHook s_snapshot_op_hook = NULL;
static void* s_snapshot_op_hook_opaque = NULL;

// Bookkeeping of the QEMU snapshot entry points, see vm_mock.h.
static int s_snapshot_call_count = 0;
static GThread* s_snapshot_last_call_thread = NULL;
static char s_snapshot_last_call_name[256] = "";

static void MockSnapshotRecordCall(const char* name) {
    s_snapshot_call_count++;
    s_snapshot_last_call_thread = g_thread_self();
    if (name) {
        g_strlcpy(s_snapshot_last_call_name, name, sizeof(s_snapshot_last_call_name));
    }
}

int mock_snapshot_call_count(void) {
    return s_snapshot_call_count;
}

GThread* mock_snapshot_last_call_thread(void) {
    return s_snapshot_last_call_thread;
}

const char* mock_snapshot_last_call_name(void) {
    return s_snapshot_last_call_name;
}

void mock_snapshot_calls_reset(void) {
    s_snapshot_call_count = 0;
    s_snapshot_last_call_thread = NULL;
    s_snapshot_last_call_name[0] = '\0';
}

const char* error_get_pretty(const Error* err) {
    assert(err != NULL);
    return "error_get_pretty";
}
void error_free(Error* err) {
    (void)err;
}

// Getter and setter for RunState
RunState runstate_get() {
    return runstate_get_value;
}

void mock_runstate_set(RunState state) {
    runstate_get_value = state;
}

// Getter and setter for ShutdownCause
ShutdownCause mock_shutdown_cause_get() {
    return mock_shutdown_cause;
}

void mock_shutdown_cause_set(ShutdownCause cause) {
    mock_shutdown_cause = cause;
}

// Getter and setter for iothread_locked
bool mock_iothread_locked_get() {
    return mock_iothread_locked;
}

void mock_iothread_locked_set(bool locked) {
    mock_iothread_locked = locked;
}

void mock_snapshot_op_ok_set(bool ok) {
    mock_snapshot_op_ok = ok;
}

// NOLINTBEGIN(readability-identifier-naming)
void mock_snapshot_op_hook_set(MockSnapshotOpHook hook, void* opaque) {
    s_snapshot_op_hook = hook;
    s_snapshot_op_hook_opaque = opaque;
}
// NOLINTEND(readability-identifier-naming)

// Implementations of the mock functions

static int mock_vm_stop_return = 0;

void mock_vm_stop_return_set(int ret) {
    mock_vm_stop_return = ret;
}

int vm_stop(RunState state) {
    if (mock_vm_stop_return != 0) {
        return mock_vm_stop_return;
    }
    if (runstate_get_value == RUN_STATE_RUNNING || runstate_get_value == RUN_STATE_SUSPENDED) {
        mock_runstate_set(state);
    }
    return 0;
}

void vm_start() {
    mock_runstate_set(RUN_STATE_RUNNING);
}

void qemu_system_reset_request(ShutdownCause cause) {
    mock_shutdown_cause_set(cause);
}

int vm_shutdown() {
    mock_runstate_set(RUN_STATE_SHUTDOWN);
    return 0;
}

void qemu_system_shutdown_request(ShutdownCause reason) {
    mock_shutdown_cause_set(reason);
}

// NOLINTBEGIN(readability-identifier-naming)
bool bql_locked(void) {
    return mock_iothread_locked_get();
}

void bql_lock_impl(const char* file, int line) {
    (void)file;
    (void)line;
    mock_iothread_locked_set(true);
}

void bql_unlock(void) {
    mock_iothread_locked_set(false);
}
// NOLINTEND(readability-identifier-naming)

bool save_snapshot(const char* name, bool overwrite, const char* vmstate, bool has_devices,
                   strList* devices, Error** errp) {
    MockSnapshotRecordCall(name);
    (void)overwrite;
    (void)vmstate;
    (void)has_devices;
    (void)devices;
    *errp = NULL;
    if (s_snapshot_op_hook) {
        s_snapshot_op_hook(name, s_snapshot_op_hook_opaque);
    }
    /* Like QEMU, report the outcome to the progress handlers before returning. */
    QemuSnapshotProgress progress = {
        .operation = QEMU_SNAPSHOT_OP_SAVE,
        .phase = mock_snapshot_op_ok ? QEMU_SNAPSHOT_PHASE_COMPLETED : QEMU_SNAPSHOT_PHASE_FAILED,
        .name = name,
    };
    mock_fire_snapshot_progress(&progress);
    return mock_snapshot_op_ok;
}

bool load_snapshot(const char* name, const char* vmstate, bool has_devices, strList* devices,
                   Error** errp) {
    MockSnapshotRecordCall(name);
    (void)vmstate;
    (void)has_devices;
    (void)devices;
    *errp = NULL;
    if (s_snapshot_op_hook) {
        s_snapshot_op_hook(name, s_snapshot_op_hook_opaque);
    }
    /* Like QEMU, report the outcome to the progress handlers before returning. */
    QemuSnapshotProgress progress = {
        .operation = QEMU_SNAPSHOT_OP_LOAD,
        .phase = mock_snapshot_op_ok ? QEMU_SNAPSHOT_PHASE_COMPLETED : QEMU_SNAPSHOT_PHASE_FAILED,
        .name = name,
    };
    mock_fire_snapshot_progress(&progress);
    return mock_snapshot_op_ok;
}

void load_snapshot_resume(RunState state) {
    mock_runstate_set(state);
}

bool delete_snapshot(const char* name, bool has_devices, strList* devices, Error** errp) {
    MockSnapshotRecordCall(name);
    (void)has_devices;
    (void)devices;
    *errp = NULL;
    return mock_snapshot_op_ok;
}

static const QEMUSnapshotInfo* s_mock_snapshot_entries = NULL;
static int s_mock_snapshot_count = -1;

void mock_snapshot_list_set(const QEMUSnapshotInfo* entries, int count) {
    s_mock_snapshot_entries = entries;
    s_mock_snapshot_count = count;
}

BlockDriverState* bdrv_all_find_vmstate_bs(const char* vmstate_bs, bool has_devices,
                                           strList* devices, Error** errp) {
    (void)vmstate_bs;
    (void)has_devices;
    (void)devices;
    *errp = NULL;
    return (s_mock_snapshot_entries != NULL || s_mock_snapshot_count >= 0) ? (BlockDriverState*)0x1
                                                                           : NULL;
}

int bdrv_snapshot_list(BlockDriverState* bs, QEMUSnapshotInfo** psn_info) {
    (void)bs;
    MockSnapshotRecordCall(NULL);
    if (s_mock_snapshot_count <= 0 || s_mock_snapshot_entries == NULL) {
        *psn_info = NULL;
        return s_mock_snapshot_count;
    }
    *psn_info = g_new0(QEMUSnapshotInfo, s_mock_snapshot_count);
    memcpy(*psn_info, s_mock_snapshot_entries,
           sizeof(QEMUSnapshotInfo) * (size_t)s_mock_snapshot_count);
    return s_mock_snapshot_count;
}

static VMChangeStateHandler* s_change_handler = NULL;
static void* s_change_opaque = NULL;

VMChangeStateEntry* qemu_add_vm_change_state_handler(VMChangeStateHandler* cb, void* opaque) {
    s_change_handler = cb;
    s_change_opaque = opaque;
    return (VMChangeStateEntry*)0x1;
}

void qemu_del_vm_change_state_handler(VMChangeStateEntry* e) {
    s_change_handler = NULL;
    s_change_opaque = NULL;
}

void mock_fire_vm_change_state(bool running, RunState state) {
    if (s_change_handler) {
        s_change_handler(s_change_opaque, running, state);
    }
}

bool mock_has_vm_change_state_handler(void) {
    return s_change_handler != NULL;
}

static QemuSnapshotProgressHandler* s_snapshot_progress_handler = NULL;
static void* s_snapshot_progress_opaque = NULL;

// NOLINTBEGIN(readability-identifier-naming)
QemuSnapshotProgressEntry* add_snapshot_progress_handler(QemuSnapshotProgressHandler* cb,
                                                         void* opaque) {
    assert(bql_locked());
    s_snapshot_progress_handler = cb;
    s_snapshot_progress_opaque = opaque;
    return (QemuSnapshotProgressEntry*)0x1;
}

void del_snapshot_progress_handler(QemuSnapshotProgressEntry* e) {
    (void)e;
    assert(bql_locked());
    s_snapshot_progress_handler = NULL;
    s_snapshot_progress_opaque = NULL;
}
// NOLINTEND(readability-identifier-naming)

void mock_fire_snapshot_progress(const QemuSnapshotProgress* progress) {
    if (s_snapshot_progress_handler) {
        s_snapshot_progress_handler(s_snapshot_progress_opaque, progress);
    }
}

bool mock_has_snapshot_progress_handler(void) {
    return s_snapshot_progress_handler != NULL;
}
