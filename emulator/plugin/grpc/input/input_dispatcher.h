// Copyright (C) 2026 The Android Open Source Project
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

#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "absl/base/call_once.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/base/clock.h"
#include "contact_table.h"
#include "goldfish/async/event_loop.h"
#include "input_dispatcher_interface.h"
#include "input_service.pb.h"
#include "keyboard_sink.h"
#include "touch_device.h"

namespace goldfish::grpc::v2 {

/**
 * @brief The `IInputDispatcher` of the emulator: an actor on the QEMU event loop.
 *
 * Sessions post operations, which the loop applies in the order they were
 * posted. The loop stamps and sequences each activity into an outbox; a
 * delivery thread fires them, so observer work stays off the loop.
 *
 * ### Invariants:
 * - The `ContactTable` matches the contacts written to each active device,
 *   except briefly between a device active-generation transition and the
 *   queued `ResyncDevices()` task (or pre-operation check) that clears them.
 * - Each device receives at most one report per operation (or two when palm
 *   cancelling contacts), written whole.
 * - A key is in `key_holders_`, with a non-empty set of open sessions, iff the
 *   last write of that key to the keyboard was a press.
 * - `outbox_` holds the undelivered activities in sequence order, ending just
 *   before `next_sequence_`.
 * - Each operation in `pending_` is posted, or being posted, and hasn't run,
 *   and its `done` is called once: by its loop task or by `Shutdown()`,
 *   whichever removes it from `pending_`. `in_flight_` counts each session's
 *   operations in `pending_`, never more than the cap, and holds no zero.
 *
 * ### Thread Safety:
 * Public methods are thread-safe and post to the loop; all other state is
 * touched only on the loop, except the pending operations, guarded by
 * `pending_mutex_`, and the outbox, guarded by `outbox_mutex_`. Lock order:
 * both are leaves; nothing holds both, and nothing holds either while calling
 * `done`, writing to a sink, or firing activity.
 *
 * ### Ownership:
 * `loop`, `devices`, `keyboard`, and `clock` must outlive `Shutdown()` and any
 * in-flight loop tasks; once `Shutdown()` returns and the loop has stopped (or
 * drained all posted tasks), the dispatcher never accesses them, so clients
 * may safely hold a stopped dispatcher past its owner. Destruction calls
 * `Shutdown()`.
 */
class InputDispatcher final : public IInputDispatcher {
  public:
    /**
     * @brief Maps a key event to its Linux evdev code, or 0 if it has none.
     * Called only on the loop.
     */
    using KeyCodeResolver =
            absl::AnyInvocable<uint32_t(const ::android::emulation::v2::KeyEvent&) const>;

    static constexpr size_t kDefaultMaxPendingPerSession = 256;

    /**
     * @brief Constructs an `InputDispatcher` running on `loop`.
     * @param keyboard Receives keys and text; null until `InstallKeyboardSink()`.
     * @param clock Stamps `dispatched_at_nanos` from its realtime clock.
     * @param max_pending_per_session The operations a session may have posted
     *     but not yet run; `Dispatch*()` rejects more. Bounds the loop's queue
     *     while it is stalled, for example by a snapshot save.
     */
    InputDispatcher(::goldfish::async::EventLoop& loop, TouchDevices& devices,
                    KeyboardSink* keyboard, KeyCodeResolver resolve_key_code,
                    ::android::base::IClock* clock = &::android::base::IClock::Get(),
                    size_t max_pending_per_session = kDefaultMaxPendingPerSession);
    /** @brief Calls `Shutdown()`. */
    ~InputDispatcher() override;

    InputDispatcher(const InputDispatcher&) = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;

    /**
     * @brief Installs the keyboard, for an owner that builds the dispatcher
     * before the guest keyboard exists.
     *
     * Takes effect on the loop, after the operations posted before it; until
     * then keys and text complete with `UNAVAILABLE`.
     *
     * @param keyboard Must outlive the dispatcher. Install it only if the
     *     dispatcher was built without one, and only once.
     */
    void InstallKeyboardSink(KeyboardSink& keyboard);

    /**
     * @brief Releases all touch slots on every active device, resets the
     * keyboard (which releases the keys it holds), clears held contacts and
     * keys, and reports their synthesized `CANCEL` and `KEY_ACTION_UP`
     * activities.
     *
     * Call once the VM is running again after a snapshot load, so contacts or
     * keys restored from the snapshot or held by sessions are released.
     *
     * Applied on the loop. After `Shutdown()`, posts nothing.
     */
    void OnSnapshotLoaded();

