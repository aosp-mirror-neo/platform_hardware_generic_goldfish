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

#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "android/emulation/control/emulator_controller.h"
#include "android/emulation/control/keyboard/key_code_resolver.h"
#include "emulator/plugin/grpc/input/contact_table.h"
#include "emulator/plugin/grpc/input/input_dispatcher.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/keyboard_sink.h"
#include "emulator/plugin/grpc/input/multi_display_touch_devices.h"
#include "emulator_controller.grpc.pb.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "goldfish/system/steady_clock.h"
#include "goldfish/system/testing/fake_clocks.h"
#include "standard-headers/linux/input-event-codes.h"

#ifdef send
#undef send
#endif

namespace android::emulation::control {
namespace {

using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::display::DisplayId;
using ::goldfish::display::test::ActiveFakePixmanDisplay;
using ::goldfish::display::test::FakeMultiDisplay;
using ::goldfish::grpc::v2::IInputDispatcher;
using ::goldfish::grpc::v2::InputDispatcher;
using ::goldfish::grpc::v2::KeyboardSink;
using ::goldfish::grpc::v2::kTouchDeviceSlots;
using ::goldfish::grpc::v2::MultiDisplayTouchDevices;
using ::google::protobuf::Empty;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class RecordingKeyboardSink final : public KeyboardSink {
  public:
    void WriteKey(uint32_t evdev_code, bool down) override ABSL_LOCKS_EXCLUDED(writes_mutex_) {
        const absl::MutexLock writes_lock(writes_mutex_);
        writes_.push_back(absl::StrCat(down ? "down " : "up ", evdev_code));
    }

    void TypeText(std::string_view utf8) override ABSL_LOCKS_EXCLUDED(writes_mutex_) {
        const absl::MutexLock writes_lock(writes_mutex_);
        writes_.push_back(absl::StrCat("text ", utf8));
    }

    void Reset() override ABSL_LOCKS_EXCLUDED(writes_mutex_) {
        const absl::MutexLock writes_lock(writes_mutex_);
        writes_.emplace_back("reset");
    }

    std::vector<std::string> Writes() const ABSL_LOCKS_EXCLUDED(writes_mutex_) {
        const absl::MutexLock writes_lock(writes_mutex_);
        return writes_;
    }

    void Clear() ABSL_LOCKS_EXCLUDED(writes_mutex_) {
        const absl::MutexLock writes_lock(writes_mutex_);
        writes_.clear();
    }

  private:
    mutable absl::Mutex writes_mutex_;
    std::vector<std::string> writes_ ABSL_GUARDED_BY(writes_mutex_);
};

class TestAvdClockHolder {
  protected:
    ::goldfish::system::testing::FakeVirtualClock virtual_clock_;
};

class TestInputUniverse final : private TestAvdClockHolder, public ::goldfish::avd::AvdUniverse {
  public:
    TestInputUniverse(::goldfish::display::IMultiDisplay* multi_display,
                      ::goldfish::async::EventLoop& loop,
                      std::shared_ptr<IInputDispatcher> dispatcher)
            : ::goldfish::avd::AvdUniverse(std::make_unique<::goldfish::avd::AvdProperties>(),
                                           virtual_clock_)
            , multi_display_(multi_display)
            , loop_(loop)
            , dispatcher_(std::move(dispatcher)) {}

    ::goldfish::async::EventLoop& GetQemuEventLoop() override { return loop_; }
    ::goldfish::metrics::MetricsReporter& GetMetricsReporter() override {
        LOG(FATAL) << "GetMetricsReporter not implemented in tests";
    }
    ::android::goldfish::VmOperations& GetVmOperations() override {
        LOG(FATAL) << "GetVmOperations not implemented in tests";
    }
    ::goldfish::display::IMultiDisplay& GetMultiDisplay() const override { return *multi_display_; }
    std::shared_ptr<IInputDispatcher> GetInputDispatcher() override { return dispatcher_; }

    absl::Status OnSave(::goldfish::archive::IWriter& /*writer*/) const override {
        return absl::OkStatus();
    }
    absl::Status OnLoad(::goldfish::archive::IReader& /*reader*/) override {
        return absl::OkStatus();
    }

