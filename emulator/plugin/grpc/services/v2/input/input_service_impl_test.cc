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

#include "input/input_service_impl.h"

#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/emulation/control/keyboard/key_code_resolver.h"
#include "emulator/plugin/grpc/input/contact_table.h"
#include "emulator/plugin/grpc/input/dispatcher_session.h"
#include "emulator/plugin/grpc/input/input_dispatcher.h"
#include "emulator/plugin/grpc/input/keyboard_sink.h"
#include "emulator/plugin/grpc/input/multi_display_touch_devices.h"
#include "emulator/plugin/grpc/input/slot_registry.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "goldfish/system/steady_clock.h"
#include "standard-headers/linux/input-event-codes.h"

// Necessary on Windows where winsock/windows headers define send as a macro.
#undef send

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::control::kMtsPointerUp;
using ::android::emulation::control::keyboard::ResolveEvdevKeyCode;
using ::android::emulation::v2::AxisType;
using ::android::emulation::v2::InputActivity;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::InputService;
using ::android::emulation::v2::KeyAction;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::SendInputEventsRequest;
using ::android::emulation::v2::SendInputEventsResponse;
using ::android::emulation::v2::StreamInputActivityRequest;
using ::android::emulation::v2::StreamInputEventsResponse;
using ::goldfish::async::EventLoop;
using ::goldfish::async::FlowId;
using ::goldfish::async::LibuvEventLoop;
using ::goldfish::async::ThreadedEventLoop;
using ::goldfish::display::test::ActiveFakePixmanDisplay;
using ::goldfish::display::test::FakeMultiDisplay;
using ::goldfish::display::test::FakePixmanDisplay;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

// The text the tests type to mark the end of the activity they expect.
constexpr std::string_view kSentinel = "<sentinel>";
constexpr int32_t kSentinelSource = 999;

bool IsSentinel(const InputActivity& activity) {
    return activity.event().has_text() && activity.event().text().text() == kSentinel;
}

// Logs each key as "down <code>" or "up <code>", and text as "text <utf8>".
// Skips the sentinel.
class RecordingKeyboard : public KeyboardSink {
  public:
    void WriteKey(uint32_t evdev_code, bool down) override {
        Record(absl::StrCat(down ? "down " : "up ", evdev_code));
    }

    void TypeText(std::string_view utf8) override {
        if (utf8 != kSentinel) {
            Record(absl::StrCat("text ", utf8));
        }
    }

    void Reset() override {}

    std::vector<std::string> Writes() const {
        absl::MutexLock writes_lock(writes_mutex_);
        return writes_;
    }

    // Waits until `write` was written.
    void AwaitWrite(std::string_view write) const {
        const auto written = [this, write]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(writes_mutex_) {
            return std::ranges::find(writes_, write) != writes_.end();
        };
        absl::MutexLock writes_lock(writes_mutex_);
        writes_mutex_.Await(absl::Condition(&written));
    }

  private:
    void Record(std::string write) {
        absl::MutexLock writes_lock(writes_mutex_);
        writes_.push_back(std::move(write));
    }

    mutable absl::Mutex writes_mutex_;
    std::vector<std::string> writes_ ABSL_GUARDED_BY(writes_mutex_);
};

