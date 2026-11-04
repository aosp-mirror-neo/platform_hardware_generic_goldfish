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

#include "goldfish/display/display.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "android/goldfish/fake_hardware_config.h"
#include "goldfish/archive/deque_archive.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/sensors/physical_model.h"
#include "goldfish/system/testing/fake_clocks.h"

using goldfish::async::EventLoop;
using goldfish::display::Dimensions;
using goldfish::display::EvDevEvent;
using goldfish::display::EvDevEvents;
using goldfish::display::FrameEvent;
using goldfish::display::FrameId;
using goldfish::display::IDisplay;
using goldfish::display::ImageRotation;
using goldfish::display::ReadRequest;
using goldfish::display::ReadTarget;
using goldfish::display::Slot;
using goldfish::display::SlotLease;

namespace {

class DummySlot final : public Slot {
  public:
    explicit DummySlot(Dimensions dims) : dims_(dims) {}
    Dimensions size() const override { return dims_; }

  protected:
    absl::Status DoReadInto(const ReadRequest& /*read*/, const ReadTarget& /*dst*/) const override {
        return absl::OkStatus();
    }

  private:
    Dimensions dims_;
};

// Test Display implementation for unit testing.
class TestDisplay : public IDisplay {
  public:
    TestDisplay(EventLoop* loop, uint8_t id, uint32_t width, uint32_t height)
            : IDisplay(loop, id, width, height) {}

    void SendEvDevEvent(uint16_t /*type*/, uint16_t /*code*/, uint32_t /*value*/) override {}
    void SendEvDevReport(EvDevEvents /*events*/) override {}
    void WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> /*events*/) override {}
    bool IsInputActive() const override { return false; }
    uint64_t InputActiveGeneration() const override { return 0; }

    void UpdateDimensions(uint32_t w, uint32_t h) { SetDimensions(w, h); }

    void Incoming() {
        frames()->PublishFrame(SlotLease(std::make_unique<DummySlot>(GetDimensions())),
                               absl::Now());
    }
};

class LifecycleMockDisplay final : public TestDisplay {
  public:
    LifecycleMockDisplay(EventLoop* loop, uint8_t id, uint32_t width, uint32_t height,
                         bool publish_on_demand = false)
            : TestDisplay(loop, id, width, height), publish_on_demand_(publish_on_demand) {}
    // OnDemand uses the members below.
    ~LifecycleMockDisplay() override { StopProducerCalls(); }

    void OnDemand(bool has_consumers) override {
        if (has_consumers) {
            demand_true_count.fetch_add(1);
            if (publish_on_demand_) {
                Incoming();
            }
            if (!demand_enabled.HasBeenNotified()) {
                demand_enabled.Notify();
            }
        } else {
            demand_false_count.fetch_add(1);
            if (!demand_disabled.HasBeenNotified()) {
                demand_disabled.Notify();
            }
        }
    }

    std::atomic<int> demand_true_count{0};
    std::atomic<int> demand_false_count{0};
    absl::Notification demand_enabled;
    absl::Notification demand_disabled;

  private:
    bool publish_on_demand_;
};

// Its producer hooks use member state that its destructor tears down, so a
// hook running into destruction dereferences null (and ASAN reports it).
class HookStateDisplay : public TestDisplay {
  public:
    using TestDisplay::TestDisplay;

    std::weak_ptr<goldfish::display::IFrameProducer> Producer() const { return weak_producer(); }

    void OnDemand(bool /*warm*/) override { Touch(); }
    void OnFrameRequested() override { Touch(); }

  private:
    void Touch() {
        ++*state_;
        absl::SleepFor(absl::Microseconds(20));  // Widens the race with destruction.
        ++*state_;
    }

    std::unique_ptr<int> state_ = std::make_unique<int>(0);
};

// A HookStateDisplay for use without a shared_ptr: stops hook calls first.
class UnownedHookDisplay final : public HookStateDisplay {
  public:
    using HookStateDisplay::HookStateDisplay;
    ~UnownedHookDisplay() override { StopProducerCalls(); }
};

}  // namespace

class DisplayTest : public ::testing::Test {
  protected:
    void SetUp() override {
        loop_ = ::goldfish::async::ThreadedEventLoop::Create(
                ::goldfish::async::LibuvEventLoop::Create());
    }

    void TearDown() override { loop_.reset(); }

    std::unique_ptr<EventLoop> loop_;
};

