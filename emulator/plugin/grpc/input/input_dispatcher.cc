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
#include "input_dispatcher.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/status/status_macros.h"
#include "touch_encoding.h"

namespace goldfish::grpc::v2 {

namespace {

using ::android::emulation::control::EvDevEvents;
using ::android::emulation::v2::InputActivity;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::KeyAction;
using ::android::emulation::v2::KeyEvent;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;

// The events an operation writes to one device. Palm cancellations populate
// both `events` (setting MT_TOOL_PALM while tracking IDs are still active) and
// `follow_up` (clearing tracking IDs to -1) so each arrives in its own
// SYN_REPORT frame.
struct DeviceReport {
    TouchDeviceId device;
    TouchDeviceSink* sink;
    EvDevEvents events;
    EvDevEvents follow_up;
};
using Reports = std::vector<DeviceReport>;

bool CreatesContact(PointerAction action) {
    return action == PointerAction::POINTER_ACTION_DOWN ||
           action == PointerAction::POINTER_ACTION_HOVER_ENTER ||
           action == PointerAction::POINTER_ACTION_HOVER_MOVE;
}

DeviceReport& ReportFor(Reports& reports, TouchDeviceId device) {
    auto it = std::ranges::find(reports, device, &DeviceReport::device);
    CHECK(it != reports.end()) << "Touch device " << device << " is not a target";
    return *it;
}

// Makes `device` a target of the operation. Fails if it can't take a report.
absl::Status AddTarget(TouchDevices& devices, TouchDeviceId device, Reports& reports) {
    if (std::ranges::find(reports, device, &DeviceReport::device) != reports.end()) {
        return absl::OkStatus();
    }
    TouchDeviceSink* sink = devices.Find(device);
    if (sink == nullptr || !sink->IsActive()) {
        return absl::UnavailableError(
                absl::StrCat("Touch device ", device, " is not active; the event was dropped."));
    }
    reports.push_back({device, sink, {}, {}});
    return absl::OkStatus();
}

template <typename FlushFn>
void WriteReports(const ::goldfish::async::EventLoop& loop, const Reports& reports,
                  absl::flat_hash_map<TouchDeviceId, uint64_t>& active_generations,
                  FlushFn&& flush_stale) {
    DCHECK(loop.IsOnLoopThread());
    for (const DeviceReport& report : reports) {
        if (!report.events.empty()) {
            flush_stale(report.device, *report.sink);
            report.sink->WriteReport(report.events);
            active_generations.try_emplace(report.device, report.sink->ActiveGeneration());
        }
        if (!report.follow_up.empty()) {
            report.sink->WriteReport(report.follow_up);
        }
    }
}

void Complete(InputDispatcher::DispatchCallback& done, InputDispatcher::DispatchResult result) {
    if (done) {
        std::move(done)(std::move(result));
    }
}

// Lifts the contact in `slot`; a CANCEL marks it as a palm in `report.events`
// and lifts it in `report.follow_up` so the guest cancels its gesture instead
// of firing a click or drop.
void AppendLift(PointerAction action, int slot, bool clear_stylus_keys, DeviceReport& report) {
    if (action == PointerAction::POINTER_ACTION_CANCEL) {
        AppendContactCancel(slot, report.events, report.follow_up, clear_stylus_keys);
    } else {
        AppendContactUp(slot, report.events, clear_stylus_keys);
    }
}

// A CANCEL from `source_id` on `display_id`, without pointers yet.
InputEvent CancelEvent(int32_t source_id, int32_t display_id) {
    InputEvent event;
    event.set_input_source_id(source_id);
    event.mutable_pointer()->set_display_id(display_id);
    event.mutable_pointer()->set_action(PointerAction::POINTER_ACTION_CANCEL);
    return event;
}

InputEvent KeyUpEvent(int32_t source_id, uint32_t code) {
    InputEvent up;
    up.set_input_source_id(source_id);
    up.mutable_key()->set_action(KeyAction::KEY_ACTION_UP);
    up.mutable_key()->set_evdev_code(static_cast<int32_t>(code));
    return up;
}

}  // namespace

InputDispatcher::InputDispatcher(::goldfish::async::EventLoop& loop, TouchDevices& devices,
                                 KeyboardSink* keyboard, KeyCodeResolver resolve_key_code,
                                 const ::goldfish::system::SteadyClock& clock,
                                 size_t max_pending_per_session)
        : loop_(loop)
        , devices_(devices)
        , resolve_key_code_(std::move(resolve_key_code))
        , clock_(clock)
        , max_pending_per_session_(max_pending_per_session)
        , active_gate_(std::make_shared<ActiveGate>(this))
        , timer_gate_(std::make_shared<TimerGate>(this))
        , keyboard_(keyboard)
        , delivery_thread_([this] { DeliverActivity(); }) {
    DCHECK_GT(max_pending_per_session_, 0U);
    devices_active_subscription_ =
            devices_.AddActiveChangedListener([this](TouchDeviceId) { OnDeviceActiveChanged(); });
}

InputDispatcher::~InputDispatcher() {
    Shutdown();
}

void InputDispatcher::Shutdown() {
    // Concurrent callers wait until the first one is done.
    absl::call_once(shutdown_once_, &InputDispatcher::ShutdownOnce, this);
}

void InputDispatcher::ShutdownOnce() {
    devices_active_subscription_.reset();
    {
        absl::MutexLock gate_lock(active_gate_->mutex);
        active_gate_->dispatcher = nullptr;
    }
    // First, so no idle timer touches the dispatcher from here on: one that is
    // running finishes before this returns, later ones find the gate closed.
    {
        absl::MutexLock gate_lock(timer_gate_->mutex);
        timer_gate_->dispatcher = nullptr;
    }
    absl::flat_hash_map<OperationId, PendingOperation> pending;
    {
        absl::MutexLock pending_lock(pending_mutex_);
        shut_down_ = true;
        pending.swap(pending_);
        in_flight_.clear();
    }
    for (auto& [id, operation] : pending) {
        Complete(operation.done,
                 {absl::UnavailableError(
                         "Cannot apply input event because the input dispatcher shut down before "
                         "the event ran; reconnect to a running emulator instance.")});
    }
    {
        absl::MutexLock outbox_lock(outbox_mutex_);
        stopping_ = true;
    }
    delivery_thread_.join();
}

bool InputDispatcher::IsShutDown() const {
    absl::MutexLock pending_lock(pending_mutex_);
    return shut_down_;
}

void InputDispatcher::OnDeviceActiveChanged() {
    if (IsShutDown()) {
        return;
    }
    loop_.Post([gate = active_gate_] {
             absl::MutexLock gate_lock(gate->mutex);
             if (gate->dispatcher != nullptr && !gate->dispatcher->IsShutDown()) {
                 gate->dispatcher->ResyncDevices();
             }
         }).IgnoreError();
}

InputSessionId InputDispatcher::OpenSession(SessionOptions options) {
    const InputSessionId session = next_session_.fetch_add(1, std::memory_order_relaxed);
    // If the loop is gone, later operations of this session fail to post too.
    PostUnlessShutDown(
            [this, session, options] { sessions_.emplace(session, Session{.options = options}); });
    return session;
}

void InputDispatcher::CloseSession(InputSessionId session) {
    PostUnlessShutDown([this, session] { ReleaseSession(session); });
}

void InputDispatcher::InstallKeyboardSink(KeyboardSink& keyboard) {
    PostUnlessShutDown([this, &keyboard] {
        DCHECK(keyboard_ == nullptr) << "The keyboard is already installed";
        keyboard_ = &keyboard;
    });
}

void InputDispatcher::OnSnapshotLoaded() {
    PostUnlessShutDown([this] { ResetAfterSnapshotLoad(); });
}

void InputDispatcher::OnDisplayAdded(TouchDeviceId device) {
    PostUnlessShutDown([this, device] {
        if (TouchDeviceSink* sink = devices_.Find(device)) {
            FlushStaleSlots(device, *sink);
        }
    });
}

void InputDispatcher::OnDisplayRemoved(TouchDeviceId device) {
    PostUnlessShutDown([this, device] {
        const ReleasedContacts released = contacts_.ClearDevice(device);
        const bool had_stylus_keys = stylus_contacts_.erase(device) > 0;
        if (auto gen_it = active_generations_.find(device); gen_it != active_generations_.end()) {
            StaleDeviceSlots& stale = stale_slots_[device];
            if (stale.generation != gen_it->second) {
                stale.generation = gen_it->second;
                stale.slots.reset();
                stale.has_stylus_keys = false;
            }
            for (const auto& [key, contact] : released) {
                stale.slots.set(static_cast<size_t>(contact.slot));
            }
            stale.has_stylus_keys = stale.has_stylus_keys || had_stylus_keys;
            if (stale.slots.none() && !stale.has_stylus_keys) {
                stale_slots_.erase(device);
            }
            active_generations_.erase(gen_it);
        }
        RecordDisplayResets(released);
        ReportCancels(released);
    });
}

void InputDispatcher::PostUnlessShutDown(absl::AnyInvocable<void() &&> task) {
    if (IsShutDown()) {
        return;
    }
    loop_.Post([this, task = std::move(task)]() mutable {
             if (!IsShutDown()) {
                 std::move(task)();
             }
         }).IgnoreError();
}

void InputDispatcher::DispatchPointer(InputSessionId session, PointerEvent event,
                                      DispatchCallback done) {
    PostOperation(
            session,
            [this, session, event = std::move(event)] { return ApplyPointer(session, event); },
            std::move(done));
}

void InputDispatcher::DispatchKey(InputSessionId session, KeyEvent event, DispatchCallback done) {
    PostOperation(
            session, [this, session, event = std::move(event)] { return ApplyKey(session, event); },
            std::move(done));
}

void InputDispatcher::DispatchText(InputSessionId session, std::string utf8,
                                   DispatchCallback done) {
    PostOperation(
            session, [this, session, utf8 = std::move(utf8)] { return ApplyText(session, utf8); },
            std::move(done));
}

void InputDispatcher::PostOperation(InputSessionId session,
                                    absl::AnyInvocable<absl::Status() &&> apply,
                                    DispatchCallback done) {
    absl::StatusOr<OperationId> id = AddPending(session, done);
    if (!id.ok()) {
        Complete(done, {id.status()});
        return;
    }
    // `done` stays in `pending_` until the task or Shutdown() claims it, so it
    // survives a rejected Post, which destroys the task.
    absl::Status posted = loop_.Post([this, session, id = *id, apply = std::move(apply)]() mutable {
        if (std::optional<DispatchCallback> claimed = ClaimPending(id)) {
            reported_at_.reset();
            absl::Status status = std::move(apply)();
            RefreshIdleTimer(session);
            Complete(*claimed, {std::move(status), reported_at_});
        }
    });
    if (!posted.ok()) {
        if (std::optional<DispatchCallback> claimed = ClaimPending(*id)) {
            Complete(*claimed, {std::move(posted)});
        }
    }
}

absl::StatusOr<InputDispatcher::OperationId> InputDispatcher::AddPending(InputSessionId session,
                                                                         DispatchCallback& done) {
    absl::MutexLock pending_lock(pending_mutex_);
    if (shut_down_) {
        return absl::UnavailableError("The input dispatcher is shut down; the event was dropped.");
    }
    size_t& in_flight = in_flight_[session];
    if (in_flight >= max_pending_per_session_) {
        absl::Status exhausted = absl::ResourceExhaustedError(absl::StrCat(
                "Input session ", session, " has ", in_flight,
                " events waiting for the emulator to apply them; the event was dropped."));
        VLOG(1) << exhausted;
        return exhausted;
    }
    ++in_flight;
    const OperationId id = next_operation_++;
    pending_.emplace(id, PendingOperation{session, std::move(done)});
    return id;
}

std::optional<InputDispatcher::DispatchCallback> InputDispatcher::ClaimPending(OperationId id) {
    absl::MutexLock pending_lock(pending_mutex_);
    auto operation = pending_.extract(id);
    if (operation.empty()) {
        return std::nullopt;
    }
    auto in_flight = in_flight_.find(operation.mapped().session);
    DCHECK(in_flight != in_flight_.end());
    if (--in_flight->second == 0) {
        in_flight_.erase(in_flight);
    }
    return std::move(operation.mapped().done);
}

absl::StatusOr<InputDispatcher::SessionOptions> InputDispatcher::FindSession(
        InputSessionId session) const {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) {
        return absl::FailedPreconditionError(
                absl::StrCat("Input session ", session, " is not open."));
    }
    return it->second.options;
}