// The dispatcher's clock too, so dispatch times follow the paced time.
class FakePacingClock : public ::goldfish::system::SteadyClock {
  public:
    explicit FakePacingClock(absl::Duration initial = absl::Nanoseconds(1'000'000'000LL))
            : current_time_(initial) {}

    absl::Duration Now() const override {
        absl::MutexLock time_lock(time_mutex_);
        const absl::Duration now = current_time_;
        current_time_ += auto_advance_;
        return now;
    }

    void Sleep(absl::Duration d) {
        absl::MutexLock time_lock(time_mutex_);
        sleep_calls_.push_back(d);
        current_time_ += d;
    }

    void SetAutoAdvance(absl::Duration step) {
        absl::MutexLock time_lock(time_mutex_);
        auto_advance_ = step;
    }

    std::vector<absl::Duration> SleepCalls() const {
        absl::MutexLock time_lock(time_mutex_);
        return sleep_calls_;
    }

  private:
    mutable absl::Mutex time_mutex_;
    mutable absl::Duration current_time_ ABSL_GUARDED_BY(time_mutex_);
    absl::Duration auto_advance_ ABSL_GUARDED_BY(time_mutex_) = absl::ZeroDuration();
    std::vector<absl::Duration> sleep_calls_ ABSL_GUARDED_BY(time_mutex_);
};

// The service's loop: posts run on `inner`, but delays and timers advance the
// fake clock instead of waiting, so paced dispatch is deterministic. Only the
// service runs on it; the dispatcher keeps real timers on `inner` itself, so
// its idle timeout doesn't fire the moment it is armed.
class PacingEventLoop final : public EventLoop {
  public:
    PacingEventLoop(EventLoop& inner, FakePacingClock& clock)
            : EventLoop("PacingEventLoop"), inner_(inner), clock_(clock) {}

    void ShutdownTimers() override { inner_.ShutdownTimers(); }
    size_t WaitUntilIdle() override { return inner_.WaitUntilIdle(); }
    std::future<absl::Status> Shutdown() override { return inner_.Shutdown(); }
    bool IsOnLoopThread() const override { return inner_.IsOnLoopThread(); }

    ScopedTimer CreateTimer(RepeatingTask task) override {
        auto timer = std::make_shared<PacingTimer>(inner_, clock_, std::move(task));
        timer->PinCaller(timer);
        return ScopedTimer(timer.get());
    }

  protected:
    absl::Status PostImmediately(Task task, FlowId /*flow_id*/) override {
        return inner_.Post(std::move(task));
    }
    absl::Status PostDelayed(Task task, absl::Duration delay, FlowId /*flow_id*/) override {
        if (!inner_.IsOnLoopThread()) {
            inner_.WaitUntilIdle();
        }
        clock_.Sleep(delay);
        return inner_.Post(std::move(task));
    }

  private:
    class PacingTimer final : public Timer, public std::enable_shared_from_this<PacingTimer> {
      public:
        PacingTimer(EventLoop& loop, FakePacingClock& clock, RepeatingTask task)
                : loop_(loop), clock_(clock), task_(std::move(task)) {}

        void PinCaller(std::shared_ptr<PacingTimer> self) { caller_pin_ = std::move(self); }

        void Schedule(absl::Duration initial_delay, absl::Duration /*interval*/) override {
            if (!loop_.IsOnLoopThread()) {
                loop_.WaitUntilIdle();
            }
            clock_.Sleep(initial_delay);
            loop_.Post([self = shared_from_this()]() {
                     if (!self->cancelled_.load(std::memory_order_acquire)) {
                         self->task_();
                     }
                 }).IgnoreError();
        }

        void Cancel() override {
            cancelled_.store(true, std::memory_order_release);
            caller_pin_.reset();
        }

      private:
        EventLoop& loop_;
        FakePacingClock& clock_;
        RepeatingTask task_;
        std::atomic<bool> cancelled_{false};
        std::shared_ptr<PacingTimer> caller_pin_;
    };

    EventLoop& inner_;
    FakePacingClock& clock_;
};

// Records every activity the dispatcher delivers, on its delivery thread.
class ActivityRecorder {
  public:
    void Record(const InputActivity& activity) {
        absl::MutexLock observed_lock(observed_mutex_);
        observed_.push_back(activity);
    }

    void AwaitCount(size_t count) {
        const auto has_count = [this, count]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(observed_mutex_) {
            return observed_.size() >= count;
        };
        absl::MutexLock observed_lock(observed_mutex_);
        observed_mutex_.Await(absl::Condition(&has_count));
    }

    // Waits for the sentinel. @return The activities before it, which are
    // forgotten together with the sentinel.
    std::vector<InputActivity> TakeUntilSentinel() {
        absl::MutexLock observed_lock(observed_mutex_);
        observed_mutex_.Await(absl::Condition(this, &ActivityRecorder::HasSentinel));
        auto sentinel = std::ranges::find_if(observed_, IsSentinel);
        std::vector<InputActivity> before(observed_.begin(), sentinel);
        observed_.erase(observed_.begin(), sentinel + 1);
        return before;
    }

  private:
    bool HasSentinel() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(observed_mutex_) {
        return std::ranges::any_of(observed_, IsSentinel);
    }

    absl::Mutex observed_mutex_;
    std::vector<InputActivity> observed_ ABSL_GUARDED_BY(observed_mutex_);
};

// An in-memory reactor whose writes, which arrive on the dispatcher's
// delivery thread, the test can wait for.
template <typename W>
class AwaitableWriteReactor {
  public:
    virtual ~AwaitableWriteReactor() = default;
    virtual void OnWriteDone(bool /*ok*/) {}
    virtual void OnDone() {}
    virtual void OnCancel() {}

    void StartWrite(const W* message) {
        absl::MutexLock reactor_lock(reactor_mutex_);
        written_.push_back(*message);
    }

    void Finish(::grpc::Status status) {
        absl::MutexLock reactor_lock(reactor_mutex_);
        finish_status_ = std::move(status);
    }

    // Waits until at least `count` messages were written. @return Them all.
    std::vector<W> AwaitWritten(size_t count) const {
        const auto written = [this, count]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(reactor_mutex_) {
            return written_.size() >= count;
        };
        absl::MutexLock reactor_lock(reactor_mutex_);
        reactor_mutex_.Await(absl::Condition(&written));
        return written_;
    }

  private:
    mutable absl::Mutex reactor_mutex_;
    std::vector<W> written_ ABSL_GUARDED_BY(reactor_mutex_);
    std::optional<::grpc::Status> finish_status_ ABSL_GUARDED_BY(reactor_mutex_);
};

InputEvent Touch(PointerAction action, std::vector<int32_t> pointer_ids, float x = 0.5F,
                 float y = 0.5F) {
    InputEvent event;
    PointerEvent* pointer = event.mutable_pointer();
    pointer->set_display_id(0);
    pointer->set_action(action);
    for (int32_t id : pointer_ids) {
        PointerEvent::Pointer* p = pointer->add_pointers();
        p->set_pointer_id(id);
        p->set_x(x);
        p->set_y(y);
    }
    return event;
}

InputEvent Text(std::string text, int32_t source_id = 0) {
    InputEvent event;
    event.set_input_source_id(source_id);
    event.mutable_text()->set_text(std::move(text));
    return event;
}

std::vector<int32_t> AgentPointerIds() {
    std::vector<int32_t> ids;
    for (int32_t id = 0; id < kTouchDeviceSlots - kReservedHumanSlots; ++id) {
        ids.push_back(id);
    }
    return ids;
}

// Describes an activity as "src<id> [synthesized ]<event>".
std::string Describe(const InputActivity& activity) {
    const InputEvent& event = activity.event();
    std::string text = absl::StrCat("src", event.input_source_id(),
                                    activity.synthesized() ? " synthesized " : " ");
    switch (event.event_case()) {
    case InputEvent::kPointer:
        absl::StrAppend(&text, PointerAction_Name(event.pointer().action()), " ids");
        for (const PointerEvent::Pointer& pointer : event.pointer().pointers()) {
            absl::StrAppend(&text, " ", pointer.pointer_id());
        }
        break;
    case InputEvent::kKey:
        absl::StrAppend(&text, KeyAction_Name(event.key().action()), " ",
                        event.key().has_dom_code() ? event.key().dom_code()
                                                   : absl::StrCat(event.key().evdev_code()));
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

class InputServiceImplTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dispatcher_loop_ = ThreadedEventLoop::Create(LibuvEventLoop::Create());
        loop_ = std::make_unique<PacingEventLoop>(*dispatcher_loop_, clock_);
        multi_display_ = std::make_unique<FakeMultiDisplay>(dispatcher_loop_.get());
        touch_devices_ =
                std::make_unique<MultiDisplayTouchDevices>(*multi_display_, /*has_hinge=*/false);
        dispatcher_ = std::make_shared<InputDispatcher>(*dispatcher_loop_, *touch_devices_,
                                                        &keyboard_, ResolveEvdevKeyCode, clock_);
        // Never removed: the dispatcher's event source drops it on destruction.
        dispatcher_->activity().AddCallback(
                [this](const InputActivity& activity) { recorder_.Record(activity); });
        StartServer(DefaultOptions());
    }

    void TearDown() override {
        StopServer();
        // Runs the session closes posted so far before the dispatcher goes.
        loop_->WaitUntilIdle();
        dispatcher_->Shutdown();
        dispatcher_.reset();
        for (auto display_weak : multi_display_->Displays()) {
            if (auto display = display_weak.lock()) {
                static_cast<ActiveFakePixmanDisplay*>(display.get())->Stop();
            }
        }
    }

    InputServiceImpl::Options DefaultOptions() {
        InputServiceImpl::Options opts;
        opts.event_loop = loop_.get();
        opts.clock = clock_;
        // Lets the dispatcher apply what was posted before time moves on, so
        // dispatch times are deterministic.
        opts.sleep_fn = [this](absl::Duration d) {
            loop_->WaitUntilIdle();
            clock_.Sleep(d);
        };
        return opts;
    }

    void StartServer(InputServiceImpl::Options opts) {
        StopServer();
        service_ =
                std::make_unique<InputServiceImpl>(dispatcher_, *multi_display_, std::move(opts));
        ::grpc::ServerBuilder builder;
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        channel_ = server_->InProcessChannel(::grpc::ChannelArguments());
        stub_ = InputService::NewStub(channel_);
    }

    void StopServer() {
        stub_.reset();
        channel_.reset();
        if (server_) {
            server_->Shutdown(absl::ToChronoTime(absl::Now() + absl::Seconds(1)));
            server_->Wait();
            server_.reset();
        }
        service_.reset();
    }

    FakePixmanDisplay* GetDisplay(uint8_t display_id = 0) {
        auto res = multi_display_->GetActiveDisplay(display_id, false);
        if (!res.ok()) {
            return nullptr;
        }
        return static_cast<FakePixmanDisplay*>(res.value().get());
    }

    ::grpc::Status Send(std::vector<InputEvent> events, SendInputEventsResponse* resp = nullptr) {
        ::grpc::ServerContext ctx;
        SendInputEventsRequest req;
        for (InputEvent& event : events) {
            *req.add_events() = std::move(event);
        }
        SendInputEventsResponse unused;
        return service_->SendInputEvents(&ctx, &req, resp != nullptr ? resp : &unused);
    }

    // Types the sentinel through a session of its own, from `source_id`.
    void DispatchSentinel(int32_t source_id = kSentinelSource) {
        DispatcherSession sentinel(dispatcher_, {.source_id = source_id});
        sentinel.Dispatch(Text(std::string(kSentinel)));
    }

    // Waits until everything dispatched so far was delivered. @return The
    // activities delivered since the last call.
    std::vector<InputActivity> TakeActivities() {
        DispatchSentinel();
        return recorder_.TakeUntilSentinel();
    }

    std::vector<std::string> Activities() {
        std::vector<std::string> described;
        for (const InputActivity& activity : TakeActivities()) {
            described.push_back(Describe(activity));
        }
        return described;
    }

    // Dispatches a sentinel from `source_id`, then acknowledges the writes of
    // `writer` until it wrote the sentinel. @return The activities before it.
    template <typename Writer>
    std::vector<InputActivity> DrainUntilSentinel(Writer* writer,
                                                  int32_t source_id = kSentinelSource) {
        DispatchSentinel(source_id);
        for (size_t acknowledged = 0;; ++acknowledged) {
            std::vector<InputActivity> written = writer->AwaitWritten(acknowledged + 1);
            if (IsSentinel(written.back())) {
                written.pop_back();
                return written;
            }
            writer->OnWriteDone(true);
        }
    }

    FakePacingClock clock_;
    RecordingKeyboard keyboard_;
    ActivityRecorder recorder_;
    // The dispatcher's loop, with real timers; `loop_` paces the service on it.
    std::unique_ptr<EventLoop> dispatcher_loop_;
    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<FakeMultiDisplay> multi_display_;
    std::unique_ptr<MultiDisplayTouchDevices> touch_devices_;
    std::shared_ptr<InputDispatcher> dispatcher_;
    std::unique_ptr<InputServiceImpl> service_;
    std::unique_ptr<::grpc::Server> server_;
    std::shared_ptr<::grpc::Channel> channel_;
    std::unique_ptr<InputService::Stub> stub_;
};

TEST_F(InputServiceImplTest, SendInputEventsEmptyBatchIsNoOp) {
    ::grpc::ServerContext ctx;
    SendInputEventsRequest req;
    SendInputEventsResponse resp;

    const auto status = service_->SendInputEvents(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.events_dispatched(), 0);
    EXPECT_EQ(resp.first_event_dispatched_at_nanos(), 0);
    EXPECT_EQ(resp.last_event_dispatched_at_nanos(), 0);
    EXPECT_TRUE(clock_.SleepCalls().empty());
}

TEST_F(InputServiceImplTest, SendInputEventsPacesEventsAccordingToTimestamps) {
    // 3-step gesture at t=100ms, t=115ms (+15ms), t=140ms (+25ms from previous, +40ms from start).
    InputEvent down = Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.5F, 0.5F);
    down.set_timestamp_nanos(100'000'000LL);
    InputEvent move = Touch(PointerAction::POINTER_ACTION_MOVE, {0}, 0.6F, 0.6F);
    move.set_timestamp_nanos(115'000'000LL);
    InputEvent up = Touch(PointerAction::POINTER_ACTION_UP, {0}, 0.6F, 0.6F);
    up.set_timestamp_nanos(140'000'000LL);

    SendInputEventsResponse resp;
    const auto status = Send({down, move, up}, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_EQ(resp.events_dispatched(), 3);
    EXPECT_EQ(resp.first_event_dispatched_at_nanos(), 1'000'000'000LL);
    EXPECT_EQ(resp.last_event_dispatched_at_nanos(), 1'040'000'000LL);

    const auto sleeps = clock_.SleepCalls();
    ASSERT_EQ(sleeps.size(), 2);
    EXPECT_EQ(sleeps[0], absl::Milliseconds(15));
    EXPECT_EQ(sleeps[1], absl::Milliseconds(25));
}

TEST_F(InputServiceImplTest, SendInputEventsReportsDispatchTimesOfItsActivity) {
    // Neither the first event nor the last reaches the guest: nothing holds
    // pointer 3, and the text is empty.
    InputEvent unheld_move = Touch(PointerAction::POINTER_ACTION_MOVE, {3});
    InputEvent first = Text("first");
    first.set_timestamp_nanos(0);
    InputEvent last = Text("last");
    last.set_timestamp_nanos(20'000'000LL);
    InputEvent empty = Text("");

    SendInputEventsResponse resp;
    ASSERT_TRUE(Send({unheld_move, first, last, empty}, &resp).ok());

    const std::vector<InputActivity> activities = TakeActivities();
    ASSERT_EQ(activities.size(), 2);
    EXPECT_EQ(resp.events_dispatched(), 4);
    EXPECT_EQ(resp.first_event_dispatched_at_nanos(), activities[0].dispatched_at_nanos());
    EXPECT_EQ(resp.last_event_dispatched_at_nanos(), activities[1].dispatched_at_nanos());
    EXPECT_EQ(resp.last_event_dispatched_at_nanos() - resp.first_event_dispatched_at_nanos(),
              20'000'000LL);
}

TEST_F(InputServiceImplTest, SendInputEventsValidatesBoundsUpFront) {
    // Decreasing timestamp_nanos must fail with INVALID_ARGUMENT without dispatching.
    {
        InputEvent ev1 = Text("a");
        ev1.set_timestamp_nanos(200'000'000LL);
        InputEvent ev2 = Text("b");
        ev2.set_timestamp_nanos(100'000'000LL);

        const auto status = Send({ev1, ev2});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
        EXPECT_THAT(status.error_message(), HasSubstr("events[1]"));
        EXPECT_THAT(status.error_message(), HasSubstr("'100000000'"));
        EXPECT_THAT(status.error_message(), HasSubstr("'200000000'"));
        EXPECT_THAT(keyboard_.Writes(), IsEmpty());
    }

    // Gesture exceeding max_batch_duration (default 30s) must fail with INVALID_ARGUMENT.
    {
        InputEvent ev1 = Text("a");
        ev1.set_timestamp_nanos(0);
        InputEvent ev2 = Text("b");
        ev2.set_timestamp_nanos(31'000'000'000LL);  // 31 seconds > 30s max

        const auto status = Send({ev1, ev2});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
        EXPECT_THAT(status.error_message(), HasSubstr("'31s'"));
        EXPECT_THAT(status.error_message(), HasSubstr("'30s'"));
        EXPECT_THAT(status.error_message(), HasSubstr("StreamInputEvents"));
        EXPECT_THAT(keyboard_.Writes(), IsEmpty());
    }

    // Missing event oneof payload must fail with INVALID_ARGUMENT.
    {
        const auto status = Send({InputEvent()});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
        EXPECT_THAT(status.error_message(), HasSubstr("'pointer', 'key', 'axis', or 'text'"));
    }

    // Unknown display_id must fail with INVALID_ARGUMENT and include quoted display_id and
    // underlying cause.
    {
        InputEvent ev;
        ev.mutable_pointer()->set_display_id(42);
        ev.mutable_pointer()->set_action(PointerAction::POINTER_ACTION_DOWN);

        const auto status = Send({ev});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
        EXPECT_THAT(status.error_message(), HasSubstr("display_id '42'"));
        EXPECT_THAT(status.error_message(), HasSubstr("CapabilitiesService"));
    }

    // Unsupported Android keycode must fail with INVALID_ARGUMENT and list allowed keycodes.
    {
        InputEvent ev;
        ev.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
        ev.mutable_key()->set_android_keycode(999);

        const auto status = Send({ev});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
        EXPECT_THAT(status.error_message(), HasSubstr("android_keycode '999'"));
        EXPECT_THAT(status.error_message(), HasSubstr("3=HOME"));
    }
}

TEST_F(InputServiceImplTest, SendInputEventsStopsAtSlotExhaustionWithEventIndex) {
    // Source 7 takes every slot agents may use, then one more.
    InputEvent fill = Touch(PointerAction::POINTER_ACTION_DOWN, AgentPointerIds());
    fill.set_input_source_id(7);
    fill.set_timestamp_nanos(0);
    InputEvent exhausted = Touch(PointerAction::POINTER_ACTION_DOWN, {9});
    exhausted.set_input_source_id(7);
    exhausted.set_timestamp_nanos(1'000'000LL);
    InputEvent after = Text("after", 7);
    after.set_timestamp_nanos(2'000'000LL);

    const auto status = Send({fill, exhausted, after});

    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_THAT(status.error_message(), HasSubstr("SendInputEvents failed at events[1]"));
    EXPECT_THAT(status.error_message(), HasSubstr("input_source_id '7'"));
    // The event after the failure isn't posted.
    EXPECT_THAT(Activities(), ElementsAre("src7 POINTER_ACTION_DOWN ids 0 1 2 3 4 5 6 7 8"));
    EXPECT_THAT(keyboard_.Writes(), IsEmpty());
}

TEST_F(InputServiceImplTest, SendInputEventsRejectsDownForHeldPointerWithEventIndex) {
    InputEvent down = Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.2F, 0.2F);
    down.set_input_source_id(7);
    InputEvent again = Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.4F, 0.4F);
    again.set_input_source_id(7);

    const auto status = Send({down, again});

    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_THAT(status.error_message(), HasSubstr("SendInputEvents failed at events[1]"));
    EXPECT_THAT(status.error_message(), HasSubstr("Pointer 0"));
    EXPECT_THAT(Activities(), ElementsAre("src7 POINTER_ACTION_DOWN ids 0"));
}

TEST_F(InputServiceImplTest, SendInputEventsKeepsTouchHeldAcrossCallsFromSameSource) {
    InputEvent down = Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.2F, 0.2F);
    down.set_input_source_id(7);
    ASSERT_TRUE(Send({down}).ok());

    // A move only applies to a held pointer.
    InputEvent move = Touch(PointerAction::POINTER_ACTION_MOVE, {0}, 0.4F, 0.4F);
    move.set_input_source_id(7);
    InputEvent up = Touch(PointerAction::POINTER_ACTION_UP, {0}, 0.4F, 0.4F);
    up.set_input_source_id(7);
    SendInputEventsResponse resp;
    ASSERT_TRUE(Send({move}, &resp).ok());
    EXPECT_NE(resp.first_event_dispatched_at_nanos(), 0);
    ASSERT_TRUE(Send({up}).ok());

    EXPECT_THAT(Activities(),
                ElementsAre("src7 POINTER_ACTION_DOWN ids 0", "src7 POINTER_ACTION_MOVE ids 0",
                            "src7 POINTER_ACTION_UP ids 0"));
}

TEST_F(InputServiceImplTest, SendInputEventsFailsForKeyThatIsNotAW3cCode) {
    InputEvent key;
    key.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
    key.mutable_key()->set_dom_code("a");

    const auto status = Send({Text("before"), key});

    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_THAT(status.error_message(), HasSubstr("events[1]"));
    EXPECT_THAT(keyboard_.Writes(), IsEmpty());
}

TEST_F(InputServiceImplTest, RejectsUnsupportedPeripheralsWithFailedPrecondition) {
    auto opts = DefaultOptions();
    opts.hw_keyboard = false;
    opts.hw_rotary_input = false;
    StartServer(std::move(opts));

    {
        InputEvent ev;
        ev.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
        ev.mutable_key()->set_dom_code("KeyA");
        const auto status = Send({ev});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
        EXPECT_THAT(status.error_message(), HasSubstr("hw.keyboard"));
    }

    {
        InputEvent ev;
        ev.mutable_axis()->set_axis(AxisType::AXIS_TYPE_ROTARY_ENCODER);
        ev.mutable_axis()->set_value(1.0F);
        const auto status = Send({ev});
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
        EXPECT_THAT(status.error_message(), HasSubstr("no guest rotary device is registered"));
    }
}

TEST_F(InputServiceImplTest, ReadOnlyModeRejectsWriteRpcsWithPermissionDenied) {
    auto opts = DefaultOptions();
    opts.read_only = true;
    StartServer(std::move(opts));

    const auto send_status = Send({});
    EXPECT_EQ(send_status.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_THAT(send_status.error_message(), HasSubstr("SendInputEvents"));

    ::grpc::ClientContext stream_ctx;
    StreamInputEventsResponse stream_resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &stream_resp);
    writer->WritesDone();
    const auto stream_status = writer->Finish();
    EXPECT_EQ(stream_status.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_THAT(stream_status.error_message(), HasSubstr("StreamInputEvents"));
}

// The slots that got a contact, and those released, in order.
struct SlotChanges {
    std::vector<uint32_t> pressed;
    std::vector<uint32_t> released;
};

SlotChanges SlotChangesOf(const FakePixmanDisplay& display) {
    SlotChanges changes;
    uint32_t current_slot = 0;
    for (const auto& ev : display.evdevs) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
            current_slot = ev.value;
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
            (ev.value == kMtsPointerUp ? changes.released : changes.pressed)
                    .push_back(current_slot);
        }
    }
    return changes;
}

TEST_F(InputServiceImplTest, ConcurrentSourcesWithSamePointerIdUseDistinctSlotsAndIsolateCleanup) {
    // Stream 1 uses x-client-id "agent-1" with input_source_id=0 (auto-derived non-zero ID).
    ::grpc::ClientContext ctx1;
    ctx1.AddMetadata("x-client-id", "agent-1");
    StreamInputEventsResponse resp1;
    auto writer1 = stub_->StreamInputEvents(&ctx1, &resp1);

    // Stream 2 uses x-client-id "agent-2" with input_source_id=0 (auto-derived non-zero ID).
    ::grpc::ClientContext ctx2;
    ctx2.AddMetadata("x-client-id", "agent-2");
    StreamInputEventsResponse resp2;
    auto writer2 = stub_->StreamInputEvents(&ctx2, &resp2);

    const int32_t expected_source1 = InputServiceImpl::DeriveSourceIdFromClientId("agent-1");
    const int32_t expected_source2 = InputServiceImpl::DeriveSourceIdFromClientId("agent-2");
    ASSERT_NE(expected_source1, 0);
    ASSERT_NE(expected_source2, 0);
    ASSERT_NE(expected_source1, expected_source2);

    // Agent 1 touches down with pointer_id = 0 and holds down KeyA.
    ASSERT_TRUE(writer1->Write(Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.2F, 0.2F)));
    InputEvent key_down1;
    key_down1.mutable_key()->set_action(KeyAction::KEY_ACTION_DOWN);
    key_down1.mutable_key()->set_dom_code("KeyA");
    ASSERT_TRUE(writer1->Write(key_down1));
    keyboard_.AwaitWrite(absl::StrCat("down ", KEY_A));