    /**
     * @brief Cancels any slots left down on `device`'s guest touch device from
     * a prior `OnDisplayRemoved()` if its driver has not reset since. Call when
     * a display is added.
     *
     * Applied on the loop. After `Shutdown()`, posts nothing.
     */
    void OnDisplayAdded(TouchDeviceId device);

    /**
     * @brief Clears all contacts on `device` without writing to it, and reports
     * their synthesized `CANCEL` activities. Call when a display is removed.
     *
     * Slots held on `device` are remembered and cancelled on the guest if
     * `device` becomes active again without a driver reset.
     *
     * Applied on the loop. After `Shutdown()`, posts nothing.
     */
    void OnDisplayRemoved(TouchDeviceId device);

    /** @brief Callbacks run on the delivery thread. */
    ActivityEventSource& activity() override { return activity_events_; }

    /**
     * @brief For when the loop may never run what is posted. Joins the delivery
     * thread. A pending operation the loop runs later applies nothing and isn't
     * completed again.
     *
     * Also callable after the loop stopped; it touches no loop-only state, and
     * doesn't wait for an operation that is already running.
     */
    void Shutdown() override ABSL_LOCKS_EXCLUDED(pending_mutex_, outbox_mutex_);

    /** @brief After `Shutdown()`, posts nothing. */
    InputSessionId OpenSession(SessionOptions options) override;

    /** @brief Applied on the loop. After `Shutdown()`, posts nothing. */
    void CloseSession(InputSessionId session) override;

    using IInputDispatcher::DispatchKey;
    using IInputDispatcher::DispatchPointer;
    using IInputDispatcher::DispatchText;

    /**
     * @brief Applied on the loop.
     *
     * @param done Runs once: on the loop with the result; on the calling thread
     *     with `RESOURCE_EXHAUSTED` if `session` already has
     *     `max_pending_per_session` operations pending, with `UNAVAILABLE`
     *     after `Shutdown()`, or with the posting error if the loop rejects the
     *     operation; or with `UNAVAILABLE` on the thread calling `Shutdown()`
     *     if the operation is still pending then.
     */
    void DispatchPointer(InputSessionId session, ::android::emulation::v2::PointerEvent event,
                         DispatchCallback done) override;

    /**
     * @brief Applied on the loop, with the key the resolver maps `event` to.
     * @param done As for `DispatchPointer()`.
     */
    void DispatchKey(InputSessionId session, ::android::emulation::v2::KeyEvent event,
                     DispatchCallback done) override;

    /**
     * @brief Applied on the loop.
     * @param done As for `DispatchPointer()`.
     */
    void DispatchText(InputSessionId session, std::string utf8, DispatchCallback done) override;

  private:
    struct SequencedActivity {
        uint64_t sequence;
        ::android::emulation::v2::InputActivity activity;
    };
    using OperationId = uint64_t;
    struct PendingOperation {
        InputSessionId session;
        DispatchCallback done;
    };
    // Shared with active-change resync tasks posted from TouchDevices push
    // callbacks so tasks that run after Shutdown() or ~InputDispatcher() are
    // safe no-ops.
    struct ActiveGate {
        explicit ActiveGate(InputDispatcher* owner) : dispatcher(owner) {}
        absl::Mutex mutex;
        InputDispatcher* dispatcher ABSL_GUARDED_BY(mutex);
    };
    struct StaleDeviceSlots {
        uint64_t generation = 0;
        std::bitset<kTouchDeviceSlots> slots;
    };