  private:
    ::goldfish::display::IMultiDisplay* multi_display_;
    ::goldfish::async::EventLoop& loop_;
    std::shared_ptr<IInputDispatcher> dispatcher_;
};

// Wraps `TestEventLoop` with a condition variable counter so tests can wait
// deterministically (without `sleep_for` or busy-spinning) for asynchronous
// gRPC callback server threads to post tasks onto the manual QEMU loop.
class NotifyingTestEventLoop final : public ::goldfish::async::EventLoop {
  public:
    NotifyingTestEventLoop()
            : ::goldfish::async::EventLoop("NotifyingTestEventLoop")
            , inner_(TestEventLoop::Create("qemu")) {}

    void RunAll() { inner_->RunAll(); }
    void AdvanceClock(absl::Duration duration) { inner_->AdvanceClock(duration); }

    uint64_t PostedCount() const ABSL_LOCKS_EXCLUDED(post_mutex_) {
        const absl::MutexLock post_lock(post_mutex_);
        return posted_count_;
    }

    void WaitForPostAfter(uint64_t seen) const ABSL_LOCKS_EXCLUDED(post_mutex_) {
        const auto has_new_post = [this, seen]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(post_mutex_) {
            return posted_count_ > seen;
        };
        const absl::MutexLock post_lock(post_mutex_);
        post_mutex_.Await(absl::Condition(&has_new_post));
    }

    void ShutdownTimers() override { inner_->ShutdownTimers(); }
    size_t WaitUntilIdle() override { return inner_->WaitUntilIdle(); }
    std::future<absl::Status> Shutdown() override { return inner_->Shutdown(); }
    bool IsOnLoopThread() const override { return inner_->IsOnLoopThread(); }
    std::shared_ptr<Timer> CreateTimer(RepeatingTask task) override {
        return inner_->CreateTimer(std::move(task));
    }

  protected:
    absl::Status PostImmediately(Task task, ::android::crashreport::FlowId /*flow_id*/) override
            ABSL_LOCKS_EXCLUDED(post_mutex_) {
        absl::Status status = inner_->Post(std::move(task));
        const absl::MutexLock post_lock(post_mutex_);
        ++posted_count_;
        return status;
    }

    absl::Status PostDelayed(Task task, absl::Duration delay,
                             ::android::crashreport::FlowId /*flow_id*/) override
            ABSL_LOCKS_EXCLUDED(post_mutex_) {
        absl::Status status = inner_->Post(std::move(task), delay);
        const absl::MutexLock post_lock(post_mutex_);
        ++posted_count_;
        return status;
    }

  private:
    std::unique_ptr<TestEventLoop> inner_;
    mutable absl::Mutex post_mutex_;
    uint64_t posted_count_ ABSL_GUARDED_BY(post_mutex_) = 0;
};

class EmulatorControllerInputTest : public ::testing::Test {
  protected:
    static constexpr size_t kMaxPendingPerSession = 4096;

    void SetUp() override {
        loop_ = std::make_unique<NotifyingTestEventLoop>();
        multi_display_ = std::make_unique<FakeMultiDisplay>(loop_.get());
        ASSERT_TRUE(multi_display_->CreateDisplay(1, 1920, 1080, 160, 0).ok());
        loop_->RunAll();
        Display(0)->evdev_reports.clear();
        Display(1)->evdev_reports.clear();

        touch_devices_ =
                std::make_unique<MultiDisplayTouchDevices>(*multi_display_, /*has_hinge=*/false);
        dispatcher_ = std::make_shared<InputDispatcher>(
                *loop_, *touch_devices_, &keyboard_sink_, keyboard::ResolveEvdevKeyCode,
                ::goldfish::system::SteadyClock::GetRealClock(), kMaxPendingPerSession);
        universe_ = std::make_unique<TestInputUniverse>(multi_display_.get(), *loop_, dispatcher_);
        service_ = getEmulatorController(/*vm_interface=*/nullptr, universe_.get(),
                                         multi_display_.get());

        grpc::ServerBuilder builder;
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = EmulatorController::NewStub(server_->InProcessChannel(grpc::ChannelArguments()));
        loop_->RunAll();
    }

    void TearDown() override {
        stub_.reset();
        if (server_ != nullptr) {
            server_->Shutdown();
            server_->Wait();
            server_.reset();
        }
        service_.reset();
        loop_->RunAll();
        dispatcher_->Shutdown();
        for (const auto& weak : multi_display_->Displays()) {
            if (auto display = weak.lock()) {
                static_cast<ActiveFakePixmanDisplay*>(display.get())->Stop();
            }
        }
    }

