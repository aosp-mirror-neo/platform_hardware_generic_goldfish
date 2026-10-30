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

#include "qemu_display.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/status/status_matcher_macros.h"
#include "goldfish/archive/deque_archive.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/display.h"
#include "goldfish/display/display_frames.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/test/display_test_peer.h"
#include "goldfish/display/virtual_display.h"
#include "standard-headers/linux/input-event-codes.h"

extern "C" {
#include "ui/console.h"
#include "ui/surface.h"
}

extern "C" {
void SetTestRegisterDclHook(void (*hook)(DisplayChangeListener*));
void SetTestGraphicHwUpdateHook(void (*hook)(QemuConsole*));
void SetTestFindVirtioDeviceResult(int result);
void SetTestVirtioInputSendHook(void (*hook)(uint16_t type, uint16_t code, uint32_t value));
void SetTestFindVirtioDeviceNullVhid(bool null_vhid);
void SetTestVirtioInputActive(bool active);
void SetTestVirtioInputInitialActiveGeneration(uint64_t generation);
void SetTestVirtioInputActiveGeneration(uint64_t generation);
void NotifyTestUnrelatedVirtioInputActiveGeneration(uint64_t generation);
void ResetTestObjectPropertyGetUintCalls(void);
uint64_t GetTestObjectPropertyGetUintCalls(void);
}

using goldfish::async::testing::TestEventLoop;
using goldfish::display::DisplayFrames;
using goldfish::display::DisplayTestPeer;
using goldfish::display::EvDevEvent;
using goldfish::display::EvDevEvents;
using goldfish::display::FrameEvent;
using goldfish::display::IDisplay;
using goldfish::display::VirtualDisplay;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
// Avoid ambiguity with the global QemuDisplay typedef from QEMU's console.h
using EmulatorDisplay = goldfish::display::QemuDisplay;

// Records the events the displays write to the virtio input device.
class ScopedDeviceEventCapture {
  public:
    ScopedDeviceEventCapture() {
        capture_events_ = &events;
        SetTestVirtioInputSendHook(&Record);
    }
    ~ScopedDeviceEventCapture() {
        SetTestVirtioInputSendHook(nullptr);
        capture_events_ = nullptr;
    }

    std::vector<EvDevEvent> events;

  private:
    static void Record(uint16_t type, uint16_t code, uint32_t value) {
        capture_events_->push_back({type, code, value});
    }

    static inline std::vector<EvDevEvent>* capture_events_ = nullptr;
};

class QemuDisplayConcurrencyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        SetTestFindVirtioDeviceResult(1);
        SetTestFindVirtioDeviceNullVhid(false);
        SetTestVirtioInputActive(false);
        SetTestVirtioInputInitialActiveGeneration(0);
        ResetTestObjectPropertyGetUintCalls();
        main_loop_ = TestEventLoop::Create("main");
        qemu_loop_ = TestEventLoop::Create("qemu");

        // QemuDisplay requires a non-null QemuConsole. In tests, we can provide
        // a dummy pointer since the QEMU stubs don't dereference it.
        console_ = reinterpret_cast<QemuConsole*>(0x1234);

        // QemuDisplay constructor dereferences ds->image, so we need a dummy surface.
        surface_ = new DisplaySurface();
        surface_->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, 100, 100, nullptr, 100 * 4);

        ASSERT_OK_AND_ASSIGN(display_, EmulatorDisplay::Create(main_loop_.get(), qemu_loop_.get(),
                                                               console_, surface_, 0));
    }

    void TearDown() override {
        SetTestFindVirtioDeviceNullVhid(false);
        SetTestVirtioInputActive(false);
        SetTestVirtioInputInitialActiveGeneration(0);
        ResetTestObjectPropertyGetUintCalls();
        if (surface_->image) {
            pixman_image_unref(surface_->image);
        }
        delete surface_;
    }

    // Runs `task` on the QEMU loop thread and waits for it.
    void RunOnQemuLoop(absl::AnyInvocable<void()> task) {
        ASSERT_OK(qemu_loop_->Post(std::move(task)));
        qemu_loop_->RunAll();
    }

    // A virtual display, which has its own input device.
    std::shared_ptr<VirtualDisplay> CreateVirtualDisplay() {
        auto display =
                VirtualDisplay::Create(main_loop_.get(), qemu_loop_.get(), 1, 800, 600, 320, 0);
        EXPECT_OK(display);
        qemu_loop_->RunAll();
        return display.ok() ? *std::move(display) : nullptr;
    }

    std::unique_ptr<TestEventLoop> main_loop_;
    std::unique_ptr<TestEventLoop> qemu_loop_;
    QemuConsole* console_;
    DisplaySurface* surface_;
    std::shared_ptr<EmulatorDisplay> display_;
};

