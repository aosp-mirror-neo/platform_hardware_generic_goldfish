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
#include "multi_display_touch_devices.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"

#include "android/emulation/control/ev_dev_event.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "standard-headers/linux/input-event-codes.h"

namespace goldfish::grpc::v2 {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::android::emulation::control::EvDevEvent;
using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::display::DisplayId;
using ::goldfish::display::test::ActiveFakePixmanDisplay;
using ::goldfish::display::test::FakeEvDevEvent;
using ::goldfish::display::test::FakeMultiDisplay;
using ::goldfish::display::test::FakePixmanDisplay;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::NotNull;

// A FakeMultiDisplay whose display 0 is inactive while folded.
class FoldableMultiDisplay : public FakeMultiDisplay {
  public:
    using FakeMultiDisplay::FakeMultiDisplay;

    bool IsActive(DisplayId display_id) const override { return display_id != 0 || !folded; }

    bool folded = false;
};

class MultiDisplayTouchDevicesTest : public ::testing::Test {
  protected:
    void TearDown() override {
        for (const auto& weak : multi_display_.Displays()) {
            if (auto display = weak.lock()) {
                static_cast<ActiveFakePixmanDisplay*>(display.get())->Stop();
            }
        }
    }

    // Adds display `id`, and runs the loop so no pending task holds it.
    void AddDisplay(DisplayId id) {
        ASSERT_TRUE(multi_display_.CreateDisplay(id, 320, 240, 160, 0).ok());
        loop_->RunAll();
    }

    void RemoveDisplay(DisplayId id) {
        ASSERT_TRUE(multi_display_.EraseDisplay(id).ok());
        loop_->RunAll();
    }

    std::shared_ptr<FakePixmanDisplay> Display(DisplayId id) {
        return multi_display_.GetDisplay<FakePixmanDisplay>(multi_display_.GetDisplay(id));
    }

    static constexpr EvDevEvent kSlot0{EV_ABS, ABS_MT_SLOT, 0};
    static constexpr FakeEvDevEvent kFakeSlot0{EV_ABS, ABS_MT_SLOT, 0};
    static constexpr FakeEvDevEvent kFakeSyn{EV_SYN, SYN_REPORT, 0};

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create("qemu");
    FoldableMultiDisplay multi_display_{loop_.get()};
};

TEST_F(MultiDisplayTouchDevicesTest, Resolve_DisplayZero_ReturnsItsId) {
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);

    EXPECT_THAT(devices.Resolve(0), IsOkAndHolds(0U));
}

TEST_F(MultiDisplayTouchDevicesTest, Resolve_FoldedFoldable_ReturnsDisplayOne) {
    AddDisplay(1);
    multi_display_.folded = true;
    MultiDisplayTouchDevices foldable(multi_display_, /*has_hinge=*/true);
    MultiDisplayTouchDevices no_hinge(multi_display_, /*has_hinge=*/false);

    EXPECT_THAT(foldable.Resolve(0), IsOkAndHolds(1U));
    EXPECT_THAT(no_hinge.Resolve(0), StatusIs(absl::StatusCode::kUnavailable));
}

TEST_F(MultiDisplayTouchDevicesTest, Resolve_UnknownOrNegativeDisplay_Fails) {
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);

    EXPECT_THAT(devices.Resolve(5), StatusIs(absl::StatusCode::kNotFound));
    EXPECT_THAT(devices.Resolve(-1), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(MultiDisplayTouchDevicesTest, Find_ReturnsStableSinkPerDisplay) {
    AddDisplay(1);
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);

    TouchDeviceSink* sink = devices.Find(0);

    ASSERT_THAT(sink, NotNull());
    EXPECT_EQ(devices.Find(0), sink);
    EXPECT_NE(devices.Find(1), sink);
    EXPECT_THAT(devices.Find(5), IsNull());
}

TEST_F(MultiDisplayTouchDevicesTest, WriteReport_WritesReportToDisplay) {
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);
    TouchDeviceSink* sink = devices.Find(0);
    ASSERT_THAT(sink, NotNull());

    const EvDevEvent report[] = {kSlot0};
    sink->WriteReport(report);

    EXPECT_THAT(Display(0)->evdev_reports, ElementsAre(ElementsAre(kFakeSlot0, kFakeSyn)));
}

TEST_F(MultiDisplayTouchDevicesTest, IsActive_FollowsDisplayInputDriver) {
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);
    TouchDeviceSink* sink = devices.Find(0);
    ASSERT_THAT(sink, NotNull());

    Display(0)->input_active = false;
    EXPECT_FALSE(sink->IsActive());

    Display(0)->input_active = true;
    EXPECT_TRUE(sink->IsActive());
}