    void ShutdownOnce() ABSL_LOCKS_EXCLUDED(pending_mutex_, outbox_mutex_);
    void OnDeviceActiveChanged() ABSL_LOCKS_EXCLUDED(pending_mutex_);
    bool IsShutDown() const ABSL_LOCKS_EXCLUDED(pending_mutex_);
    // Posts `task` unless shut down. The task does nothing if it runs after
    // Shutdown().
    void PostUnlessShutDown(absl::AnyInvocable<void() &&> task) ABSL_LOCKS_EXCLUDED(pending_mutex_);
    // Posts `apply` for `session` and completes `done` with its result.
    void PostOperation(InputSessionId session, absl::AnyInvocable<absl::Status() &&> apply,
                       DispatchCallback done) ABSL_LOCKS_EXCLUDED(pending_mutex_);
    // Adds `done` to the pending operations of `session`. @return Its ID, or
    // why the operation can't be posted, leaving `done` unchanged.
    absl::StatusOr<OperationId> AddPending(InputSessionId session, DispatchCallback& done)
            ABSL_LOCKS_EXCLUDED(pending_mutex_);
    // Removes operation `id` from the pending ones. @return Its `done`, or
    // nothing if Shutdown() already completed it.
    std::optional<DispatchCallback> ClaimPending(OperationId id)
            ABSL_LOCKS_EXCLUDED(pending_mutex_);
    absl::StatusOr<SessionOptions> FindSession(InputSessionId session) const;
    absl::Status ApplyPointer(InputSessionId session,
                              const ::android::emulation::v2::PointerEvent& event);
    absl::Status ApplyKey(InputSessionId session, const ::android::emulation::v2::KeyEvent& event);
    void HoldKey(InputSessionId session, uint32_t code);
    // @return Whether `session` held the key.
    bool ReleaseKey(InputSessionId session, uint32_t code);
    absl::Status ApplyText(InputSessionId session, std::string_view utf8);
    // @return UNAVAILABLE if no keyboard is installed.
    absl::Status CheckKeyboard() const;
    // Releases the contacts of `session` on the device `display_id` resolves
    // to, lifting them as `action` (CANCEL or HOVER_EXIT) does.
    absl::Status CancelOnDisplay(InputSessionId session, int32_t source_id, int32_t display_id,
                                 ::android::emulation::v2::PointerAction action);
    void ReleaseSession(InputSessionId session);
    void ResetAfterSnapshotLoad();
    // Cancels slots left down on `device` by OnDisplayRemoved() if `sink` is
    // still on the same driver generation.
    void FlushStaleSlots(TouchDeviceId device, TouchDeviceSink& sink);
    // Clears the contacts of each device whose guest driver went active or
    // inactive since the dispatcher recorded its generation, as the guest
    // forgot them, and reports their synthesized CANCELs. Writes nothing.
    void ResyncDevices();
    // Reports a synthesized CANCEL per session and requested display of
    // `released`, whose sessions must be open.
    void ReportCancels(const ReleasedContacts& released) ABSL_LOCKS_EXCLUDED(outbox_mutex_);

    // Stamps `event` and queues it for delivery. Called on the loop right
    // after the write it reports; records the stamp of an unsynthesized
    // activity in `reported_at_`.
    void Report(::android::emulation::v2::InputEvent event, bool synthesized = false)
            ABSL_LOCKS_EXCLUDED(outbox_mutex_);
    // The delivery thread: fires the outbox in order until stopped and empty.
    void DeliverActivity() ABSL_LOCKS_EXCLUDED(outbox_mutex_);
    bool HasActivityOrStopping() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(outbox_mutex_);

    ::goldfish::async::EventLoop& loop_;
    TouchDevices& devices_;
    const KeyCodeResolver resolve_key_code_;
    ::android::base::IClock* const clock_;
    const size_t max_pending_per_session_;
    std::atomic<InputSessionId> next_session_{1};
    absl::once_flag shutdown_once_;
    const std::shared_ptr<ActiveGate> active_gate_;
    ScopedActiveChangedListener devices_active_subscription_;

    mutable absl::Mutex pending_mutex_;
    bool shut_down_ ABSL_GUARDED_BY(pending_mutex_) = false;
    OperationId next_operation_ ABSL_GUARDED_BY(pending_mutex_) = 0;
    absl::flat_hash_map<OperationId, PendingOperation> pending_ ABSL_GUARDED_BY(pending_mutex_);
    absl::flat_hash_map<InputSessionId, size_t> in_flight_ ABSL_GUARDED_BY(pending_mutex_);

    // Loop-only state.
    KeyboardSink* keyboard_;
    ContactTable contacts_;
    absl::flat_hash_map<InputSessionId, SessionOptions> sessions_;
    // The sessions holding each pressed evdev code.
    absl::flat_hash_map<uint32_t, absl::flat_hash_set<InputSessionId>> key_holders_;
    // The active generation of each device written to, as of the first write
    // or the last resync; see ResyncDevices().
    absl::flat_hash_map<TouchDeviceId, uint64_t> active_generations_;
    // Slots held on a removed display's permanent virtio-input device, to be
    // cancelled if the display is recreated without a guest driver reset.
    absl::flat_hash_map<TouchDeviceId, StaleDeviceSlots> stale_slots_;
    // When the activity of the running Dispatch*() operation, which reports
    // at most one, was stamped; unset until it reports it.
    std::optional<absl::Time> reported_at_;

    ActivityEventSource activity_events_;
    absl::Mutex outbox_mutex_;
    std::deque<SequencedActivity> outbox_ ABSL_GUARDED_BY(outbox_mutex_);
    uint64_t next_sequence_ ABSL_GUARDED_BY(outbox_mutex_) = 0;
    bool stopping_ ABSL_GUARDED_BY(outbox_mutex_) = false;
    // Last, so it starts once the state it uses is built.
    std::thread delivery_thread_;
};

}  // namespace goldfish::grpc::v2
