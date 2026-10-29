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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"

#include "android/base/testing/test_temp_dir.h"
#include "android/crashreport/hang_detector.h"
#include "android/goldfish/qemu_vm_operations.h"
#include "android/goldfish/vm_interface.h"
#include "android/status/status_matcher_macros.h"
#include "emulator/plugin/vminterface/test/test_vm_lock.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/avd/fake_avd_universe.h"
#include "goldfish/file/file.h"
#include "goldfish/snapshot/snapshot_store.h"

extern "C" {
#include "emulator/plugin/vminterface/test/vm_mock.h"
}
#undef shutdown

namespace android {
namespace goldfish {

using ::absl_testing::StatusIs;
using ::android::base::TestTempDir;
using ::goldfish::async::LibuvEventLoop;
using ::goldfish::async::ThreadedEventLoop;
using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::snapshot::FileSnapshotStore;

// Arbitrary snapshot version for the store under test.
constexpr int kSnapshotVersion = ::goldfish::snapshot::MakeSnapshotVersion(5);
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

class TestHangDetector : public ::android::crashreport::HangDetector {
  public:
    void AddWatchedLooper(std::string /*loop_name*/, ::goldfish::async::EventLoop& /*event_loop*/,
                          absl::Duration /*task_timeout*/, ActivePredicate /*is_active*/) override {
    }
    void RemoveWatchedLooper(::goldfish::async::EventLoop& /*event_loop*/) override {}
    void AddPredicate(std::string name, HangPredicate check) override {
        absl::MutexLock lock(&mutex_);
        predicates_[std::move(name)] = std::move(check);
    }
    void RemovePredicate(std::string_view name) override {
        absl::MutexLock lock(&mutex_);
        predicates_.erase(name);
    }
    void Stop() override {
        absl::MutexLock lock(&mutex_);
        predicates_.clear();
    }

    bool HasPredicate(std::string_view name) const {
        absl::MutexLock lock(&mutex_);
        return predicates_.contains(name);
    }

    absl::Status EvaluatePredicate(std::string_view name) const {
        HangPredicate check;
        {
            absl::MutexLock lock(&mutex_);
            const auto it = predicates_.find(name);
            if (it == predicates_.end()) {
                return absl::NotFoundError(
                        absl::StrCat("Predicate '", name, "' is not registered"));
            }
            check = it->second;
        }
        return check();
    }

  private:
    mutable absl::Mutex mutex_;
    absl::flat_hash_map<std::string, HangPredicate> predicates_ ABSL_GUARDED_BY(mutex_);
};

class QemuVmOperationsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        clock_.SetTime(absl::UnixEpoch() + absl::Seconds(100));

        // Ensure we have a clean state before each test.
        mock_runstate_set(RUN_STATE_DEBUG);
        mock_shutdown_cause_set(SHUTDOWN_CAUSE_NONE);
        mock_iothread_locked_set(false);
        mock_vm_stop_return_set(0);
        mock_snapshot_op_ok_set(false);
        mock_snapshot_op_hook_set(/*hook=*/nullptr, /*opaque=*/nullptr);
        snapshot_op_hook_ = nullptr;
        mock_snapshot_list_set(/*entries=*/nullptr, /*count=*/-1);
        test_vm_lock_ = TestVmLock::GetInstance();
        test_vm_lock_->lock_count_ = 0;
        test_vm_lock_->unlock_count_ = 0;
        old_lock_ = VmLock::Set(test_vm_lock_);

        ASSERT_TRUE(VmLock::HasInstance());

        mock_snapshot_calls_reset();
        CreateVmOps(/*snapshots_enabled=*/true);
        EXPECT_EQ(test_vm_lock_->lock_count_, 1);
        EXPECT_EQ(test_vm_lock_->unlock_count_, 1);
        test_vm_lock_->lock_count_ = 0;
        test_vm_lock_->unlock_count_ = 0;
    }

    // (Re)creates vm_ops_ over a FileSnapshotStore rooted at temp_dir_.
    void CreateVmOps(bool snapshots_enabled) {
        vm_ops_.reset();
        auto store = std::make_unique<FileSnapshotStore>(temp_dir_.Path(), kSnapshotVersion);
        store_ = store.get();
        vm_ops_ = CreateQemuVmOperations(
                /*event_loop=*/loop_.get(), /*qemu_loop=*/qemu_loop_.get(),
                QemuSnapshotConfig{.store = std::move(store), .enabled = snapshots_enabled},
                /*hang_detector=*/&hang_detector_, /*clock=*/clock_);
    }

    // Marks `name` as a snapshot this emulator can load.
    void MakeCompatibleSnapshot(std::string_view name) { ASSERT_OK(store_->WriteVersion(name)); }

    void SetSnapshotOpHook(std::function<void(std::string_view)> hook) {
        snapshot_op_hook_ = std::move(hook);
        mock_snapshot_op_hook_set(
                /*hook=*/
                [](const char* name, void* opaque) {
                    auto* fn = static_cast<std::function<void(std::string_view)>*>(opaque);
                    if (*fn) {
                        (*fn)(name != nullptr ? std::string_view(name) : std::string_view());
                    }
                },
                /*opaque=*/&snapshot_op_hook_);
    }

    // The thread qemu_loop_ runs its tasks on.
    GThread* QemuLoopThread() {
        GThread* thread = nullptr;
        EXPECT_OK(qemu_loop_->PostAndWait([&thread] { thread = g_thread_self(); }));
        return thread;
    }

    void TearDown() override {
        vm_ops_.reset();
        VmLock::Set(old_lock_);
        test_vm_lock_->Release();
        mock_runstate_set(RUN_STATE_DEBUG);
        mock_shutdown_cause_set(SHUTDOWN_CAUSE_NONE);
        mock_iothread_locked_set(false);
        mock_vm_stop_return_set(0);
        mock_snapshot_op_ok_set(false);
        mock_snapshot_op_hook_set(/*hook=*/nullptr, /*opaque=*/nullptr);
        snapshot_op_hook_ = nullptr;
        mock_snapshot_list_set(/*entries=*/nullptr, /*count=*/-1);
    }

    absl::SimulatedClock clock_;
    TestHangDetector hang_detector_;
    std::function<void(std::string_view)> snapshot_op_hook_;
    TestVmLock* test_vm_lock_ = nullptr;
    VmLock* old_lock_ = nullptr;
    TestTempDir temp_dir_{"vm_test"};
    // Declared before vm_ops_, so they outlive the VmOperations that posts to them.
    const std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create("EventLoop");
    const std::unique_ptr<ThreadedEventLoop> qemu_loop_ =
            ThreadedEventLoop::Create(LibuvEventLoop::Create("QemuLoop"));
    std::unique_ptr<VmOperations> vm_ops_;
    // Owned by vm_ops_.
    FileSnapshotStore* store_ = nullptr;
};

TEST_F(QemuVmOperationsTest, Stop) {
    mock_runstate_set(RUN_STATE_RUNNING);
    const size_t initial_tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->Stop());
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), initial_tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(runstate_get(), RUN_STATE_PAUSED);
}

TEST_F(QemuVmOperationsTest, Start) {
    const size_t initial_tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->Start());
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), initial_tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(runstate_get(), RUN_STATE_RUNNING);
}

TEST_F(QemuVmOperationsTest, Reset) {
    const size_t initial_tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->Reset());
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), initial_tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(mock_shutdown_cause_get(), SHUTDOWN_CAUSE_SUBSYSTEM_RESET);
}

TEST_F(QemuVmOperationsTest, Shutdown) {
    const size_t initial_tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->Shutdown());
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), initial_tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    // Note: qemu_system_shutdown_request is asynchronous in QEMU.
    // The mock implementation only records the shutdown cause and does not
    // simulate an immediate transition to RUN_STATE_SHUTDOWN.
    EXPECT_EQ(mock_shutdown_cause_get(), SHUTDOWN_CAUSE_HOST_UI);
}

