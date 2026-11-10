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
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/async/event_loop_dispatcher.h"

namespace android {
namespace goldfish {

// Enumeration of various causes for shutdown.
enum class QemuShutdownCause {
    None,                ///< No shutdown request pending.
    HostError,           ///< An error prevents further use of guest.
    HostQmpQuit,         ///< Reaction to the QMP command 'quit'.
    HostQmpSystemReset,  ///< Reaction to the QMP command 'system_reset'.
    HostSignal,          ///< Reaction to a signal, such as SIGINT.
    HostUi,              ///< Reaction to UI event, like window close.
    GuestShutdown,   ///< Guest shutdown/suspend request, via ACPI or other hardware-specific means.
    GuestReset,      ///< Guest reset request, and command line turns that into a shutdown.
    GuestPanic,      ///< Guest panicked, and command line turns that into a shutdown.
    SubsystemReset,  ///< Partial guest reset that does not trigger QMP events and ignores
                     ///< --no-reboot.
    SnapshotLoad,    ///< A snapshot is being loaded by the record & replay subsystem.
    Max,             ///< Maximum value for this enum.
};

template <typename Sink>
void AbslStringify(Sink& sink, QemuShutdownCause e) {
    switch (e) {
    case QemuShutdownCause::None:
        absl::Format(&sink, "None");
        break;
    case QemuShutdownCause::HostError:
        absl::Format(&sink, "HostError");
        break;
    case QemuShutdownCause::HostQmpQuit:
        absl::Format(&sink, "HostQmpQuit");
        break;
    case QemuShutdownCause::HostQmpSystemReset:
        absl::Format(&sink, "HostQmpSystemReset");
        break;
    case QemuShutdownCause::HostSignal:
        absl::Format(&sink, "HostSignal");
        break;
    case QemuShutdownCause::HostUi:
        absl::Format(&sink, "HostUi");
        break;
    case QemuShutdownCause::GuestShutdown:
        absl::Format(&sink, "GuestShutdown");
        break;
    case QemuShutdownCause::GuestReset:
        absl::Format(&sink, "GuestReset");
        break;
    case QemuShutdownCause::GuestPanic:
        absl::Format(&sink, "GuestPanic");
        break;
    case QemuShutdownCause::SubsystemReset:
        absl::Format(&sink, "SubsystemReset");
        break;
    case QemuShutdownCause::SnapshotLoad:
        absl::Format(&sink, "SnapshotLoad");
        break;
    case QemuShutdownCause::Max:
        absl::Format(&sink, "Max");
        break;
    default:
        absl::Format(&sink, "Unknown QemuShutdownCause (%d)", static_cast<int>(e));
        break;
    }
}

// VM Hypervisor types
enum class VmHypervisorType {
    Unknown = 0,  ///< Unknown hypervisor type.
    None,         ///< No hypervisor (tiny code generator).
    Kvm,          ///< KVM hypervisor.
    Hvf,          ///< HVF hypervisor.
    Whpx,         ///< WHPX hypervisor.
    Max,          ///< Maximum value for this enum (unused)
};

template <typename Sink>
void AbslStringify(Sink& sink, VmHypervisorType e) {
    switch (e) {
    case VmHypervisorType::Unknown:
        absl::Format(&sink, "Unknown");
        break;
    case VmHypervisorType::None:
        absl::Format(&sink, "None");
        break;
    case VmHypervisorType::Kvm:
        absl::Format(&sink, "Kvm");
        break;
    case VmHypervisorType::Hvf:
        absl::Format(&sink, "Hvf");
        break;
    case VmHypervisorType::Whpx:
        absl::Format(&sink, "Whpx");
        break;
    case VmHypervisorType::Max:
        absl::Format(&sink, "Max");
        break;
    default:
        absl::Format(&sink, "Unknown VmHypervisorType (%d)", static_cast<int>(e));
        break;
    }
}

// VM Configuration
struct VmConfiguration {
    VmHypervisorType hypervisor_type;  ///< Type of hypervisor.
    int32_t number_of_cpu_cores;       ///< Number of CPU cores.
    const char* cpu_type;              ///< Type of the cpu
};

template <typename Sink>
void AbslStringify(Sink& sink, const VmConfiguration& config) {
    absl::Format(&sink,
                 "VmConfiguration { hypervisor_type: %v, number_of_cpu_cores: %d, cpu_type: %s }",
                 config.hypervisor_type, config.number_of_cpu_cores, config.cpu_type);
}

/**
 * @brief Enumeration of emulator run states.
 *
 * Defines the different states the emulator can be in during its lifecycle.
 */
enum class EmuRunState {
    Debug = 0,      ///< QEMU is running on a debugger.
    InMigrate,      ///< Guest is paused waiting for an incoming migration.
    InternalError,  ///< An internal error that prevents further guest execution has occurred.
    IoError,  ///< The last I/O operation has failed and the device is configured to pause on I/O
              ///< errors.
    Paused,   ///< Guest has been paused via the 'stop' command.
    PostMigrate,    ///< Guest is paused following a successful 'migrate'.
    PreLaunch,      ///< QEMU was started with -S and guest has not started.
    FinishMigrate,  ///< Guest is paused to finish the migration process.
    RestoreVm,      ///< Guest is paused to restore VM state.
    Running,        ///< Guest is actively running.
    SaveVm,         ///< Guest is paused to save the VM state.
    Shutdown,       ///< Guest is shut down (and -no-shutdown is in use).
    Suspended,      ///< Guest is suspended (ACPI S3).
    Watchdog,       ///< The watchdog action is configured to pause and has been triggered.
    GuestPanicked,  ///< Guest has been panicked as a result of guest OS panic.
    Colo,           ///< Guest is paused to save/restore VM state under colo checkpoint.
    Max,            ///< Maximum value for this enum.
};

template <typename Sink>
void AbslStringify(Sink& sink, EmuRunState e) {
    switch (e) {
    case EmuRunState::Debug:
        absl::Format(&sink, "Debug");
        break;
    case EmuRunState::InMigrate:
        absl::Format(&sink, "InMigrate");
        break;
    case EmuRunState::InternalError:
        absl::Format(&sink, "InternalError");
        break;
    case EmuRunState::IoError:
        absl::Format(&sink, "IoError");
        break;
    case EmuRunState::Paused:
        absl::Format(&sink, "Paused");
        break;
    case EmuRunState::PostMigrate:
        absl::Format(&sink, "PostMigrate");
        break;
    case EmuRunState::PreLaunch:
        absl::Format(&sink, "PreLaunch");
        break;
    case EmuRunState::FinishMigrate:
        absl::Format(&sink, "FinishMigrate");
        break;
    case EmuRunState::RestoreVm:
        absl::Format(&sink, "RestoreVm");
        break;
    case EmuRunState::Running:
        absl::Format(&sink, "Running");
        break;
    case EmuRunState::SaveVm:
        absl::Format(&sink, "SaveVm");
        break;
    case EmuRunState::Shutdown:
        absl::Format(&sink, "Shutdown");
        break;
    case EmuRunState::Suspended:
        absl::Format(&sink, "Suspended");
        break;
    case EmuRunState::Watchdog:
        absl::Format(&sink, "Watchdog");
        break;
    case EmuRunState::GuestPanicked:
        absl::Format(&sink, "GuestPanicked");
        break;
    case EmuRunState::Colo:
        absl::Format(&sink, "Colo");
        break;
    case EmuRunState::Max:
        absl::Format(&sink, "Max");
        break;
    default:
        absl::Format(&sink, "Unknown EmuRunState (%d)", static_cast<int>(e));
        break;
    }
}

/**
 * @brief Interface for performing operations on a virtual machine.
 *
 * This interface provides methods to control and manage the lifecycle of a VM,
 * including starting, stopping, pausing, resuming, querying its configuration,
 * and subscribing to run state changes.
 *
 * Threading contract:
 * - State-mutating and snapshot methods (`Stop`, `Start`, `Pause`, `Resume`,
 *   `Reset`, `Shutdown`, `SaveSnapshot`, `LoadSnapshot`, `LoadSnapshotResume`,
 *   `DeleteSnapshot`, `ListSnapshots`) are expected to be called from external
 *   threads (such as gRPC server or worker threads). They must not be called
 *   from the callback `EventLoop` passed to the constructor (to prevent
 *   re-entrant callbacks and cross-loop deadlocks) or from the QEMU main event
 *   loop.
 * - Read-only queries (`IsRunning`, `GetRunState`, `GetConfiguration`,
 *   `GetSnapshotProgress`) and atomic requests (`SystemShutdownRequest`) do
 *   not block or trigger callbacks synchronously and may be called from any
 *   thread, including `qemu_loop`, `RunStateEvents()` and `SnapshotEvents()`
 *   callbacks, and hang detector predicates.
 */
class VmOperations {
  public:
    /** @brief Source of run state changes, delivered on an event loop. */
    using RunStateSource = ::goldfish::async::LoopBoundCallbackSource<EmuRunState>;