absl::Status InputDispatcher::ApplyPointer(InputSessionId session, const PointerEvent& event) {
    ASSIGN_OR_RETURN(SessionOptions options, FindSession(session));
    ResyncDevices();
    const PointerAction action = event.action();
    if (action == PointerAction::POINTER_ACTION_RELATIVE_MOVE ||
        action == PointerAction::POINTER_ACTION_SCROLL) {
        return absl::OkStatus();
    }
    if (event.pointers().empty() && (action == PointerAction::POINTER_ACTION_CANCEL ||
                                     action == PointerAction::POINTER_ACTION_HOVER_EXIT)) {
        return CancelOnDisplay(session, options.source_id, event.display_id(), action);
    }

    // Find every target device before changing anything, so a rejected event
    // leaves no trace. Pointers of one event share the action, so an event
    // either creates contacts or releases them, never both.
    const bool reject_held =
            action == PointerAction::POINTER_ACTION_DOWN && options.reject_down_on_held;
    Reports reports;
    std::optional<TouchDeviceId> resolved;
    for (const PointerEvent::Pointer& pointer : event.pointers()) {
        const ContactKey key{session, event.display_id(), pointer.pointer_id()};
        if (std::optional<Contact> held = contacts_.Find(key)) {
            if (!reject_held) {
                RETURN_IF_ERROR(AddTarget(devices_, held->device, reports));
            }
        } else if (CreatesContact(action)) {
            if (!resolved) {
                ASSIGN_OR_RETURN(resolved, devices_.Resolve(event.display_id()));
            }
            RETURN_IF_ERROR(AddTarget(devices_, *resolved, reports));
        }
    }

    // The event as reported: only the pointers written to a device.
    InputEvent applied;
    applied.set_input_source_id(options.source_id);
    PointerEvent& reported = *applied.mutable_pointer();
    reported = event;
    reported.clear_pointers();
    absl::Status result;
    for (const PointerEvent::Pointer& pointer : event.pointers()) {
        const ContactKey key{session, event.display_id(), pointer.pointer_id()};
        if (std::optional<Contact> held = contacts_.Find(key)) {
            if (reject_held) {
                result.Update(absl::FailedPreconditionError(absl::StrCat(
                        "Cannot apply POINTER_ACTION_DOWN because Pointer ", pointer.pointer_id(),
                        " on display ", event.display_id(), " is already held by session ", session,
                        "; send POINTER_ACTION_UP or POINTER_ACTION_CANCEL before pressing it "
                        "again.")));
                continue;
            }
            DeviceReport& report = ReportFor(reports, held->device);
            if (IsContactReleased(action, pointer)) {
                contacts_.Release(key);
                AppendLift(action, held->slot, EraseStylusContact(held->device, key), report);
            } else {
                RecordContactDownOrMove(held->device, key, pointer, action, held->slot,
                                        /*new_contact=*/false, report.events);
            }
            *reported.add_pointers() = pointer;
        } else if (CreatesContact(action)) {
            absl::StatusOr<int> slot = contacts_.Acquire(key, *resolved, options.human);
            if (!slot.ok()) {
                result.Update(slot.status());
                continue;
            }
            RecordContactDownOrMove(*resolved, key, pointer, action, *slot, /*new_contact=*/true,
                                    ReportFor(reports, *resolved).events);
            *reported.add_pointers() = pointer;
        }
    }
    WriteReports(loop_, reports, active_generations_,
                 [this](TouchDeviceId d, TouchDeviceSink& s) { FlushStaleSlots(d, s); });
    if (!reported.pointers().empty()) {
        Report(std::move(applied));
    }
    return result;
}

