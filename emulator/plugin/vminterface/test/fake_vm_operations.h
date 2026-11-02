#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

#include "android/goldfish/vm_interface.h"

namespace android {
namespace goldfish {

/**
 * A fake VmOperations for testing.
 */
class FakeVmOperations : public VmOperations {
  public:
    explicit FakeVmOperations(::goldfish::async::EventLoop* loop) : VmOperations(loop) {}

    absl::Status Stop() override { return Pause(); }
    absl::Status Start() override { return Resume(); }
    absl::Status Reset() override {
        if (run_state == EmuRunState::Shutdown || run_state == EmuRunState::SaveVm ||
            run_state == EmuRunState::RestoreVm) {
            return absl::FailedPreconditionError("Cannot reset VM in current state");
        }
        ++reset_count;
        ClearLoadedSnapshot();
        SetRunState(EmuRunState::Running);
        return absl::OkStatus();
    }
    absl::Status Shutdown() override {
        if (run_state == EmuRunState::SaveVm || run_state == EmuRunState::RestoreVm) {
            return absl::FailedPreconditionError("Cannot shut down VM during snapshot");
        }
        ++shutdown_count;
        SetRunState(EmuRunState::Shutdown);
        return absl::OkStatus();
    }
    absl::Status Pause() override {
        if (run_state == EmuRunState::Paused) {
            return absl::OkStatus();
        }
        if (run_state != EmuRunState::Running && run_state != EmuRunState::Suspended) {
            return absl::FailedPreconditionError("Cannot pause VM in current state");
        }
        SetRunState(EmuRunState::Paused);
        return absl::OkStatus();
    }
    absl::Status Resume() override {
        if (run_state == EmuRunState::Running) {
            return absl::OkStatus();
        }
        if (run_state != EmuRunState::Paused && run_state != EmuRunState::Suspended) {
            return absl::FailedPreconditionError("Cannot resume VM in current state");
        }
        SetRunState(EmuRunState::Running);
        return absl::OkStatus();
    }
    bool IsRunning() override { return run_state == EmuRunState::Running; }

    VmConfiguration GetConfiguration() override { return configuration; }
    EmuRunState GetRunState() override { return run_state; }
    void SetRunState(EmuRunState state) {
        run_state = state;
        RunStateEvents().FireEvent(run_state);
    }

    void SystemShutdownRequest(QemuShutdownCause /*reason*/) override {}
    void LoadSnapshotResume(EmuRunState /*state*/) override {}

    absl::Status SaveSnapshot(std::string name, bool /*overwrite*/) override {
        if (on_save) {
            on_save(name);
        }
        if (fail_save) {
            return absl::InternalError("Failed to write snapshot to disk");
        }
        if (!save_status.ok()) {
            return save_status;
        }
        const auto existing = entries.find(name);
        const bool was_loaded = existing != entries.end() && existing->second.loaded;
        entries[name] = SnapshotEntry{
            .name = name,
            .timestamp = absl::FromUnixSeconds(1700000000),
            .compatible = true,
            .loaded = was_loaded,
        };
        return absl::OkStatus();
    }

    absl::Status LoadSnapshot(std::string id_or_name, bool /*and_resume*/) override {
        if (!load_status.ok()) {
            return load_status;
        }
        if (on_load) {
            on_load(id_or_name);
        }
        if (fail_load) {
            ClearLoadedSnapshot();
            return absl::InternalError("Failed to read snapshot from disk");
        }
        const auto it = entries.find(id_or_name);
        if (it == entries.end()) {
            return absl::NotFoundError("Snapshot does not exist");
        }
        if (!it->second.compatible) {
            return absl::FailedPreconditionError("Snapshot is incompatible");
        }
        SetLoadedSnapshot(std::move(id_or_name));
        return absl::OkStatus();
    }

    absl::Status DeleteSnapshot(std::string id_or_name) override {
        if (entries.erase(id_or_name) == 0) {
            return absl::NotFoundError("Snapshot does not exist");
        }
        if (last_loaded_snapshot == id_or_name) {
            last_loaded_snapshot.clear();
        }
        return absl::OkStatus();
    }

    absl::Status ListSnapshots(SnapshotEntrySink sink) override {
        if (!list_status.ok()) {
            return list_status;
        }
        for (const auto& [id, entry] : entries) {
            sink(entry);
        }
        return absl::OkStatus();
    }

    SnapshotProgress GetSnapshotProgress() const override { return progress; }

    void EmitSnapshotProgress(const SnapshotProgress& next_progress) {
        progress = next_progress;
        if (next_progress.operation == SnapshotOperationType::Load) {
            if (next_progress.phase == SnapshotPhase::Preparing ||
                next_progress.phase == SnapshotPhase::Failed) {
                ClearLoadedSnapshot();
            } else if (next_progress.phase == SnapshotPhase::Completed &&
                       !next_progress.snapshot_id.empty()) {
                SetLoadedSnapshot(next_progress.snapshot_id);
            }
        }
        SnapshotEvents().FireEvent(next_progress);
    }

    void ClearLoadedSnapshot() {
        last_loaded_snapshot.clear();
        for (auto& [id, entry] : entries) {
            entry.loaded = false;
        }
    }

    void SetLoadedSnapshot(std::string snapshot_id) {
        for (auto& [id, entry] : entries) {
            entry.loaded = entry.compatible && (id == snapshot_id);
        }
        last_loaded_snapshot = std::move(snapshot_id);
    }

    bool fail_save = false;
    bool fail_load = false;
    // Returned by SaveSnapshot / LoadSnapshot / ListSnapshots when not OK, e.g.
    // to mimic the FailedPrecondition of a session started with -no-snapshot.
    absl::Status save_status = absl::OkStatus();
    absl::Status load_status = absl::OkStatus();
    absl::Status list_status = absl::OkStatus();
    int reset_count = 0;
    int shutdown_count = 0;
    EmuRunState run_state = EmuRunState::Running;
    VmConfiguration configuration = {
        .hypervisor_type = VmHypervisorType::Hvf,
        .number_of_cpu_cores = 4,
        .cpu_type = "arm",
    };
    std::string last_loaded_snapshot;
    SnapshotProgress progress;
    std::function<void(std::string_view)> on_save;
    std::function<void(std::string_view)> on_load;
    absl::flat_hash_map<std::string, SnapshotEntry> entries;
};

}  // namespace goldfish
}  // namespace android