    /**
     * @brief Type of snapshot operation tracked by `SnapshotProgress`.
     */
    enum class SnapshotOperationType {
        Idle = 0,  ///< No snapshot operation has started yet.
        Save = 1,  ///< Saving VM state and disk snapshot (`save_snapshot`).
        Load = 2,  ///< Restoring VM state and disk snapshot (`load_snapshot`).
    };

    /**
     * @brief Lifecycle phase of a snapshot save or load operation.
     */
    enum class SnapshotPhase {
        None = 0,          ///< No snapshot phase has been recorded yet.
        Preparing = 1,     ///< Validating block devices, pausing the VM, and preparing metadata.
        Transferring = 2,  ///< Streaming RAM pages and device state sections to or from storage.
        Completed = 3,     ///< Terminal phase indicating the snapshot operation succeeded.
        Failed = 4,        ///< Terminal phase indicating the snapshot operation aborted.
    };

    /**
     * @brief Snapshot operation progress report.
     *
     * `GetSnapshotProgress()` retains the most recent `SnapshotProgress` (including terminal
     * `Completed` or `Failed` states) after an operation finishes until the next snapshot
     * operation enters `SnapshotPhase::Preparing`.
     */
    struct SnapshotProgress {
        /// Weight applied to byte transfer progress when both byte and device counts are known.
        static constexpr double kByteProgressWeight = 0.85;
        /// Weight applied to device section progress when both byte and device counts are known.
        static constexpr double kDeviceProgressWeight = 0.15;
        /// Maximum progress percentage reported before `SnapshotPhase::Completed`.
        static constexpr int32_t kMaxInProgressPercent = 99;
        /// Progress percentage reported when `SnapshotPhase::Completed` is reached.
        static constexpr int32_t kCompletedProgressPercent = 100;