absl::Status InputDispatcher::CancelOnDisplay(InputSessionId session, int32_t source_id,
                                              int32_t display_id, PointerAction action) {
    const ReleasedContacts held = contacts_.FindSessionOn(session, display_id);
    if (held.empty()) {
        ASSIGN_OR_RETURN(TouchDeviceId device, devices_.Resolve(display_id));
        Reports unused;
        return AddTarget(devices_, device, unused);
    }
    Reports reports;
    for (const auto& [key, contact] : held) {
        RETURN_IF_ERROR(AddTarget(devices_, contact.device, reports));
    }
    InputEvent cancel = CancelEvent(source_id, display_id);
    for (const auto& [key, contact] : contacts_.ReleaseSessionOn(session, display_id)) {
        AppendLift(action, contact.slot, EraseStylusContact(contact.device, key),
                   ReportFor(reports, contact.device));
        cancel.mutable_pointer()->add_pointers()->set_pointer_id(key.pointer_id);
    }
    WriteReports(loop_, reports, active_generations_,
                 [this](TouchDeviceId d, TouchDeviceSink& s) { FlushStaleSlots(d, s); });
    Report(std::move(cancel));
    return absl::OkStatus();
}

void InputDispatcher::RecordContactDownOrMove(TouchDeviceId device, const ContactKey& key,
                                              const PointerEvent::Pointer& pointer,
                                              PointerAction action, int slot, bool new_contact,
                                              ::android::emulation::control::EvDevEvents& events) {
    if (IsStylusOrEraser(pointer.tool_type())) {
        stylus_contacts_[device].insert(key);
    } else if (!new_contact && EraseStylusContact(device, key)) {
        AppendStylusKeysReset(events);
    }
    AppendContactDownOrMove(pointer, action, slot, new_contact, events);
}