TEST_F(QemuVmOperationsTest, Pause) {
    mock_runstate_set(RUN_STATE_RUNNING);
    const size_t initial_tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->Pause());
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), initial_tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(runstate_get(), RUN_STATE_PAUSED);
}

TEST_F(QemuVmOperationsTest, Resume) {
    const size_t initial_tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->Resume());
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), initial_tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(runstate_get(), RUN_STATE_RUNNING);
}

TEST_F(QemuVmOperationsTest, StartResumePauseStopAndShutdownAreIdempotent) {
    mock_runstate_set(RUN_STATE_RUNNING);
    EXPECT_OK(vm_ops_->Start());
    EXPECT_OK(vm_ops_->Resume());

    mock_runstate_set(RUN_STATE_PAUSED);
    EXPECT_OK(vm_ops_->Pause());
    EXPECT_OK(vm_ops_->Stop());

    mock_runstate_set(RUN_STATE_SHUTDOWN);
    EXPECT_OK(vm_ops_->Shutdown());
}

TEST_F(QemuVmOperationsTest, StartAndResumeFailInConflictingStates) {
    for (RunState state : {RUN_STATE_SHUTDOWN, RUN_STATE_SAVE_VM, RUN_STATE_RESTORE_VM,
                           RUN_STATE_INTERNAL_ERROR, RUN_STATE_GUEST_PANICKED}) {
        mock_runstate_set(state);
        EXPECT_THAT(vm_ops_->Start(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                               HasSubstr("Running, Paused, Suspended")));
        EXPECT_THAT(vm_ops_->Resume(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                                HasSubstr("Running, Paused, Suspended")));
        EXPECT_EQ(runstate_get(), state);
    }

    mock_runstate_set(RUN_STATE_SHUTDOWN);
    EXPECT_THAT(vm_ops_->Resume(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                            AllOf(HasSubstr("Shutdown"),
                                                  HasSubstr("Launch a new emulator instance"))));

    mock_runstate_set(RUN_STATE_SAVE_VM);
    EXPECT_THAT(
            vm_ops_->Resume(),
            StatusIs(absl::StatusCode::kFailedPrecondition,
                     AllOf(HasSubstr("SaveVm"), HasSubstr("Wait for the operation to complete"))));

    mock_runstate_set(RUN_STATE_GUEST_PANICKED);
    EXPECT_THAT(vm_ops_->Resume(),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("GuestPanicked"), HasSubstr("Reset the VM"))));
}

TEST_F(QemuVmOperationsTest, PauseAndStopFailInConflictingStates) {
    for (RunState state : {RUN_STATE_DEBUG, RUN_STATE_PRELAUNCH, RUN_STATE_POSTMIGRATE,
                           RUN_STATE_SHUTDOWN, RUN_STATE_SAVE_VM, RUN_STATE_RESTORE_VM,
                           RUN_STATE_INTERNAL_ERROR, RUN_STATE_GUEST_PANICKED}) {
        mock_runstate_set(state);
        EXPECT_THAT(vm_ops_->Pause(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                               HasSubstr("Running, Paused, or Suspended")));
        EXPECT_THAT(vm_ops_->Stop(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                              HasSubstr("Running, Paused, or Suspended")));
        EXPECT_EQ(runstate_get(), state);
    }

    mock_runstate_set(RUN_STATE_SHUTDOWN);
    EXPECT_THAT(vm_ops_->Pause(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                           AllOf(HasSubstr("Shutdown"),
                                                 HasSubstr("Launch a new emulator instance"))));

    mock_runstate_set(RUN_STATE_SAVE_VM);
    EXPECT_THAT(vm_ops_->Pause(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                           AllOf(HasSubstr("SaveVm"),
                                                 HasSubstr("Wait for the operation to complete"))));

    mock_runstate_set(RUN_STATE_DEBUG);
    EXPECT_THAT(vm_ops_->Pause(), StatusIs(absl::StatusCode::kFailedPrecondition,
                                           AllOf(HasSubstr("Debug"), HasSubstr("Reset the VM"))));
}

TEST_F(QemuVmOperationsTest, PauseAndStopPropagateVmStopErrors) {
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_vm_stop_return_set(-ENOSPC);

    EXPECT_THAT(vm_ops_->Pause(),
                StatusIs(absl::StatusCode::kResourceExhausted,
                         AllOf(HasSubstr("Cannot pause VM"), HasSubstr("check host storage"))));
    EXPECT_THAT(vm_ops_->Stop(),
                StatusIs(absl::StatusCode::kResourceExhausted, HasSubstr("Cannot pause VM")));

    mock_vm_stop_return_set(1);
    EXPECT_THAT(vm_ops_->Pause(), StatusIs(absl::StatusCode::kInternal,
                                           AllOf(HasSubstr("device stop callback failed"),
                                                 HasSubstr("Reset the VM"))));
    EXPECT_THAT(vm_ops_->Stop(),
                StatusIs(absl::StatusCode::kInternal, HasSubstr("device stop callback failed")));
}

TEST_F(QemuVmOperationsTest, ResetAndShutdownFailInConflictingStates) {
    mock_runstate_set(RUN_STATE_SHUTDOWN);
    EXPECT_THAT(vm_ops_->Reset(),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("Shutdown"),
                               HasSubstr("Running, Paused, Suspended, Debug, or fault state"),
                               HasSubstr("Launch a new emulator instance"))));

    for (RunState state : {RUN_STATE_SAVE_VM, RUN_STATE_RESTORE_VM}) {
        mock_runstate_set(state);
        EXPECT_THAT(vm_ops_->Reset(),
                    StatusIs(absl::StatusCode::kFailedPrecondition,
                             AllOf(HasSubstr("Running, Paused, Suspended, Debug, or fault state"),
                                   HasSubstr("Wait for the operation to complete"))));
        EXPECT_THAT(vm_ops_->Shutdown(),
                    StatusIs(absl::StatusCode::kFailedPrecondition,
                             AllOf(HasSubstr("Running, Paused, Suspended, Debug, or fault state"),
                                   HasSubstr("Wait for the operation to complete"))));
    }
}

TEST_F(QemuVmOperationsTest, IsRunning) {
    // Tests that IsRunning correctly reports the VM's running state.
    mock_runstate_set(RUN_STATE_RUNNING);
    ASSERT_TRUE(vm_ops_->IsRunning());
    mock_runstate_set(RUN_STATE_PAUSED);
    ASSERT_FALSE(vm_ops_->IsRunning());
    mock_runstate_set(RUN_STATE_DEBUG);
    ASSERT_FALSE(vm_ops_->IsRunning());
}

TEST_F(QemuVmOperationsTest, GetRunState) {
    // Tests that GetRunState correctly returns the VM's run state.
    mock_runstate_set(RUN_STATE_RUNNING);
    ASSERT_EQ(vm_ops_->GetRunState(), EmuRunState::Running);
    mock_runstate_set(RUN_STATE_PAUSED);
    ASSERT_EQ(vm_ops_->GetRunState(), EmuRunState::Paused);
    mock_runstate_set(RUN_STATE_DEBUG);
    ASSERT_EQ(vm_ops_->GetRunState(), EmuRunState::Debug);
}

TEST_F(QemuVmOperationsTest, SystemShutdownRequest) {
    // Tests that SystemShutdownRequest correctly sets the shutdown cause.
    vm_ops_->SystemShutdownRequest(QemuShutdownCause::GuestShutdown);
    ASSERT_EQ(mock_shutdown_cause_get(), SHUTDOWN_CAUSE_GUEST_SHUTDOWN);

    vm_ops_->SystemShutdownRequest(QemuShutdownCause::HostError);
    ASSERT_EQ(mock_shutdown_cause_get(), SHUTDOWN_CAUSE_HOST_ERROR);
}