TEST_F(DisplayTest, WaitForFrameTimeout) {
    const TestDisplay display(loop_.get(), 0, 100, 100);
    const FrameId initial_seq = display.CurrentFrameId();

    auto result = display.WaitForFrame(absl::Now() + absl::Milliseconds(10), initial_seq);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kDeadlineExceeded);
}

TEST_F(DisplayTest, WaitForFrameSuccess) {
    TestDisplay display(loop_.get(), 0, 100, 100);
    const FrameId initial_seq = display.CurrentFrameId();

    std::thread frame_updater([&display]() {
        absl::SleepFor(absl::Milliseconds(5));
        display.Incoming();
    });

    auto result = display.WaitForFrame(absl::Now() + absl::Seconds(1), initial_seq);
    EXPECT_TRUE(result.ok());
    frame_updater.join();
}

TEST_F(DisplayTest, WaitForFreshFrameSuccess) {
    LifecycleMockDisplay display(loop_.get(), 0, 100, 100, /*publish_on_demand=*/true);

    auto result = display.WaitForFreshFrame(absl::Now() + absl::Seconds(1));
    ASSERT_TRUE(result.ok());
    EXPECT_NE(*result, nullptr);
    EXPECT_EQ((*result)->dimensions(), (Dimensions{100, 100}));
}

TEST_F(DisplayTest, WaitForFreshFrameTimeout) {
    const TestDisplay display(loop_.get(), 0, 100, 100);

    auto result = display.WaitForFreshFrame(absl::Now() + absl::Milliseconds(10));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kDeadlineExceeded);
}

TEST_F(DisplayTest, GetDimensions) {
    const uint32_t width = 800;
    const uint32_t height = 600;
    const TestDisplay display(loop_.get(), 0, width, height);
    const Dimensions dims = display.GetDimensions();
    EXPECT_EQ(dims.width, width);
    EXPECT_EQ(dims.height, height);
}

TEST_F(DisplayTest, SetDimensions) {
    TestDisplay display(loop_.get(), 0, 100, 100);
    display.UpdateDimensions(1920, 1080);
    const Dimensions dims = display.GetDimensions();
    EXPECT_EQ(dims.width, 1920);
    EXPECT_EQ(dims.height, 1080);
}

TEST_F(DisplayTest, ThreadSafeDimensions) {
    TestDisplay display(loop_.get(), 0, 100, 101);
    std::atomic<bool> running{true};

    std::thread writer([&]() {
        uint32_t i = 0;
        while (running) {
            display.UpdateDimensions(i, i + 1);
            i++;
        }
    });

    for (int i = 0; i < 10000; ++i) {
        const Dimensions dims = display.GetDimensions();
        EXPECT_EQ(dims.height, dims.width + 1);
    }

    running = false;
    writer.join();
}

TEST_F(DisplayTest, AddFrameListener_ReceivesFramesAsync) {
    LifecycleMockDisplay display(loop_.get(), 0, 100, 100);

    absl::Notification frame_arrived;
    auto listener = display.AddFrameListener([&](const FrameEvent&) {
        if (!frame_arrived.HasBeenNotified()) {
            frame_arrived.Notify();
        }
    });

    display.Incoming();

    EXPECT_TRUE(frame_arrived.WaitForNotificationWithTimeout(absl::Seconds(1)));
}

TEST_F(DisplayTest, AddFrameListener_DeliversCatchUpWhenAlreadyWarm) {
    LifecycleMockDisplay display(loop_.get(), 0, 100, 100);

    absl::Notification frame1_arrived;
    auto listener1 = display.AddFrameListener([&](const FrameEvent&) {
        if (!frame1_arrived.HasBeenNotified()) {
            frame1_arrived.Notify();
        }
    });

    ASSERT_TRUE(display.demand_enabled.WaitForNotificationWithTimeout(absl::Seconds(1)));
    display.Incoming();
    ASSERT_TRUE(frame1_arrived.WaitForNotificationWithTimeout(absl::Seconds(1)));

    absl::Notification listener2_arrived;
    FrameId listener2_received_id = 0;
    auto listener2 = display.AddFrameListener([&](const FrameEvent& event) {
        listener2_received_id = event.frame_id;
        if (!listener2_arrived.HasBeenNotified()) {
            listener2_arrived.Notify();
        }
    });

    ASSERT_TRUE(listener2_arrived.WaitForNotificationWithTimeout(absl::Seconds(1)));
    EXPECT_EQ(1U, listener2_received_id);
}