TEST_F(QemuDisplayConcurrencyTest, Lifecycle_HappyPath) {
    auto listener = display_->AddFrameListener([](const FrameEvent&) {});
    main_loop_->RunAll();
    EXPECT_TRUE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);  // REG task posted

    // QEMU loop executes REG task
    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);

    listener = {};
    main_loop_->RunAll();
    main_loop_->AdvanceClock(DisplayFrames::kWarmTime);
    EXPECT_FALSE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);  // UNREG task posted

    // QEMU loop executes UNREG task
    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, FlappingListener_RapidAddRemove) {
    DisplayTestPeer::producer(*display_)->SetDemand(true);
    DisplayTestPeer::producer(*display_)->SetDemand(false);

    EXPECT_FALSE(display_->IsActive());

    // QEMU hasn't run yet, but both tasks are queued.
    EXPECT_EQ(qemu_loop_->TaskCount(), 2);

    // Run both tasks. The internal state should gracefully handle
    // registering and immediately unregistering.
    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, ReRegistration_Bounce) {
    DisplayTestPeer::producer(*display_)->SetDemand(true);
    DisplayTestPeer::producer(*display_)->SetDemand(false);
    DisplayTestPeer::producer(*display_)->SetDemand(true);

    EXPECT_TRUE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 3);  // REG, UNREG, REG

    // Processing the queue should leave the display in a registered state.
    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, Teardown_WithPendingTask) {
    static int s_registrations = 0;
    s_registrations = 0;
    SetTestRegisterDclHook([](DisplayChangeListener* /*dcl*/) { ++s_registrations; });
    DisplayTestPeer::producer(*display_)->SetDemand(true);
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    // The pending demand task holds only a weak reference, so dropping ours
    // destroys the display at once.
    std::weak_ptr<EmulatorDisplay> weak_display = display_;
    display_.reset();
    EXPECT_TRUE(weak_display.lock() == nullptr);

    // The stale task must not register a DCL for the destroyed display.
    qemu_loop_->RunAll();
    EXPECT_EQ(s_registrations, 0);

    SetTestRegisterDclHook(nullptr);
}

TEST_F(QemuDisplayConcurrencyTest, ThreadSafety_ConcurrentAccess) {
    // Simulate background thread aggressively toggling demand
    std::thread stress_thread([&]() {
        for (int i = 0; i < 1000; ++i) {
            DisplayTestPeer::producer(*display_)->SetDemand(true);
            DisplayTestPeer::producer(*display_)->SetDemand(false);
        }
    });

    // Simulate QEMU loop processing tasks concurrently
    for (int i = 0; i < 100; ++i) {
        qemu_loop_->RunOne();
    }

    stress_thread.join();

    // Clean up any remaining tasks
    qemu_loop_->RunAll();
    EXPECT_FALSE(display_->IsActive());
}