TEST_F(QemuVmOperationsTest, RunStateChangeCallback) {
    std::vector<EmuRunState> received_states;
    auto& events = vm_ops_->RunStateEvents();
    auto id = events.AddCallback([&](EmuRunState state) { received_states.push_back(state); });

    mock_fire_vm_change_state(true, RUN_STATE_RUNNING);
    mock_fire_vm_change_state(false, RUN_STATE_PAUSED);
    loop_->RunAll();

    ASSERT_EQ(received_states.size(), 2);
    EXPECT_EQ(received_states[0], EmuRunState::Running);
    EXPECT_EQ(received_states[1], EmuRunState::Paused);

    events.RemoveCallback(id);
    mock_fire_vm_change_state(true, RUN_STATE_RUNNING);
    loop_->RunAll();
    EXPECT_EQ(received_states.size(), 2);
}

TEST_F(QemuVmOperationsTest, RunStateChangeScopedCallback) {
    std::vector<EmuRunState> received_states;
    {
        auto handle = android::base::eventing::MakeScopedCallback(
                vm_ops_->RunStateEvents(),
                [&](EmuRunState state) { received_states.push_back(state); });

        mock_fire_vm_change_state(true, RUN_STATE_RUNNING);
        loop_->RunAll();
        EXPECT_EQ(received_states.size(), 1);
        EXPECT_EQ(received_states[0], EmuRunState::Running);
    }
    // Handle went out of scope, callback should be automatically unregistered.
    mock_fire_vm_change_state(false, RUN_STATE_PAUSED);
    loop_->RunAll();
    EXPECT_EQ(received_states.size(), 1);
}

TEST_F(QemuVmOperationsTest, RunStateEventsAreDeliveredOnTheLoop) {
    std::vector<EmuRunState> received_states;
    bool delivered_on_loop = false;
    auto handle = android::base::eventing::MakeScopedCallback(
            vm_ops_->RunStateEvents(), [&](EmuRunState state) {
                delivered_on_loop = loop_->IsOnLoopThread();
                received_states.push_back(state);
            });

    mock_fire_vm_change_state(true, RUN_STATE_RUNNING);
    // The thread that changed the run state holds the BQL, so subscribers must not run on it.
    EXPECT_THAT(received_states, IsEmpty());

    loop_->RunAll();
    EXPECT_THAT(received_states, ElementsAre(EmuRunState::Running));
    EXPECT_TRUE(delivered_on_loop);
}

TEST_F(QemuVmOperationsTest, CallbackRemovedWhileItsEventIsQueuedIsNotCalled) {
    std::vector<EmuRunState> received_states;
    {
        auto handle = android::base::eventing::MakeScopedCallback(
                vm_ops_->RunStateEvents(),
                [&](EmuRunState state) { received_states.push_back(state); });
        mock_fire_vm_change_state(true, RUN_STATE_RUNNING);
        // The handle goes away while its event is still queued on the loop.
    }
    loop_->RunAll();

    EXPECT_THAT(received_states, IsEmpty());
}

TEST_F(QemuVmOperationsTest, EventsQueuedWhenVmOperationsIsDestroyedAreDropped) {
    std::vector<EmuRunState> received_states;
    vm_ops_->RunStateEvents().AddCallback(
            [&](EmuRunState state) { received_states.push_back(state); });
    mock_fire_vm_change_state(true, RUN_STATE_RUNNING);

    vm_ops_.reset();
    loop_->RunAll();

    EXPECT_THAT(received_states, IsEmpty());
}

using QemuVmOperationsDeathTest = QemuVmOperationsTest;

TEST_F(QemuVmOperationsDeathTest, VmOperationOnEventLoopDies) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
            {
                loop_->Post([this] { vm_ops_->Pause().IgnoreError(); }).IgnoreError();
                loop_->RunAll();
            },
            "VmOperations cannot be called from the event loop");
}

TEST_F(QemuVmOperationsDeathTest, VmOperationOnQemuLoopDies) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
            { qemu_loop_->PostAndWait([this] { vm_ops_->Pause().IgnoreError(); }).IgnoreError(); },
            "postAndWait cannot be called from the event loop");
}

TEST_F(QemuVmOperationsDeathTest, RunStateChangeOnTheLoopThreadDies) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto change_run_state = [] { mock_fire_vm_change_state(true, RUN_STATE_RUNNING); };
    // On the loop thread the subscribers would run inline, under the BQL.
    EXPECT_DEBUG_DEATH(
            {
                loop_->Post(change_run_state).IgnoreError();
                loop_->RunAll();
            },
            "event loop thread");
}

TEST_F(QemuVmOperationsTest, DestructorUnregistersRunStateHandler) {
    ASSERT_TRUE(mock_has_vm_change_state_handler());

    vm_ops_.reset();

    EXPECT_FALSE(mock_has_vm_change_state_handler());
    EXPECT_EQ(test_vm_lock_->lock_count_, 1);
    EXPECT_EQ(test_vm_lock_->unlock_count_, 1);
}

TEST_F(QemuVmOperationsTest, SnapshotMethodsExecuteOnQemuLoopWithoutVmLock) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    mock_runstate_set(RUN_STATE_RUNNING);

    // SaveSnapshot failure (with null errp) and success.
    mock_snapshot_op_ok_set(false);
    size_t tasks = qemu_loop_->WaitUntilIdle();
    EXPECT_THAT(
            vm_ops_->SaveSnapshot("snap_a", /*overwrite=*/true),
            StatusIs(absl::StatusCode::kInternal, AllOf(HasSubstr("Cannot save snapshot 'snap_a'"),
                                                        HasSubstr("writable qcow2 disk image"))));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    mock_snapshot_op_ok_set(true);
    tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->SaveSnapshot("snap_a", /*overwrite=*/true));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    // LoadSnapshot failure and success (without auto-resume, followed by LoadSnapshotResume).
    mock_snapshot_op_ok_set(false);
    tasks = qemu_loop_->WaitUntilIdle();
    EXPECT_THAT(
            vm_ops_->LoadSnapshot("snap_a", /*and_resume=*/false),
            StatusIs(absl::StatusCode::kInternal, AllOf(HasSubstr("Cannot load snapshot 'snap_a'"),
                                                        HasSubstr("ListSnapshots"))));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);
    tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->LoadSnapshot("snap_a", /*and_resume=*/false));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(runstate_get(), RUN_STATE_RESTORE_VM);

    tasks = qemu_loop_->WaitUntilIdle();
    vm_ops_->LoadSnapshotResume(EmuRunState::Running);
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(runstate_get(), RUN_STATE_RUNNING);

    // DeleteSnapshot failure and success.
    mock_snapshot_op_ok_set(false);
    tasks = qemu_loop_->WaitUntilIdle();
    EXPECT_THAT(vm_ops_->DeleteSnapshot("snap_a"),
                StatusIs(absl::StatusCode::kInternal,
                         AllOf(HasSubstr("Cannot delete snapshot 'snap_a'"),
                               HasSubstr("disk image is writable"))));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    mock_snapshot_op_ok_set(true);
    tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->DeleteSnapshot("snap_a"));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
}

