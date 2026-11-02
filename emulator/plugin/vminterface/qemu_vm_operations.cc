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
#include "android/goldfish/qemu_vm_operations.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/cleanup/cleanup.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"

#include "android/crashreport/hang_detector.h"
#include "android/goldfish/vm_interface.h"
#include "android/status/status_macros.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/avd/avd_info.h"
#include "goldfish/metrics/snapshot_metrics.h"
#include "goldfish/snapshot/snapshot_store.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/runstate.h"
#include "migration/snapshot.h"
#include "block/snapshot.h"
}
// IWYU pragma: end_keep
// clang-format on

// Remove leaked shutdown redefinition from external/qemu/include/sysemu/os-win32.h
#undef shutdown

#include "qemu_machine_info.h"
#include "vm_lock.h"

namespace android {
namespace goldfish {
namespace {
constexpr absl::Duration kMinTransferEventInterval = absl::Milliseconds(250);
constexpr std::string_view kSnapshotWatchdogName = "SnapshotWatchdog";
// HangDetector evaluates predicates every 15s (DefaultTiming().hang_check_timeout)
// and crashes after 2 consecutive stalled checks (30s total without progress).
constexpr absl::Duration kSnapshotStallTimeout = absl::Seconds(30);

// Who registered the SnapshotWatchdog predicate, if anyone.
enum class SnapshotWatchdogOwner {
    kNone,
    // SaveSnapshot() or LoadSnapshot(), for the duration of the call.
    kVmOperations,
    // OnSnapshotProgress(), for a snapshot QEMU runs without VmOperations: the
    // exit-time -savevm and HMP savevm/loadvm.
    kQemu,
};

std::string_view SnapshotPhaseName(VmOperations::SnapshotPhase phase) {
    switch (phase) {
    case VmOperations::SnapshotPhase::None:
        return "NONE";
    case VmOperations::SnapshotPhase::Preparing:
        return "PREPARING";
    case VmOperations::SnapshotPhase::Transferring:
        return "TRANSFERRING";
    case VmOperations::SnapshotPhase::Completed:
        return "COMPLETED";
    case VmOperations::SnapshotPhase::Failed:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

bool HasSnapshotProgressAdvanced(const VmOperations::SnapshotProgress& previous,
                                 const VmOperations::SnapshotProgress& current) {
    return current.operation != previous.operation || current.phase != previous.phase ||
           current.snapshot_id != previous.snapshot_id ||
           current.transferred_bytes > previous.transferred_bytes ||
           current.disk_bytes > previous.disk_bytes ||
           current.completed_devices > previous.completed_devices ||
           current.active_component != previous.active_component;
}

std::string FormatSnapshotStallMessage(const VmOperations::SnapshotProgress& progress,
                                       absl::Duration stall_timeout) {
    const std::string_view action =
            progress.operation == VmOperations::SnapshotOperationType::Load ? "load" : "save";
    const std::string_view name = progress.snapshot_id.empty()
                                          ? std::string_view("<unnamed>")
                                          : std::string_view(progress.snapshot_id);
    const std::string_view component = progress.active_component.empty()
                                               ? std::string_view("<none>")
                                               : std::string_view(progress.active_component);
    return absl::StrCat(
            "Snapshot ", action, " '", name, "' stalled: no progress for ",
            absl::FormatDuration(stall_timeout), " (phase=", SnapshotPhaseName(progress.phase),
            ", progress=", progress.ProgressPercent(), "%, component='", component,
            "', transferred_bytes=", progress.transferred_bytes, "/", progress.total_bytes,
            ", disk_bytes=", progress.disk_bytes,
            ", completed_devices=", progress.completed_devices, "/", progress.total_devices, ").");
}

absl::Status ToStatus(const absl::StatusCode code, std::string_view prefix, Error** errp,
                      std::string_view remediation) {
    const char* detail =
            (errp && *errp) ? ::error_get_pretty(*errp) : "QEMU reported an unspecified error";
    absl::Status status(code, absl::StrCat(prefix, ": ", detail, ". ", remediation));
    if (errp && *errp) {
        ::error_free(*errp);
        *errp = nullptr;
    }
    return status;
}
absl::Status MakeInvalidTransitionError(std::string_view action, EmuRunState current_state,
                                        std::string_view expected_states) {
    switch (current_state) {
    case EmuRunState::Shutdown:
        return absl::FailedPreconditionError(absl::StrCat(
                "Cannot ", action, " VM: emulator is shutting down (current state: ", current_state,
                ", expected: ", expected_states,
                "). Launch a new emulator instance to start a new session."));
    case EmuRunState::SaveVm:
    case EmuRunState::RestoreVm:
    case EmuRunState::InMigrate:
    case EmuRunState::FinishMigrate:
    case EmuRunState::Colo:
        return absl::FailedPreconditionError(absl::StrCat(
                "Cannot ", action, " VM: snapshot or migration is in progress (current state: ",
                current_state, ", expected: ", expected_states,
                "). Wait for the operation to complete and retry."));
    default:
        return absl::FailedPreconditionError(absl::StrCat(
                "Cannot ", action, " VM from current state ", current_state, " (expected ",
                expected_states, "). Reset the VM or shut down the emulator to recover."));
    }
}

VmOperations::SnapshotProgress ToSnapshotProgress(const QemuSnapshotProgress& qprog) {
    VmOperations::SnapshotProgress progress;
    progress.operation = static_cast<VmOperations::SnapshotOperationType>(qprog.operation);
    progress.phase = static_cast<VmOperations::SnapshotPhase>(qprog.phase);
    if (qprog.name) {
        progress.snapshot_id = qprog.name;
    }
    if (qprog.active_component) {
        progress.active_component = qprog.active_component;
    }
    if (qprog.error_message && qprog.error_message[0] != '\0') {
        progress.error_message = qprog.error_message;
    } else if (progress.phase == VmOperations::SnapshotPhase::Failed) {
        const std::string_view action =
                progress.operation == VmOperations::SnapshotOperationType::Load ? "load" : "save";
        const std::string_view target = progress.snapshot_id.empty()
                                                ? std::string_view("<unnamed>")
                                                : std::string_view(progress.snapshot_id);
        progress.error_message = absl::StrCat("Cannot ", action, " snapshot '", target,
                                              "': QEMU reported an unspecified snapshot failure");
    }
    progress.transferred_bytes = qprog.transferred_bytes;
    progress.total_bytes = qprog.total_bytes;
    progress.disk_bytes = qprog.disk_bytes;
    progress.completed_devices = qprog.completed_devices;
    progress.total_devices = qprog.total_devices;
    return progress;
}
}  // namespace

static_assert(static_cast<int>(QemuShutdownCause::Max) == static_cast<int>(SHUTDOWN_CAUSE__MAX));
static_assert(static_cast<int>(EmuRunState::Max) == static_cast<int>(RUN_STATE__MAX));

static_assert(static_cast<int>(VmOperations::SnapshotOperationType::Idle) ==
              static_cast<int>(QEMU_SNAPSHOT_OP_NONE));
static_assert(static_cast<int>(VmOperations::SnapshotOperationType::Save) ==
              static_cast<int>(QEMU_SNAPSHOT_OP_SAVE));
static_assert(static_cast<int>(VmOperations::SnapshotOperationType::Load) ==
              static_cast<int>(QEMU_SNAPSHOT_OP_LOAD));

static_assert(static_cast<int>(VmOperations::SnapshotPhase::None) ==
              static_cast<int>(QEMU_SNAPSHOT_PHASE_NONE));
static_assert(static_cast<int>(VmOperations::SnapshotPhase::Preparing) ==
              static_cast<int>(QEMU_SNAPSHOT_PHASE_PREPARING));
static_assert(static_cast<int>(VmOperations::SnapshotPhase::Transferring) ==
              static_cast<int>(QEMU_SNAPSHOT_PHASE_TRANSFERRING));
static_assert(static_cast<int>(VmOperations::SnapshotPhase::Completed) ==
              static_cast<int>(QEMU_SNAPSHOT_PHASE_COMPLETED));
static_assert(static_cast<int>(VmOperations::SnapshotPhase::Failed) ==
              static_cast<int>(QEMU_SNAPSHOT_PHASE_FAILED));

/**
 * @brief QemuVmOperations class implementing the VmOperations interface.
 *
 * This class provides a concrete implementation of the VmOperations interface
 * for interacting with a QEMU virtual machine. Mutating and snapshot methods
 * execute synchronously on `qemu_loop` via `PostAndWait` and must not be called
 * from either `event_loop` or `qemu_loop`.
 */
class QemuVmOperations : public VmOperations {
  private:
    // Defined before SaveSnapshot()/LoadSnapshot() because C++ requires a member
    // function with a deduced return type to be defined before its call sites.
    //
    // The SnapshotWatchdog predicate is registered only while a snapshot is in flight.
    // SaveSnapshot() and LoadSnapshot() register it before they call QEMU, so a stall
    // before QEMU's first progress report (in vm_stop() or while draining the block
    // layer) is caught as well. OnSnapshotProgress() registers it for the snapshots QEMU
    // runs without VmOperations.
    [[nodiscard]] auto BeginSnapshotWatchdog(SnapshotOperationType operation,
                                             std::string_view snapshot_id) {
        AssertOnQemuLoop();
        {
            absl::MutexLock lock(&progress_mutex_);
            snapshot_watchdog_owner_ = SnapshotWatchdogOwner::kVmOperations;
            snapshot_watchdog_saw_qemu_progress_ = false;
            snapshot_watchdog_initial_progress_ = SnapshotProgress{
                .operation = operation,
                .phase = SnapshotPhase::Preparing,
                .snapshot_id = std::string(snapshot_id),
            };
            snapshot_watchdog_last_progress_ = snapshot_watchdog_initial_progress_;
        }
        RegisterSnapshotWatchdog();
        return absl::MakeCleanup([this] { EndSnapshotWatchdog(); });
    }

  public:
    // RecursiveScopedVmLock allows registration/deregistration to run safely on
    // both the QEMU main thread as well as external threads during startup/teardown.
    QemuVmOperations(::goldfish::async::EventLoop* event_loop,
                     ::goldfish::async::EventLoop* qemu_loop, QemuSnapshotConfig snapshot_config,
                     ::android::crashreport::HangDetector* hang_detector, absl::Clock& clock)
            : VmOperations(event_loop)
            , event_loop_(event_loop)
            , qemu_loop_(qemu_loop)
            , snapshot_store_(std::move(snapshot_config.store))
            , snapshots_enabled_(snapshot_config.enabled)
            , hang_detector_(hang_detector)
            , clock_(clock) {
        DCHECK(event_loop_ != nullptr);
        DCHECK(qemu_loop_ != nullptr);
        DCHECK(event_loop_ != qemu_loop_)
                << "event_loop and qemu_loop must be distinct event loops.";
        DCHECK(snapshot_store_ != nullptr) << "QemuSnapshotConfig::store must not be null.";
        DCHECK(hang_detector_ != nullptr);
        transfer_flush_timer_ = event_loop_->CreateTimer([this] {
            FlushPendingTransferProgress();
            return true;
        });
        RecursiveScopedVmLock lock;
        change_state_entry_ = qemu_add_vm_change_state_handler(OnVmChangeState, this);
        snapshot_progress_entry_ = add_snapshot_progress_handler(OnSnapshotProgress, this);
    }
    ~QemuVmOperations() override {
        if (change_state_entry_ || snapshot_progress_entry_) {
            RecursiveScopedVmLock lock;
            if (change_state_entry_) {
                qemu_del_vm_change_state_handler(change_state_entry_);
            }
            if (snapshot_progress_entry_) {
                del_snapshot_progress_handler(snapshot_progress_entry_);
            }
        }
        // No progress report can register the watchdog any more. Drop one left by a
        // snapshot that is still in flight: its predicate captures `this`.
        bool watchdog_registered = false;
        {
            absl::MutexLock lock(&progress_mutex_);
            watchdog_registered = snapshot_watchdog_owner_ != SnapshotWatchdogOwner::kNone;
            snapshot_watchdog_owner_ = SnapshotWatchdogOwner::kNone;
        }
        if (watchdog_registered) {
            hang_detector_->RemovePredicate(kSnapshotWatchdogName);
        }
        transfer_flush_timer_.reset();
    }

    absl::Status Stop() override { return Pause(); }

    absl::Status Start() override { return Resume(); }

    absl::Status Reset() override {
        return RunOnQemuLoop([this] {
            RETURN_IF_ERROR(CheckCanExecuteAction("reset"));
            qemu_system_reset_request(SHUTDOWN_CAUSE_SUBSYSTEM_RESET);
            absl::MutexLock lock(&progress_mutex_);
            loaded_snapshot_.clear();
            return absl::OkStatus();
        });
    }

    absl::Status Shutdown() override {
        return RunOnQemuLoop([this] {
            if (GetRunState() == EmuRunState::Shutdown) {
                return absl::OkStatus();
            }
            RETURN_IF_ERROR(CheckCanExecuteAction("shut down"));
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            return absl::OkStatus();
        });
    }

    absl::Status Pause() override {
        return RunOnQemuLoop([this] {
            switch (const EmuRunState state = GetRunState()) {
            case EmuRunState::Paused:
                return absl::OkStatus();
            case EmuRunState::Running:
            case EmuRunState::Suspended: {
                const int ret = vm_stop(RUN_STATE_PAUSED);
                if (ret < 0) {
                    return absl::ErrnoToStatus(
                            -ret,
                            absl::StrCat("Cannot pause VM from current state ", state,
                                         ": QEMU vm_stop(RUN_STATE_PAUSED) failed (code ", ret,
                                         "). Reset the VM or check host storage availability"));
                }
                if (ret > 0) {
                    return absl::InternalError(absl::StrCat(
                            "Cannot pause VM from current state ", state,
                            ": QEMU vm_stop(RUN_STATE_PAUSED) device stop callback failed with "
                            "code ",
                            ret, ". Reset the VM to recover."));
                }
                return absl::OkStatus();
            }
            default:
                return MakeInvalidTransitionError(
                        /*action=*/"pause", /*current_state=*/state,
                        /*expected_states=*/"Running, Paused, or Suspended");
            }
        });
    }

    absl::Status Resume() override {
        return RunOnQemuLoop([this] {
            switch (const EmuRunState state = GetRunState()) {
            case EmuRunState::Running:
                return absl::OkStatus();
            case EmuRunState::Paused:
            case EmuRunState::Suspended:
            case EmuRunState::PreLaunch:
            case EmuRunState::Debug:
            case EmuRunState::PostMigrate:
                vm_start();
                return absl::OkStatus();
            default:
                return MakeInvalidTransitionError(
                        /*action=*/"resume", /*current_state=*/state,
                        /*expected_states=*/
                        "Running, Paused, Suspended, Debug, PreLaunch, or PostMigrate");
            }
        });
    }

    /**
     * @brief Checks if the QEMU virtual machine is currently running.
     *
     * @return True if the VM is running, false otherwise.
     */
    bool IsRunning() override { return runstate_get() == RUN_STATE_RUNNING; }

    /**
     * @brief Gets the configuration of the QEMU virtual machine.
     *
     * @return The VM's configuration.
     * @note This is a placeholder implementation.
     *       A real implementation would need to query QEMU for the actual
     * configuration.
     */
    VmConfiguration GetConfiguration() override {
        VmConfiguration config;
        const std::string_view accel = ::accel_name_cwrap();

        // ** Update this when you add a new hypervisor **
        // Make sure that ac->name matches with pair.first!
        constexpr static std::array<std::pair<std::string_view, VmHypervisorType>, 5>
                kHyperVisorData = {{
                    {"unknown", VmHypervisorType::Unknown},  // Unused.
                    {"tcg", VmHypervisorType::None},         // qemu/accel/tcg/tcg-all.c
                    {"KVM", VmHypervisorType::Kvm},          // qemu/accel/kvm/kvm-all.c
                    {"HVF", VmHypervisorType::Hvf},          // qemu/accel/hvf/hvf-accel-ops.c
                    {"WHPX", VmHypervisorType::Whpx},        // qemu/target/i386/whpx/whpx-all.c
                }};
        static_assert(std::size(kHyperVisorData) == static_cast<size_t>(VmHypervisorType::Max));

        config.hypervisor_type = VmHypervisorType::Unknown;
        for (const auto& pair : kHyperVisorData) {
            if (pair.first == accel) {
                config.hypervisor_type = pair.second;
                break;
            }
        }
        config.number_of_cpu_cores = ::cpu_count_cwrap();
        config.cpu_type = ::cpu_type_cwrap();
        return config;
    }

    /**
     * @brief Gets the current run state of the QEMU emulator.
     *
     * @return The emulator's run state.
     */
    EmuRunState GetRunState() override { return static_cast<EmuRunState>(runstate_get()); }

    /**
     * @brief Requests a system shutdown with a specified reason.
     *
     * @param reason The reason for the shutdown request.
     */
    void SystemShutdownRequest(QemuShutdownCause reason) override {
        qemu_system_shutdown_request(static_cast<ShutdownCause>(reason));
    }

    // Snapshot operations are split in three: validation and SnapshotStore
    // preparation on the calling thread, the QEMU call alone on the QEMU main
    // thread (RunOnQemuLoop, BQL held), and the SnapshotStore commit or cleanup
    // back on the calling thread. QEMU's save_snapshot()/load_snapshot() drive
    // the migration state machine and the device save/load hooks, so they must
    // never run anywhere else; the file system work does not need the BQL.
    //
    // `vmstate` is deliberately nullptr in every call below: the launcher
    // names the VM-state node once with -vmstate-node (<avd>/snapshots.qcow2,
    // see SnapshotDevice) and QEMU resolves nullptr to it. The exit-time
    // -savevm, the boot-time -loadvm and HMP savevm/loadvm go through the same
    // default, so all paths agree on the node by construction instead of this
    // plugin carrying a second copy of its name. If the node is missing QEMU
    // reports "vmstate block device ... does not exist" rather than falling
    // back to a disk image.

    absl::Status SaveSnapshot(std::string name, const bool overwrite) override {
        RETURN_IF_ERROR(CheckSnapshotName(name));
        RETURN_IF_ERROR(CheckSnapshotsEnabled("save"));
        // The virtio-gpu device (virtio-gpu-rutabaga / virtio-gpu-gfxstream) writes below this
        // directory.
        const bool existed_before = snapshot_store_->Exists(name);
        RETURN_IF_ERROR(snapshot_store_->Prepare(name));

        const absl::Status saved = RunOnQemuLoop([this, &name, overwrite] {
            AssertOnQemuLoop();
            absl::Time start = absl::Now();
            RETURN_IF_ERROR(CheckCanExecuteAction("save snapshot for"));
            auto watchdog_cleanup = BeginSnapshotWatchdog(SnapshotOperationType::Save, name);
            auto& avd = ::goldfish::avd::GetAvd();
            avd.SetGenericSnapshotInProgress(true);
            auto cleanup = absl::MakeCleanup([&avd] { avd.SetGenericSnapshotInProgress(false); });
            ::Error* errp = nullptr;
            bool ok = ::save_snapshot(name.c_str(), overwrite, /*vmstate=*/nullptr,
                                      /*has_devices=*/false, /*devices=*/nullptr, &errp);
            ::goldfish::metrics::ReportGenericSnapshotSave(avd.GetMetricsReporter(), name, ok,
                                                           absl::Now() - start);
            if (!ok) {
                return ToStatus(
                        absl::StatusCode::kInternal,
                        /*prefix=*/absl::StrCat("Cannot save snapshot '", name, "'"), &errp,
                        /*remediation=*/
                        "Verify that the writable qcow2 disk image has free space and retry.");
            }
            return absl::OkStatus();
        });

        if (!saved.ok()) {
            // Leave no half-written metadata behind for a brand new snapshot. A
            // pre-existing snapshot keeps its directory: its qcow2 state is intact
            // when the save was rejected up front, and otherwise it shares the
            // fate of the qcow2 snapshot that QEMU already replaced.
            if (!existed_before) {
                snapshot_store_->Remove(name).IgnoreError();
            }
            return saved;
        }
        // version.txt was written by OnSnapshotProgress when QEMU reported the save
        // complete, before save_snapshot() returned.
        if (!snapshot_store_->IsCompatible(name)) {
            return absl::InternalError(absl::StrCat(
                    "Snapshot '", name, "' was saved but its version could not be recorded in ",
                    snapshot_store_->Dir(name).string(),
                    "; it will not be loadable. Check that the AVD directory is writable."));
        }
        return absl::OkStatus();
    }

    absl::Status LoadSnapshot(std::string id_or_name, const bool and_resume) override {
        RETURN_IF_ERROR(CheckSnapshotName(id_or_name));
        RETURN_IF_ERROR(CheckSnapshotsEnabled("load"));
        RETURN_IF_ERROR(CheckSnapshotCompatible(id_or_name));

        return RunOnQemuLoop([this, id_or_name = std::move(id_or_name), and_resume] {
            AssertOnQemuLoop();
            absl::Time start = absl::Now();
            RETURN_IF_ERROR(CheckCanExecuteAction("load snapshot into"));
            auto watchdog_cleanup = BeginSnapshotWatchdog(SnapshotOperationType::Load, id_or_name);
            auto& avd = ::goldfish::avd::GetAvd();
            ::Error* errp = nullptr;

            ::vm_stop(RUN_STATE_RESTORE_VM);

            avd.SetGenericSnapshotInProgress(true);
            auto cleanup = absl::MakeCleanup([&avd] { avd.SetGenericSnapshotInProgress(false); });
            bool ok = ::load_snapshot(id_or_name.c_str(), /*vmstate=*/nullptr,
                                      /*has_devices=*/false, /*devices=*/nullptr, &errp);
            ::goldfish::metrics::ReportGenericSnapshotLoad(avd.GetMetricsReporter(), id_or_name, ok,
                                                           absl::Now() - start);
            if (!ok) {
                {
                    absl::MutexLock lock(&progress_mutex_);
                    loaded_snapshot_.clear();
                }
                // do_vm_stop(RUN_STATE_RESTORE_VM) only transitions from live states
                // (Running/Suspended); non-live states (e.g., Paused) remain unchanged.
                if (::runstate_get() == ::RUN_STATE_RESTORE_VM) {
                    ::load_snapshot_resume(::RUN_STATE_PRELAUNCH);
                }
                return ToStatus(absl::StatusCode::kInternal,
                                /*prefix=*/absl::StrCat("Cannot load snapshot '", id_or_name, "'"),
                                &errp,
                                /*remediation=*/
                                "Verify the snapshot exists with ListSnapshots, or reset/resume "
                                "the VM.");
            }

            if (and_resume) {
                ::load_snapshot_resume(::RUN_STATE_RUNNING);
            }

            return absl::OkStatus();
        });
    }

    void LoadSnapshotResume(const EmuRunState ers) override {
        if (absl::Status status =
                    RunOnQemuLoop([ers] { ::load_snapshot_resume(static_cast<::RunState>(ers)); });
            !status.ok()) {
            LOG(WARNING) << "Cannot resume VM to state " << ers
                         << " after loading snapshot: " << status;
        }
    }

    absl::Status DeleteSnapshot(std::string id_or_name) override {
        RETURN_IF_ERROR(CheckSnapshotName(id_or_name));

        RETURN_IF_ERROR(RunOnQemuLoop([this, &id_or_name] {
            AssertOnQemuLoop();
            RETURN_IF_ERROR(CheckCanExecuteAction("delete snapshot from"));
            ::Error* errp = nullptr;
            if (!::delete_snapshot(id_or_name.c_str(), /*has_devices=*/false, /*devices=*/nullptr,
                                   &errp)) {
                return ToStatus(
                        absl::StatusCode::kInternal,
                        /*prefix=*/absl::StrCat("Cannot delete snapshot '", id_or_name, "'"), &errp,
                        /*remediation=*/
                        "Verify that the snapshot exists and the disk image is writable.");
            }
            {
                absl::MutexLock lock(&progress_mutex_);
                if (loaded_snapshot_ == id_or_name) {
                    loaded_snapshot_.clear();
                }
            }

            return absl::OkStatus();
        }));

        return snapshot_store_->Remove(id_or_name);
    }

    absl::Status ListSnapshots(const SnapshotEntrySink sink) override {
        std::vector<SnapshotEntry> entries;
        std::string loaded_snapshot;
        RETURN_IF_ERROR(RunOnQemuLoop([this, &entries, &loaded_snapshot] {
            AssertOnQemuLoop();
            ::Error* errp = nullptr;
            ::BlockDriverState* bs = ::bdrv_all_find_vmstate_bs(
                    /*vmstate_bs=*/nullptr, /*has_devices=*/false, /*devices=*/nullptr, &errp);
            if (!bs) {
                return ToStatus(
                        absl::StatusCode::kFailedPrecondition,
                        /*prefix=*/
                        "Cannot list snapshots: no block device supports VM state snapshots", &errp,
                        /*remediation=*/
                        "Ensure snapshots are enabled: with -no-snapshot the VM-state image "
                        "snapshots.qcow2 is only attached when it already exists.");
            }

            ::QEMUSnapshotInfo* qsi = nullptr;
            const int qsi_size = ::bdrv_snapshot_list(bs, &qsi);
            if (qsi_size < 0) {
                return absl::ErrnoToStatus(
                        -qsi_size,
                        "Cannot list snapshots: failed to read snapshot table from block device. "
                        "Verify that the disk image is accessible and not corrupted.");
            }

            entries.reserve(qsi_size);
            for (int i = 0; i < qsi_size; ++i) {
                entries.push_back(SnapshotEntry{
                    .name = qsi[i].name,
                    .timestamp = absl::FromUnixSeconds(qsi[i].date_sec) +
                                 absl::Nanoseconds(qsi[i].date_nsec),
                });
            }

            ::g_free(qsi);
            {
                absl::MutexLock lock(&progress_mutex_);
                loaded_snapshot = loaded_snapshot_;
            }
            return absl::OkStatus();
        }));

        // Compatibility is read from the file system, off the QEMU main thread.
        for (SnapshotEntry& se : entries) {
            se.compatible = snapshot_store_->IsCompatible(se.name);
            se.loaded = se.compatible && (se.name == loaded_snapshot);
            sink(std::move(se));
        }
        return absl::OkStatus();
    }

    SnapshotProgress GetSnapshotProgress() const override {
        absl::MutexLock lock(&progress_mutex_);
        return cached_progress_;
    }

  private:
    absl::Status CheckSnapshotsEnabled(std::string_view action) const {
        if (snapshots_enabled_) {
            return absl::OkStatus();
        }
        return absl::FailedPreconditionError(absl::StrCat(
                "Cannot ", action,
                " snapshot: snapshots are disabled because the emulator was started with "
                "-no-snapshot. Restart the emulator without -no-snapshot to use snapshots."));
    }

    static absl::Status CheckSnapshotName(std::string_view name) {
        return ::goldfish::snapshot::ValidateSnapshotName(name);
    }

    absl::Status CheckSnapshotCompatible(std::string_view name) const {
        const absl::StatusOr<int> version = snapshot_store_->ReadVersion(name);
        if (version.ok() && *version == snapshot_store_->Version()) {
            return absl::OkStatus();
        }
        const std::string recorded =
                version.ok() ? absl::StrCat(*version)
                             : (absl::IsNotFound(version.status()) ? "missing" : "unreadable");
        return absl::FailedPreconditionError(
                absl::StrCat("Cannot load snapshot '", name,
                             "': it is incompatible with this emulator (snapshot version ",
                             recorded, ", expected ", snapshot_store_->Version(),
                             "). Delete it with DeleteSnapshot and save a new one."));
    }

    void AssertOnQemuLoop() const {
        DCHECK(qemu_loop_->IsOnLoopThread())
                << "QEMU snapshot calls must run on the QEMU main loop thread";
    }

    absl::Status CheckCanExecuteAction(std::string_view action) {
        switch (const EmuRunState state = GetRunState()) {
        case EmuRunState::Shutdown:
        case EmuRunState::SaveVm:
        case EmuRunState::RestoreVm:
        case EmuRunState::InMigrate:
        case EmuRunState::FinishMigrate:
        case EmuRunState::Colo:
            return MakeInvalidTransitionError(
                    action, /*current_state=*/state,
                    /*expected_states=*/"Running, Paused, Suspended, Debug, or fault state");
        default:
            return absl::OkStatus();
        }
    }

    template <typename F>
    absl::Status RunOnQemuLoop(F&& fn) {
        if (event_loop_->IsOnLoopThread()) {
            LOG(FATAL) << "VmOperations cannot be called from the event loop.";
        }
        // PostAndWait itself is fatal when called from qemu_loop's own thread.
        return qemu_loop_->PostAndWait(std::forward<F>(fn));
    }

    static void OnVmChangeState(void* opaque, bool /*running*/, RunState state) {
        auto* self = static_cast<QemuVmOperations*>(opaque);
        // On the loop thread the dispatcher would run the subscribers inline, under the BQL.
        DCHECK(!self->event_loop_->IsOnLoopThread())
                << "Run state changed on the event loop thread";
        self->RunStateEvents().FireEvent(static_cast<EmuRunState>(state));
    }

    static void OnSnapshotProgress(void* opaque, const QemuSnapshotProgress* qprog) {
        if (!qprog) {
            return;
        }
        auto* self = static_cast<QemuVmOperations*>(opaque);
        // On the loop thread the dispatcher would run the subscribers inline, under the BQL.
        DCHECK(!self->event_loop_->IsOnLoopThread())
                << "Snapshot progress reported on the event loop thread";

        const SnapshotProgress progress = ToSnapshotProgress(*qprog);
        self->RecordCompletedSave(progress);
        bool should_forward = true;
        absl::Duration flush_delay = absl::ZeroDuration();
        bool should_schedule_flush = false;
        bool register_watchdog = false;
        bool unregister_watchdog = false;
        {
            absl::MutexLock lock(&self->progress_mutex_);
            self->cached_progress_ = progress;
            self->UpdateLoadedSnapshotLocked(progress);
            self->snapshot_watchdog_saw_qemu_progress_ = true;
            if (progress.phase == SnapshotPhase::Preparing) {
                self->last_forwarded_transfer_time_ = absl::InfinitePast();
                self->transfer_flush_pending_ = false;
            } else if (progress.phase == SnapshotPhase::Transferring) {
                const absl::Time now = self->clock_.TimeNow();
                const absl::Duration elapsed = now - self->last_forwarded_transfer_time_;
                if (elapsed < kMinTransferEventInterval) {
                    should_forward = false;
                    if (!self->transfer_flush_pending_) {
                        self->transfer_flush_pending_ = true;
                        flush_delay = kMinTransferEventInterval - elapsed;
                        should_schedule_flush = true;
                    }
                } else {
                    self->last_forwarded_transfer_time_ = now;
                    self->transfer_flush_pending_ = false;
                }
            } else {
                self->transfer_flush_pending_ = false;
                self->snapshot_watchdog_last_progress_ = {};
            }

            // A snapshot that QEMU runs without VmOperations (the exit-time -savevm,
            // HMP savevm/loadvm) is watched from its first progress report to its last.
            const bool in_flight = progress.phase == SnapshotPhase::Preparing ||
                                   progress.phase == SnapshotPhase::Transferring;
            if (in_flight && self->snapshot_watchdog_owner_ == SnapshotWatchdogOwner::kNone) {
                self->snapshot_watchdog_owner_ = SnapshotWatchdogOwner::kQemu;
                self->snapshot_watchdog_last_progress_ = progress;
                register_watchdog = true;
            } else if (!in_flight &&
                       self->snapshot_watchdog_owner_ == SnapshotWatchdogOwner::kQemu) {
                self->snapshot_watchdog_owner_ = SnapshotWatchdogOwner::kNone;
                unregister_watchdog = true;
            }
        }

        // Outside progress_mutex_: RemovePredicate() waits for a CheckSnapshotWatchdog()
        // that is running, and that takes progress_mutex_.
        if (register_watchdog) {
            self->RegisterSnapshotWatchdog();
        } else if (unregister_watchdog) {
            self->hang_detector_->RemovePredicate(kSnapshotWatchdogName);
        }
        if (should_schedule_flush && self->transfer_flush_timer_) {
            self->transfer_flush_timer_->Schedule(flush_delay);
        }
        if (should_forward) {
            self->SnapshotEvents().FireEvent(progress);
        }
    }

    void EndSnapshotWatchdog() {
        AssertOnQemuLoop();
        {
            absl::MutexLock lock(&progress_mutex_);
            snapshot_watchdog_owner_ = SnapshotWatchdogOwner::kNone;
        }
        hang_detector_->RemovePredicate(kSnapshotWatchdogName);
    }

    void RegisterSnapshotWatchdog() {
        hang_detector_->AddPredicate(std::string(kSnapshotWatchdogName),
                                     [this]() -> absl::Status { return CheckSnapshotWatchdog(); });
    }

    absl::Status CheckSnapshotWatchdog() {
        SnapshotProgress current;
        {
            absl::MutexLock lock(&progress_mutex_);
            current = snapshot_watchdog_saw_qemu_progress_ ? cached_progress_
                                                           : snapshot_watchdog_initial_progress_;
            if (current.phase != SnapshotPhase::Preparing &&
                current.phase != SnapshotPhase::Transferring) {
                snapshot_watchdog_last_progress_ = {};
                return absl::OkStatus();
            }
            if (HasSnapshotProgressAdvanced(snapshot_watchdog_last_progress_, current)) {
                snapshot_watchdog_last_progress_ = current;
                return absl::OkStatus();
            }
        }
        return absl::DeadlineExceededError(
                FormatSnapshotStallMessage(current, kSnapshotStallTimeout));
    }

    void UpdateLoadedSnapshotLocked(const SnapshotProgress& progress)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(progress_mutex_) {
        if (progress.operation != SnapshotOperationType::Load) {
            return;
        }
        if (progress.phase == SnapshotPhase::Preparing || progress.phase == SnapshotPhase::Failed) {
            loaded_snapshot_.clear();
        } else if (progress.phase == SnapshotPhase::Completed && !progress.snapshot_id.empty()) {
            loaded_snapshot_ = progress.snapshot_id;
        }
    }

    // Records the snapshot version of every save QEMU completes: the runtime saves
    // of SaveSnapshot() and the exit-time save of the launcher's -savevm, which
    // does not go through VmOperations. Runs on the QEMU main thread with the BQL
    // held, the only thread still available at exit; writing version.txt is a
    // few bytes, the one SnapshotStore operation done under the BQL. Writing it
    // only now, rather than when the launcher starts, means a crash before the
    // save cannot leave a version file next to stale or missing qcow2 state.
    void RecordCompletedSave(const SnapshotProgress& progress) {
        if (progress.operation != SnapshotOperationType::Save ||
            progress.phase != SnapshotPhase::Completed || progress.snapshot_id.empty()) {
            return;
        }
        if (const absl::Status s = snapshot_store_->WriteVersion(progress.snapshot_id); !s.ok()) {
            LOG(WARNING) << "Snapshot '" << progress.snapshot_id
                         << "' was saved but its version could not be recorded; it will not be "
                            "loadable: "
                         << s;
        }
    }

    void FlushPendingTransferProgress() {
        SnapshotProgress progress;
        absl::Duration remaining = absl::ZeroDuration();
        {
            absl::MutexLock lock(&progress_mutex_);
            if (!transfer_flush_pending_ || cached_progress_.phase != SnapshotPhase::Transferring) {
                transfer_flush_pending_ = false;
                return;
            }
            const absl::Time now = clock_.TimeNow();
            const absl::Duration elapsed = now - last_forwarded_transfer_time_;
            if (elapsed < kMinTransferEventInterval) {
                remaining = kMinTransferEventInterval - elapsed;
            } else {
                transfer_flush_pending_ = false;
                last_forwarded_transfer_time_ = now;
                progress = cached_progress_;
            }
        }
        if (remaining > absl::ZeroDuration()) {
            if (transfer_flush_timer_) {
                transfer_flush_timer_->Schedule(remaining);
            }
            return;
        }
        SnapshotEvents().FireEvent(progress);
    }

    ::goldfish::async::EventLoop* const event_loop_;
    ::goldfish::async::EventLoop* const qemu_loop_;
    const std::unique_ptr<::goldfish::snapshot::SnapshotStore> snapshot_store_;
    const bool snapshots_enabled_;
    ::android::crashreport::HangDetector* const hang_detector_;
    absl::Clock& clock_;
    ::goldfish::async::EventLoop::ScopedTimer transfer_flush_timer_;
    VMChangeStateEntry* change_state_entry_{nullptr};
    QemuSnapshotProgressEntry* snapshot_progress_entry_{nullptr};
    // Leaf mutex protecting cached_progress_, loaded_snapshot_, transfer-throttle, and watchdog
    // state for BQL-free reads.
    mutable absl::Mutex progress_mutex_;
    SnapshotProgress cached_progress_ ABSL_GUARDED_BY(progress_mutex_);
    std::string loaded_snapshot_ ABSL_GUARDED_BY(progress_mutex_);
    SnapshotProgress snapshot_watchdog_initial_progress_ ABSL_GUARDED_BY(progress_mutex_);
    SnapshotProgress snapshot_watchdog_last_progress_ ABSL_GUARDED_BY(progress_mutex_);
    bool snapshot_watchdog_saw_qemu_progress_ ABSL_GUARDED_BY(progress_mutex_){false};
    SnapshotWatchdogOwner snapshot_watchdog_owner_ ABSL_GUARDED_BY(progress_mutex_){
        SnapshotWatchdogOwner::kNone};
    absl::Time last_forwarded_transfer_time_ ABSL_GUARDED_BY(progress_mutex_){absl::InfinitePast()};
    bool transfer_flush_pending_ ABSL_GUARDED_BY(progress_mutex_){false};
};

std::unique_ptr<VmOperations> CreateQemuVmOperations(
        ::goldfish::async::EventLoop* event_loop, ::goldfish::async::EventLoop* qemu_loop,
        QemuSnapshotConfig snapshot_config, ::android::crashreport::HangDetector* hang_detector,
        absl::Clock& clock) {
    return std::make_unique<QemuVmOperations>(event_loop, qemu_loop, std::move(snapshot_config),
                                              hang_detector, clock);
}

}  // namespace goldfish
}  // namespace android