        /// Type of the active or most recently completed snapshot operation.
        SnapshotOperationType operation = SnapshotOperationType::Idle;
        /// Current lifecycle phase of `operation`.
        SnapshotPhase phase = SnapshotPhase::None;
        /// QEMU snapshot tag name being saved or loaded (e.g. `"default_boot"`).
        std::string snapshot_id;
        /// VM state section or device currently being saved or restored (e.g. `"ram"`), or the
        /// component that was active when a failure occurred.
        std::string active_component;
        /// Cumulative VM state bytes written (`Save`) or read (`Load`) so far.
        int64_t transferred_bytes = 0;
        /// Total VM state bytes expected: guest RAM bytes (transferred plus remaining) during
        /// `Save`, or the stored `vm_state_size` from the snapshot header during `Load`.
        int64_t total_bytes = 0;
        /// Size in bytes of the VM state stream stored on the qcow2 block device.
        int64_t disk_bytes = 0;
        /// Number of non-iterable device/subsystem VM state sections saved or loaded so far.
        int32_t completed_devices = 0;
        /// Total number of non-iterable VM state sections (`Save`) or upper bound on expected
        /// sections (`Load`).
        int32_t total_devices = 0;
        /// Human-readable failure reason populated when `phase == SnapshotPhase::Failed`; empty
        /// in non-failure phases.
        std::string error_message;

