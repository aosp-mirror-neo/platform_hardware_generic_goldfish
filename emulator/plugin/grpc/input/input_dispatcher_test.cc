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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "absl/types/span.h"

#include "android/base/clock.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "keyboard_sink.h"
#include "standard-headers/linux/input-event-codes.h"
#include "standard-headers/linux/input.h"

namespace goldfish::grpc::v2 {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::android::base::ClockType;
using ::android::emulation::control::EvDevEvent;
using ::android::emulation::v2::InputActivity;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::KeyAction;
using ::android::emulation::v2::KeyEvent;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::goldfish::async::EventLoop;
using ::goldfish::async::testing::TestEventLoop;
using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;
using DispatchResult = InputDispatcher::DispatchResult;

constexpr TouchDeviceId kInner = 0;
constexpr TouchDeviceId kOuter = 1;
// ABS_MT_TRACKING_ID -1: the slot's contact lifted.
constexpr auto kTrackingIdUp = static_cast<uint32_t>(-1);

// Converts a normalized coordinate to the evdev value the guest receives.
constexpr uint32_t Abs(float v) {
    return static_cast<uint32_t>(v * 32767.0F);
}

struct GuestContact {
    int slot;
    int32_t tracking_id;
    uint32_t x;
    uint32_t y;
    uint32_t tool_type = 0;
};

auto ContactAt(float x, float y) {
    return ::testing::AllOf(Field(&GuestContact::x, Abs(x)), Field(&GuestContact::y, Abs(y)));
}

// Minimal model of Android's MultiTouchMotionAccumulator over Linux multi-touch
// protocol B: ABS_MT_SLOT selects a slot, later ABS_MT_* values update it, and
// ABS_MT_TRACKING_ID < 0 immediately clears the slot before SYN_REPORT syncs
// TouchInputMapper. Each WriteReport() call is one SYN_REPORT frame.
class GuestTouchState {
  public:
    void ApplyReport(absl::Span<const EvDevEvent> events) {
        for (const EvDevEvent& event : events) {
            ASSERT_EQ(event.type, EV_ABS) << "the sink, not the dispatcher, appends SYN_REPORT";
            if (event.code == ABS_MT_SLOT) {
                ASSERT_LT(event.value, slots_.size());
                selected_ = static_cast<int>(event.value);
                continue;
            }
            Slot& slot = slots_[selected_];
            if (event.code == ABS_MT_TRACKING_ID) {
                const int32_t id = static_cast<int32_t>(event.value);
                if (id < 0) {
                    slot = Slot{};
                } else {
                    slot.tracking_id = id;
                }
            } else if (event.code == ABS_MT_POSITION_X) {
                slot.x = event.value;
            } else if (event.code == ABS_MT_POSITION_Y) {
                slot.y = event.value;
            } else if (event.code == ABS_MT_TOOL_TYPE) {
                slot.tool_type = event.value;
            }
        }
    }

    std::vector<GuestContact> Active() const {
        std::vector<GuestContact> active;
        for (int i = 0; i < kTouchDeviceSlots; ++i) {
            if (slots_[i].tracking_id != -1) {
                active.push_back(
                        {i, slots_[i].tracking_id, slots_[i].x, slots_[i].y, slots_[i].tool_type});
            }
        }
        return active;
    }

  private:
    struct Slot {
        int32_t tracking_id = -1;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t tool_type = 0;
    };
    int selected_ = 0;
    std::array<Slot, kTouchDeviceSlots> slots_{};
};

// Every write to a touch or keyboard sink, in the order it happened, with
// whether it ran on the loop thread.
struct SinkLog {
    void Record(const EventLoop& loop, std::string write) {
        writes.push_back(std::move(write));
        on_loop_thread.push_back(loop.IsOnLoopThread());
    }

    std::vector<std::string> writes;
    std::vector<bool> on_loop_thread;
};

class FakeSink : public TouchDeviceSink {
  public:
    FakeSink(const EventLoop& loop, std::string name, SinkLog& log)
            : loop_(loop), name_(std::move(name)), log_(log) {}

    bool IsActive() const override { return active; }
    uint64_t ActiveGeneration() const override { return active_generation; }
    void WriteReport(absl::Span<const EvDevEvent> events) override {
        reports.emplace_back(events.begin(), events.end());
        on_loop_thread.push_back(loop_.IsOnLoopThread());
        log_.Record(loop_, absl::StrCat("touch ", name_));
        guest.ApplyReport(events);
        frames.push_back(guest.Active());
    }

    bool active = true;
    // Bump to simulate a reset or re-probe of the guest driver.
    uint64_t active_generation = 0;
    std::vector<std::vector<EvDevEvent>> reports;
    std::vector<bool> on_loop_thread;
    GuestTouchState guest;
    // The guest's active contacts after each report.
    std::vector<std::vector<GuestContact>> frames;

  private:
    const EventLoop& loop_;
    std::string name_;
    SinkLog& log_;
};

class FakeTouchDevices : public TouchDevices {
  public:
    FakeTouchDevices(const EventLoop& loop, SinkLog& log)
            : inner(loop, "inner", log), outer(loop, "outer", log) {}

    absl::StatusOr<TouchDeviceId> Resolve(int32_t display_id) override {
        if (!resolve_status.ok()) {
            return resolve_status;
        }
        auto it = remap.find(display_id);
        return it == remap.end() ? static_cast<TouchDeviceId>(display_id) : it->second;
    }

    TouchDeviceSink* Find(TouchDeviceId device) override {
        if (device == kInner) {
            return &inner;
        }
        if (device == kOuter) {
            return &outer;
        }
        auto it = extra.find(device);
        return it == extra.end() ? nullptr : it->second;
    }

    std::vector<TouchDeviceId> Devices() override { return {kInner, kOuter}; }

    // Simulates a guest driver reset or re-probe on `device`: advances its
    // active generation and fires the push notification.
    void ResetDriver(TouchDeviceId device) {
        if (FakeSink* sink = device == kInner ? &inner : (device == kOuter ? &outer : nullptr)) {
            ++sink->active_generation;
            NotifyActiveChanged(device);
        }
    }

    FakeSink inner;
    FakeSink outer;
    absl::flat_hash_map<TouchDeviceId, TouchDeviceSink*> extra;
    absl::flat_hash_map<int32_t, TouchDeviceId> remap;
    absl::Status resolve_status;
};

// Logs each key as "down <code>" or "up <code>", and text as "text <utf8>".
class FakeKeyboard : public KeyboardSink {
  public:
    FakeKeyboard(const EventLoop& loop, SinkLog& log) : loop_(loop), log_(log) {}

    void WriteKey(uint32_t evdev_code, bool down) override {
        log_.Record(loop_, absl::StrCat(down ? "down " : "up ", evdev_code));
    }
    void TypeText(std::string_view utf8) override {
        log_.Record(loop_, absl::StrCat("text ", utf8));
    }
    void Reset() override { log_.Record(loop_, "reset"); }

  private:
    const EventLoop& loop_;
    SinkLog& log_;
};

// Maps only `evdev_code`; every other key is unmapped.
uint32_t ResolveEvdevOnly(const KeyEvent& event) {
    return event.key_identifier_case() == KeyEvent::kEvdevCode
                   ? static_cast<uint32_t>(event.evdev_code())
                   : 0;
}

KeyEvent Key(KeyAction action, uint32_t evdev_code) {
    KeyEvent event;
    event.set_action(action);
    event.set_evdev_code(static_cast<int32_t>(evdev_code));
    return event;
}

constexpr KeyAction kKeyDown = KeyAction::KEY_ACTION_DOWN;
constexpr KeyAction kKeyUp = KeyAction::KEY_ACTION_UP;
constexpr KeyAction kKeyPress = KeyAction::KEY_ACTION_PRESS;

struct P {
    int32_t id;
    float x = 0.5F;
    float y = 0.5F;
    std::optional<float> pressure;
};

PointerEvent Touch(PointerAction action, std::vector<P> pointers, int32_t display_id = 0) {
    PointerEvent event;
    event.set_display_id(display_id);
    event.set_action(action);
    for (const P& p : pointers) {
        auto* pointer = event.add_pointers();
        pointer->set_pointer_id(p.id);
        pointer->set_x(p.x);
        pointer->set_y(p.y);
        if (p.pressure) {
            pointer->set_pressure(*p.pressure);
        }
    }
    return event;
}

constexpr PointerAction kDown = PointerAction::POINTER_ACTION_DOWN;
constexpr PointerAction kMove = PointerAction::POINTER_ACTION_MOVE;
constexpr PointerAction kUp = PointerAction::POINTER_ACTION_UP;

// Advances one microsecond on every read, so each activity gets a distinct
// timestamp.
class TickingClock : public ::android::base::IClock {
  public:
    absl::Time Now(ClockType type) const override {
        EXPECT_EQ(type, ClockType::kRealtime);
        return absl::UnixEpoch() + absl::Microseconds(ticks_.fetch_add(1) + 1);
    }