bool InputDispatcher::EraseStylusContact(TouchDeviceId device, const ContactKey& key) {
    auto it = stylus_contacts_.find(device);
    if (it == stylus_contacts_.end() || it->second.erase(key) == 0) {
        return false;
    }
    if (it->second.empty()) {
        stylus_contacts_.erase(it);
        return true;
    }
    return false;
}

absl::Status InputDispatcher::ApplyKey(InputSessionId session, const KeyEvent& event) {
    ASSIGN_OR_RETURN(SessionOptions options, FindSession(session));
    const uint32_t code = resolve_key_code_(event);
    if (code == 0) {
        return absl::InvalidArgumentError("The key has no evdev code; the event was dropped.");
    }
    RETURN_IF_ERROR(CheckKeyboard());
    switch (event.action()) {
    case KeyAction::KEY_ACTION_DOWN:
        HoldKey(session, code);
        break;
    case KeyAction::KEY_ACTION_UP:
        if (!ReleaseKey(session, code)) {
            return absl::OkStatus();
        }
        break;
    default:
        HoldKey(session, code);
        ReleaseKey(session, code);
        break;
    }
    InputEvent applied;
    applied.set_input_source_id(options.source_id);
    *applied.mutable_key() = event;
    Report(std::move(applied));
    return absl::OkStatus();
}