    template <typename Pred>
    void PumpUntil(Pred&& pred) {
        while (true) {
            const uint64_t seen = loop_->PostedCount();
            loop_->RunAll();
            if (pred()) {
                return;
            }
            loop_->WaitForPostAfter(seen);
        }
    }

    std::shared_ptr<ActiveFakePixmanDisplay> Display(DisplayId id) {
        return multi_display_->GetDisplay<ActiveFakePixmanDisplay>(multi_display_->GetDisplay(id));
    }

    int ActiveSlotCount(DisplayId display_id) {
        int active = 0;
        uint32_t selected_slot = 0;
        std::array<int32_t, kTouchDeviceSlots> tracking_ids{};
        tracking_ids.fill(-1);
        for (const auto& report : Display(display_id)->evdev_reports) {
            for (const auto& ev : report) {
                if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
                    selected_slot = ev.value;
                } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID &&
                           selected_slot < tracking_ids.size()) {
                    tracking_ids[selected_slot] = static_cast<int32_t>(ev.value);
                }
            }
        }
        for (const int32_t id : tracking_ids) {
            if (id != -1) {
                ++active;
            }
        }
        return active;
    }

    std::unique_ptr<NotifyingTestEventLoop> loop_;
    std::unique_ptr<FakeMultiDisplay> multi_display_;
    RecordingKeyboardSink keyboard_sink_;
    std::unique_ptr<MultiDisplayTouchDevices> touch_devices_;
    std::shared_ptr<InputDispatcher> dispatcher_;
    std::unique_ptr<TestInputUniverse> universe_;
    std::shared_ptr<grpc::Service> service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<EmulatorController::Stub> stub_;
};

TEST_F(EmulatorControllerInputTest, UnarySendTouchSendMouseAndSendKeyPersistAcrossRpcs) {
    // 1. Multi-RPC unary touch drag (emu-dev-cli sends identifier=1, pressure=1024 -> 0).
    {
        grpc::ClientContext ctx;
        TouchEvent down;
        Touch* t = down.add_touches();
        t->set_identifier(1);
        t->set_x(100);
        t->set_y(150);
        t->set_pressure(1024);
        Empty reply;
        ASSERT_TRUE(stub_->sendTouch(&ctx, down, &reply).ok());
    }
    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 1);

    {
        grpc::ClientContext ctx;
        TouchEvent move;
        Touch* t = move.add_touches();
        t->set_identifier(1);
        t->set_x(200);
        t->set_y(250);
        t->set_pressure(1024);
        Empty reply;
        ASSERT_TRUE(stub_->sendTouch(&ctx, move, &reply).ok());
    }
    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 1);

    {
        grpc::ClientContext ctx;
        TouchEvent up;
        Touch* t = up.add_touches();
        t->set_identifier(1);
        t->set_x(200);
        t->set_y(250);
        t->set_pressure(0);
        Empty reply;
        ASSERT_TRUE(stub_->sendTouch(&ctx, up, &reply).ok());
    }
    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 0);

    // 2. Multi-RPC unary mouse press -> drag -> release.
    for (const int32_t buttons : {1, 1, 0}) {
        grpc::ClientContext ctx;
        MouseEvent mouse;
        mouse.set_x(120);
        mouse.set_y(180);
        mouse.set_buttons(buttons);
        Empty reply;
        ASSERT_TRUE(stub_->sendMouse(&ctx, mouse, &reply).ok());
        loop_->RunAll();
    }
    EXPECT_EQ(ActiveSlotCount(0), 0);

    // 3. Multi-RPC unary keydown -> keyup and Unicode text typing.
    {
        grpc::ClientContext ctx;
        KeyboardEvent down;
        down.set_key("KeyA");
        down.set_eventtype(KeyboardEvent::keydown);
        Empty reply;
        ASSERT_TRUE(stub_->sendKey(&ctx, down, &reply).ok());
    }
    {
        grpc::ClientContext ctx;
        KeyboardEvent up;
        up.set_key("KeyA");
        up.set_eventtype(KeyboardEvent::keyup);
        Empty reply;
        ASSERT_TRUE(stub_->sendKey(&ctx, up, &reply).ok());
    }
    {
        grpc::ClientContext ctx;
        KeyboardEvent text;
        text.set_text("测试_🚀_é");
        Empty reply;
        ASSERT_TRUE(stub_->sendKey(&ctx, text, &reply).ok());
    }
    loop_->RunAll();
    EXPECT_THAT(keyboard_sink_.Writes(), ElementsAre(absl::StrCat("down ", KEY_A),
                                                     absl::StrCat("up ", KEY_A), "text 测试_🚀_é"));
}