TEST_F(DisplayTest, AddFrameListener_AndRemove_TriggersSetDemand) {
    LifecycleMockDisplay display(loop_.get(), 0, 100, 100);

    EXPECT_EQ(0, display.demand_true_count.load());
    EXPECT_EQ(0, display.demand_false_count.load());

    auto listener = display.AddFrameListener([](const FrameEvent&) {});
    ASSERT_TRUE(display.demand_enabled.WaitForNotificationWithTimeout(absl::Seconds(1)));
    EXPECT_EQ(1, display.demand_true_count.load());
    EXPECT_EQ(0, display.demand_false_count.load());

    // Remove notifies demand_disabled midway: wait for all of it before the
    // display is destroyed.
    absl::Notification removed;
    ASSERT_TRUE(loop_->Post([&]() {
                         display.Remove();
                         removed.Notify();
                     }).ok());
    ASSERT_TRUE(removed.WaitForNotificationWithTimeout(absl::Seconds(10)));
    ASSERT_TRUE(display.demand_disabled.WaitForNotificationWithTimeout(absl::Seconds(1)));
    EXPECT_EQ(1, display.demand_false_count.load());
}

// A producer call racing display destruction must never invoke a hook on a
// display whose destruction has already begun.
TEST_F(DisplayTest, ProducerCallsRacingDestructionNeverReachADestroyedDisplay) {
    for (int i = 0; i < 100; ++i) {
        auto display = std::make_shared<HookStateDisplay>(loop_.get(), 0, 100, 100);
        const std::weak_ptr<goldfish::display::IFrameProducer> weak = display->Producer();
        absl::Notification calling;
        std::thread caller([&] {
            while (const std::shared_ptr<goldfish::display::IFrameProducer> producer =
                           weak.lock()) {
                producer->SetDemand(true);
                producer->RequestFrame();
                if (!calling.HasBeenNotified()) calling.Notify();
            }
        });
        calling.WaitForNotification();
        display.reset();
        caller.join();
    }
}

// Without a shared_ptr, StopProducerCalls first in the destructor waits for a
// hook in progress and stops new ones before derived state is destroyed.
TEST_F(DisplayTest, StopProducerCallsProtectsAnUnownedDisplaysState) {
    for (int i = 0; i < 100; ++i) {
        auto display = std::make_unique<UnownedHookDisplay>(loop_.get(), 0, 100, 100);
        const std::shared_ptr<goldfish::display::IFrameProducer> producer =
                display->Producer().lock();
        ASSERT_NE(producer, nullptr);
        std::atomic<bool> stop{false};
        absl::Notification calling;
        std::thread caller([&] {
            while (!stop.load()) {
                producer->SetDemand(true);
                producer->RequestFrame();
                if (!calling.HasBeenNotified()) calling.Notify();
            }
        });
        calling.WaitForNotification();
        display.reset();
        stop = true;
        caller.join();
    }
}

TEST_F(DisplayTest, SetRotationEmitsFrameWithUpdatedRotation) {
    LifecycleMockDisplay display(loop_.get(), 0, 100, 100);
    EXPECT_EQ(display.GetRotation(), ImageRotation::kRotation0);

    absl::Notification initial_arrived;
    absl::Notification rotated_arrived;
    ImageRotation received_rotation = ImageRotation::kRotation0;
    auto listener = display.AddFrameListener([&](const FrameEvent& event) {
        received_rotation = event.rotation;
        if (!initial_arrived.HasBeenNotified()) {
            initial_arrived.Notify();
        } else if (event.rotation == ImageRotation::kRotation90 &&
                   !rotated_arrived.HasBeenNotified()) {
            rotated_arrived.Notify();
        }
    });

    display.Incoming();
    ASSERT_TRUE(initial_arrived.WaitForNotificationWithTimeout(absl::Seconds(1)));
    EXPECT_EQ(received_rotation, ImageRotation::kRotation0);

    display.SetRotation(ImageRotation::kRotation90);
    EXPECT_EQ(display.GetRotation(), ImageRotation::kRotation90);
    EXPECT_TRUE(rotated_arrived.WaitForNotificationWithTimeout(absl::Seconds(1)));
    EXPECT_EQ(received_rotation, ImageRotation::kRotation90);
}