void InputDispatcher::HoldKey(InputSessionId session, uint32_t code) {
    key_holders_[code].insert(session);
    keyboard_->WriteKey(code, /*down=*/true);
}

bool InputDispatcher::ReleaseKey(InputSessionId session, uint32_t code) {
    auto holders = key_holders_.find(code);
    if (holders == key_holders_.end() || holders->second.erase(session) == 0) {
        return false;
    }
    if (holders->second.empty()) {
        key_holders_.erase(holders);
        keyboard_->WriteKey(code, /*down=*/false);
    }
    return true;
}

absl::Status InputDispatcher::ApplyText(InputSessionId session, std::string_view utf8) {
    ASSIGN_OR_RETURN(SessionOptions options, FindSession(session));
    if (utf8.empty()) {
        return absl::OkStatus();
    }
    RETURN_IF_ERROR(CheckKeyboard());
    keyboard_->TypeText(utf8);
    InputEvent typed;
    typed.set_input_source_id(options.source_id);
    typed.mutable_text()->set_text(std::string(utf8));
    Report(std::move(typed));
    return absl::OkStatus();
}

absl::Status InputDispatcher::CheckKeyboard() const {
    if (keyboard_ == nullptr) {
        return absl::UnavailableError(
                "Cannot dispatch keyboard input because the guest keyboard is not installed yet; "
                "wait for VM display initialization to complete and retry.");
    }
    return absl::OkStatus();
}