TEST_F(QemuVmOperationsTest, ListSnapshotsExecutesOnQemuLoopAndPropagatesErrors) {
    // No VM state block device -> FAILED_PRECONDITION.
    mock_snapshot_list_set(/*entries=*/nullptr, /*count=*/-1);
    size_t tasks = qemu_loop_->WaitUntilIdle();
    EXPECT_THAT(vm_ops_->ListSnapshots([](VmOperations::SnapshotEntry) {}),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("no block device supports VM state snapshots"),
                               HasSubstr("snapshots.qcow2"))));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    // Negative -errno from bdrv_snapshot_list -> mapped via absl::ErrnoToStatus.
    std::array<QEMUSnapshotInfo, 1> qsi{};
    std::snprintf(qsi[0].id_str, sizeof(qsi[0].id_str), "1");
    std::snprintf(qsi[0].name, sizeof(qsi[0].name), "default_boot");
    qsi[0].date_sec = 1700000000;
    mock_snapshot_list_set(/*entries=*/qsi.data(), /*count=*/-EIO);
    tasks = qemu_loop_->WaitUntilIdle();
    EXPECT_THAT(vm_ops_->ListSnapshots([](VmOperations::SnapshotEntry) {}),
                StatusIs(absl::ErrnoToStatusCode(EIO),
                         HasSubstr("Cannot list snapshots: failed to read snapshot table")));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);

    // Valid snapshot list.
    mock_snapshot_list_set(/*entries=*/qsi.data(), /*count=*/static_cast<int>(qsi.size()));
    std::vector<VmOperations::SnapshotEntry> entries;
    tasks = qemu_loop_->WaitUntilIdle();
    ASSERT_OK(vm_ops_->ListSnapshots(
            [&](VmOperations::SnapshotEntry entry) { entries.push_back(std::move(entry)); }));
    EXPECT_EQ(qemu_loop_->WaitUntilIdle(), tasks + 1);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].name, "default_boot");
}

TEST_F(QemuVmOperationsTest, QemuSnapshotCallsRunOnQemuLoopAndStoreWorkOnCaller) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);
    GThread* const qemu_thread = QemuLoopThread();
    ASSERT_NE(qemu_thread, nullptr);
    ASSERT_NE(qemu_thread, g_thread_self());

    // Save: QEMU is called exactly once, on the QEMU loop thread, and the
    // metadata is written by the time SaveSnapshot returns.
    mock_snapshot_calls_reset();
    ASSERT_OK(vm_ops_->SaveSnapshot("named", /*overwrite=*/true));
    EXPECT_EQ(mock_snapshot_call_count(), 1);
    EXPECT_EQ(mock_snapshot_last_call_thread(), qemu_thread);
    EXPECT_STREQ(mock_snapshot_last_call_name(), "named");
    EXPECT_TRUE(store_->IsCompatible("named"));

    // Load.
    mock_snapshot_calls_reset();
    ASSERT_OK(vm_ops_->LoadSnapshot("named", /*and_resume=*/true));
    EXPECT_EQ(mock_snapshot_call_count(), 1);
    EXPECT_EQ(mock_snapshot_last_call_thread(), qemu_thread);
    EXPECT_STREQ(mock_snapshot_last_call_name(), "named");

    // List.
    std::array<QEMUSnapshotInfo, 1> qsi{};
    std::snprintf(qsi[0].name, sizeof(qsi[0].name), "named");
    mock_snapshot_list_set(qsi.data(), static_cast<int>(qsi.size()));
    mock_snapshot_calls_reset();
    ASSERT_OK(vm_ops_->ListSnapshots([](VmOperations::SnapshotEntry) {}));
    EXPECT_EQ(mock_snapshot_call_count(), 1);
    EXPECT_EQ(mock_snapshot_last_call_thread(), qemu_thread);

    // Delete.
    mock_snapshot_calls_reset();
    ASSERT_OK(vm_ops_->DeleteSnapshot("named"));
    EXPECT_EQ(mock_snapshot_call_count(), 1);
    EXPECT_EQ(mock_snapshot_last_call_thread(), qemu_thread);
    EXPECT_STREQ(mock_snapshot_last_call_name(), "named");
    EXPECT_FALSE(store_->Exists("named"));
}

TEST_F(QemuVmOperationsTest, SaveSnapshotRecordsVersionOnlyOnSuccess) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    mock_runstate_set(RUN_STATE_RUNNING);

    // A failed save of a new snapshot leaves nothing behind.
    mock_snapshot_op_ok_set(false);
    EXPECT_THAT(vm_ops_->SaveSnapshot("fresh", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kInternal));
    EXPECT_FALSE(store_->Exists("fresh"));

    // A successful save records the current version.
    mock_snapshot_op_ok_set(true);
    ASSERT_OK(vm_ops_->SaveSnapshot("fresh", /*overwrite=*/true));
    EXPECT_THAT(store_->ReadVersion("fresh"), ::absl_testing::IsOkAndHolds(kSnapshotVersion));

    // A failed overwrite of an existing snapshot keeps its directory.
    mock_snapshot_op_ok_set(false);
    EXPECT_THAT(vm_ops_->SaveSnapshot("fresh", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kInternal));
    EXPECT_TRUE(store_->Exists("fresh"));

    // Rejected up front (bad state): nothing is created for a new name.
    mock_runstate_set(RUN_STATE_SAVE_VM);
    EXPECT_THAT(vm_ops_->SaveSnapshot("later", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_FALSE(store_->Exists("later"));
}

TEST_F(QemuVmOperationsTest, ExitTimeSaveCompletionRecordsVersion) {
    // The launcher's -savevm save at exit does not go through SaveSnapshot(); QEMU only
    // reports its completion. That report must record the version, and a failed one must not.
    ASSERT_OK(store_->Prepare("default_boot"));
    EXPECT_FALSE(store_->IsCompatible("default_boot"));

    QemuSnapshotProgress failed{};
    failed.operation = QEMU_SNAPSHOT_OP_SAVE;
    failed.phase = QEMU_SNAPSHOT_PHASE_FAILED;
    failed.name = "default_boot";
    ASSERT_OK(qemu_loop_->PostAndWait([&] { mock_fire_snapshot_progress(&failed); }));
    EXPECT_FALSE(store_->IsCompatible("default_boot"));

    QemuSnapshotProgress completed = failed;
    completed.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    ASSERT_OK(qemu_loop_->PostAndWait([&] { mock_fire_snapshot_progress(&completed); }));
    EXPECT_TRUE(store_->IsCompatible("default_boot"));

    // A completed load does not touch the version.
    ASSERT_OK(store_->Prepare("loaded"));
    QemuSnapshotProgress loaded = completed;
    loaded.operation = QEMU_SNAPSHOT_OP_LOAD;
    loaded.name = "loaded";
    ASSERT_OK(qemu_loop_->PostAndWait([&] { mock_fire_snapshot_progress(&loaded); }));
    EXPECT_FALSE(store_->IsCompatible("loaded"));
}

TEST_F(QemuVmOperationsTest, LoadSnapshotRejectsIncompatibleVersionWithoutStoppingVm) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);

    // No version file at all (e.g. saved by a QEMU-only path).
    mock_snapshot_calls_reset();
    EXPECT_THAT(vm_ops_->LoadSnapshot("unknown", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("Cannot load snapshot 'unknown'"),
                               HasSubstr("incompatible"), HasSubstr("missing"),
                               HasSubstr("DeleteSnapshot"))));
    EXPECT_EQ(mock_snapshot_call_count(), 0);
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::Running);

    // Stale version file.
    ASSERT_OK(store_->Prepare("old"));
    {
        std::ofstream ofs(store_->Dir("old") / "version.txt");
        ofs << (kSnapshotVersion - 1);
    }
    EXPECT_THAT(vm_ops_->LoadSnapshot("old", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("snapshot version"),
                               HasSubstr(absl::StrCat(kSnapshotVersion - 1)),
                               HasSubstr(absl::StrCat("expected ", kSnapshotVersion)))));
    EXPECT_EQ(mock_snapshot_call_count(), 0);
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::Running);

    // Matching version loads.
    MakeCompatibleSnapshot("old");
    EXPECT_OK(vm_ops_->LoadSnapshot("old", /*and_resume=*/true));
    EXPECT_EQ(mock_snapshot_call_count(), 1);
}