TEST_F(DisplayTest, MultiDisplayPropagatesPhysicalModelRotationToDisplays) {
    android::goldfish::HardwareConfig hw = android::goldfish::FakeHardwareConfig::GetHwConfig();
    goldfish::system::testing::FakeVirtualClock virtual_clock;
    goldfish::sensors::PhysicalModel physical_model(hw, virtual_clock);
    goldfish::display::test::FakeMultiDisplay multi_display(loop_.get());
    auto sub = multi_display.RegisterPhysicalModel(physical_model);

    auto d0_res = multi_display.GetDisplay(0);
    ASSERT_TRUE(d0_res.ok());
    auto d0 = d0_res->lock();
    ASSERT_NE(d0, nullptr);
    EXPECT_EQ(d0->GetRotation(), ImageRotation::kRotation0);

    physical_model.SetTargetRotation(goldfish::sensors::vec3(0.0F, 0.0F, 90.0F),
                                     PhysicalInterpolation::kStep);
    EXPECT_EQ(multi_display.GetRotation(), ImageRotation::kRotation90);
    EXPECT_EQ(d0->GetRotation(), ImageRotation::kRotation90);

    auto d1_res = multi_display.CreateDisplay(1, 200, 300, 160, 0);
    ASSERT_TRUE(d1_res.ok());
    auto d1 = d1_res->lock();
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(d1->GetRotation(), ImageRotation::kRotation90);
}

TEST_F(DisplayTest, MultiDisplayImplCreateDisplayReportsActionableErrors) {
    using ::absl_testing::StatusIs;
    using ::goldfish::async::testing::TestEventLoop;
    using ::goldfish::display::IMultiDisplay;
    using ::testing::AllOf;
    using ::testing::HasSubstr;

    auto main_loop = TestEventLoop::Create("main");
    auto qemu_loop = TestEventLoop::Create("qemu");
    auto multi_display = IMultiDisplay::Create(main_loop.get(), qemu_loop.get(), nullptr);

    EXPECT_THAT(multi_display->CreateDisplay(0, 800, 600, 320, 0),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         AllOf(HasSubstr("Failed to create virtual display 0"),
                               HasSubstr("display ID must be in [1, 10]"),
                               HasSubstr("Specify a valid virtual display ID."))));

    EXPECT_THAT(multi_display->CreateDisplay(IMultiDisplay::kMaxDisplays, 800, 600, 320, 0),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         AllOf(HasSubstr("Failed to create virtual display 11"),
                               HasSubstr("display ID must be in [1, 10]"),
                               HasSubstr("Specify a valid virtual display ID."))));

    EXPECT_THAT(multi_display->CreateDisplay(1, 0, 600, 320, 0),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         AllOf(HasSubstr("Failed to create virtual display 1"),
                               HasSubstr("dimensions 0x600 are out of range"),
                               HasSubstr("[1, 8192]"))));

    EXPECT_THAT(
            multi_display->CreateDisplay(1, 800, IMultiDisplay::kMaxDisplayDimension + 1, 320, 0),
            StatusIs(absl::StatusCode::kInvalidArgument,
                     AllOf(HasSubstr("Failed to create virtual display 1"),
                           HasSubstr("dimensions 800x8193 are out of range"),
                           HasSubstr("[1, 8192]"))));

    ASSERT_TRUE(multi_display->CreateDisplay(1, 800, 600, 320, 0).ok());
    EXPECT_THAT(multi_display->CreateDisplay(1, 800, 600, 320, 0),
                StatusIs(absl::StatusCode::kAlreadyExists,
                         AllOf(HasSubstr("Failed to create virtual display 1"),
                               HasSubstr("already exists"), HasSubstr("EraseDisplay(1)"))));
    ASSERT_TRUE(multi_display->EraseDisplay(1).ok());
    qemu_loop->RunAll();
    main_loop->RunAll();
}