    // Agent 2 simultaneously touches down with pointer_id = 0 and sends a text marker.
    ASSERT_TRUE(writer2->Write(Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.8F, 0.8F)));
    ASSERT_TRUE(writer2->Write(Text("ready")));
    keyboard_.AwaitWrite("text ready");

    // Close Stream 1 while its pointer 0 and KeyA are still depressed.
    ASSERT_TRUE(writer1->WritesDone());
    ASSERT_TRUE(writer1->Finish().ok());
    EXPECT_EQ(resp1.events_dispatched(), 2);

    // Agent 1's input, agent 2's input, then the release of agent 1's input.
    const std::vector<InputActivity> activities = TakeActivities();
    const std::string src1 = absl::StrCat("src", expected_source1);
    const std::string src2 = absl::StrCat("src", expected_source2);
    std::vector<std::string> described;
    for (const InputActivity& activity : activities) {
        described.push_back(Describe(activity));
    }
    EXPECT_THAT(described,
                ElementsAre(src1 + " POINTER_ACTION_DOWN ids 0", src1 + " KEY_ACTION_DOWN KeyA",
                            src2 + " POINTER_ACTION_DOWN ids 0", src2 + " text ready",
                            src1 + " synthesized POINTER_ACTION_CANCEL ids 0",
                            absl::StrCat(src1, " synthesized KEY_ACTION_UP ", KEY_A)));
    EXPECT_THAT(keyboard_.Writes(), ElementsAre(absl::StrCat("down ", KEY_A), "text ready",
                                                absl::StrCat("up ", KEY_A)));

    // Agent 1 and Agent 2 got distinct guest multi-touch slots, and only agent 1's slot was
    // released when it disconnected.
    auto* display = GetDisplay(0);
    ASSERT_NE(display, nullptr);
    const SlotChanges changes = SlotChangesOf(*display);
    ASSERT_EQ(changes.pressed.size(), 2);
    EXPECT_NE(changes.pressed[0], changes.pressed[1]);
    EXPECT_THAT(changes.released, ElementsAre(changes.pressed[0]));

    // Now finish Stream 2; its held pointer should be released on exit.
    ASSERT_TRUE(writer2->WritesDone());
    ASSERT_TRUE(writer2->Finish().ok());
    EXPECT_EQ(resp2.events_dispatched(), 2);
    EXPECT_THAT(Activities(), ElementsAre(src2 + " synthesized POINTER_ACTION_CANCEL ids 0"));
}