bool InputDispatcher::SessionHasHeldInput(InputSessionId session) const {
    if (contacts_.HasSession(session)) {
        return true;
    }
    return std::ranges::any_of(
            key_holders_, [session](const auto& entry) { return entry.second.contains(session); });
}

void InputDispatcher::RefreshIdleTimer(InputSessionId session) {
    auto it = sessions_.find(session);
    if (it == sessions_.end() || it->second.options.idle_timeout <= absl::ZeroDuration()) {
        return;
    }
    Session& state = it->second;
    const uint64_t generation = ++state.idle_generation;
    if (!SessionHasHeldInput(session)) {
        return;
    }
    if (state.idle_timer == nullptr) {
        state.idle_timer = loop_.CreateTimer([gate = timer_gate_, session] {
            absl::MutexLock gate_lock(gate->mutex);
            if (gate->dispatcher != nullptr) {
                gate->dispatcher->OnIdleTimer(session);
            }
            return true;  // Keeps the timer armable; Schedule() re-arms it.
        });
    }
    state.idle_timer->Schedule(state.options.idle_timeout);
    // Schedule() arms the timer from a loop task of its own, so record the
    // generation it fires for in the same order. A firing for the previous
    // deadline that lands in between then reads the previous generation and
    // is stale, rather than releasing input the operation just held.
    loop_.Post([gate = timer_gate_, session, generation] {
             absl::MutexLock gate_lock(gate->mutex);
             if (gate->dispatcher != nullptr) {
                 gate->dispatcher->ArmIdleGeneration(session, generation);
             }
         }).IgnoreError();
}

void InputDispatcher::ArmIdleGeneration(InputSessionId session, uint64_t generation) {
    DCHECK(loop_.IsOnLoopThread());
    if (auto it = sessions_.find(session); it != sessions_.end()) {
        it->second.armed_generation = generation;
    }
}

void InputDispatcher::OnIdleTimer(InputSessionId session) {
    DCHECK(loop_.IsOnLoopThread());
    auto it = sessions_.find(session);
    if (it == sessions_.end() || it->second.idle_generation != it->second.armed_generation) {
        return;  // Closed, or an operation ran since the timer was armed.
    }
    const std::shared_ptr<SessionResetTracker> tracker = it->second.options.reset_tracker;
    ReleaseSessionInput(session);
    if (tracker != nullptr) {
        tracker->RecordFullReset();
    }
}