TEST_F(DisplayTest, MultiDisplayImplNotFoundAndInactiveErrorsIncludeDisplayIdAndNextStep) {
    using ::absl_testing::StatusIs;
    using ::goldfish::async::testing::TestEventLoop;
    using ::goldfish::display::IMultiDisplay;
    using ::testing::AllOf;
    using ::testing::HasSubstr;

    auto main_loop = TestEventLoop::Create("main");
    auto qemu_loop = TestEventLoop::Create("qemu");
    auto multi_display = IMultiDisplay::Create(main_loop.get(), qemu_loop.get(), nullptr);

    EXPECT_THAT(
            multi_display->GetDisplay(7),
            StatusIs(absl::StatusCode::kNotFound, AllOf(HasSubstr("Failed to find display 7"),
                                                        HasSubstr("display 7 does not exist"),
                                                        HasSubstr("Create the display first"))));

    EXPECT_THAT(multi_display->SetActive(7, true),
                StatusIs(absl::StatusCode::kNotFound,
                         AllOf(HasSubstr("Failed to set active state for display 7 to true"),
                               HasSubstr("display 7 does not exist"),
                               HasSubstr("Create the display first"))));

    EXPECT_THAT(multi_display->EraseDisplay(7),
                StatusIs(absl::StatusCode::kNotFound,
                         AllOf(HasSubstr("Failed to remove display 7"),
                               HasSubstr("display 7 does not exist or was already removed"),
                               HasSubstr("Verify the display ID before calling EraseDisplay."))));

    ASSERT_TRUE(multi_display->CreateDisplay(2, 800, 600, 320, 0).ok());
    ASSERT_TRUE(multi_display->SetActive(2, false).ok());
    EXPECT_THAT(multi_display->GetActiveDisplay(2, false),
                StatusIs(absl::StatusCode::kUnavailable,
                         AllOf(HasSubstr("Failed to acquire active display 2"),
                               HasSubstr("display is currently inactive"),
                               HasSubstr("SetActive(2, true)"))));
    ASSERT_TRUE(multi_display->EraseDisplay(2).ok());
    qemu_loop->RunAll();
    main_loop->RunAll();
}

TEST_F(DisplayTest, MultiDisplayImplLoadReportsActionableSnapshotErrors) {
    using ::absl_testing::StatusIs;
    using ::goldfish::async::testing::TestEventLoop;
    using ::goldfish::display::IMultiDisplay;
    using ::testing::AllOf;
    using ::testing::HasSubstr;

    auto main_loop = TestEventLoop::Create("main");
    auto qemu_loop = TestEventLoop::Create("qemu");
    auto multi_display = IMultiDisplay::Create(main_loop.get(), qemu_loop.get(), nullptr);

    {
        goldfish::archive::DequeArchive archive;
        archive << static_cast<uint32_t>(IMultiDisplay::kMaxDisplays);
        EXPECT_THAT(multi_display->Load(archive),
                    StatusIs(absl::StatusCode::kInvalidArgument,
                             AllOf(HasSubstr("Failed to load multi-display snapshot"),
                                   HasSubstr("virtual display count 11 exceeds maximum 10"),
                                   HasSubstr("cold-boot the AVD"))));
    }

    {
        goldfish::archive::DequeArchive archive;
        archive << uint32_t{1} << uint32_t{1} << (IMultiDisplay::kMaxDisplayDimension + 1)
                << uint32_t{600} << uint32_t{320} << uint32_t{0};
        EXPECT_THAT(multi_display->Load(archive),
                    StatusIs(absl::StatusCode::kInvalidArgument,
                             AllOf(HasSubstr("Failed to load multi-display snapshot"),
                                   HasSubstr("virtual display entry 0 has out-of-range id=1 or "
                                             "dimensions=8193x600"),
                                   HasSubstr("cold-boot the AVD"))));
    }

    {
        goldfish::archive::DequeArchive archive;
        archive << uint32_t{0} << static_cast<uint32_t>(IMultiDisplay::kMaxDisplays + 1);
        EXPECT_THAT(multi_display->Load(archive),
                    StatusIs(absl::StatusCode::kInvalidArgument,
                             AllOf(HasSubstr("Failed to load multi-display snapshot"),
                                   HasSubstr("active display state count 12 exceeds maximum 11"),
                                   HasSubstr("cold-boot the AVD"))));
    }

    {
        goldfish::archive::DequeArchive archive;
        archive << uint32_t{0} << uint32_t{1} << static_cast<uint32_t>(IMultiDisplay::kMaxDisplays)
                << true;
        EXPECT_THAT(multi_display->Load(archive),
                    StatusIs(absl::StatusCode::kInvalidArgument,
                             AllOf(HasSubstr("Failed to load multi-display snapshot"),
                                   HasSubstr("active_states entry 0 has out-of-range display ID "
                                             "11 (expected [0, 10])"),
                                   HasSubstr("cold-boot the AVD"))));
    }
}