TEST_F(InputServiceImplTest, StreamDisconnectOnlyReleasesSourcesUsedByTheStream) {
    // Source 1 holds a touch down across batch calls.
    InputEvent touch_down = Touch(PointerAction::POINTER_ACTION_DOWN, {0});
    touch_down.set_input_source_id(1);
    ASSERT_TRUE(Send({touch_down}).ok());

    // A stream without x-client-id touches down as source 7, then disconnects.
    ::grpc::ClientContext stream_ctx;
    StreamInputEventsResponse stream_resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &stream_resp);
    touch_down.set_input_source_id(7);
    ASSERT_TRUE(writer->Write(touch_down));
    ASSERT_TRUE(writer->WritesDone());
    ASSERT_TRUE(writer->Finish().ok());

    // Only the stream's own contact is cancelled; source 1's touch stays down.
    std::vector<int32_t> synthesized_sources;
    for (const InputActivity& activity : TakeActivities()) {
        if (activity.synthesized()) {
            synthesized_sources.push_back(activity.event().input_source_id());
        }
    }
    EXPECT_THAT(synthesized_sources, ElementsAre(7));
}

TEST_F(InputServiceImplTest, AnonymousSendInputEventsReleasesHeldInputOnReturn) {
    InputEvent touch_down = Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.2F, 0.2F);
    InputEvent key_down;
    key_down.mutable_key()->set_action(KeyAction::KEY_ACTION_DOWN);
    key_down.mutable_key()->set_dom_code("KeyA");

    ASSERT_TRUE(Send({touch_down, key_down}).ok());

    EXPECT_THAT(Activities(),
                ElementsAre("src0 POINTER_ACTION_DOWN ids 0", "src0 KEY_ACTION_DOWN KeyA",
                            "src0 synthesized POINTER_ACTION_CANCEL ids 0",
                            absl::StrCat("src0 synthesized KEY_ACTION_UP ", KEY_A)));
    EXPECT_THAT(keyboard_.Writes(),
                ElementsAre(absl::StrCat("down ", KEY_A), absl::StrCat("up ", KEY_A)));

    // A subsequent anonymous call gets a fresh session where pointer 0 is not held.
    ASSERT_TRUE(Send({touch_down, Touch(PointerAction::POINTER_ACTION_UP, {0}, 0.2F, 0.2F)}).ok());
}

