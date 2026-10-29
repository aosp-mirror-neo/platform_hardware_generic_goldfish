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

#include <memory>

#include "absl/time/clock_interface.h"
#include "android/crashreport/hang_detector.h"
#include "android/goldfish/vm_interface.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/snapshot/snapshot_store.h"

namespace android {
namespace goldfish {

/**
 * @brief Snapshot configuration of the QEMU-backed VmOperations.
 */
struct QemuSnapshotConfig {
    /**
     * Owns the per-snapshot metadata under <avd>/snapshots/<name>/. Snapshot
     * operations consult it off the QEMU main thread: `SaveSnapshot` records the
     * snapshot version after QEMU saved the VM state, `LoadSnapshot` refuses
     * snapshots whose version does not match, `DeleteSnapshot` removes it and
     * `ListSnapshots` reports compatibility from it.
     */
    std::unique_ptr<::goldfish::snapshot::SnapshotStore> store;

    /**
     * Whether snapshots can be saved and loaded in this session. False when the
     * emulator was started with -no-snapshot: `SaveSnapshot` and `LoadSnapshot`
     * then fail with FailedPrecondition without touching QEMU, while
     * `ListSnapshots` and `DeleteSnapshot` keep working.
     */
    bool enabled = true;
};

/**
 * @brief Creates the VmOperations implementation that controls the QEMU VM.
 *
 * The instance registers QEMU run-state and snapshot-progress handlers when
 * constructed and unregisters them when destroyed, both under the VM lock.
 * Create it once the VM lock is usable (for example while devices are realized)
 * and keep it alive for as long as consumers hold references to it.
 *
 * Mutating and snapshot operations are dispatched synchronously to `qemu_loop`
 * via `PostAndWait`. They are expected to be called from external threads
 * (such as gRPC server or worker threads) and will fatally abort if called
 * from either `event_loop` or `qemu_loop`. Read-only queries (`IsRunning`,
 * `GetRunState`, `GetConfiguration`, `GetSnapshotProgress`) execute directly
 * without posting to `qemu_loop` and may be called from any thread.
 *
 * Only the QEMU calls themselves (`save_snapshot`, `load_snapshot`,
 * `delete_snapshot`, `bdrv_snapshot_list`, `vm_stop`) run on `qemu_loop`; the
 * `SnapshotStore` file system work happens on the calling thread before and
 * after, so no extra I/O is done under the BQL.
 *
 * @param event_loop The loop that runs the `RunStateEvents()` and
 *        `SnapshotEvents()` callbacks. Must be distinct from `qemu_loop` and
 *        outlive the returned instance.
 * @param qemu_loop The QEMU main event loop used to execute VM operations. Must
 *        outlive the returned instance.
 * @param snapshot_config Snapshot store and policy, see `QemuSnapshotConfig`.
 *        `store` must not be null.
 * @param hang_detector Hang detector with which the snapshot progress watchdog
 *        predicate is registered while a snapshot save or load is in flight,
 *        including the ones QEMU runs without VmOperations (the exit-time
 *        -savevm, HMP savevm/loadvm). Must not be null and must outlive the
 *        returned instance.
 * @param clock Clock source used for snapshot progress throttling. Must outlive
 *        the returned instance. Defaults to `absl::Clock::GetRealClock()`.
 * @return The QEMU-backed VmOperations.
 */
std::unique_ptr<VmOperations> CreateQemuVmOperations(
        ::goldfish::async::EventLoop* event_loop, ::goldfish::async::EventLoop* qemu_loop,
        QemuSnapshotConfig snapshot_config, ::android::crashreport::HangDetector* hang_detector,
        absl::Clock& clock = absl::Clock::GetRealClock());

}  // namespace goldfish
}  // namespace android