TEST_F(QemuVmOperationsTest, ListSnapshotsReportsCompatibility) {
    std::array<QEMUSnapshotInfo, 3> qsi{};
    std::snprintf(qsi[0].name, sizeof(qsi[0].name), "current");
    std::snprintf(qsi[1].name, sizeof(qsi[1].name), "stale");
    std::snprintf(qsi[2].name, sizeof(qsi[2].name), "no_metadata");
    mock_snapshot_list_set(qsi.data(), static_cast<int>(qsi.size()));
    MakeCompatibleSnapshot("current");
    ASSERT_OK(store_->Prepare("stale"));
    {
        std::ofstream ofs(store_->Dir("stale") / "version.txt");
        ofs << 1;
    }

    std::vector<VmOperations::SnapshotEntry> entries;
    ASSERT_OK(vm_ops_->ListSnapshots(
            [&](VmOperations::SnapshotEntry entry) { entries.push_back(std::move(entry)); }));
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].name, "current");
    EXPECT_TRUE(entries[0].compatible);
    EXPECT_EQ(entries[1].name, "stale");
    EXPECT_FALSE(entries[1].compatible);
    EXPECT_EQ(entries[2].name, "no_metadata");
    EXPECT_FALSE(entries[2].compatible);
}

TEST_F(QemuVmOperationsTest, InvalidSnapshotNamesAreRejectedBeforeQemu) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);
    for (const char* name : {"", "../escape", "a/b", "a\\b"}) {
        SCOPED_TRACE(name);
        mock_snapshot_calls_reset();
        EXPECT_THAT(vm_ops_->SaveSnapshot(name, /*overwrite=*/true),
                    StatusIs(absl::StatusCode::kInvalidArgument));
        EXPECT_THAT(vm_ops_->LoadSnapshot(name, /*and_resume=*/true),
                    StatusIs(absl::StatusCode::kInvalidArgument));
        EXPECT_THAT(vm_ops_->DeleteSnapshot(name), StatusIs(absl::StatusCode::kInvalidArgument));
        EXPECT_EQ(mock_snapshot_call_count(), 0);
    }
    EXPECT_FALSE(android::base::file::exists(temp_dir_.Path() / "escape"));
}

TEST_F(QemuVmOperationsTest, SnapshotsDisabledRejectsSaveAndLoadButAllowsListAndDelete) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    CreateVmOps(/*snapshots_enabled=*/false);
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);
    MakeCompatibleSnapshot("existing");

    mock_snapshot_calls_reset();
    EXPECT_THAT(vm_ops_->SaveSnapshot("new", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("Cannot save snapshot"), HasSubstr("-no-snapshot"))));
    EXPECT_THAT(vm_ops_->LoadSnapshot("existing", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         AllOf(HasSubstr("Cannot load snapshot"), HasSubstr("-no-snapshot"))));
    EXPECT_EQ(mock_snapshot_call_count(), 0);
    EXPECT_FALSE(store_->Exists("new"));
    // The VM was never stopped for the rejected load.
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::Running);

    std::array<QEMUSnapshotInfo, 1> qsi{};
    std::snprintf(qsi[0].name, sizeof(qsi[0].name), "existing");
    mock_snapshot_list_set(qsi.data(), static_cast<int>(qsi.size()));
    std::vector<VmOperations::SnapshotEntry> entries;
    EXPECT_OK(vm_ops_->ListSnapshots(
            [&](VmOperations::SnapshotEntry entry) { entries.push_back(std::move(entry)); }));
    EXPECT_EQ(entries.size(), 1U);

    EXPECT_OK(vm_ops_->DeleteSnapshot("existing"));
    EXPECT_FALSE(store_->Exists("existing"));
    EXPECT_EQ(mock_snapshot_call_count(), 2);

    // Invalid names are reported as such even when snapshots are disabled.
    EXPECT_THAT(vm_ops_->SaveSnapshot("../x", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(vm_ops_->LoadSnapshot("../x", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(QemuVmOperationsTest, SnapshotOperationsFailInConflictingStates) {
    MakeCompatibleSnapshot("snap");
    for (RunState state : {RUN_STATE_SHUTDOWN, RUN_STATE_SAVE_VM, RUN_STATE_RESTORE_VM,
                           RUN_STATE_INMIGRATE, RUN_STATE_FINISH_MIGRATE, RUN_STATE_COLO}) {
        mock_runstate_set(state);
        EXPECT_THAT(vm_ops_->SaveSnapshot("snap", /*overwrite=*/true),
                    StatusIs(absl::StatusCode::kFailedPrecondition));
        EXPECT_THAT(vm_ops_->LoadSnapshot("snap", /*and_resume=*/true),
                    StatusIs(absl::StatusCode::kFailedPrecondition));
        EXPECT_THAT(vm_ops_->DeleteSnapshot("snap"),
                    StatusIs(absl::StatusCode::kFailedPrecondition));
    }

    mock_runstate_set(RUN_STATE_SHUTDOWN);
    EXPECT_THAT(vm_ops_->SaveSnapshot("snap", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         HasSubstr("Launch a new emulator instance")));

    mock_runstate_set(RUN_STATE_SAVE_VM);
    EXPECT_THAT(vm_ops_->LoadSnapshot("snap", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         HasSubstr("Wait for the operation to complete")));
}

TEST_F(QemuVmOperationsTest, LoadSnapshotLeavesVmStoppedInRecoverableStateOnFailure) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    for (const char* name : {"corrupt_snap", "good_snap", "missing_snap"}) {
        MakeCompatibleSnapshot(name);
    }

    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(false);
    EXPECT_THAT(vm_ops_->LoadSnapshot("corrupt_snap", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kInternal));
    // Must not resume RUNNING on a potentially half-restored VM, nor stay stuck in RestoreVm.
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::PreLaunch);
    EXPECT_OK(vm_ops_->Reset());

    // Can also retry LoadSnapshot or resume from PreLaunch.
    mock_snapshot_op_ok_set(true);
    EXPECT_OK(vm_ops_->LoadSnapshot("good_snap", /*and_resume=*/true));
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::Running);

    // When already Paused before LoadSnapshot fails, remains Paused and recoverable.
    EXPECT_OK(vm_ops_->Pause());
    ASSERT_EQ(vm_ops_->GetRunState(), EmuRunState::Paused);
    mock_snapshot_op_ok_set(false);
    EXPECT_THAT(vm_ops_->LoadSnapshot("missing_snap", /*and_resume=*/true),
                StatusIs(absl::StatusCode::kInternal));
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::Paused);
    EXPECT_OK(vm_ops_->Resume());
    EXPECT_EQ(vm_ops_->GetRunState(), EmuRunState::Running);
}

TEST_F(QemuVmOperationsTest, DestructorUnregistersSnapshotProgressHandler) {
    ASSERT_TRUE(mock_has_snapshot_progress_handler());

    vm_ops_.reset();

    EXPECT_FALSE(mock_has_snapshot_progress_handler());
}