TEST_F(InputServiceImplTest, IdentifiedSendInputEventsExpiresHeldInputAfterIdleTimeout) {
    auto opts = DefaultOptions();
    opts.held_input_idle_timeout = absl::Milliseconds(30);
    StartServer(std::move(opts));

    InputEvent touch_down = Touch(PointerAction::POINTER_ACTION_DOWN, {0}, 0.2F, 0.2F);
    touch_down.set_input_source_id(7);
    InputEvent key_down;
    key_down.set_input_source_id(7);
    key_down.mutable_key()->set_action(KeyAction::KEY_ACTION_DOWN);
    key_down.mutable_key()->set_dom_code("KeyA");

    ASSERT_TRUE(Send({touch_down, key_down}).ok());

    // Wait for the idle timer to release KeyA and cancel pointer 0.
    keyboard_.AwaitWrite(absl::StrCat("up ", KEY_A));
    EXPECT_THAT(Activities(),
                ElementsAre("src7 POINTER_ACTION_DOWN ids 0", "src7 KEY_ACTION_DOWN KeyA",
                            "src7 synthesized POINTER_ACTION_CANCEL ids 0",
                            absl::StrCat("src7 synthesized KEY_ACTION_UP ", KEY_A)));

    // Pointer 0 is no longer held on source 7's session, so a new DOWN succeeds.
    ASSERT_TRUE(Send({touch_down}).ok());
}