  private:
    mutable std::atomic<int64_t> ticks_{0};
};

// The text the tests type to mark the end of the activities they expect.
constexpr std::string_view kSentinel = "<sentinel>";

bool IsSentinel(const InputActivity& activity) {
    return activity.event().has_text() && activity.event().text().text() == kSentinel;
}

struct ObservedActivity {
    InputActivity activity;
    std::thread::id thread;
    bool on_loop_thread;
};

// Records each activity a subscriber receives and the thread it arrived on.
class ActivityRecorder {
  public:
    explicit ActivityRecorder(const EventLoop& loop) : loop_(loop) {}

    void Record(const InputActivity& activity) {
        absl::MutexLock observed_lock(observed_mutex_);
        observed_.push_back({activity, std::this_thread::get_id(), loop_.IsOnLoopThread()});
    }

    // Waits for the sentinel. @return The activities before it, which are
    // forgotten together with the sentinel.
    std::vector<ObservedActivity> TakeUntilSentinel() {
        absl::MutexLock observed_lock(observed_mutex_);
        observed_mutex_.Await(absl::Condition(this, &ActivityRecorder::HasSentinel));
        auto sentinel = std::ranges::find_if(observed_, [](const ObservedActivity& observed) {
            return IsSentinel(observed.activity);
        });
        std::vector<ObservedActivity> before(std::make_move_iterator(observed_.begin()),
                                             std::make_move_iterator(sentinel));
        observed_.erase(observed_.begin(), sentinel + 1);
        return before;
    }

    // @return Everything recorded so far, without waiting.
    std::vector<ObservedActivity> Observed() const {
        absl::MutexLock observed_lock(observed_mutex_);
        return observed_;
    }

  private:
    bool HasSentinel() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(observed_mutex_) {
        return std::ranges::any_of(observed_, [](const ObservedActivity& observed) {
            return IsSentinel(observed.activity);
        });
    }

    const EventLoop& loop_;
    mutable absl::Mutex observed_mutex_;
    std::vector<ObservedActivity> observed_ ABSL_GUARDED_BY(observed_mutex_);
};

// Describes an activity as "src<id> [synthesized ]<event>".
std::string Describe(const InputActivity& activity) {
    const InputEvent& event = activity.event();
    std::string text = absl::StrCat("src", event.input_source_id(),
                                    activity.synthesized() ? " synthesized " : " ");
    switch (event.event_case()) {
    case InputEvent::kPointer:
        absl::StrAppend(&text, PointerAction_Name(event.pointer().action()), " display ",
                        event.pointer().display_id(), " ids");
        for (const PointerEvent::Pointer& pointer : event.pointer().pointers()) {
            absl::StrAppend(&text, " ", pointer.pointer_id());
        }
        break;
    case InputEvent::kKey:
        absl::StrAppend(&text, KeyAction_Name(event.key().action()), " ", event.key().evdev_code());
        break;
    case InputEvent::kText:
        absl::StrAppend(&text, "text ", event.text().text());
        break;
    case InputEvent::kAxis:
    case InputEvent::EVENT_NOT_SET:
        absl::StrAppend(&text, "unexpected event");
        break;
    }
    return text;
}

class InputDispatcherTest : public ::testing::Test {
  protected:
    InputDispatcherTest() {
        // Never removed: the dispatcher's event source drops it on destruction.
        dispatcher_.activity().AddCallback(
                [this](const InputActivity& activity) { recorder_.Record(activity); });
    }

    // Dispatches `event` and runs the loop. @return The completion status.
    absl::Status Dispatch(InputSessionId session, PointerEvent event) {
        std::optional<absl::Status> result;
        dispatcher_.DispatchPointer(session, std::move(event), [&result](DispatchResult done) {
            result = std::move(done.status);
        });
        loop_->RunAll();
        if (!result) {
            return absl::InternalError("done was not called");
        }
        return *result;
    }

    absl::Status Dispatch(InputSessionId session, KeyEvent event) {
        std::optional<absl::Status> result;
        dispatcher_.DispatchKey(session, std::move(event), [&result](DispatchResult done) {
            result = std::move(done.status);
        });
        loop_->RunAll();
        return result.value_or(absl::InternalError("done was not called"));
    }

    absl::Status Type(InputSessionId session, std::string text) {
        std::optional<absl::Status> result;
        dispatcher_.DispatchText(session, std::move(text), [&result](DispatchResult done) {
            result = std::move(done.status);
        });
        loop_->RunAll();
        return result.value_or(absl::InternalError("done was not called"));
    }

    InputSessionId Agent() { return dispatcher_.OpenSession({}); }
    InputSessionId Human() { return dispatcher_.OpenSession({.human = true}); }
    InputSessionId Source(int32_t source_id) {
        return dispatcher_.OpenSession({.source_id = source_id});
    }

    // Runs everything posted so far, then waits until its activity is
    // delivered. @return The activities delivered since the last call.
    std::vector<ObservedActivity> TakeActivities() {
        const InputSessionId sentinel = dispatcher_.OpenSession({});
        dispatcher_.DispatchText(sentinel, std::string(kSentinel));
        dispatcher_.CloseSession(sentinel);
        loop_->RunAll();
        return recorder_.TakeUntilSentinel();
    }

    // As TakeActivities(), but described.
    std::vector<std::string> Activities() {
        std::vector<std::string> described;
        for (const ObservedActivity& observed : TakeActivities()) {
            described.push_back(Describe(observed.activity));
        }
        return described;
    }

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    SinkLog log_;
    FakeTouchDevices devices_{*loop_, log_};
    FakeSink& inner_ = devices_.inner;
    FakeSink& outer_ = devices_.outer;
    FakeKeyboard keyboard_{*loop_, log_};
    TickingClock clock_;
    ActivityRecorder recorder_{*loop_};
    InputDispatcher dispatcher_{*loop_, devices_, &keyboard_, ResolveEvdevOnly, &clock_};
};

TEST_F(InputDispatcherTest, SessionIdsStartAtOneAndAreUnique) {
    EXPECT_EQ(Agent(), 1);
    EXPECT_EQ(Human(), 2);
    EXPECT_EQ(Agent(), 3);
}

TEST_F(InputDispatcherTest, SessionsSharingPointerIdGetDistinctContacts) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0, 0.75F, 0.75F}})), IsOk());

    const auto active = inner_.guest.Active();
    EXPECT_THAT(active, UnorderedElementsAre(ContactAt(0.25F, 0.25F), ContactAt(0.75F, 0.75F)));
    ASSERT_THAT(active, SizeIs(2));
    EXPECT_NE(active[0].slot, active[1].slot);
    EXPECT_NE(active[0].tracking_id, active[1].tracking_id);
}

TEST_F(InputDispatcherTest, UpReleasesOnlyThatSessionsContact) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0, 0.75F, 0.75F}})), IsOk());

    ASSERT_THAT(Dispatch(a, Touch(kUp, {{0, 0.25F, 0.25F}})), IsOk());

    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
}

TEST_F(InputDispatcherTest, ContactStaysOnBoundDeviceWhenDisplayRemaps) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());

    devices_.remap[0] = kOuter;  // The foldable unfolds mid-gesture.
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{0, 0.5F, 0.5F}})), IsOk());
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.5F, 0.5F)));
    ASSERT_THAT(Dispatch(a, Touch(kUp, {{0, 0.5F, 0.5F}})), IsOk());

    EXPECT_THAT(inner_.reports, SizeIs(3));
    EXPECT_THAT(inner_.guest.Active(), IsEmpty());
    EXPECT_THAT(outer_.reports, IsEmpty());

    // A new contact goes to the newly resolved device.
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.75F, 0.75F}})), IsOk());
    EXPECT_THAT(outer_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
}

TEST_F(InputDispatcherTest, AgentsExhaustNonReservedSlotsButHumanStillGetsOne) {
    for (int i = 0; i < kTouchDeviceSlots - kReservedHumanSlots; ++i) {
        ASSERT_THAT(Dispatch(Agent(), Touch(kDown, {{0}})), IsOk());
    }
    const size_t reports = inner_.reports.size();

    EXPECT_THAT(Dispatch(Agent(), Touch(kDown, {{0}})),
                StatusIs(absl::StatusCode::kResourceExhausted));
    EXPECT_THAT(inner_.reports, SizeIs(reports));

    EXPECT_THAT(Dispatch(Human(), Touch(kDown, {{0}})), IsOk());
    EXPECT_THAT(inner_.guest.Active(), SizeIs(kTouchDeviceSlots - kReservedHumanSlots + 1));
}

TEST_F(InputDispatcherTest, ExhaustedPointerIsSkippedWhileOthersApply) {
    const InputSessionId a = Agent();
    std::vector<P> pointers;
    for (int i = 0; i <= kTouchDeviceSlots - kReservedHumanSlots; ++i) {
        pointers.push_back({i});
    }
    EXPECT_THAT(Dispatch(a, Touch(kDown, pointers)),
                StatusIs(absl::StatusCode::kResourceExhausted));
    EXPECT_THAT(inner_.reports, SizeIs(1));
    EXPECT_THAT(inner_.guest.Active(), SizeIs(kTouchDeviceSlots - kReservedHumanSlots));
}