void InputDispatcher::ReleaseSessionInput(InputSessionId session) {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) {
        return;  // Only open sessions hold contacts or keys.
    }
    const int32_t source_id = it->second.options.source_id;
    ResyncDevices();
    // Contacts on inactive devices are forgotten without a report.
    Reports reports;
    ReleasedContacts written;
    for (const auto& [key, contact] : contacts_.ReleaseSession(session)) {
        const bool clear_stylus_keys = EraseStylusContact(contact.device, key);
        if (!AddTarget(devices_, contact.device, reports).ok()) {
            continue;
        }
        DeviceReport& report = ReportFor(reports, contact.device);
        AppendContactCancel(contact.slot, report.events, report.follow_up, clear_stylus_keys);
        written.emplace_back(key, contact);
    }
    WriteReports(loop_, reports, active_generations_,
                 [this](TouchDeviceId d, TouchDeviceSink& s) { FlushStaleSlots(d, s); });
    ReportCancels(written);

    std::vector<uint32_t> held;
    for (const auto& [code, holders] : key_holders_) {
        if (holders.contains(session)) {
            held.push_back(code);
        }
    }
    std::ranges::sort(held);
    for (uint32_t code : held) {
        ReleaseKey(session, code);
        Report(KeyUpEvent(source_id, code), /*synthesized=*/true);
    }
}

void InputDispatcher::ReleaseSession(InputSessionId session) {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) {
        return;  // Only open sessions hold contacts or keys.
    }
    // On the loop, so this cancels in place rather than posting and waiting.
    it->second.idle_timer.reset();
    ReleaseSessionInput(session);
    sessions_.erase(session);
}

void InputDispatcher::ResetAfterSnapshotLoad() {
    DCHECK(loop_.IsOnLoopThread());
    stale_slots_.clear();
    stylus_contacts_.clear();
    EvDevEvents palm_all;
    EvDevEvents lift_all;
    for (int slot = 0; slot < kTouchDeviceSlots; ++slot) {
        AppendContactCancel(slot, palm_all, lift_all);
    }
    AppendStylusKeysReset(lift_all);
    for (TouchDeviceId device : devices_.Devices()) {
        TouchDeviceSink* sink = devices_.Find(device);
        if (sink != nullptr && sink->IsActive()) {
            sink->WriteReport(palm_all);
            sink->WriteReport(lift_all);
            active_generations_[device] = sink->ActiveGeneration();
        } else {
            active_generations_.erase(device);
        }
    }
    ReportCancels(contacts_.Clear());
    for (const auto& [session, state] : sessions_) {
        if (state.options.reset_tracker != nullptr) {
            state.options.reset_tracker->RecordFullReset();
        }
    }

    struct HeldKey {
        uint32_t code;
        std::vector<InputSessionId> sessions;
    };
    std::vector<HeldKey> held_keys;
    held_keys.reserve(key_holders_.size());
    for (const auto& [code, holders] : key_holders_) {
        std::vector<InputSessionId> sessions(holders.begin(), holders.end());
        std::ranges::sort(sessions);
        held_keys.push_back({code, std::move(sessions)});
    }
    key_holders_.clear();
    std::ranges::sort(held_keys, {}, &HeldKey::code);
    // The sink's Reset() releases the held keys in the guest, paced with the
    // ones the snapshot restored; writing them here first could overflow the
    // guest's input queue ahead of that sweep.
    for (const HeldKey& held : held_keys) {
        for (InputSessionId session : held.sessions) {
            Report(KeyUpEvent(sessions_.at(session).options.source_id, held.code),
                   /*synthesized=*/true);
        }
    }
    if (keyboard_ != nullptr) {
        keyboard_->Reset();
    }
}