TEST_F(InputServiceImplTest, StreamInputEventsFinishesWithFirstDispatchError) {
    ::grpc::ClientContext stream_ctx;
    StreamInputEventsResponse resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &resp);
    InputEvent fill = Touch(PointerAction::POINTER_ACTION_DOWN, AgentPointerIds());
    fill.set_input_source_id(7);
    InputEvent exhausted = Touch(PointerAction::POINTER_ACTION_DOWN, {9});
    exhausted.set_input_source_id(7);
    ASSERT_TRUE(writer->Write(fill));
    ASSERT_TRUE(writer->Write(exhausted));
    writer->WritesDone();

    const ::grpc::Status status = writer->Finish();

    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_THAT(status.error_message(), HasSubstr("StreamInputEvents failed at event #1"));
    EXPECT_THAT(status.error_message(), HasSubstr("input_source_id '7'"));
}

TEST_F(InputServiceImplTest, StreamInputEventsFinishesOnDispatchFailureWhileClientIsIdle) {
    // The failure completes on the dispatcher's loop after the read returned.
    // The client sends nothing more and never half-closes, so only the server
    // ending the stream on its own lets Finish() return before the deadline.
    ::grpc::ClientContext stream_ctx;
    stream_ctx.set_deadline(absl::ToChronoTime(absl::Now() + absl::Seconds(10)));
    StreamInputEventsResponse resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &resp);
    InputEvent fill = Touch(PointerAction::POINTER_ACTION_DOWN, AgentPointerIds());
    fill.set_input_source_id(7);
    InputEvent exhausted = Touch(PointerAction::POINTER_ACTION_DOWN, {9});
    exhausted.set_input_source_id(7);
    ASSERT_TRUE(writer->Write(fill));
    ASSERT_TRUE(writer->Write(exhausted));

    const ::grpc::Status status = writer->Finish();

    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_THAT(status.error_message(), HasSubstr("StreamInputEvents failed at event #1"));
    EXPECT_THAT(status.error_message(), HasSubstr("input_source_id '7'"));
}