TEST_F(QemuVmOperationsTest, GetSnapshotProgressDoesNotTakeBqlAndHandlesNullStrings) {
    const auto idle = vm_ops_->GetSnapshotProgress();
    EXPECT_EQ(idle.operation, VmOperations::SnapshotOperationType::Idle);
    EXPECT_EQ(idle.phase, VmOperations::SnapshotPhase::None);
    EXPECT_EQ(idle.ProgressPercent(), 0);
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(test_vm_lock_->unlock_count_, 0);

    QemuSnapshotProgress qsave{};
    qsave.operation = QEMU_SNAPSHOT_OP_SAVE;
    qsave.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    qsave.name = nullptr;
    qsave.active_component = nullptr;
    qsave.error_message = nullptr;
    mock_fire_snapshot_progress(&qsave);

    const auto prep_prog = vm_ops_->GetSnapshotProgress();
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(prep_prog.operation, VmOperations::SnapshotOperationType::Save);
    EXPECT_EQ(prep_prog.phase, VmOperations::SnapshotPhase::Preparing);
    EXPECT_TRUE(prep_prog.snapshot_id.empty());
    EXPECT_TRUE(prep_prog.active_component.empty());
    EXPECT_TRUE(prep_prog.error_message.empty());
    EXPECT_EQ(prep_prog.ProgressPercent(), 0);

    qsave.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    qsave.name = "default_boot";
    qsave.active_component = "0000:00:02.0/virtio-gpu-rutabaga";
    qsave.transferred_bytes = 4LL * 1024 * 1024 * 1024;
    qsave.total_bytes = 8LL * 1024 * 1024 * 1024;
    qsave.disk_bytes = 1LL * 1024 * 1024 * 1024;
    qsave.completed_devices = 2;
    qsave.total_devices = 10;
    mock_fire_snapshot_progress(&qsave);

    const auto save_prog = vm_ops_->GetSnapshotProgress();
    EXPECT_EQ(test_vm_lock_->lock_count_, 0);
    EXPECT_EQ(save_prog.operation, VmOperations::SnapshotOperationType::Save);
    EXPECT_EQ(save_prog.phase, VmOperations::SnapshotPhase::Transferring);
    EXPECT_EQ(save_prog.snapshot_id, "default_boot");
    EXPECT_EQ(save_prog.active_component, "0000:00:02.0/virtio-gpu-rutabaga");
    EXPECT_EQ(save_prog.transferred_bytes, qsave.transferred_bytes);
    EXPECT_EQ(save_prog.total_bytes, qsave.total_bytes);
    EXPECT_EQ(save_prog.disk_bytes, qsave.disk_bytes);
    EXPECT_EQ(save_prog.completed_devices, 2);
    EXPECT_EQ(save_prog.total_devices, 10);
    // 0.85 * (4 / 8) + 0.15 * (2 / 10) = 0.425 + 0.03 = 45.5% -> 45.
    EXPECT_EQ(save_prog.ProgressPercent(), 45);

    // Null error_message on Failed populates operation and snapshot context.
    qsave.phase = QEMU_SNAPSHOT_PHASE_FAILED;
    qsave.error_message = nullptr;
    mock_fire_snapshot_progress(&qsave);

    const auto failed_prog = vm_ops_->GetSnapshotProgress();
    EXPECT_EQ(failed_prog.phase, VmOperations::SnapshotPhase::Failed);
    EXPECT_THAT(failed_prog.error_message,
                AllOf(HasSubstr("Cannot save snapshot 'default_boot'"),
                      HasSubstr("unspecified snapshot failure")));
}

TEST_F(QemuVmOperationsTest, SnapshotProgressPercentCoversWeightingAndBounds) {
    VmOperations::SnapshotProgress prog;
    prog.transferred_bytes = 100;
    prog.total_bytes = 100;
    EXPECT_EQ(prog.ProgressPercent(), 0);

    prog.operation = VmOperations::SnapshotOperationType::Save;
    prog.phase = VmOperations::SnapshotPhase::Preparing;
    EXPECT_EQ(prog.ProgressPercent(), 0);

    // Bytes only (total_devices == 0).
    prog.phase = VmOperations::SnapshotPhase::Transferring;
    prog.transferred_bytes = 75;
    prog.total_bytes = 100;
    prog.completed_devices = 0;
    prog.total_devices = 0;
    EXPECT_EQ(prog.ProgressPercent(), 75);

    // Devices only (total_bytes == 0).
    prog.transferred_bytes = 0;
    prog.total_bytes = 0;
    prog.completed_devices = 4;
    prog.total_devices = 10;
    EXPECT_EQ(prog.ProgressPercent(), 40);

    // Weighted bytes (85%) and devices (15%), clamped at 99% while Transferring.
    prog.transferred_bytes = 200;
    prog.total_bytes = 100;
    prog.completed_devices = 12;
    prog.total_devices = 10;
    EXPECT_EQ(prog.ProgressPercent(), VmOperations::SnapshotProgress::kMaxInProgressPercent);

    prog.phase = VmOperations::SnapshotPhase::Completed;
    EXPECT_EQ(prog.ProgressPercent(), VmOperations::SnapshotProgress::kCompletedProgressPercent);
}

TEST_F(QemuVmOperationsTest, SnapshotEventsAreDeliveredOnLoopAndFilteredAt250Ms) {
    std::vector<VmOperations::SnapshotProgress> events;
    bool delivered_on_loop = false;
    auto handle = android::base::eventing::MakeScopedCallback(
            vm_ops_->SnapshotEvents(), [&](const VmOperations::SnapshotProgress& prog) {
                delivered_on_loop = loop_->IsOnLoopThread();
                events.push_back(prog);
            });

    QemuSnapshotProgress qprog{};
    qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
    qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    qprog.name = "default_boot";
    qprog.total_bytes = 1000;
    mock_fire_snapshot_progress(&qprog);

    // First TRANSFERRING event (phase change from PREPARING -> TRANSFERRING) is always forwarded.
    clock_.AdvanceTime(absl::Milliseconds(10));
    qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    qprog.transferred_bytes = 100;
    mock_fire_snapshot_progress(&qprog);

    // Subsequent TRANSFERRING event within 250 ms is throttled out of SnapshotEvents(),
    // though GetSnapshotProgress() still reflects the latest counters immediately.
    clock_.AdvanceTime(absl::Milliseconds(100));
    qprog.transferred_bytes = 300;
    mock_fire_snapshot_progress(&qprog);
    EXPECT_EQ(vm_ops_->GetSnapshotProgress().transferred_bytes, 300);

    // After 250 ms from the last forwarded TRANSFERRING event, the next one is forwarded.
    clock_.AdvanceTime(absl::Milliseconds(150));
    qprog.transferred_bytes = 600;
    mock_fire_snapshot_progress(&qprog);

    // COMPLETED is always forwarded even if < 250 ms since the last TRANSFERRING event.
    clock_.AdvanceTime(absl::Milliseconds(20));
    qprog.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    qprog.transferred_bytes = 1000;
    mock_fire_snapshot_progress(&qprog);

    // Events are queued on the loop and not delivered inline on the BQL thread.
    EXPECT_THAT(events, IsEmpty());

    loop_->RunAll();
    EXPECT_TRUE(delivered_on_loop);
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].phase, VmOperations::SnapshotPhase::Preparing);
    EXPECT_EQ(events[1].phase, VmOperations::SnapshotPhase::Transferring);
    EXPECT_EQ(events[1].transferred_bytes, 100);
    EXPECT_EQ(events[2].phase, VmOperations::SnapshotPhase::Transferring);
    EXPECT_EQ(events[2].transferred_bytes, 600);
    EXPECT_EQ(events[3].phase, VmOperations::SnapshotPhase::Completed);
    EXPECT_EQ(events[3].ProgressPercent(), 100);

    // Next operation resets the 250 ms filter on PREPARING so its first TRANSFERRING
    // and FAILED events are immediately forwarded.
    events.clear();
    clock_.AdvanceTime(absl::Milliseconds(10));
    qprog.operation = QEMU_SNAPSHOT_OP_LOAD;
    qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    qprog.transferred_bytes = 0;
    mock_fire_snapshot_progress(&qprog);

    clock_.AdvanceTime(absl::Milliseconds(5));
    qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    qprog.transferred_bytes = 250;
    mock_fire_snapshot_progress(&qprog);

    clock_.AdvanceTime(absl::Milliseconds(5));
    qprog.phase = QEMU_SNAPSHOT_PHASE_FAILED;
    qprog.error_message = "Corrupt qcow2 vmstate";
    mock_fire_snapshot_progress(&qprog);

    loop_->RunAll();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].phase, VmOperations::SnapshotPhase::Preparing);
    EXPECT_EQ(events[1].phase, VmOperations::SnapshotPhase::Transferring);
    EXPECT_EQ(events[2].phase, VmOperations::SnapshotPhase::Failed);
    EXPECT_EQ(events[2].error_message, "Corrupt qcow2 vmstate");
}