        /**
         * @brief Computes an overall completion percentage in the range [0, 100].
         *
         * Returns 100 only when `phase == SnapshotPhase::Completed`, and 0 while
         * `phase == SnapshotPhase::None` or `phase == SnapshotPhase::Preparing`.
         * During `Transferring` (or `Failed`), combines byte and device ratios
         * (weighted 85% / 15% when both totals are positive) and caps the result
         * at `kMaxInProgressPercent` (99) so 100% is reserved for `Completed`.
         */
        int32_t ProgressPercent() const {
            if (phase == SnapshotPhase::Completed) {
                return kCompletedProgressPercent;
            }
            if (phase == SnapshotPhase::None || phase == SnapshotPhase::Preparing) {
                return 0;
            }
            double ratio = 0.0;
            if (total_bytes > 0) {
                const double byte_ratio = std::clamp(
                        static_cast<double>(transferred_bytes) / static_cast<double>(total_bytes),
                        0.0, 1.0);
                if (total_devices > 0) {
                    const double dev_ratio = std::clamp(static_cast<double>(completed_devices) /
                                                                static_cast<double>(total_devices),
                                                        0.0, 1.0);
                    ratio = kByteProgressWeight * byte_ratio + kDeviceProgressWeight * dev_ratio;
                } else {
                    ratio = byte_ratio;
                }
            } else if (total_devices > 0) {
                ratio = std::clamp(
                        static_cast<double>(completed_devices) / static_cast<double>(total_devices),
                        0.0, 1.0);
            }
            return std::clamp(static_cast<int32_t>(ratio * 100.0), 0, kMaxInProgressPercent);
        }
    };

    /** @brief Source of snapshot progress updates, delivered on an event loop. */
    using SnapshotSource = ::goldfish::async::LoopBoundCallbackSource<SnapshotProgress>;

    /**
     * @brief Creates the interface with run state and snapshot events delivered on |loop|.
     *
     * @param loop The loop that runs the callbacks. Must outlive this object.
     */
    explicit VmOperations(::goldfish::async::EventLoop* loop)
            : run_state_events_(loop), snapshot_events_(loop) {}

    /**
     * @brief Virtual destructor for the VmOperations interface.
     */
    virtual ~VmOperations() = default;

    /**
     * @brief Run state changes.
     *
     * Callbacks run on the loop passed to the constructor, never on the thread that changed
     * the run state, which may hold the BQL. Events still queued when a callback is removed,
     * or when this object is destroyed, are dropped. Subscribers must not invoke mutating
     * or snapshot methods on `VmOperations` from inside a callback.
     */
    RunStateSource& RunStateEvents() { return run_state_events_; }

    /**
     * @brief Snapshot progress events.
     *
     * Callbacks run on the loop passed to the constructor, never on the thread performing the
     * snapshot operation, which holds the BQL. Every operation that begins in QEMU reports
     * `SnapshotPhase::Preparing` once, optional `SnapshotPhase::Transferring` updates (throttled
     * to at most 4 Hz with a trailing flush of the latest suppressed update), and finally
     * `SnapshotPhase::Completed` or `SnapshotPhase::Failed` once. Calls that fail before QEMU
     * starts the snapshot operation (such as `FAILED_PRECONDITION` run-state checks) emit no
     * events.
     *
     * Events still queued when a callback is removed, or when this object is destroyed, are
     * dropped. `RemoveCallback` (and `ScopedSubscription` destruction) synchronously waits for any
     * in-flight callback invocation to finish, so subscribers must not remove their own callback
     * from inside the callback. Subscribers must not invoke mutating or snapshot methods on
     * `VmOperations` from inside a callback.
     */
    SnapshotSource& SnapshotEvents() { return snapshot_events_; }

    /**
     * @brief Stops the virtual machine.
     *
     * Idempotent if the VM is already paused.
     *
     * @return OK on success, or FailedPrecondition/Internal on failure.
     */
    virtual absl::Status Stop() = 0;

    /**
     * @brief Starts the virtual machine.
     *
     * Idempotent if the VM is already running.
     *
     * @return OK on success, or FailedPrecondition on failure.
     */
    virtual absl::Status Start() = 0;

    /**
     * @brief Resets the virtual machine.
     *
     * @return OK on success, or FailedPrecondition if the VM cannot be reset.
     */
    virtual absl::Status Reset() = 0;