TEST_F(MultiDisplayTouchDevicesTest, ActiveGeneration_FollowsDisplayInputDriver) {
    AddDisplay(1);
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);
    TouchDeviceSink* sink = devices.Find(1);
    ASSERT_THAT(sink, NotNull());

    Display(1)->input_active_generation = 5;
    EXPECT_EQ(sink->ActiveGeneration(), 5U);

    RemoveDisplay(1);
    EXPECT_EQ(sink->ActiveGeneration(), 0U);
}

TEST_F(MultiDisplayTouchDevicesTest,
       ActiveChangedListener_ForwardsBoundDisplayResetsAndRebindsCleanly) {
    AddDisplay(1);
    auto devices = std::make_unique<MultiDisplayTouchDevices>(multi_display_, /*has_hinge=*/false);
    std::vector<TouchDeviceId> changed;
    auto listener = devices->AddActiveChangedListener(
            [&changed](TouchDeviceId device) { changed.push_back(device); });

    // Unbound displays do not fire until Find() binds a sink to them.
    Display(1)->AdvanceInputActiveGeneration();
    EXPECT_THAT(changed, IsEmpty());

    ASSERT_THAT(devices->Find(0), NotNull());
    ASSERT_THAT(devices->Find(1), NotNull());
    // Repeated Find() on the same display must not duplicate listeners.
    ASSERT_THAT(devices->Find(1), NotNull());

    EXPECT_EQ(Display(0)->AdvanceInputActiveGeneration(), 1U);
    EXPECT_EQ(Display(1)->AdvanceInputActiveGeneration(), 2U);
    EXPECT_THAT(changed, ElementsAre(TouchDeviceId{0}, TouchDeviceId{1}));

    // Removing and recreating display 1 unsubscribes the old display and
    // binds the new one on Find(1).
    RemoveDisplay(1);
    AddDisplay(1);
    ASSERT_THAT(devices->Find(1), NotNull());
    EXPECT_EQ(Display(1)->AdvanceInputActiveGeneration(), 1U);
    EXPECT_THAT(changed, ElementsAre(TouchDeviceId{0}, TouchDeviceId{1}, TouchDeviceId{1}));

    // Listener handle safely outlives TouchDevices.
    devices.reset();
    Display(0)->AdvanceInputActiveGeneration();
    EXPECT_THAT(changed, ElementsAre(TouchDeviceId{0}, TouchDeviceId{1}, TouchDeviceId{1}));
    listener.reset();
}

TEST_F(MultiDisplayTouchDevicesTest, RemovedDisplay_SinkIsInactiveAndWritesNothing) {
    AddDisplay(1);
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);
    TouchDeviceSink* sink = devices.Find(1);
    ASSERT_THAT(sink, NotNull());

    RemoveDisplay(1);
    AddDisplay(1);  // A new display, which the sink has not been rebound to.
    const EvDevEvent report[] = {kSlot0};
    sink->WriteReport(report);

    EXPECT_FALSE(sink->IsActive());
    EXPECT_THAT(Display(1)->evdev_reports, IsEmpty());
}

TEST_F(MultiDisplayTouchDevicesTest, RemovedDisplay_FindReturnsNull) {
    AddDisplay(1);
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);
    ASSERT_THAT(devices.Find(1), NotNull());

    RemoveDisplay(1);

    EXPECT_THAT(devices.Find(1), IsNull());
}

// Each display ID has its own guest input device, so a display re-created
// with the same ID is written through the same sink.
TEST_F(MultiDisplayTouchDevicesTest, RecreatedDisplay_FindRebindsSameSink) {
    AddDisplay(1);
    MultiDisplayTouchDevices devices(multi_display_, /*has_hinge=*/false);
    TouchDeviceSink* sink = devices.Find(1);
    RemoveDisplay(1);
    AddDisplay(1);

    ASSERT_EQ(devices.Find(1), sink);
    const EvDevEvent report[] = {kSlot0};
    sink->WriteReport(report);

    EXPECT_TRUE(sink->IsActive());
    EXPECT_THAT(Display(1)->evdev_reports, ElementsAre(ElementsAre(kFakeSlot0, kFakeSyn)));
}

TEST_F(MultiDisplayTouchDevicesTest, Devices_ReturnsAllExistingDisplaysIncludingFoldedZero) {
    AddDisplay(1);
    AddDisplay(3);
    multi_display_.folded = true;
    MultiDisplayTouchDevices foldable(multi_display_, /*has_hinge=*/true);

    EXPECT_THAT(foldable.Devices(), ElementsAre(0U, 1U, 3U));

    RemoveDisplay(1);
    EXPECT_THAT(foldable.Devices(), ElementsAre(0U, 3U));
}

}  // namespace
}  // namespace goldfish::grpc::v2