TEST_F(QemuVmOperationsTest, TrailingFlushDeliversLatestThrottledTransferringUpdate) {
    std::vector<VmOperations::SnapshotProgress> events;
    auto handle = android::base::eventing::MakeScopedCallback(
            vm_ops_->SnapshotEvents(),
            [&](const VmOperations::SnapshotProgress& prog) { events.push_back(prog); });

    QemuSnapshotProgress qprog{};
    qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
    qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    qprog.name = "default_boot";
    qprog.total_bytes = 1000;
    qprog.total_devices = 10;
    mock_fire_snapshot_progress(&qprog);

    // First TRANSFERRING event at t = 10 ms is forwarded immediately.
    clock_.AdvanceTime(absl::Milliseconds(10));
    qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    qprog.active_component = "ram";
    qprog.transferred_bytes = 100;
    mock_fire_snapshot_progress(&qprog);

    // Two rapid updates within the 250 ms window (at t = 100 ms and t = 180 ms) are throttled.
    clock_.AdvanceTime(absl::Milliseconds(90));
    qprog.transferred_bytes = 500;
    mock_fire_snapshot_progress(&qprog);

    clock_.AdvanceTime(absl::Milliseconds(80));
    qprog.active_component = "0000:00:02.0/virtio-gpu-rutabaga";
    qprog.transferred_bytes = 1000;
    qprog.completed_devices = 8;
    mock_fire_snapshot_progress(&qprog);

    loop_->RunAll();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].phase, VmOperations::SnapshotPhase::Preparing);
    EXPECT_EQ(events[1].phase, VmOperations::SnapshotPhase::Transferring);
    EXPECT_EQ(events[1].transferred_bytes, 100);

    // The trailing flush was scheduled at t = 100 ms with a 160 ms delay (due at t = 260 ms).
    // Advancing 159 ms on the loop (and 79 ms on clock_ from t = 180 ms to t = 259 ms) does not
    // flush yet.
    clock_.AdvanceTime(absl::Milliseconds(79));
    loop_->AdvanceClock(absl::Milliseconds(159));
    loop_->RunAll();
    EXPECT_EQ(events.size(), 2U);

    // Reaching t = 260 ms (250 ms after the first forwarded Transferring event) flushes the
    // latest suppressed state (transferred_bytes = 1000, completed_devices = 8).
    clock_.AdvanceTime(absl::Milliseconds(1));
    loop_->AdvanceClock(absl::Milliseconds(1));
    loop_->RunAll();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[2].phase, VmOperations::SnapshotPhase::Transferring);
    EXPECT_EQ(events[2].active_component, "0000:00:02.0/virtio-gpu-rutabaga");
    EXPECT_EQ(events[2].transferred_bytes, 1000);
    EXPECT_EQ(events[2].completed_devices, 8);

    // A subsequent throttled update followed by COMPLETED before the timer fires must not emit a
    // stale TRANSFERRING event after COMPLETED.
    clock_.AdvanceTime(absl::Milliseconds(40));
    qprog.completed_devices = 9;
    mock_fire_snapshot_progress(&qprog);

    clock_.AdvanceTime(absl::Milliseconds(20));
    qprog.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    qprog.completed_devices = 10;
    mock_fire_snapshot_progress(&qprog);

    loop_->RunAll();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[3].phase, VmOperations::SnapshotPhase::Completed);

    clock_.AdvanceTime(absl::Milliseconds(300));
    loop_->AdvanceClock(absl::Milliseconds(300));
    loop_->RunAll();
    EXPECT_EQ(events.size(), 4U);
}

TEST_F(QemuVmOperationsTest, SnapshotWatchdogIsRegisteredOnlyDuringActiveSnapshotOperations) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    constexpr std::string_view kWatchdogName = "SnapshotWatchdog";
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    // Non-snapshot VM operations and metadata-only snapshot operations do not register the
    // watchdog.
    mock_runstate_set(RUN_STATE_RUNNING);
    ASSERT_OK(vm_ops_->Pause());
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
    ASSERT_OK(vm_ops_->Resume());
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
    mock_snapshot_list_set(/*entries=*/nullptr, /*count=*/0);
    ASSERT_OK(vm_ops_->ListSnapshots([](const VmOperations::SnapshotEntry& /*entry*/) {}));
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
    mock_snapshot_op_ok_set(true);
    ASSERT_OK(vm_ops_->DeleteSnapshot("snap_del"));
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    // Pre-flight and runstate rejections never arm the watchdog.
    EXPECT_THAT(vm_ops_->SaveSnapshot("../invalid", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
    EXPECT_THAT(vm_ops_->LoadSnapshot("missing_snap", /*and_resume=*/false),
                StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
    mock_runstate_set(RUN_STATE_SHUTDOWN);
    EXPECT_THAT(vm_ops_->SaveSnapshot("snap_shutdown", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
    MakeCompatibleSnapshot("snap_shutdown_load");
    EXPECT_THAT(vm_ops_->LoadSnapshot("snap_shutdown_load", /*and_resume=*/false),
                StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    // SaveSnapshot registers the watchdog while in flight (catching stalls even before QEMU emits
    // its first progress callback) and unregisters it on completion.
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);
    int save_hook_calls = 0;
    SetSnapshotOpHook([&](std::string_view name) {
        ++save_hook_calls;
        EXPECT_EQ(name, "snap_save_ok");
        ASSERT_TRUE(hang_detector_.HasPredicate(kWatchdogName));
        EXPECT_THAT(hang_detector_.EvaluatePredicate(kWatchdogName),
                    StatusIs(absl::StatusCode::kDeadlineExceeded,
                             AllOf(HasSubstr("Snapshot save 'snap_save_ok' stalled: no progress "
                                             "for 30s"),
                                   HasSubstr("phase=PREPARING"))));
    });
    ASSERT_OK(vm_ops_->SaveSnapshot("snap_save_ok", /*overwrite=*/true));
    EXPECT_EQ(save_hook_calls, 1);
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    // Failed SaveSnapshot also unregisters the watchdog cleanly.
    mock_snapshot_op_ok_set(false);
    int failed_save_hook_calls = 0;
    SetSnapshotOpHook([&](std::string_view /*name*/) {
        ++failed_save_hook_calls;
        EXPECT_TRUE(hang_detector_.HasPredicate(kWatchdogName));
    });
    EXPECT_THAT(vm_ops_->SaveSnapshot("snap_save_fail", /*overwrite=*/true),
                StatusIs(absl::StatusCode::kInternal));
    EXPECT_EQ(failed_save_hook_calls, 1);
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    // LoadSnapshot registers the watchdog while in flight (ignoring stale cached_progress_ from a
    // prior completed save before QEMU reports progress) and unregisters it on both success and
    // failure.
    mock_snapshot_op_ok_set(true);
    int load_hook_calls = 0;
    SetSnapshotOpHook([&](std::string_view name) {
        ++load_hook_calls;
        EXPECT_EQ(name, "snap_save_ok");
        ASSERT_TRUE(hang_detector_.HasPredicate(kWatchdogName));
        EXPECT_THAT(hang_detector_.EvaluatePredicate(kWatchdogName),
                    StatusIs(absl::StatusCode::kDeadlineExceeded,
                             AllOf(HasSubstr("Snapshot load 'snap_save_ok' stalled: no progress "
                                             "for 30s"),
                                   HasSubstr("phase=PREPARING"))));
    });
    ASSERT_OK(vm_ops_->LoadSnapshot("snap_save_ok", /*and_resume=*/true));
    EXPECT_EQ(load_hook_calls, 1);
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    mock_snapshot_op_ok_set(false);
    int failed_load_hook_calls = 0;
    SetSnapshotOpHook([&](std::string_view /*name*/) {
        ++failed_load_hook_calls;
        EXPECT_TRUE(hang_detector_.HasPredicate(kWatchdogName));
    });
    EXPECT_THAT(vm_ops_->LoadSnapshot("snap_save_ok", /*and_resume=*/false),
                StatusIs(absl::StatusCode::kInternal));
    EXPECT_EQ(failed_load_hook_calls, 1);
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
}

TEST_F(QemuVmOperationsTest, SnapshotWatchdogReturnsOkWhileProgressAdvances) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    constexpr std::string_view kWatchdogName = "SnapshotWatchdog";
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);

    int hook_calls = 0;
    SetSnapshotOpHook([&](std::string_view /*name*/) {
        ++hook_calls;
        ASSERT_TRUE(hang_detector_.HasPredicate(kWatchdogName));

        QemuSnapshotProgress qprog{};
        qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
        qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
        qprog.name = "snap_advancing";
        qprog.active_component = "prepare";
        qprog.total_bytes = 10000;
        qprog.total_devices = 10;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Phase transition PREPARING -> TRANSFERRING counts as progress.
        qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
        qprog.active_component = nullptr;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Advancing transferred_bytes counts as progress.
        for (int step = 1; step <= 4; ++step) {
            qprog.transferred_bytes = static_cast<int64_t>(step) * 1000;
            mock_fire_snapshot_progress(&qprog);
            EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));
        }

        // Advancing disk_bytes counts as progress even when transferred_bytes is unchanged.
        qprog.disk_bytes = 2048;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Advancing completed_devices counts as progress.
        qprog.completed_devices = 5;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Changing active_component counts as progress.
        qprog.active_component = "0000:00:02.0/virtio-gpu-rutabaga";
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Clearing active_component also counts as progress, even when no counter changes.
        qprog.active_component = nullptr;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Terminal COMPLETED and FAILED phases always return OkStatus before unregistration.
        qprog.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        qprog.phase = QEMU_SNAPSHOT_PHASE_FAILED;
        qprog.error_message = "I/O error";
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));
    });

    ASSERT_OK(vm_ops_->SaveSnapshot("snap_advancing", /*overwrite=*/true));
    EXPECT_EQ(hook_calls, 1);
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
}