    /**
     * @brief Shuts down the virtual machine.
     *
     * Idempotent if the VM is already shutting down.
     *
     * @return OK on success, or FailedPrecondition if a snapshot is in progress.
     */
    virtual absl::Status Shutdown() = 0;

    /**
     * @brief Pauses the virtual machine.
     *
     * Idempotent if the VM is already paused.
     *
     * @return OK on success, or FailedPrecondition/Internal on failure.
     */
    virtual absl::Status Pause() = 0;

    /**
     * @brief Resumes the virtual machine.
     *
     * Idempotent if the VM is already running.
     *
     * @return OK on success, or FailedPrecondition on failure.
     */
    virtual absl::Status Resume() = 0;

    /**
     * @brief Checks if the virtual machine is currently running.
     *
     * @return True if the VM is running, false otherwise.
     */
    virtual bool IsRunning() = 0;

    /**
     * @brief Gets the configuration of the virtual machine.
     *
     * @return The VM's configuration.
     */
    virtual VmConfiguration GetConfiguration() = 0;

    /**
     * @brief Gets the current run state of the emulator.
     *
     * @return The emulator's run state.
     */
    virtual EmuRunState GetRunState() = 0;

    /**
     * @brief Requests a system shutdown with a specified reason.
     *
     * @param reason The reason for the shutdown request.
     */
    virtual void SystemShutdownRequest(QemuShutdownCause reason) = 0;

    struct SnapshotEntry {
        std::string name;
        absl::Time timestamp;
        // Whether this emulator can load the snapshot: its recorded snapshot
        // version matches the emulator's (see goldfish/snapshot/snapshot_store.h).
        bool compatible = true;
        // Whether this snapshot is compatible and was the one most recently
        // loaded in this session (via startup -loadvm, LoadSnapshot, or HMP
        // loadvm) without having been invalidated by a subsequent load,
        // deletion, or VM reset.
        bool loaded = false;
    };

    using SnapshotEntrySink = absl::FunctionRef<void(SnapshotEntry)>;

    /**
     * @brief Saves the VM state as the internal snapshot `name`, together with
     * the per-snapshot metadata kept under <avd>/snapshots/<name>/.
     *
     * @return OK on success; InvalidArgument for an invalid name;
     *   FailedPrecondition when snapshots are disabled for this session
     *   (-no-snapshot), or a snapshot/migration is already in progress, or the
     *   VM is shutting down; Internal when QEMU fails to save.
     */
    virtual absl::Status SaveSnapshot(std::string name, bool overwrite) = 0;

    /**
     * @brief Restores the VM from the internal snapshot `id_or_name`.
     *
     * @return OK on success; InvalidArgument for an invalid name;
     *   FailedPrecondition when snapshots are disabled for this session, the
     *   snapshot's recorded version does not match this emulator (the VM keeps
     *   running), a snapshot/migration is already in progress, or the VM is
     *   shutting down; Internal when QEMU fails to load.
     */
    virtual absl::Status LoadSnapshot(std::string id_or_name, bool and_resume) = 0;
    virtual void LoadSnapshotResume(EmuRunState state) = 0;

    /**
     * @brief Deletes the internal snapshot `id_or_name` and its per-snapshot
     * metadata. Available even when snapshots are disabled for this session.
     */
    virtual absl::Status DeleteSnapshot(std::string id_or_name) = 0;

    /**
     * @brief Lists the internal snapshots. Available even when snapshots are
     * disabled for this session.
     */
    virtual absl::Status ListSnapshots(SnapshotEntrySink sink) = 0;

    /**
     * @brief Queries the latest snapshot progress without acquiring the BQL.
     *
     * Returns a default-constructed `SnapshotProgress` (`operation == Idle`, `phase == None`)
     * before any snapshot operation has started. Once an operation runs, retains the most recent
     * progress report (including terminal `Completed` or `Failed` states) until the next snapshot
     * operation enters `SnapshotPhase::Preparing`.
     */
    virtual SnapshotProgress GetSnapshotProgress() const { return {}; }

  private:
    RunStateSource run_state_events_;
    SnapshotSource snapshot_events_;
};

}  // namespace goldfish
}  // namespace android