TEST_F(InputDispatcherTest, InactiveDeviceRejectsEventAndKeepsState) {
    const InputSessionId a = Agent();
    inner_.active = false;
    EXPECT_THAT(Dispatch(a, Touch(kDown, {{0}})), StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_THAT(inner_.reports, IsEmpty());

    inner_.active = true;
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    EXPECT_THAT(inner_.reports.back(), Contains(Field(&EvDevEvent::code, ABS_MT_TRACKING_ID)));

    // The contact survives an UP the device couldn't take.
    inner_.active = false;
    EXPECT_THAT(Dispatch(a, Touch(kUp, {{0}})), StatusIs(absl::StatusCode::kUnavailable));
    inner_.active = true;
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{0, 0.5F, 0.5F}})), IsOk());
    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.5F, 0.5F)));
}

TEST_F(InputDispatcherTest, DriverResetPushEagerlyCancelsContactsWithoutSubsequentInput) {
    const InputSessionId a = Source(1);
    const InputSessionId b = Source(2);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{2, 0.25F, 0.25F}}, 1)), IsOk());
    Activities();
    const size_t written = inner_.reports.size();

    // The guest driver of the inner device resets while fingers are down and no
    // new client pointer RPC arrives: the push notification immediately posts a
    // resync task onto the loop.
    devices_.ResetDriver(kInner);
    loop_->RunAll();

    EXPECT_THAT(inner_.reports, SizeIs(written));
    EXPECT_THAT(Activities(),
                ElementsAre("src1 synthesized POINTER_ACTION_CANCEL display 0 ids 0 1",
                            "src2 synthesized POINTER_ACTION_CANCEL display 0 ids 0"));

    // Subsequent moves of the cleared pointer are ignored without duplicate
    // cancels; a new DOWN gets a fresh slot; the outer contact is intact.
    std::optional<DispatchResult> moved;
    dispatcher_.DispatchPointer(a, Touch(kMove, {{0, 0.5F, 0.5F}}),
                                [&moved](DispatchResult done) { moved = std::move(done); });
    loop_->RunAll();
    ASSERT_TRUE(moved.has_value());
    EXPECT_THAT(moved->status, IsOk());
    EXPECT_EQ(moved->dispatched_at, std::nullopt);
    EXPECT_THAT(inner_.reports, SizeIs(written));
    EXPECT_THAT(Activities(), IsEmpty());

    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    ASSERT_THAT(inner_.reports.back(), SizeIs(::testing::Ge(2)));
    EXPECT_EQ(inner_.reports.back()[0], (EvDevEvent{EV_ABS, ABS_MT_SLOT, 0}));
    EXPECT_EQ(inner_.reports.back()[1], (EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, 0}));
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{2, 0.75F, 0.75F}})), IsOk());
    EXPECT_THAT(outer_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
}

TEST_F(InputDispatcherTest, DriverResetBeforeQueuedOperationsResyncsOnceAndPreservesNewDown) {
    const InputSessionId a = dispatcher_.OpenSession({.source_id = 1, .reject_down_on_held = true});
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.1F, 0.1F}})), IsOk());
    Activities();
    const size_t written = inner_.reports.size();

    // Queue a MOVE and a fresh DOWN on the loop before the vCPU push posts its
    // resync task behind them: ApplyPointer()'s pre-operation ResyncDevices()
    // fences the generation change ahead of MOVE, lets DOWN acquire slot 0 on
    // the new generation, and makes the trailing push task a no-op.
    dispatcher_.DispatchPointer(a, Touch(kMove, {{0, 0.5F, 0.5F}}));
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0, 0.8F, 0.8F}}));
    devices_.ResetDriver(kInner);
    loop_->RunAll();

    EXPECT_THAT(inner_.reports, SizeIs(written + 1));
    EXPECT_THAT(Activities(), ElementsAre("src1 synthesized POINTER_ACTION_CANCEL display 0 ids 0",
                                          "src1 POINTER_ACTION_DOWN display 0 ids 0"));
}

TEST_F(InputDispatcherTest, CloseSessionAfterDriverResetWritesNoRelease) {
    const InputSessionId a = Source(1);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    Activities();
    const size_t written = inner_.reports.size();

    // Queue CloseSession ahead of the push resync task so CloseSession's
    // pre-operation ResyncDevices() runs first.
    dispatcher_.CloseSession(a);
    devices_.ResetDriver(kInner);

    EXPECT_THAT(Activities(),
                ElementsAre("src1 synthesized POINTER_ACTION_CANCEL display 0 ids 0"));
    EXPECT_THAT(inner_.reports, SizeIs(written));
}

TEST_F(InputDispatcherTest, DriverResetPushSafeWhenDispatcherDestroyedBeforeLoopDrains) {
    auto dispatcher = std::make_unique<InputDispatcher>(*loop_, devices_, &keyboard_,
                                                        ResolveEvdevOnly, &clock_);
    const InputSessionId a = dispatcher->OpenSession({.source_id = 1});
    dispatcher->DispatchPointer(a, Touch(kDown, {{0}}));
    loop_->RunAll();

    // Queue an active-change resync task on `loop_`, then destroy `dispatcher`
    // before `loop_->RunAll()` drains it.
    devices_.ResetDriver(kInner);
    dispatcher.reset();
    loop_->RunAll();
}

TEST_F(InputDispatcherTest, MissingDeviceIsUnavailable) {
    EXPECT_THAT(Dispatch(Agent(), Touch(kDown, {{0}}, /*display_id=*/7)),
                StatusIs(absl::StatusCode::kUnavailable));
}

TEST_F(InputDispatcherTest, ResolveFailureChangesNothing) {
    const InputSessionId a = Agent();
    devices_.resolve_status = absl::NotFoundError("no display 3");
    EXPECT_THAT(Dispatch(a, Touch(kDown, {{0}}, 3)), StatusIs(absl::StatusCode::kNotFound));
    devices_.resolve_status = absl::OkStatus();
    EXPECT_THAT(Dispatch(a, Touch(kMove, {{0}})), IsOk());
    EXPECT_THAT(inner_.reports, IsEmpty());
}

TEST_F(InputDispatcherTest, MoveAndUpForUnheldPointerAreIgnored) {
    const InputSessionId a = Agent();
    EXPECT_THAT(Dispatch(a, Touch(kMove, {{0}})), IsOk());
    EXPECT_THAT(Dispatch(a, Touch(kUp, {{0}})), IsOk());
    EXPECT_THAT(inner_.reports, IsEmpty());
}

TEST_F(InputDispatcherTest, DownForHeldPointerMovesIt) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.5F, 0.5F}})), IsOk());

    ASSERT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(inner_.reports[1], Not(Contains(Field(&EvDevEvent::code, ABS_MT_TRACKING_ID))));
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.5F, 0.5F)));
}

TEST_F(InputDispatcherTest, DownForHeldPointerIsRejectedWhileOthersApply) {
    const InputSessionId a = dispatcher_.OpenSession({.source_id = 1, .reject_down_on_held = true});
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    Activities();

    const absl::Status status = Dispatch(a, Touch(kDown, {{0, 0.5F, 0.5F}, {1, 0.75F, 0.75F}}));

    EXPECT_THAT(status, StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("Pointer 0")));
    ASSERT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(inner_.reports[1], Not(Contains(EvDevEvent{EV_ABS, ABS_MT_SLOT, 0})));
    EXPECT_THAT(inner_.guest.Active(),
                ElementsAre(ContactAt(0.25F, 0.25F), ContactAt(0.75F, 0.75F)));
    EXPECT_THAT(Activities(), ElementsAre("src1 POINTER_ACTION_DOWN display 0 ids 1"));

    // The contact is intact: it still moves and lifts.
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{0, 0.3F, 0.3F}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kUp, {{0}})), IsOk());
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
}

TEST_F(InputDispatcherTest, RelativeMoveAndScrollChangeNothing) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());

    EXPECT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_RELATIVE_MOVE, {{0, 0.5F, 0.5F}})),
                IsOk());
    EXPECT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_SCROLL, {{0, 0.5F, 0.5F, 0.0F}})),
                IsOk());
    EXPECT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_SCROLL, {{1}})), IsOk());

    EXPECT_THAT(inner_.reports, SizeIs(1));
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.25F, 0.25F)));
}

TEST_F(InputDispatcherTest, HoverCreatesContactAndHoverExitReleasesIt) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_ENTER, {{0}})), IsOk());
    EXPECT_THAT(inner_.guest.Active(), SizeIs(1));
    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_EXIT, {{0}})), IsOk());
    EXPECT_THAT(inner_.guest.Active(), IsEmpty());
}