TEST_F(QemuDisplayConcurrencyTest, MultipleSubscribers_RefCounted) {
    auto listener1 = display_->AddFrameListener([](const FrameEvent&) {});
    main_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);  // 0 -> 1 posts REG task

    auto listener2 = display_->AddFrameListener([](const FrameEvent&) {});
    main_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);  // 1 -> 2 does NOT post additional task

    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);

    listener1 = {};
    main_loop_->RunAll();
    main_loop_->AdvanceClock(DisplayFrames::kWarmTime);
    EXPECT_TRUE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);  // 2 -> 1 does NOT post UNREG task

    listener2 = {};
    main_loop_->RunAll();
    main_loop_->AdvanceClock(DisplayFrames::kWarmTime);
    EXPECT_FALSE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);  // 1 -> 0 posts UNREG task

    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, Teardown_FromNonLoopThread_RegisteredDCL) {
    DisplayTestPeer::producer(*display_)->SetDemand(true);
    qemu_loop_->RunAll();  // DCL is now registered with QEMU

    // Destroy display on the current (non-QEMU) thread
    display_.reset();

    // The destructor should have posted an unregister_and_delete task to qemu_loop_
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    // Executing the loop should clean up without error
    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, Teardown_FromNonLoopThread_UnregisteredDCL) {
    // DCL was never registered (0 subscribers)
    display_.reset();

    // Destructor posts cleanup task
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    // Cleanup executes safely without calling unregister
    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, InputEvents_DispatchedToQemuLoop) {
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);

    display_->SendEvDevEvent(1, 2, 3);
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

// The guest device applies events up to each SYN_REPORT, so another sender's
// task must not run between a report's events: the report is one loop task.
TEST_F(QemuDisplayConcurrencyTest, EvDevReport_PostedAsOneTask) {
    ScopedDeviceEventCapture device;
    const EvDevEvents report = {
        {EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, 0}, {EV_ABS, ABS_MT_SLOT, 1}};

    display_->SendEvDevReport(report);
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    qemu_loop_->RunAll();
    EXPECT_THAT(device.events,
                ElementsAre(report[0], report[1], report[2], EvDevEvent{EV_SYN, SYN_REPORT, 0}));
}

TEST_F(QemuDisplayConcurrencyTest, VirtualDisplayEvDevReport_PostedAsOneTask) {
    ASSERT_OK_AND_ASSIGN(auto display,
                         goldfish::display::VirtualDisplay::Create(
                                 main_loop_.get(), qemu_loop_.get(), 1, 800, 600, 320, 0));
    qemu_loop_->RunAll();
    ScopedDeviceEventCapture device;
    const EvDevEvents report = {{EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, 0}};

    display->SendEvDevReport(report);
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    qemu_loop_->RunAll();
    EXPECT_THAT(device.events,
                ElementsAre(report[0], report[1], EvDevEvent{EV_SYN, SYN_REPORT, 0}));
}

// The input dispatcher writes on the QEMU loop. A posted report would queue
// behind later loop work and could reorder touches relative to keys.
TEST_F(QemuDisplayConcurrencyTest, WriteEvDevReportOnLoop_WritesBeforeReturning) {
    const std::shared_ptr<VirtualDisplay> virtual_display = CreateVirtualDisplay();
    ASSERT_NE(virtual_display, nullptr);
    ScopedDeviceEventCapture device;
    const EvDevEvent report[] = {{EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, 0}};
    const EvDevEvent syn{EV_SYN, SYN_REPORT, 0};

    for (IDisplay* display :
         {static_cast<IDisplay*>(display_.get()), static_cast<IDisplay*>(virtual_display.get())}) {
        device.events.clear();
        std::vector<EvDevEvent> written_on_return;
        RunOnQemuLoop([&] {
            display->WriteEvDevReportOnLoop(report);
            written_on_return = device.events;
        });
        EXPECT_THAT(written_on_return, ElementsAre(report[0], report[1], syn));
        EXPECT_THAT(device.events, ElementsAre(report[0], report[1], syn));
        EXPECT_EQ(qemu_loop_->TaskCount(), 0);
    }
}

TEST_F(QemuDisplayConcurrencyTest, IsInputActive_FollowsGuestDriver) {
    const std::shared_ptr<VirtualDisplay> virtual_display = CreateVirtualDisplay();
    ASSERT_NE(virtual_display, nullptr);

    for (IDisplay* display :
         {static_cast<IDisplay*>(display_.get()), static_cast<IDisplay*>(virtual_display.get())}) {
        bool active = true;
        SetTestVirtioInputActive(false);
        RunOnQemuLoop([&] { active = display->IsInputActive(); });
        EXPECT_FALSE(active);

        SetTestVirtioInputActive(true);
        RunOnQemuLoop([&] { active = display->IsInputActive(); });
        EXPECT_TRUE(active);
    }
}