void InputDispatcher::FlushStaleSlots(TouchDeviceId device, TouchDeviceSink& sink) {
    auto it = stale_slots_.find(device);
    if (it == stale_slots_.end()) {
        return;
    }
    if (sink.ActiveGeneration() != it->second.generation) {
        stale_slots_.erase(it);
        return;
    }
    if (!sink.IsActive()) {
        return;
    }
    EvDevEvents palm;
    EvDevEvents lift;
    const bool reset_stylus_keys = it->second.has_stylus_keys;
    for (int slot = 0; slot < kTouchDeviceSlots; ++slot) {
        if (it->second.slots.test(static_cast<size_t>(slot))) {
            AppendContactCancel(slot, palm, lift);
        }
    }
    if (reset_stylus_keys) {
        AppendStylusKeysReset(lift);
    }
    stale_slots_.erase(it);
    if (!palm.empty() || !lift.empty()) {
        if (!palm.empty()) {
            sink.WriteReport(palm);
        }
        sink.WriteReport(lift);
        active_generations_[device] = sink.ActiveGeneration();
    }
}

void InputDispatcher::ResyncDevices() {
    for (auto& [device, generation] : active_generations_) {
        const TouchDeviceSink* sink = devices_.Find(device);
        if (sink == nullptr || sink->ActiveGeneration() == generation) {
            continue;
        }
        generation = sink->ActiveGeneration();
        stylus_contacts_.erase(device);
        const ReleasedContacts released = contacts_.ClearDevice(device);
        RecordDisplayResets(released);
        ReportCancels(released);
    }
}

void InputDispatcher::RecordDisplayResets(const ReleasedContacts& released) {
    absl::flat_hash_set<std::pair<InputSessionId, int32_t>> seen;
    for (const auto& [key, contact] : released) {
        if (!seen.emplace(key.session, key.display_id).second) {
            continue;
        }
        if (auto it = sessions_.find(key.session);
            it != sessions_.end() && it->second.options.reset_tracker != nullptr) {
            it->second.options.reset_tracker->RecordDisplayReset(key.display_id);
        }
    }
}

void InputDispatcher::ReportCancels(const ReleasedContacts& released) {
    // Activity reports the requested display, not the device it resolved to.
    std::vector<std::pair<InputSessionId, InputEvent>> cancels;
    for (const auto& [key, contact] : released) {
        auto cancel = std::ranges::find_if(cancels, [&](const auto& session_cancel) {
            return session_cancel.first == key.session &&
                   session_cancel.second.pointer().display_id() == key.display_id;
        });
        if (cancel == cancels.end()) {
            const int32_t source_id = sessions_.at(key.session).options.source_id;
            cancel = cancels.emplace(cancels.end(), key.session,
                                     CancelEvent(source_id, key.display_id));
        }
        cancel->second.mutable_pointer()->add_pointers()->set_pointer_id(key.pointer_id);
    }
    for (auto& [session, cancel] : cancels) {
        Report(std::move(cancel), /*synthesized=*/true);
    }
}

void InputDispatcher::Report(InputEvent event, bool synthesized) {
    DCHECK(loop_.IsOnLoopThread());
    const absl::Duration now = clock_.Now();
    if (!synthesized) {
        reported_at_ = now;
    }
    InputActivity activity;
    *activity.mutable_event() = std::move(event);
    activity.set_dispatched_at_nanos(absl::ToInt64Nanoseconds(now));
    activity.set_synthesized(synthesized);
    absl::MutexLock outbox_lock(outbox_mutex_);
    outbox_.push_back({next_sequence_++, std::move(activity)});
}

void InputDispatcher::DeliverActivity() {
    uint64_t next_delivery = 0;
    while (true) {
        std::deque<SequencedActivity> batch;
        {
            absl::MutexLock outbox_lock(outbox_mutex_);
            outbox_mutex_.Await(absl::Condition(this, &InputDispatcher::HasActivityOrStopping));
            if (outbox_.empty()) {
                return;  // Stopping, and everything queued was delivered.
            }
            batch.swap(outbox_);
        }
        for (const SequencedActivity& queued : batch) {
            DCHECK_EQ(queued.sequence, next_delivery);
            next_delivery = queued.sequence + 1;
            activity_events_.FireEvent(queued.activity);
        }
    }
}

bool InputDispatcher::HasActivityOrStopping() const {
    return !outbox_.empty() || stopping_;
}

}  // namespace goldfish::grpc::v2