TEST_F(InputDispatcherTest, PointerlessCancelReleasesSessionContactsOnDisplay) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{2}}, 1)), IsOk());
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0, 0.75F, 0.75F}})), IsOk());

    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_CANCEL, {})), IsOk());

    EXPECT_THAT(inner_.reports, SizeIs(4));
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
    EXPECT_THAT(outer_.guest.Active(), SizeIs(1));
}

// The events turning `slot` into a palm or lifting it.
std::vector<EvDevEvent> Palm(uint32_t slot) {
    return {{EV_ABS, ABS_MT_SLOT, slot}, {EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM}};
}

std::vector<EvDevEvent> Lift(uint32_t slot) {
    return {{EV_ABS, ABS_MT_SLOT, slot}, {EV_ABS, ABS_MT_TRACKING_ID, kTrackingIdUp}};
}

TEST_F(InputDispatcherTest, CancelLiftsAsPalmButUpAndHoverExitLiftNormally) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}, {2}})), IsOk());
    inner_.reports.clear();
    inner_.frames.clear();

    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_CANCEL, {{0}})), IsOk());
    ASSERT_THAT(inner_.reports, ElementsAre(Palm(0), Lift(0)));
    // Frame 0 observes slot 0 as MT_TOOL_PALM while still active; frame 1 lifts it.
    EXPECT_THAT(inner_.frames[0],
                ElementsAre(::testing::AllOf(Field(&GuestContact::slot, 0),
                                             Field(&GuestContact::tool_type, MT_TOOL_PALM)),
                            Field(&GuestContact::slot, 1), Field(&GuestContact::slot, 2)));
    EXPECT_THAT(inner_.frames[1],
                ElementsAre(Field(&GuestContact::slot, 1), Field(&GuestContact::slot, 2)));

    ASSERT_THAT(Dispatch(a, Touch(kUp, {{1}})), IsOk());
    EXPECT_EQ(inner_.reports.back(), Lift(1));
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{2, 0.5F, 0.5F, 0.0F}})), IsOk());
    EXPECT_EQ(inner_.reports.back(), Lift(2));

    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_ENTER, {{3}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_EXIT, {{3}})), IsOk());
    EXPECT_EQ(inner_.reports.back(), Lift(0));
    EXPECT_THAT(inner_.guest.Active(), IsEmpty());
}

TEST_F(InputDispatcherTest, PointerlessCancelLiftsAsPalmButHoverExitLiftsNormally) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    inner_.reports.clear();
    inner_.frames.clear();
    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_CANCEL, {})), IsOk());
    EXPECT_THAT(inner_.reports, ElementsAre(Palm(0), Lift(0)));
    EXPECT_THAT(inner_.frames[0], ElementsAre(Field(&GuestContact::tool_type, MT_TOOL_PALM)));
    EXPECT_THAT(inner_.frames[1], IsEmpty());

    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_ENTER, {{0}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_EXIT, {})), IsOk());
    EXPECT_EQ(inner_.reports.back(), Lift(0));
}

TEST_F(InputDispatcherTest, CloseSessionLiftsContactsAsPalm) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}})), IsOk());
    inner_.reports.clear();
    inner_.frames.clear();

    dispatcher_.CloseSession(a);
    loop_->RunAll();

    std::vector<EvDevEvent> expected_palm = Palm(0);
    std::ranges::copy(Palm(1), std::back_inserter(expected_palm));
    std::vector<EvDevEvent> expected_lift = Lift(0);
    std::ranges::copy(Lift(1), std::back_inserter(expected_lift));
    EXPECT_THAT(inner_.reports, ElementsAre(expected_palm, expected_lift));
    EXPECT_THAT(inner_.frames[0], ElementsAre(Field(&GuestContact::tool_type, MT_TOOL_PALM),
                                              Field(&GuestContact::tool_type, MT_TOOL_PALM)));
    EXPECT_THAT(inner_.frames[1], IsEmpty());
}

TEST_F(InputDispatcherTest, CloseSessionReleasesOnlyItsContactsBatchingSlotsPerDevice) {
    const InputSessionId a = Agent();
    const InputSessionId b = Human();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{2}}, 1)), IsOk());
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0, 0.75F, 0.75F}})), IsOk());
    inner_.reports.clear();
    outer_.reports.clear();

    dispatcher_.CloseSession(a);
    loop_->RunAll();

    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(outer_.reports, SizeIs(2));
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
    EXPECT_THAT(outer_.guest.Active(), IsEmpty());

    // The closed session's events no longer create contacts.
    EXPECT_THAT(Dispatch(a, Touch(kDown, {{0}})), StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_THAT(inner_.reports, SizeIs(2));
}

TEST_F(InputDispatcherTest, MultiPointerEventWritesOneReportPerDevice) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.1F, 0.1F}, {1, 0.2F, 0.2F}, {2, 0.3F, 0.3F}})),
                IsOk());
    EXPECT_THAT(inner_.reports, SizeIs(1));
    EXPECT_THAT(inner_.guest.Active(), SizeIs(3));

    // A partial lift: pointer 1 keeps pressure, the others lift.
    ASSERT_THAT(Dispatch(a, Touch(kUp, {{0}, {1, 0.4F, 0.4F, 1.0F}, {2}})), IsOk());
    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.4F, 0.4F)));
}

TEST_F(InputDispatcherTest, MoveWithZeroPressureLiftsOnlyThatPointer) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.1F, 0.1F}, {1, 0.2F, 0.2F}, {2, 0.3F, 0.3F}})),
                IsOk());

    ASSERT_THAT(Dispatch(a, Touch(kMove,
                                  {{0, 0.15F, 0.15F}, {1, 0.2F, 0.2F, 0.0F}, {2, 0.35F, 0.35F}})),
                IsOk());

    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(inner_.guest.Active(),
                ElementsAre(ContactAt(0.15F, 0.15F), ContactAt(0.35F, 0.35F)));
}

TEST_F(InputDispatcherTest, BackToBackEventsApplyInOrder) {
    const InputSessionId a = Agent();
    std::vector<int> completed;
    const std::vector<PointerEvent> events = {
        Touch(kDown, {{0, 0.1F, 0.1F}}), Touch(kMove, {{0, 0.2F, 0.2F}}),
        Touch(kMove, {{0, 0.3F, 0.3F}}), Touch(kUp, {{0, 0.3F, 0.3F}})};
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        dispatcher_.DispatchPointer(a, events[i], [&completed, i](DispatchResult done) {
            EXPECT_THAT(done.status, IsOk());
            completed.push_back(i);
        });
    }
    loop_->RunAll();

    EXPECT_THAT(completed, ElementsAre(0, 1, 2, 3));
    ASSERT_THAT(inner_.frames, SizeIs(4));
    EXPECT_THAT(inner_.frames[0], ElementsAre(ContactAt(0.1F, 0.1F)));
    EXPECT_THAT(inner_.frames[1], ElementsAre(ContactAt(0.2F, 0.2F)));
    EXPECT_THAT(inner_.frames[2], ElementsAre(ContactAt(0.3F, 0.3F)));
    EXPECT_THAT(inner_.frames[3], IsEmpty());
}

TEST_F(InputDispatcherTest, ReportsAndCompletionsRunOnLoopThread) {
    const InputSessionId a = Agent();
    std::optional<bool> done_on_loop;
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}, {1}}),
                                [&](DispatchResult) { done_on_loop = loop_->IsOnLoopThread(); });
    loop_->RunAll();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{2}}, 1)), IsOk());
    dispatcher_.CloseSession(a);
    loop_->RunAll();

    EXPECT_EQ(done_on_loop, true);
    EXPECT_THAT(inner_.on_loop_thread, ElementsAre(true, true, true));
    EXPECT_THAT(outer_.on_loop_thread, ElementsAre(true, true, true));
}

TEST_F(InputDispatcherTest, RejectedPostCompletesOnCallingThread) {
    const InputSessionId a = Agent();
    loop_->RunAll();
    ASSERT_THAT(loop_->ShutdownAndWait(), IsOk());

    std::optional<absl::Status> result;
    std::thread::id done_thread;
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}}), [&](DispatchResult done) {
        result = std::move(done.status);
        done_thread = std::this_thread::get_id();
    });

    EXPECT_THAT(result, Optional(StatusIs(absl::StatusCode::kUnavailable)));
    EXPECT_EQ(done_thread, std::this_thread::get_id());
    EXPECT_THAT(inner_.reports, IsEmpty());
}

std::string Down(uint32_t evdev_code) {
    return absl::StrCat("down ", evdev_code);
}

std::string Up(uint32_t evdev_code) {
    return absl::StrCat("up ", evdev_code);
}

TEST_F(InputDispatcherTest, KeyDownAndUpReachGuest) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_A)), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_A), Up(KEY_A)));
}

TEST_F(InputDispatcherTest, AutorepeatForwardsEveryDownAndOneUp) {
    const InputSessionId a = Agent();
    for (int i = 0; i < 3; ++i) {
        ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_A)), IsOk());
    }
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_A)), IsOk());

    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_A), Down(KEY_A), Down(KEY_A), Up(KEY_A)));
}