TEST_F(QemuDisplayConcurrencyTest, InputActiveGeneration_FollowsGuestDevice) {
    const std::shared_ptr<VirtualDisplay> virtual_display = CreateVirtualDisplay();
    ASSERT_NE(virtual_display, nullptr);

    for (IDisplay* display :
         {static_cast<IDisplay*>(display_.get()), static_cast<IDisplay*>(virtual_display.get())}) {
        uint64_t generation = 0;
        SetTestVirtioInputActiveGeneration(3);
        RunOnQemuLoop([&] { generation = display->InputActiveGeneration(); });
        EXPECT_EQ(generation, 3U);

        SetTestVirtioInputActiveGeneration(4);
        RunOnQemuLoop([&] { generation = display->InputActiveGeneration(); });
        EXPECT_EQ(generation, 4U);
    }
    SetTestVirtioInputInitialActiveGeneration(0);
}

TEST_F(QemuDisplayConcurrencyTest,
       InputActiveGeneration_SeedsOnceFromQomAndServesSubsequentReadsFromPushCache) {
    SetTestVirtioInputInitialActiveGeneration(5);
    ResetTestObjectPropertyGetUintCalls();

    // Construction happens off the QEMU loop (e.g., gRPC VirtualDisplay::Create)
    // and must not touch QOM without the BQL.
    ASSERT_OK_AND_ASSIGN(
            auto qemu_display,
            EmulatorDisplay::Create(main_loop_.get(), qemu_loop_.get(), console_, surface_, 0));
    const std::shared_ptr<VirtualDisplay> virtual_display = CreateVirtualDisplay();
    ASSERT_NE(virtual_display, nullptr);
    EXPECT_EQ(GetTestObjectPropertyGetUintCalls(), 0U);

    // First read on qemu_loop_ seeds once from QOM; repeated reads hit the atomic cache.
    uint64_t q_gen = 0;
    uint64_t v_gen = 0;
    RunOnQemuLoop([&] {
        q_gen = qemu_display->InputActiveGeneration();
        v_gen = virtual_display->InputActiveGeneration();
    });
    EXPECT_EQ(q_gen, 5U);
    EXPECT_EQ(v_gen, 5U);
    EXPECT_EQ(GetTestObjectPropertyGetUintCalls(), 2U);

    RunOnQemuLoop([&] {
        q_gen = qemu_display->InputActiveGeneration();
        v_gen = virtual_display->InputActiveGeneration();
    });
    EXPECT_EQ(q_gen, 5U);
    EXPECT_EQ(v_gen, 5U);
    EXPECT_EQ(GetTestObjectPropertyGetUintCalls(), 2U);

    // Push updates notify display listeners, ignore unrelated devices, and
    // update the cached generation with zero QOM reads.
    std::vector<uint64_t> q_pushed;
    std::vector<uint64_t> v_pushed;
    auto q_sub = qemu_display->AddInputActiveListener(
            [&q_pushed](uint64_t g) { q_pushed.push_back(g); });
    auto v_sub = virtual_display->AddInputActiveListener(
            [&v_pushed](uint64_t g) { v_pushed.push_back(g); });

    NotifyTestUnrelatedVirtioInputActiveGeneration(99);
    EXPECT_THAT(q_pushed, IsEmpty());
    EXPECT_THAT(v_pushed, IsEmpty());

    SetTestVirtioInputActiveGeneration(6);
    EXPECT_THAT(q_pushed, ElementsAre(6U));
    EXPECT_THAT(v_pushed, ElementsAre(6U));
    EXPECT_EQ(GetTestObjectPropertyGetUintCalls(), 2U);

    RunOnQemuLoop([&] {
        q_gen = qemu_display->InputActiveGeneration();
        v_gen = virtual_display->InputActiveGeneration();
    });
    EXPECT_EQ(q_gen, 6U);
    EXPECT_EQ(v_gen, 6U);
    EXPECT_EQ(GetTestObjectPropertyGetUintCalls(), 2U);
}