TEST_F(EmulatorControllerInputTest,
       UnaryIdleTimeoutReleasesStuckContactsAndRecoversFreshDownAndExternalIdPool) {
    // Hold mouse pointer 0 on Display 0, exhaust all 10 external touch IDs (100..109) on
    // Display 1, and hold KEY_A down without releasing.
    {
        grpc::ClientContext ctx;
        MouseEvent mouse;
        mouse.set_display(0);
        mouse.set_x(100);
        mouse.set_y(100);
        mouse.set_buttons(1);
        Empty reply;
        ASSERT_TRUE(stub_->sendMouse(&ctx, mouse, &reply).ok());
    }
    {
        grpc::ClientContext ctx;
        TouchEvent touch;
        touch.set_display(1);
        for (int i = 0; i < 10; ++i) {
            Touch* t = touch.add_touches();
            t->set_identifier(100 + i);
            t->set_x(50 + i * 10);
            t->set_y(50);
            t->set_pressure(500);
        }
        Empty reply;
        ASSERT_TRUE(stub_->sendTouch(&ctx, touch, &reply).ok());
    }
    {
        grpc::ClientContext ctx;
        KeyboardEvent key;
        key.set_key("KeyA");
        key.set_eventtype(KeyboardEvent::keydown);
        Empty reply;
        ASSERT_TRUE(stub_->sendKey(&ctx, key, &reply).ok());
    }
    loop_->RunAll();
    ASSERT_EQ(ActiveSlotCount(0), 1);
    ASSERT_EQ(ActiveSlotCount(1), 10);
    keyboard_sink_.Clear();

    // Advance past the 60s unary session idle timeout: contacts and held keys are released,
    // and SessionResetTracker records a full reset.
    loop_->AdvanceClock(absl::Seconds(60));
    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 0);
    EXPECT_EQ(ActiveSlotCount(1), 0);
    EXPECT_THAT(keyboard_sink_.Writes(), ElementsAre(absl::StrCat("up ", KEY_A)));

    // Subsequent unary mouse press on Display 0 and a brand-new external touch ID (200) on
    // Display 1 sync the reset tracker and acquire fresh DOWN contacts instead of failing.
    {
        grpc::ClientContext ctx;
        MouseEvent mouse;
        mouse.set_display(0);
        mouse.set_x(150);
        mouse.set_y(150);
        mouse.set_buttons(1);
        Empty reply;
        ASSERT_TRUE(stub_->sendMouse(&ctx, mouse, &reply).ok());
    }
    {
        grpc::ClientContext ctx;
        TouchEvent touch;
        touch.set_display(1);
        Touch* t = touch.add_touches();
        t->set_identifier(200);
        t->set_x(300);
        t->set_y(300);
        t->set_pressure(500);
        Empty reply;
        ASSERT_TRUE(stub_->sendTouch(&ctx, touch, &reply).ok());
    }
    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 1);
    EXPECT_EQ(ActiveSlotCount(1), 1);
}