TEST_F(InputDispatcherTest, KeyIsReleasedWhenLastHolderReleases) {
    const InputSessionId a = Agent();
    const InputSessionId b = Human();
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_LEFTSHIFT)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyDown, KEY_LEFTSHIFT)), IsOk());

    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_LEFTSHIFT)), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_LEFTSHIFT), Down(KEY_LEFTSHIFT)));

    ASSERT_THAT(Dispatch(b, Key(kKeyUp, KEY_LEFTSHIFT)), IsOk());
    EXPECT_THAT(log_.writes,
                ElementsAre(Down(KEY_LEFTSHIFT), Down(KEY_LEFTSHIFT), Up(KEY_LEFTSHIFT)));
}

TEST_F(InputDispatcherTest, StrayUpFromNonHolderWritesNothing) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    ASSERT_THAT(Dispatch(b, Key(kKeyUp, KEY_A)), IsOk());
    EXPECT_THAT(log_.writes, IsEmpty());

    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyUp, KEY_A)), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_A)));

    // `a` still holds the key.
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_A)), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_A), Up(KEY_A)));
}

TEST_F(InputDispatcherTest, PressWithoutOtherHolderIsDownThenUp) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Dispatch(a, Key(kKeyPress, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(KeyAction::KEY_ACTION_UNSPECIFIED, KEY_B)), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_A), Up(KEY_A), Down(KEY_B), Up(KEY_B)));
}

TEST_F(InputDispatcherTest, PressWhileAnotherSessionHoldsKeyIsDownOnly) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_LEFTSHIFT)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyPress, KEY_LEFTSHIFT)), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_LEFTSHIFT), Down(KEY_LEFTSHIFT)));

    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_LEFTSHIFT)), IsOk());
    EXPECT_THAT(log_.writes,
                ElementsAre(Down(KEY_LEFTSHIFT), Down(KEY_LEFTSHIFT), Up(KEY_LEFTSHIFT)));
}

TEST_F(InputDispatcherTest, UnmappedKeyIsInvalidAndWritesNothing) {
    const InputSessionId a = Agent();
    KeyEvent event;
    event.set_action(kKeyDown);
    event.set_dom_code("KeyA");
    EXPECT_THAT(Dispatch(a, event), StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(Dispatch(a, KeyEvent()), StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(log_.writes, IsEmpty());
}

TEST_F(InputDispatcherTest, KeyOrTextFromUnknownSessionFails) {
    EXPECT_THAT(Dispatch(7, Key(kKeyDown, KEY_A)), StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_THAT(Type(7, "hi"), StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_THAT(log_.writes, IsEmpty());
}

TEST_F(InputDispatcherTest, CloseSessionReleasesUnsharedKeysInAscendingOrder) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    for (uint32_t code : {KEY_B, KEY_LEFTSHIFT, KEY_A, KEY_LEFTCTRL}) {
        ASSERT_THAT(Dispatch(a, Key(kKeyDown, code)), IsOk());
    }
    ASSERT_THAT(Dispatch(b, Key(kKeyDown, KEY_LEFTSHIFT)), IsOk());
    log_.writes.clear();

    dispatcher_.CloseSession(a);
    loop_->RunAll();

    static_assert(KEY_LEFTCTRL < KEY_A && KEY_A < KEY_B);
    EXPECT_THAT(log_.writes, ElementsAre(Up(KEY_LEFTCTRL), Up(KEY_A), Up(KEY_B)));
    ASSERT_THAT(Dispatch(b, Key(kKeyUp, KEY_LEFTSHIFT)), IsOk());
    EXPECT_THAT(log_.writes,
                ElementsAre(Up(KEY_LEFTCTRL), Up(KEY_A), Up(KEY_B), Up(KEY_LEFTSHIFT)));
}

TEST_F(InputDispatcherTest, TextIsTypedAndEmptyTextWritesNothing) {
    const InputSessionId a = Agent();
    ASSERT_THAT(Type(a, ""), IsOk());
    ASSERT_THAT(Type(a, "こんにちは"), IsOk());
    EXPECT_THAT(log_.writes, ElementsAre("text こんにちは"));
}

TEST_F(InputDispatcherTest, KeysTextAndTouchesAreWrittenInPostingOrderOnLoop) {
    const InputSessionId a = Agent();
    const InputSessionId b = Agent();
    loop_->RunAll();

    dispatcher_.DispatchKey(a, Key(kKeyDown, KEY_LEFTSHIFT));
    dispatcher_.DispatchPointer(b, Touch(kDown, {{0}}));
    dispatcher_.DispatchText(b, "x");
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}}, 1));
    dispatcher_.DispatchKey(a, Key(kKeyUp, KEY_LEFTSHIFT));
    dispatcher_.DispatchKey(b, Key(kKeyPress, KEY_A));
    dispatcher_.CloseSession(b);
    loop_->RunAll();

    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_LEFTSHIFT), "touch inner", "text x",
                                         "touch outer", Up(KEY_LEFTSHIFT), Down(KEY_A), Up(KEY_A),
                                         "touch inner", "touch inner"));
    EXPECT_THAT(log_.on_loop_thread, Each(true));
}

TEST_F(InputDispatcherTest, PointerActivityHasSourceIdAndRequestedDisplay) {
    const InputSessionId a = Source(7);
    devices_.remap[0] = kOuter;  // A foldable routes display 0 to the outer device.
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{0}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kUp, {{0}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kUp, {{1}})), IsOk());
    ASSERT_THAT(outer_.reports, SizeIs(4));

    const std::vector<ObservedActivity> observed = TakeActivities();
    ASSERT_THAT(observed, SizeIs(4));
    EXPECT_EQ(observed[0].activity.event().pointer().pointers(0).x(), 0.25F);
    std::vector<std::string> described;
    for (const ObservedActivity& o : observed) {
        described.push_back(Describe(o.activity));
    }
    EXPECT_THAT(described, ElementsAre("src7 POINTER_ACTION_DOWN display 0 ids 0 1",
                                       "src7 POINTER_ACTION_MOVE display 0 ids 0 1",
                                       "src7 POINTER_ACTION_UP display 0 ids 0",
                                       "src7 POINTER_ACTION_UP display 0 ids 1"));
}

TEST_F(InputDispatcherTest, ActivityListsOnlyPointersThatTookEffect) {
    const InputSessionId a = Source(1);
    std::vector<P> pointers;
    for (int i = 0; i < kTouchDeviceSlots - kReservedHumanSlots - 1; ++i) {
        pointers.push_back({i});
    }
    ASSERT_THAT(Dispatch(a, Touch(kDown, pointers)), IsOk());
    Activities();

    // Pointer 0 moves, 8 takes the last slot, 9 is exhausted and 10 is unheld.
    EXPECT_THAT(Dispatch(a, Touch(kDown, {{0}, {8}, {9}})),
                StatusIs(absl::StatusCode::kResourceExhausted));
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{0}, {10}})), IsOk());
    // Nothing takes effect.
    EXPECT_THAT(Dispatch(Source(2), Touch(kDown, {{0}})),
                StatusIs(absl::StatusCode::kResourceExhausted));
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{10}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kUp, {{11}})), IsOk());

    EXPECT_THAT(Activities(), ElementsAre("src1 POINTER_ACTION_DOWN display 0 ids 0 8",
                                          "src1 POINTER_ACTION_MOVE display 0 ids 0"));
}

TEST_F(InputDispatcherTest, PointerlessReleaseIsReportedAsCancelOfReleasedIds) {
    const InputSessionId a = Source(3);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{2}}, 1)), IsOk());
    Activities();

    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_HOVER_EXIT, {})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(PointerAction::POINTER_ACTION_CANCEL, {})), IsOk());

    EXPECT_THAT(Activities(), ElementsAre("src3 POINTER_ACTION_CANCEL display 0 ids 0 1"));
}

TEST_F(InputDispatcherTest, EverySessionsKeyDownAndUpIsReported) {
    const InputSessionId a = Source(1);
    const InputSessionId b = Source(2);
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_LEFTSHIFT)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyDown, KEY_LEFTSHIFT)), IsOk());
    // The guest gets no UP: `b` still holds the key.
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_LEFTSHIFT)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyUp, KEY_LEFTSHIFT)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyPress, KEY_A)), IsOk());

    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_LEFTSHIFT), Down(KEY_LEFTSHIFT),
                                         Up(KEY_LEFTSHIFT), Down(KEY_A), Up(KEY_A)));
    EXPECT_THAT(Activities(), ElementsAre(absl::StrCat("src1 KEY_ACTION_DOWN ", KEY_LEFTSHIFT),
                                          absl::StrCat("src2 KEY_ACTION_DOWN ", KEY_LEFTSHIFT),
                                          absl::StrCat("src1 KEY_ACTION_UP ", KEY_LEFTSHIFT),
                                          absl::StrCat("src2 KEY_ACTION_UP ", KEY_LEFTSHIFT),
                                          absl::StrCat("src1 KEY_ACTION_PRESS ", KEY_A)));
}