TEST_F(QemuDisplayConcurrencyTest, InputActiveListener_SafeWhenHandleOutlivesDisplay) {
    auto virtual_display = CreateVirtualDisplay();
    ASSERT_NE(virtual_display, nullptr);

    std::vector<uint64_t> pushed;
    auto listener =
            virtual_display->AddInputActiveListener([&pushed](uint64_t g) { pushed.push_back(g); });

    SetTestVirtioInputActiveGeneration(2);
    EXPECT_THAT(pushed, ElementsAre(2U));

    // Destroy the display while `listener` is still alive, then fire another
    // device event and finally destroy `listener`.
    virtual_display.reset();
    qemu_loop_->RunAll();

    SetTestVirtioInputActiveGeneration(3);
    EXPECT_THAT(pushed, ElementsAre(2U));
    listener.reset();
}

TEST_F(QemuDisplayConcurrencyTest, NoInputDevice_WritesNothingAndIsInactive) {
    SetTestFindVirtioDeviceNullVhid(true);
    SetTestVirtioInputActive(true);
    ASSERT_OK_AND_ASSIGN(
            auto qemu_display,
            EmulatorDisplay::Create(main_loop_.get(), qemu_loop_.get(), console_, surface_, 0));
    const std::shared_ptr<VirtualDisplay> virtual_display = CreateVirtualDisplay();
    ASSERT_NE(virtual_display, nullptr);
    ScopedDeviceEventCapture device;
    const EvDevEvent report[] = {{EV_ABS, ABS_MT_SLOT, 0}};

    for (IDisplay* display : {static_cast<IDisplay*>(qemu_display.get()),
                              static_cast<IDisplay*>(virtual_display.get())}) {
        bool active = true;
        uint64_t generation = 1;
        RunOnQemuLoop([&] {
            display->WriteEvDevReportOnLoop(report);
            active = display->IsInputActive();
            generation = display->InputActiveGeneration();
        });
        EXPECT_FALSE(active);
        EXPECT_EQ(generation, 0U);
    }
    EXPECT_THAT(device.events, IsEmpty());
}

TEST_F(QemuDisplayConcurrencyTest, FrameListener_DrivesLifecycleAndReceivesFrames) {
    EXPECT_FALSE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);

    {
        absl::Notification notification;
        auto listener = display_->AddFrameListener([&](const FrameEvent&) {
            if (!notification.HasBeenNotified()) {
                notification.Notify();
            }
        });
        main_loop_->RunAll();
        EXPECT_TRUE(display_->IsActive());
        EXPECT_EQ(qemu_loop_->TaskCount(), 1);  // 0 -> 1 posts REG task

        qemu_loop_->RunAll();
        EXPECT_EQ(qemu_loop_->TaskCount(), 0);

        display_->OnGuestFrame(surface_);
        main_loop_->RunAll();
        EXPECT_TRUE(notification.HasBeenNotified());
    }

    // Listener destroyed -> cool-down expires -> posts UNREG task
    main_loop_->RunAll();
    main_loop_->AdvanceClock(DisplayFrames::kWarmTime);
    EXPECT_FALSE(display_->IsActive());
    EXPECT_EQ(qemu_loop_->TaskCount(), 1);

    qemu_loop_->RunAll();
    EXPECT_EQ(qemu_loop_->TaskCount(), 0);
}

TEST_F(QemuDisplayConcurrencyTest, SurfaceSwap_UpdatesPixmanImage) {
    auto* new_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, 200, 200, nullptr, 200 * 4);
    display_->UpdateSourceImage(new_image);

    EXPECT_EQ(display_->GetDimensions().width, 200);
    EXPECT_EQ(display_->GetDimensions().height, 200);

    pixman_image_unref(new_image);
}