TEST_F(InputServiceImplTest, StreamInputEventsFinishesOnDispatchFailureWhilePacing) {
    // The failing event is followed by one that waits on a pacing timer, so the
    // failure completes while the stream is between reads. The stream must end
    // with the failure, and the timer must neither dispatch nor read again.
    ::grpc::ClientContext stream_ctx;
    stream_ctx.set_deadline(absl::ToChronoTime(absl::Now() + absl::Seconds(10)));
    StreamInputEventsResponse resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &resp);
    InputEvent fill = Touch(PointerAction::POINTER_ACTION_DOWN, AgentPointerIds());
    fill.set_input_source_id(7);
    fill.set_timestamp_nanos(0);
    InputEvent exhausted = Touch(PointerAction::POINTER_ACTION_DOWN, {9});
    exhausted.set_input_source_id(7);
    exhausted.set_timestamp_nanos(0);
    InputEvent later = Text("later", 7);
    later.set_timestamp_nanos(500'000'000LL);
    ASSERT_TRUE(writer->Write(fill));
    ASSERT_TRUE(writer->Write(exhausted));
    // May fail: the failure can end the stream before this write arrives.
    // Either way the stream must finish with the failure below.
    writer->Write(later);

    const ::grpc::Status status = writer->Finish();

    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_THAT(status.error_message(), HasSubstr("StreamInputEvents failed at event #1"));
    EXPECT_THAT(keyboard_.Writes(), IsEmpty());
}

TEST_F(InputServiceImplTest, PointerlessHoverExitReleasesHoveringContacts) {
    InputEvent hover_exit;
    hover_exit.mutable_pointer()->set_action(PointerAction::POINTER_ACTION_HOVER_EXIT);
    ASSERT_TRUE(Send({Touch(PointerAction::POINTER_ACTION_HOVER_MOVE, {0}), hover_exit}).ok());

    auto* display = GetDisplay(0);
    ASSERT_NE(display, nullptr);
    EXPECT_THAT(display->evdevs, Contains(FieldsAre(EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp)));
}