TEST_F(InputDispatcherTest, StrayUpAndRejectedEventsAreNotReported) {
    const InputSessionId a = Source(1);
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_A)), IsOk());
    EXPECT_THAT(Dispatch(a, KeyEvent()), StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(Dispatch(99, Key(kKeyDown, KEY_A)),
                StatusIs(absl::StatusCode::kFailedPrecondition));
    EXPECT_THAT(Dispatch(99, Touch(kDown, {{0}})), StatusIs(absl::StatusCode::kFailedPrecondition));
    ASSERT_THAT(Type(a, ""), IsOk());

    EXPECT_THAT(Activities(), IsEmpty());
}

TEST_F(InputDispatcherTest, TextIsReported) {
    ASSERT_THAT(Type(Source(5), "こんにちは"), IsOk());
    EXPECT_THAT(Activities(), ElementsAre("src5 text こんにちは"));
}

TEST_F(InputDispatcherTest, CloseSessionReportsSynthesizedReleases) {
    const InputSessionId a = Source(4);
    const InputSessionId b = Source(5);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}, {1}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{2}}, 1)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_B)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyDown, KEY_A)), IsOk());
    Activities();

    dispatcher_.CloseSession(a);

    static_assert(KEY_A < KEY_B);
    EXPECT_THAT(Activities(),
                ElementsAre("src4 synthesized POINTER_ACTION_CANCEL display 0 ids 0 1",
                            "src4 synthesized POINTER_ACTION_CANCEL display 1 ids 2",
                            absl::StrCat("src4 synthesized KEY_ACTION_UP ", KEY_A),
                            absl::StrCat("src4 synthesized KEY_ACTION_UP ", KEY_B)));
}

TEST_F(InputDispatcherTest, SynthesizedCancelReportsRequestedDisplay) {
    const InputSessionId a = Source(6);
    devices_.remap[0] = kOuter;  // A foldable routes display 0 to the outer device.
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    Activities();

    dispatcher_.CloseSession(a);

    EXPECT_THAT(Activities(),
                ElementsAre("src6 synthesized POINTER_ACTION_CANCEL display 0 ids 0"));
    EXPECT_THAT(outer_.guest.Active(), IsEmpty());
}

TEST_F(InputDispatcherTest, SynthesizedCancelGroupsContactsByRequestedDisplay) {
    const InputSessionId a = Source(6);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    devices_.remap[0] = kOuter;  // The foldable folds mid-gesture.
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1}})), IsOk());
    Activities();
    inner_.reports.clear();
    outer_.reports.clear();

    dispatcher_.CloseSession(a);

    // Palm + lift reports per device, and one CANCEL for the display both went down on.
    EXPECT_THAT(Activities(),
                ElementsAre("src6 synthesized POINTER_ACTION_CANCEL display 0 ids 0 1"));
    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(outer_.reports, SizeIs(2));
}

TEST_F(InputDispatcherTest, InterleavedSessionsAreReportedInLoopOrderWithRisingTimestamps) {
    const InputSessionId a = Source(1);
    const InputSessionId b = Source(2);
    loop_->RunAll();

    dispatcher_.DispatchKey(a, Key(kKeyDown, KEY_LEFTSHIFT));
    dispatcher_.DispatchPointer(b, Touch(kDown, {{0}}));
    dispatcher_.DispatchText(b, "x");
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}}, 1));
    dispatcher_.DispatchKey(a, Key(kKeyUp, KEY_LEFTSHIFT));
    dispatcher_.DispatchKey(b, Key(kKeyPress, KEY_A));
    dispatcher_.CloseSession(b);

    const std::vector<ObservedActivity> observed = TakeActivities();
    std::vector<std::string> described;
    for (size_t i = 0; i < observed.size(); ++i) {
        described.push_back(Describe(observed[i].activity));
        if (i > 0) {
            EXPECT_GT(observed[i].activity.dispatched_at_nanos(),
                      observed[i - 1].activity.dispatched_at_nanos());
        }
    }
    EXPECT_THAT(described, ElementsAre(absl::StrCat("src1 KEY_ACTION_DOWN ", KEY_LEFTSHIFT),
                                       "src2 POINTER_ACTION_DOWN display 0 ids 0", "src2 text x",
                                       "src1 POINTER_ACTION_DOWN display 1 ids 0",
                                       absl::StrCat("src1 KEY_ACTION_UP ", KEY_LEFTSHIFT),
                                       absl::StrCat("src2 KEY_ACTION_PRESS ", KEY_A),
                                       "src2 synthesized POINTER_ACTION_CANCEL display 0 ids 0"));
    ASSERT_THAT(observed, Not(IsEmpty()));
    EXPECT_GT(observed[0].activity.dispatched_at_nanos(), 0);
}

TEST_F(InputDispatcherTest, CompletionCarriesTheTimeOfItsActivity) {
    const InputSessionId a = Source(1);
    std::vector<DispatchResult> results;
    const auto record = [&results](DispatchResult result) { results.push_back(std::move(result)); };
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}}), record);
    dispatcher_.DispatchKey(a, Key(kKeyPress, KEY_A), record);
    dispatcher_.DispatchText(a, "x", record);
    dispatcher_.DispatchPointer(a, Touch(PointerAction::POINTER_ACTION_CANCEL, {}), record);
    // Nothing reaches the guest: an unheld pointer, a stray UP, empty text and
    // a rejected key.
    dispatcher_.DispatchPointer(a, Touch(kMove, {{5}}), record);
    dispatcher_.DispatchKey(a, Key(kKeyUp, KEY_B), record);
    dispatcher_.DispatchText(a, "", record);
    dispatcher_.DispatchKey(a, KeyEvent(), record);

    const std::vector<ObservedActivity> observed = TakeActivities();
    ASSERT_THAT(observed, SizeIs(4));
    ASSERT_THAT(results, SizeIs(8));
    for (size_t i = 0; i < observed.size(); ++i) {
        EXPECT_THAT(results[i].status, IsOk());
        ASSERT_TRUE(results[i].dispatched_at.has_value()) << i;
        EXPECT_EQ(absl::ToUnixNanos(*results[i].dispatched_at),
                  observed[i].activity.dispatched_at_nanos());
    }
    for (size_t i = observed.size(); i < results.size(); ++i) {
        EXPECT_EQ(results[i].dispatched_at, std::nullopt) << i;
    }
    EXPECT_THAT(results.back().status, StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(InputDispatcherTest, PartlyAppliedEventCompletesWithErrorAndTime) {
    std::vector<P> pointers;
    for (int i = 0; i <= kTouchDeviceSlots - kReservedHumanSlots; ++i) {
        pointers.push_back({i});
    }
    std::optional<DispatchResult> result;
    dispatcher_.DispatchPointer(Source(1), Touch(kDown, pointers),
                                [&result](DispatchResult done) { result = std::move(done); });

    const std::vector<ObservedActivity> observed = TakeActivities();
    ASSERT_THAT(observed, SizeIs(1));
    ASSERT_TRUE(result.has_value());
    EXPECT_THAT(result->status, StatusIs(absl::StatusCode::kResourceExhausted));
    ASSERT_TRUE(result->dispatched_at.has_value());
    EXPECT_EQ(absl::ToUnixNanos(*result->dispatched_at),
              observed[0].activity.dispatched_at_nanos());
}

TEST_F(InputDispatcherTest, CallbacksRunOnDeliveryThread) {
    const InputSessionId a = Source(1);
    ASSERT_THAT(Type(a, "x"), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyPress, KEY_A)), IsOk());

    const std::vector<ObservedActivity> observed = TakeActivities();
    ASSERT_THAT(observed, SizeIs(2));
    for (const ObservedActivity& o : observed) {
        EXPECT_FALSE(o.on_loop_thread);
        EXPECT_NE(o.thread, std::this_thread::get_id());
        EXPECT_EQ(o.thread, observed[0].thread);
    }
}

TEST_F(InputDispatcherTest, DestructorDeliversQueuedActivity) {
    auto dispatcher = std::make_unique<InputDispatcher>(*loop_, devices_, &keyboard_,
                                                        ResolveEvdevOnly, &clock_);
    ActivityRecorder recorder(*loop_);
    absl::Notification first_arrived;
    absl::Notification release_first;
    // Never removed: the dispatcher's event source drops it on destruction.
    dispatcher->activity().AddCallback([&](const InputActivity& activity) {
        recorder.Record(activity);
        if (!first_arrived.HasBeenNotified()) {
            first_arrived.Notify();
            release_first.WaitForNotification();
        }
    });
    const InputSessionId a = dispatcher->OpenSession({.source_id = 1});
    dispatcher->DispatchText(a, "1");
    loop_->RunAll();
    first_arrived.WaitForNotification();

    // Delivery is blocked in the first callback, so these stay queued.
    dispatcher->DispatchText(a, "2");
    dispatcher->DispatchText(a, "3");
    loop_->RunAll();
    release_first.Notify();
    dispatcher.reset();

    std::vector<std::string> described;
    for (const ObservedActivity& o : recorder.Observed()) {
        described.push_back(Describe(o.activity));
    }
    EXPECT_THAT(described, ElementsAre("src1 text 1", "src1 text 2", "src1 text 3"));
}