TEST_F(QemuDisplayConcurrencyTest, Registration_SuppressesPrematureGfxUpdateUntilHardwareUpdate) {
    static bool s_gfx_update_null_during_register = false;
    static bool s_gfx_update_restored_during_hw_update = false;
    static DisplayChangeListener* s_captured_dcl = nullptr;

    s_gfx_update_null_during_register = false;
    s_gfx_update_restored_during_hw_update = false;
    s_captured_dcl = nullptr;

    SetTestRegisterDclHook([](DisplayChangeListener* dcl) {
        s_captured_dcl = dcl;
        // Verify dpy_gfx_update is suppressed during registration.
        s_gfx_update_null_during_register = (dcl->ops->dpy_gfx_update == nullptr);
    });

    SetTestGraphicHwUpdateHook([](QemuConsole* /*con*/) {
        // Verify dpy_gfx_update is restored before graphic_hw_update runs.
        if (s_captured_dcl != nullptr) {
            s_gfx_update_restored_during_hw_update =
                    (s_captured_dcl->ops->dpy_gfx_update != nullptr);
        }
    });

    auto listener = display_->AddFrameListener([](const FrameEvent&) {});
    main_loop_->RunAll();
    qemu_loop_->RunAll();

    EXPECT_TRUE(s_gfx_update_null_during_register);
    EXPECT_TRUE(s_gfx_update_restored_during_hw_update);

    SetTestRegisterDclHook(nullptr);
    SetTestGraphicHwUpdateHook(nullptr);
}

TEST_F(QemuDisplayConcurrencyTest, InitialPlaceholderSurface_WarmUpAndRealSurfaceSwitch) {
    DisplaySurface placeholder{};
    placeholder.flags = QEMU_PLACEHOLDER_FLAG;
    placeholder.image = pixman_image_create_bits(PIXMAN_x8r8g8b8, 640, 480, nullptr, 640 * 4);

    ASSERT_OK_AND_ASSIGN(auto display, EmulatorDisplay::Create(main_loop_.get(), qemu_loop_.get(),
                                                               console_, &placeholder, 0));

    static EmulatorDisplay* s_display = nullptr;
    static DisplaySurface* s_placeholder = nullptr;
    s_display = display.get();
    s_placeholder = &placeholder;
    SetTestRegisterDclHook(
            [](DisplayChangeListener* /*dcl*/) { s_display->OnSurfaceSwitch(s_placeholder); });

    auto listener = display->AddFrameListener([](const FrameEvent&) {});
    main_loop_->RunAll();
    qemu_loop_->RunAll();
    main_loop_->RunAll();

    auto initial = display->AcquireLatestFrame();
    EXPECT_TRUE(initial.warm);
    ASSERT_NE(initial.frame, nullptr);
    EXPECT_TRUE(initial.frame->event().blank);

    // Switch to a real guest surface and flush a frame.
    display->OnSurfaceSwitch(surface_);
    display->OnGuestFrame(surface_);
    qemu_loop_->RunAll();
    main_loop_->RunAll();

    auto after_flush = display->AcquireLatestFrame();
    EXPECT_TRUE(after_flush.warm);
    ASSERT_NE(after_flush.frame, nullptr);
    EXPECT_FALSE(after_flush.frame->event().blank);
    EXPECT_EQ(after_flush.frame->dimensions().width, 100U);
    EXPECT_EQ(after_flush.frame->dimensions().height, 100U);

    SetTestRegisterDclHook(nullptr);
    s_display = nullptr;
    s_placeholder = nullptr;
    pixman_image_unref(placeholder.image);
}