TEST_F(EmulatorControllerInputTest,
       ConcurrentStreamInputEventCallsIsolatePointerZeroAndCloseCleanly) {
    grpc::ClientContext ctx_a;
    Empty reply_a;
    auto stream_a = stub_->streamInputEvent(&ctx_a, &reply_a);

    grpc::ClientContext ctx_b;
    Empty reply_b;
    auto stream_b = stub_->streamInputEvent(&ctx_b, &reply_b);

    InputEvent ev_a;
    Touch* ta = ev_a.mutable_touch_event()->add_touches();
    ta->set_identifier(0);
    ta->set_x(100);
    ta->set_y(100);
    ta->set_pressure(500);
    ASSERT_TRUE(stream_a->Write(ev_a));

    InputEvent key_a;
    key_a.mutable_key_event()->set_key("KeyB");
    key_a.mutable_key_event()->set_eventtype(KeyboardEvent::keydown);
    ASSERT_TRUE(stream_a->Write(key_a));

    InputEvent ev_b;
    Touch* tb = ev_b.mutable_touch_event()->add_touches();
    tb->set_identifier(0);
    tb->set_x(300);
    tb->set_y(300);
    tb->set_pressure(500);
    ASSERT_TRUE(stream_b->Write(ev_b));

    // Wait until both open streams have delivered their initial DOWN events to distinct slots.
    PumpUntil([&]() { return ActiveSlotCount(0) == 2 && keyboard_sink_.Writes().size() == 1; });
    EXPECT_EQ(ActiveSlotCount(0), 2);
    EXPECT_THAT(keyboard_sink_.Writes(), ElementsAre(absl::StrCat("down ", KEY_B)));

    // Close stream A while stream B remains open: stream A's touch and key are released
    // before Finish() returns OK, leaving stream B's pointer 0 active.
    ASSERT_TRUE(stream_a->WritesDone());
    EXPECT_TRUE(stream_a->Finish().ok());
    loop_->RunAll();

    EXPECT_EQ(ActiveSlotCount(0), 1);
    EXPECT_THAT(keyboard_sink_.Writes(),
                ElementsAre(absl::StrCat("down ", KEY_B), absl::StrCat("up ", KEY_B)));

    ASSERT_TRUE(stream_b->WritesDone());
    EXPECT_TRUE(stream_b->Finish().ok());
    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 0);
}

TEST_F(EmulatorControllerInputTest, ConcurrentUnaryAndStreamRpcsAreThreadSafe) {
    absl::Notification start;
    constexpr int kIterations = 25;

    std::thread mouse_thread([&]() {
        start.WaitForNotification();
        for (int i = 0; i < kIterations; ++i) {
            for (const int32_t buttons : {1, 0}) {
                grpc::ClientContext ctx;
                MouseEvent mouse;
                mouse.set_display(0);
                mouse.set_x(50 + i);
                mouse.set_y(60 + i);
                mouse.set_buttons(buttons);
                Empty reply;
                EXPECT_TRUE(stub_->sendMouse(&ctx, mouse, &reply).ok());
            }
        }
    });

    std::thread touch_thread([&]() {
        start.WaitForNotification();
        for (int i = 0; i < kIterations; ++i) {
            for (const int32_t pressure : {500, 0}) {
                grpc::ClientContext ctx;
                TouchEvent touch;
                touch.set_display(1);
                Touch* t = touch.add_touches();
                t->set_identifier(1);
                t->set_x(100 + i);
                t->set_y(120 + i);
                t->set_pressure(pressure);
                Empty reply;
                EXPECT_TRUE(stub_->sendTouch(&ctx, touch, &reply).ok());
            }
        }
    });

    std::thread key_thread([&]() {
        start.WaitForNotification();
        for (int i = 0; i < kIterations; ++i) {
            grpc::ClientContext ctx;
            KeyboardEvent key;
            key.set_key("KeyC");
            key.set_eventtype(KeyboardEvent::keypress);
            Empty reply;
            EXPECT_TRUE(stub_->sendKey(&ctx, key, &reply).ok());
        }
    });

    std::thread stream_thread([&]() {
        start.WaitForNotification();
        grpc::ClientContext ctx;
        Empty reply;
        auto stream = stub_->streamInputEvent(&ctx, &reply);
        for (int i = 0; i < kIterations; ++i) {
            for (const int32_t buttons : {1, 0}) {
                InputEvent ev;
                ev.mutable_mouse_event()->set_display(0);
                ev.mutable_mouse_event()->set_x(200 + i);
                ev.mutable_mouse_event()->set_y(220 + i);
                ev.mutable_mouse_event()->set_buttons(buttons);
                EXPECT_TRUE(stream->Write(ev));
            }
        }
        EXPECT_TRUE(stream->WritesDone());
        EXPECT_TRUE(stream->Finish().ok());
    });

    start.Notify();
    mouse_thread.join();
    touch_thread.join();
    key_thread.join();
    stream_thread.join();

    loop_->RunAll();
    EXPECT_EQ(ActiveSlotCount(0), 0);
    EXPECT_EQ(ActiveSlotCount(1), 0);
}

}  // namespace
}  // namespace android::emulation::control