TEST_F(InputServiceImplTest, StreamInputActivityFiltersBySourceAndEnforcesObservationPolicy) {
    // Subscribe with source filter = 0 (human operator only).
    StreamInputActivityRequest filter_req;
    filter_req.set_input_source_id(0);
    auto* human_observer =
            service_->CreateInputActivityWriter<AwaitableWriteReactor<InputActivity>>(filter_req);

    // Send an agent event (source = 7) followed by a human event (source = 0).
    ASSERT_TRUE(Send({Text("agent", 7), Text("human", 0)}).ok());

    const std::vector<InputActivity> observed = DrainUntilSentinel(human_observer, /*source_id=*/0);
    ASSERT_EQ(observed.size(), 1);
    EXPECT_EQ(observed[0].event().input_source_id(), 0);
    EXPECT_EQ(observed[0].event().text().text(), "human");
    human_observer->OnDone();

    // When supports_input_activity_observation is false, subscribing must fail with
    // PERMISSION_DENIED.
    auto opts = DefaultOptions();
    opts.supports_input_activity_observation = false;
    StartServer(std::move(opts));

    ::grpc::ClientContext denied_ctx;
    auto denied_reader = stub_->StreamInputActivity(&denied_ctx, StreamInputActivityRequest());
    InputActivity unused;
    EXPECT_FALSE(denied_reader->Read(&unused));
    const ::grpc::Status denied_status = denied_reader->Finish();
    EXPECT_EQ(denied_status.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_THAT(denied_status.error_message(), HasSubstr("supports_input_activity_observation"));
}

TEST_F(InputServiceImplTest, StreamInputActivitySeesInputOfOtherDispatcherSessions) {
    auto* observer = service_->CreateInputActivityWriter<AwaitableWriteReactor<InputActivity>>(
            StreamInputActivityRequest());

    // Another client of the dispatcher, such as a WebRTC v2 channel.
    {
        DispatcherSession web_rtc(dispatcher_, {.human = true, .source_id = 0});
        web_rtc.Dispatch(Touch(PointerAction::POINTER_ACTION_DOWN, {0}));
    }

    std::vector<std::string> described;
    for (const InputActivity& activity : DrainUntilSentinel(observer)) {
        described.push_back(Describe(activity));
    }
    EXPECT_THAT(described, ElementsAre("src0 POINTER_ACTION_DOWN ids 0",
                                       "src0 synthesized POINTER_ACTION_CANCEL ids 0"));
    observer->OnDone();
}

TEST_F(InputServiceImplTest, StreamInputEventsPacesAndCapsStaleInterBurstDeltas) {
    ::grpc::ClientContext stream_ctx;
    StreamInputEventsResponse resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &resp);

    // Event 1 at t=10ms.
    InputEvent ev1 = Text("1");
    ev1.set_timestamp_nanos(10'000'000LL);
    ASSERT_TRUE(writer->Write(ev1));

    // Event 2 at t=30ms (+20ms delta -> sleeps 20ms).
    InputEvent ev2 = Text("2");
    ev2.set_timestamp_nanos(30'000'000LL);
    ASSERT_TRUE(writer->Write(ev2));

    // Event 3 at t=10,030ms (+10s delta -> capped at max_stream_step_duration = 1s).
    InputEvent ev3 = Text("3");
    ev3.set_timestamp_nanos(10'030'000'000LL);
    ASSERT_TRUE(writer->Write(ev3));

    ASSERT_TRUE(writer->WritesDone());
    ASSERT_TRUE(writer->Finish().ok());
    EXPECT_EQ(resp.events_dispatched(), 3);

    const auto sleeps = clock_.SleepCalls();
    ASSERT_EQ(sleeps.size(), 2);
    EXPECT_EQ(sleeps[0], absl::Milliseconds(20));
    EXPECT_EQ(sleeps[1], absl::Seconds(1));
}

TEST_F(InputServiceImplTest, StreamInputEventsCancellationReleasesHeldContactsCleanly) {
    EXPECT_NE(InputServiceImpl::DeriveSourceIdFromClientId("测试_🚀_é"), 0);

    ::grpc::ClientContext stream_ctx;
    stream_ctx.AddMetadata("x-client-id", "cancelled-client");
    StreamInputEventsResponse resp;
    auto writer = stub_->StreamInputEvents(&stream_ctx, &resp);

    const int32_t expected_source =
            InputServiceImpl::DeriveSourceIdFromClientId("cancelled-client");
    InputEvent down = Touch(PointerAction::POINTER_ACTION_DOWN, {0});
    down.set_timestamp_nanos(10'000'000LL);
    ASSERT_TRUE(writer->Write(down));

    InputEvent marker = Text("测试_🚀_é");
    marker.set_timestamp_nanos(20'000'000LL);
    ASSERT_TRUE(writer->Write(marker));
    keyboard_.AwaitWrite("text 测试_🚀_é");

    stream_ctx.TryCancel();
    const ::grpc::Status status = writer->Finish();
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::CANCELLED);

    // Client-side Finish() on a cancelled stream can return before the server's
    // async OnCancel()/OnDone() callback runs; wait for the synthesized cancel.
    recorder_.AwaitCount(3);
    const std::string src = absl::StrCat("src", expected_source);
    EXPECT_THAT(Activities(),
                ElementsAre(src + " POINTER_ACTION_DOWN ids 0", src + " text 测试_🚀_é",
                            src + " synthesized POINTER_ACTION_CANCEL ids 0"));
}

TEST_F(InputServiceImplTest,
       ConcurrentDispatchesPreserveStrictActivityOrderAndMonotonicTimestamps) {
    clock_.SetAutoAdvance(absl::Nanoseconds(1));

    auto* activity_writer =
            service_->CreateInputActivityWriter<AwaitableWriteReactor<InputActivity>>(
                    StreamInputActivityRequest());

    constexpr int kNumThreads = 4;
    constexpr int kEventsPerThread = 25;
    std::atomic<bool> start_barrier{false};
    std::vector<std::thread> workers;
    workers.reserve(kNumThreads);
    for (int t = 0; t < kNumThreads; ++t) {
        workers.emplace_back([this, t, &start_barrier]() {
            while (!start_barrier.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kEventsPerThread; ++i) {
                ::grpc::ServerContext ctx;
                SendInputEventsRequest req;
                auto* ev = req.add_events();
                ev->set_input_source_id(t + 1);
                ev->mutable_text()->set_text(absl::StrCat("测试_🚀_é_", t, "_", i));
                SendInputEventsResponse resp;
                EXPECT_TRUE(service_->SendInputEvents(&ctx, &req, &resp).ok());
            }
        });
    }
    start_barrier.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    const std::vector<InputActivity> observed = DrainUntilSentinel(activity_writer);
    const std::vector<std::string> hw_writes = keyboard_.Writes();
    ASSERT_EQ(hw_writes.size(), kNumThreads * kEventsPerThread);
    ASSERT_EQ(observed.size(), hw_writes.size());
    for (size_t i = 0; i < hw_writes.size(); ++i) {
        EXPECT_EQ(absl::StrCat("text ", observed[i].event().text().text()), hw_writes[i])
                << "Activity order diverged from hardware dispatch order at index " << i;
        if (i > 0) {
            EXPECT_GT(observed[i].dispatched_at_nanos(), observed[i - 1].dispatched_at_nanos())
                    << "Non-monotonic dispatched_at_nanos at index " << i;
        }
    }
    activity_writer->OnDone();
}

}  // namespace
}  // namespace goldfish::grpc::v2