TEST_F(QemuDisplayConcurrencyTest, OnDeviceReset_InvalidatesOldFrameAndRecapturesWhenWarm) {
    auto listener = display_->AddFrameListener([](const FrameEvent&) {});
    main_loop_->RunAll();
    qemu_loop_->RunAll();
    main_loop_->RunAll();

    auto pre_reset = display_->AcquireLatestFrame().frame;
    ASSERT_NE(pre_reset, nullptr);
    EXPECT_FALSE(pre_reset->event().blank);

    static int s_hw_updates = 0;
    s_hw_updates = 0;
    SetTestGraphicHwUpdateHook([](QemuConsole* /*con*/) { ++s_hw_updates; });

    // Simulate snapshot load or device reset on the QEMU loop while warm.
    display_->OnDeviceReset();

    // Old frame lease is immediately invalidated by reset epoch bump.
    std::vector<uint8_t> buf(static_cast<size_t>(100) * 100 * 4);
    const goldfish::display::ReadRequest req{};
    const goldfish::display::Rgba8888Target target{
        .plane = {.data = absl::MakeSpan(buf), .stride = static_cast<size_t>(100 * 4)},
    };
    EXPECT_FALSE(pre_reset->ReadInto(req, target).ok());

    // Running the posted RequestFrame pump refreshes and captures the restored surface.
    qemu_loop_->RunAll();
    main_loop_->RunAll();

    EXPECT_EQ(s_hw_updates, 1);
    auto post_reset = display_->AcquireLatestFrame();
    EXPECT_TRUE(post_reset.warm);
    ASSERT_NE(post_reset.frame, nullptr);
    EXPECT_FALSE(post_reset.frame->event().blank);
    EXPECT_GT(post_reset.frame->event().frame_id, pre_reset->event().frame_id);
    EXPECT_TRUE(post_reset.frame->ReadInto(req, target).ok());

    SetTestGraphicHwUpdateHook(nullptr);
}

TEST_F(QemuDisplayConcurrencyTest, Create_NullConsoleFails) {
    EXPECT_THAT(EmulatorDisplay::Create(main_loop_.get(), qemu_loop_.get(), nullptr, surface_, 0),
                absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(QemuDisplayConcurrencyTest, Create_VirtioDeviceNotFoundFails) {
    SetTestFindVirtioDeviceResult(0);
    EXPECT_THAT(EmulatorDisplay::Create(main_loop_.get(), qemu_loop_.get(), console_, surface_, 0),
                absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(QemuDisplayConcurrencyTest, VirtualDisplayCreate_VirtioDeviceNotFoundFails) {
    SetTestFindVirtioDeviceResult(0);
    EXPECT_THAT(goldfish::display::VirtualDisplay::Create(main_loop_.get(), qemu_loop_.get(), 1,
                                                          800, 600, 320, 0),
                absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(QemuDisplayConcurrencyTest, VirtualDisplayCreate_InvalidIdOrDimensionsFails) {
    EXPECT_THAT(goldfish::display::VirtualDisplay::Create(main_loop_.get(), qemu_loop_.get(), 0,
                                                          800, 600, 320, 0),
                absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(goldfish::display::VirtualDisplay::Create(
                        main_loop_.get(), qemu_loop_.get(),
                        static_cast<uint8_t>(goldfish::display::IMultiDisplay::kMaxDisplays), 800,
                        600, 320, 0),
                absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(goldfish::display::VirtualDisplay::Create(main_loop_.get(), qemu_loop_.get(), 1, 0,
                                                          600, 320, 0),
                absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_THAT(goldfish::display::VirtualDisplay::Create(
                        main_loop_.get(), qemu_loop_.get(), 1,
                        goldfish::display::IMultiDisplay::kMaxDisplayDimension + 1, 600, 320, 0),
                absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(QemuDisplayConcurrencyTest, MultiDisplayLoad_RejectsCorruptedSnapshot) {
    auto multi_display =
            goldfish::display::IMultiDisplay::Create(main_loop_.get(), qemu_loop_.get(), nullptr);

    // 1. Too many virtual displays
    {
        goldfish::archive::DequeArchive archive;
        archive << static_cast<uint32_t>(goldfish::display::IMultiDisplay::kMaxDisplays);
        EXPECT_THAT(multi_display->Load(archive),
                    absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
    }

    // 2. Oversized virtual display dimensions
    {
        goldfish::archive::DequeArchive archive;
        archive << uint32_t{1} << uint32_t{1}
                << (goldfish::display::IMultiDisplay::kMaxDisplayDimension + 1) << uint32_t{600}
                << uint32_t{320} << uint32_t{0};
        EXPECT_THAT(multi_display->Load(archive),
                    absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
    }

    // 3. Invalid active_states display ID
    {
        goldfish::archive::DequeArchive archive;
        archive << uint32_t{0} << uint32_t{1}
                << static_cast<uint32_t>(goldfish::display::IMultiDisplay::kMaxDisplays) << true;
        EXPECT_THAT(multi_display->Load(archive),
                    absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
    }
}