TEST_F(QemuVmOperationsTest, SnapshotWatchdogReturnsDeadlineExceededWithDiagnosticContextOnStall) {
    ::goldfish::avd::FakeAvdUniverse fake_avd;
    constexpr std::string_view kWatchdogName = "SnapshotWatchdog";
    mock_runstate_set(RUN_STATE_RUNNING);
    mock_snapshot_op_ok_set(true);

    int hook_calls = 0;
    SetSnapshotOpHook([&](std::string_view /*name*/) {
        ++hook_calls;
        ASSERT_TRUE(hang_detector_.HasPredicate(kWatchdogName));

        QemuSnapshotProgress qprog{};
        qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
        qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
        qprog.name = "snap_stall_日本語_🚀";
        qprog.active_component = "0000:00:02.0/virtio-gpu-rutabaga";
        qprog.transferred_bytes = 1024;
        qprog.total_bytes = 4096;
        qprog.disk_bytes = 512;
        qprog.completed_devices = 3;
        qprog.total_devices = 8;
        mock_fire_snapshot_progress(&qprog);

        // First evaluation observes the new snapshot progress and returns OkStatus.
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

        // Repeating an identical progress callback does not advance any progress counters.
        mock_fire_snapshot_progress(&qprog);

        // Subsequent evaluation with no forward progress returns DeadlineExceededError with
        // diagnostic context.
        EXPECT_THAT(
                hang_detector_.EvaluatePredicate(kWatchdogName),
                StatusIs(absl::StatusCode::kDeadlineExceeded,
                         AllOf(HasSubstr("Snapshot save 'snap_stall_日本語_🚀' stalled: no "
                                         "progress for 30s"),
                               HasSubstr("phase=TRANSFERRING"), HasSubstr("progress=26%"),
                               HasSubstr("component='0000:00:02.0/virtio-gpu-rutabaga'"),
                               HasSubstr("transferred_bytes=1024/4096"),
                               HasSubstr("disk_bytes=512"), HasSubstr("completed_devices=3/8"))));

        // Once progress advances again, the predicate recovers and returns OkStatus.
        qprog.transferred_bytes = 2048;
        mock_fire_snapshot_progress(&qprog);
        EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));
    });

    ASSERT_OK(vm_ops_->SaveSnapshot("snap_stall_日本語_🚀", /*overwrite=*/true));
    EXPECT_EQ(hook_calls, 1);
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
}

TEST_F(QemuVmOperationsTest, SnapshotWatchdogCoversSnapshotsQemuRunsWithoutVmOperations) {
    // The launcher's exit-time -savevm and HMP savevm/loadvm call save_snapshot() and
    // load_snapshot() directly. Only QEMU's progress reports show that they are running.
    constexpr std::string_view kWatchdogName = "SnapshotWatchdog";
    ASSERT_OK(store_->Prepare("default_boot"));
    QemuSnapshotProgress qprog{};
    auto fire = [&] {
        return qemu_loop_->PostAndWait([&] { mock_fire_snapshot_progress(&qprog); });
    };

    qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
    qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    qprog.name = "default_boot";
    qprog.total_bytes = 4096;
    qprog.total_devices = 8;
    ASSERT_OK(fire());
    ASSERT_TRUE(hang_detector_.HasPredicate(kWatchdogName));
    // No progress since the save started.
    EXPECT_THAT(hang_detector_.EvaluatePredicate(kWatchdogName),
                StatusIs(absl::StatusCode::kDeadlineExceeded,
                         AllOf(HasSubstr("Snapshot save 'default_boot' stalled: no progress "
                                         "for 30s"),
                               HasSubstr("phase=PREPARING"))));

    qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    qprog.active_component = "0000:00:02.0/virtio-gpu-rutabaga";
    ASSERT_OK(fire());
    EXPECT_OK(hang_detector_.EvaluatePredicate(kWatchdogName));

    // A device save handler that never returns (b/574046502) reports no further progress.
    ASSERT_OK(fire());
    EXPECT_THAT(hang_detector_.EvaluatePredicate(kWatchdogName),
                StatusIs(absl::StatusCode::kDeadlineExceeded,
                         AllOf(HasSubstr("phase=TRANSFERRING"),
                               HasSubstr("component='0000:00:02.0/virtio-gpu-rutabaga'"))));

    qprog.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    qprog.active_component = nullptr;
    ASSERT_OK(fire());
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));

    // A load that QEMU runs on its own is watched the same way, and a failure ends it.
    qprog.operation = QEMU_SNAPSHOT_OP_LOAD;
    qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    ASSERT_OK(fire());
    EXPECT_TRUE(hang_detector_.HasPredicate(kWatchdogName));
    qprog.phase = QEMU_SNAPSHOT_PHASE_FAILED;
    qprog.error_message = "I/O error";
    ASSERT_OK(fire());
    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
}

TEST_F(QemuVmOperationsTest, DestructorUnregistersWatchdogOfSnapshotStillInFlight) {
    constexpr std::string_view kWatchdogName = "SnapshotWatchdog";
    QemuSnapshotProgress qprog{};
    qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
    qprog.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    qprog.name = "default_boot";
    ASSERT_OK(qemu_loop_->PostAndWait([&] { mock_fire_snapshot_progress(&qprog); }));
    ASSERT_TRUE(hang_detector_.HasPredicate(kWatchdogName));

    vm_ops_.reset();

    EXPECT_FALSE(hang_detector_.HasPredicate(kWatchdogName));
}

TEST_F(QemuVmOperationsDeathTest, SnapshotProgressOnTheLoopThreadDies) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto fire_progress = [] {
        QemuSnapshotProgress qprog{};
        qprog.operation = QEMU_SNAPSHOT_OP_SAVE;
        qprog.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
        qprog.name = "default_boot";
        mock_fire_snapshot_progress(&qprog);
    };
    EXPECT_DEBUG_DEATH(
            {
                loop_->Post(fire_progress).IgnoreError();
                loop_->RunAll();
            },
            "event loop thread");
}

}  // namespace goldfish
}  // namespace android