// Records how often an operation completed, and its last status and thread.
struct Completion {
    InputDispatcher::DispatchCallback Callback() {
        return [this](DispatchResult result) {
            ++calls;
            status = std::move(result.status);
            thread = std::this_thread::get_id();
        };
    }

    int calls = 0;
    absl::Status status;
    std::thread::id thread;
};

TEST_F(InputDispatcherTest, ShutdownCompletesPendingOperationsOnceWithUnavailable) {
    const InputSessionId a = Agent();
    loop_->RunAll();
    std::array<Completion, 3> pending;
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}}), pending[0].Callback());
    dispatcher_.DispatchKey(a, Key(kKeyDown, KEY_A), pending[1].Callback());
    dispatcher_.DispatchText(a, "x", pending[2].Callback());

    dispatcher_.Shutdown();

    for (const Completion& completion : pending) {
        EXPECT_EQ(completion.calls, 1);
        EXPECT_THAT(completion.status, StatusIs(absl::StatusCode::kUnavailable));
        EXPECT_EQ(completion.thread, std::this_thread::get_id());
    }
    // The loop didn't stop, so the operations still run, but they apply
    // nothing and don't complete again.
    loop_->RunAll();
    for (const Completion& completion : pending) {
        EXPECT_EQ(completion.calls, 1);
    }
    EXPECT_THAT(inner_.reports, IsEmpty());
    EXPECT_THAT(log_.writes, IsEmpty());
}

TEST_F(InputDispatcherTest, AfterShutdownNothingIsPostedAndDispatchIsUnavailable) {
    const InputSessionId a = Agent();
    loop_->RunAll();
    dispatcher_.Shutdown();

    EXPECT_GT(Agent(), a);
    dispatcher_.CloseSession(a);
    std::array<Completion, 3> rejected;
    dispatcher_.DispatchPointer(a, Touch(kDown, {{0}}), rejected[0].Callback());
    dispatcher_.DispatchKey(a, Key(kKeyDown, KEY_A), rejected[1].Callback());
    dispatcher_.DispatchText(a, "x", rejected[2].Callback());

    EXPECT_EQ(loop_->TaskCount(), 0);
    for (const Completion& completion : rejected) {
        EXPECT_EQ(completion.calls, 1);
        EXPECT_THAT(completion.status, StatusIs(absl::StatusCode::kUnavailable));
        EXPECT_EQ(completion.thread, std::this_thread::get_id());
    }
}

TEST_F(InputDispatcherTest, ShutdownIsIdempotentAndCallableFromAnyThread) {
    const InputSessionId a = Agent();
    loop_->RunAll();
    Completion pending;
    dispatcher_.DispatchText(a, "x", pending.Callback());

    std::thread other([this] { dispatcher_.Shutdown(); });
    dispatcher_.Shutdown();
    other.join();
    dispatcher_.Shutdown();

    EXPECT_EQ(pending.calls, 1);
    EXPECT_THAT(pending.status, StatusIs(absl::StatusCode::kUnavailable));
}

TEST_F(InputDispatcherTest, ShutdownDeliversQueuedActivityAndJoins) {
    absl::Notification first_arrived;
    absl::Notification release_first;
    std::vector<std::string> delivered;
    dispatcher_.activity().AddCallback([&](const InputActivity& activity) {
        delivered.push_back(activity.event().text().text());
        if (!first_arrived.HasBeenNotified()) {
            first_arrived.Notify();
            release_first.WaitForNotification();
        }
    });
    const InputSessionId a = Agent();
    ASSERT_THAT(Type(a, "1"), IsOk());
    first_arrived.WaitForNotification();
    // Delivery is blocked in the first callback, so these stay queued.
    ASSERT_THAT(Type(a, "2"), IsOk());
    ASSERT_THAT(Type(a, "3"), IsOk());
    release_first.Notify();

    dispatcher_.Shutdown();

    // Joined, so reading what the delivery thread wrote is safe.
    EXPECT_THAT(delivered, ElementsAre("1", "2", "3"));
}

TEST_F(InputDispatcherTest, DestructorShutsDownAfterLoopStopped) {
    Completion pending;
    auto dispatcher = std::make_unique<InputDispatcher>(*loop_, devices_, &keyboard_,
                                                        ResolveEvdevOnly, &clock_);
    const InputSessionId a = dispatcher->OpenSession({});
    loop_->RunAll();
    dispatcher->DispatchText(a, "x", pending.Callback());

    // The emulator exits: the loop stops with the operation still queued.
    ASSERT_THAT(loop_->ShutdownAndWait(), IsOk());
    dispatcher.reset();

    EXPECT_EQ(pending.calls, 1);
    EXPECT_THAT(pending.status, StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_THAT(log_.writes, IsEmpty());
}

TEST_F(InputDispatcherTest, SessionAtCapIsRejectedOnCallingThreadWhileOthersPost) {
    std::array<Completion, 4> done;
    Completion again;
    InputDispatcher capped(*loop_, devices_, &keyboard_, ResolveEvdevOnly, &clock_,
                           /*max_pending_per_session=*/2);
    const InputSessionId a = capped.OpenSession({});
    const InputSessionId b = capped.OpenSession({});
    loop_->RunAll();

    capped.DispatchText(a, "a1", done[0].Callback());
    capped.DispatchText(a, "a2", done[1].Callback());
    capped.DispatchText(a, "a3", done[2].Callback());
    capped.DispatchText(b, "b1", done[3].Callback());

    EXPECT_EQ(done[2].calls, 1);
    EXPECT_THAT(done[2].status, StatusIs(absl::StatusCode::kResourceExhausted));
    EXPECT_EQ(done[2].thread, std::this_thread::get_id());
    EXPECT_EQ(done[0].calls + done[1].calls + done[3].calls, 0);

    loop_->RunAll();
    EXPECT_THAT(log_.writes, ElementsAre("text a1", "text a2", "text b1"));
    for (int i : {0, 1, 3}) {
        EXPECT_EQ(done[i].calls, 1);
        EXPECT_THAT(done[i].status, IsOk());
    }
    EXPECT_EQ(done[2].calls, 1);

    // The operations ran, so the session may post again.
    capped.DispatchText(a, "a4", again.Callback());
    loop_->RunAll();
    EXPECT_EQ(again.calls, 1);
    EXPECT_THAT(again.status, IsOk());
}

TEST_F(InputDispatcherTest, CloseSessionRunsWhenSessionIsAtCap) {
    InputDispatcher capped(*loop_, devices_, &keyboard_, ResolveEvdevOnly, &clock_,
                           /*max_pending_per_session=*/1);
    const InputSessionId a = capped.OpenSession({});
    loop_->RunAll();
    capped.DispatchPointer(a, Touch(kDown, {{0}}));

    capped.CloseSession(a);
    loop_->RunAll();

    EXPECT_THAT(inner_.reports, SizeIs(3));
    EXPECT_THAT(inner_.guest.Active(), IsEmpty());
}

TEST_F(InputDispatcherTest, KeysWaitForInstalledKeyboard) {
    InputDispatcher late(*loop_, devices_, /*keyboard=*/nullptr, ResolveEvdevOnly, &clock_);
    const InputSessionId a = late.OpenSession({});
    std::array<Completion, 4> done;
    late.DispatchKey(a, Key(kKeyPress, KEY_A), done[0].Callback());
    late.DispatchText(a, "x", done[1].Callback());

    late.InstallKeyboardSink(keyboard_);
    late.DispatchKey(a, Key(kKeyPress, KEY_B), done[2].Callback());
    late.DispatchText(a, "y", done[3].Callback());
    loop_->RunAll();

    EXPECT_THAT(done[0].status, StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_THAT(done[1].status, StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_THAT(done[2].status, IsOk());
    EXPECT_THAT(done[3].status, IsOk());
    EXPECT_THAT(log_.writes, ElementsAre(Down(KEY_B), Up(KEY_B), "text y"));
}

TEST_F(InputDispatcherTest, SnapshotLoadReleasesAllGuestSlotsAndResetsKeyboardEvenWhenIdle) {
    // Simulate ghost contacts left in the guest by a loaded snapshot.
    const EvDevEvent ghost_inner[] = {{EV_ABS, ABS_MT_SLOT, 7}, {EV_ABS, ABS_MT_TRACKING_ID, 7}};
    const EvDevEvent ghost_outer[] = {{EV_ABS, ABS_MT_SLOT, 10}, {EV_ABS, ABS_MT_TRACKING_ID, 10}};
    inner_.guest.ApplyReport(ghost_inner);
    outer_.guest.ApplyReport(ghost_outer);
    ASSERT_THAT(inner_.guest.Active(), SizeIs(1));
    ASSERT_THAT(outer_.guest.Active(), SizeIs(1));

    dispatcher_.OnSnapshotLoaded();
    loop_->RunAll();

    std::vector<EvDevEvent> expected_palm;
    std::vector<EvDevEvent> expected_lift;
    for (uint32_t slot = 0; slot < static_cast<uint32_t>(kTouchDeviceSlots); ++slot) {
        std::ranges::copy(Palm(slot), std::back_inserter(expected_palm));
        std::ranges::copy(Lift(slot), std::back_inserter(expected_lift));
    }
    EXPECT_THAT(inner_.reports, ElementsAre(expected_palm, expected_lift));
    EXPECT_THAT(outer_.reports, ElementsAre(expected_palm, expected_lift));
    EXPECT_THAT(inner_.frames[0],
                ElementsAre(::testing::AllOf(Field(&GuestContact::slot, 7),
                                             Field(&GuestContact::tool_type, MT_TOOL_PALM))));
    EXPECT_THAT(outer_.frames[0],
                ElementsAre(::testing::AllOf(Field(&GuestContact::slot, 10),
                                             Field(&GuestContact::tool_type, MT_TOOL_PALM))));
    EXPECT_THAT(inner_.guest.Active(), IsEmpty());
    EXPECT_THAT(outer_.guest.Active(), IsEmpty());
    EXPECT_THAT(log_.writes,
                ElementsAre("touch inner", "touch inner", "touch outer", "touch outer", "reset"));
    EXPECT_THAT(log_.on_loop_thread, Each(true));
    EXPECT_THAT(Activities(), IsEmpty());
}

TEST_F(InputDispatcherTest, SnapshotLoadReleasesHeldContactsAndKeysAndReportsSynthesizedActivity) {
    const InputSessionId a = Source(1);
    const InputSessionId b = Source(2);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}, {1, 0.5F, 0.5F}})), IsOk());
    devices_.remap[0] = kOuter;  // Foldable folds: display 0 resolves to device 1.
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0, 0.75F, 0.75F}})), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_B)), IsOk());
    ASSERT_THAT(Dispatch(a, Key(kKeyDown, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyDown, KEY_A)), IsOk());
    Activities();
    log_.writes.clear();
    inner_.reports.clear();
    outer_.reports.clear();

    dispatcher_.OnSnapshotLoaded();
    loop_->RunAll();

    static_assert(KEY_A < KEY_B);
    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(outer_.reports, SizeIs(2));
    EXPECT_THAT(inner_.guest.Active(), IsEmpty());
    EXPECT_THAT(outer_.guest.Active(), IsEmpty());
    // The keyboard's Reset() releases the held keys; nothing is written first.
    EXPECT_THAT(log_.writes,
                ElementsAre("touch inner", "touch inner", "touch outer", "touch outer", "reset"));
    EXPECT_THAT(Activities(),
                ElementsAre("src1 synthesized POINTER_ACTION_CANCEL display 0 ids 0 1",
                            "src2 synthesized POINTER_ACTION_CANCEL display 0 ids 0",
                            absl::StrCat("src1 synthesized KEY_ACTION_UP ", KEY_A),
                            absl::StrCat("src2 synthesized KEY_ACTION_UP ", KEY_A),
                            absl::StrCat("src1 synthesized KEY_ACTION_UP ", KEY_B)));

    // Subsequent DOWNs start fresh in slot 0, and released keys are no longer held.
    devices_.remap.clear();
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    ASSERT_THAT(inner_.reports.back(), SizeIs(::testing::Ge(2)));
    EXPECT_EQ(inner_.reports.back()[0], (EvDevEvent{EV_ABS, ABS_MT_SLOT, 0}));
    EXPECT_EQ(inner_.reports.back()[1], (EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, 0}));
    log_.writes.clear();
    ASSERT_THAT(Dispatch(a, Key(kKeyUp, KEY_A)), IsOk());
    ASSERT_THAT(Dispatch(b, Key(kKeyUp, KEY_A)), IsOk());
    EXPECT_THAT(log_.writes, IsEmpty());
}

TEST_F(InputDispatcherTest, SnapshotLoadSkipsInactiveDeviceWriteButClearsItsContacts) {
    const InputSessionId a = Source(1);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1}}, 1)), IsOk());
    Activities();
    inner_.reports.clear();
    outer_.reports.clear();
    outer_.active = false;

    dispatcher_.OnSnapshotLoaded();
    loop_->RunAll();

    EXPECT_THAT(inner_.reports, SizeIs(2));
    EXPECT_THAT(outer_.reports, IsEmpty());
    EXPECT_THAT(Activities(),
                ElementsAre("src1 synthesized POINTER_ACTION_CANCEL display 0 ids 0",
                            "src1 synthesized POINTER_ACTION_CANCEL display 1 ids 1"));

    outer_.active = true;
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1}}, 1)), IsOk());
    ASSERT_THAT(outer_.reports.back(), SizeIs(::testing::Ge(2)));
    EXPECT_EQ(outer_.reports.back()[0], (EvDevEvent{EV_ABS, ABS_MT_SLOT, 0}));
    EXPECT_EQ(outer_.reports.back()[1], (EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, 0}));
}

TEST_F(InputDispatcherTest, DisplayRemovedClearsOnlyThatDevicesContactsWithoutWriting) {
    FakeSink secondary(*loop_, "secondary", log_);
    devices_.extra[2] = &secondary;
    const InputSessionId a = Source(1);
    const InputSessionId b = Source(2);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1}, {2}}, 2)), IsOk());
    devices_.remap[5] = 2;
    ASSERT_THAT(Dispatch(b, Touch(kDown, {{0}}, 5)), IsOk());
    Activities();
    secondary.reports.clear();

    dispatcher_.OnDisplayRemoved(2);
    loop_->RunAll();

    EXPECT_THAT(secondary.reports, IsEmpty());
    EXPECT_THAT(Activities(),
                ElementsAre("src1 synthesized POINTER_ACTION_CANCEL display 2 ids 1 2",
                            "src2 synthesized POINTER_ACTION_CANCEL display 5 ids 0"));

    // Device 0's contact is intact, and a recreated device 2 cancels its stale
    // guest slots before starting fresh at slot 0.
    ASSERT_THAT(Dispatch(a, Touch(kMove, {{0, 0.75F, 0.75F}})), IsOk());
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.75F, 0.75F)));
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1, 0.1F, 0.9F}}, 2)), IsOk());
    EXPECT_THAT(secondary.guest.Active(), ElementsAre(ContactAt(0.1F, 0.9F)));
    ASSERT_THAT(secondary.reports.back(), SizeIs(::testing::Ge(2)));
    EXPECT_EQ(secondary.reports.back()[0], (EvDevEvent{EV_ABS, ABS_MT_SLOT, 0}));
    EXPECT_EQ(secondary.reports.back()[1], (EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, 0}));
}

TEST_F(InputDispatcherTest, DisplayAddedProactivelyCancelsStaleSlotsUnlessDriverReset) {
    FakeSink secondary(*loop_, "secondary", log_);
    devices_.extra[2] = &secondary;
    const InputSessionId a = Source(1);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1, 0.2F, 0.3F}, {2, 0.4F, 0.5F}}, 2)), IsOk());
    EXPECT_THAT(secondary.guest.Active(), SizeIs(2));
    secondary.reports.clear();

    devices_.extra.erase(2);
    dispatcher_.OnDisplayRemoved(2);
    loop_->RunAll();
    EXPECT_THAT(secondary.reports, IsEmpty());

    // Recreating display 2 on the same permanent virtio-input device cancels
    // the previously held guest slots right when the display is added.
    devices_.extra[2] = &secondary;
    dispatcher_.OnDisplayAdded(2);
    loop_->RunAll();
    EXPECT_THAT(secondary.reports, SizeIs(2));
    EXPECT_THAT(secondary.guest.Active(), IsEmpty());

    // If the guest driver resets while the display is removed, OnDisplayAdded
    // drops the stale slot record without writing to the fresh driver.
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{1, 0.2F, 0.3F}}, 2)), IsOk());
    devices_.extra.erase(2);
    dispatcher_.OnDisplayRemoved(2);
    loop_->RunAll();

    ++secondary.active_generation;
    secondary.guest = GuestTouchState{};
    secondary.reports.clear();
    devices_.extra[2] = &secondary;
    dispatcher_.OnDisplayAdded(2);
    loop_->RunAll();
    EXPECT_THAT(secondary.reports, IsEmpty());
}

TEST_F(InputDispatcherTest, DisplayRemovedWithNoContactsDoesNothing) {
    const InputSessionId a = Source(1);
    ASSERT_THAT(Dispatch(a, Touch(kDown, {{0, 0.25F, 0.25F}})), IsOk());
    Activities();
    log_.writes.clear();

    dispatcher_.OnDisplayRemoved(2);
    loop_->RunAll();

    EXPECT_THAT(log_.writes, IsEmpty());
    EXPECT_THAT(Activities(), IsEmpty());
    EXPECT_THAT(inner_.guest.Active(), ElementsAre(ContactAt(0.25F, 0.25F)));
}

}  // namespace
}  // namespace goldfish::grpc::v2
