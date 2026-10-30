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
#include "emulator/plugin/grpc/input/v1_input_translator.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"

#include "emulator/plugin/grpc/input/dispatcher_session.h"
#include "emulator/plugin/grpc/input/input_dispatcher.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/input_session_id.h"
#include "emulator/plugin/grpc/input/keyboard_sink.h"
#include "emulator/plugin/grpc/input/multi_display_touch_devices.h"
#include "emulator/plugin/grpc/input/slot_registry.h"
#include "emulator_controller.pb.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/display.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "input_service.pb.h"
#include "standard-headers/linux/input-event-codes.h"
#include "standard-headers/linux/input.h"

namespace goldfish::grpc::v2 {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::android::emulation::control::AndroidEvent;
using ::android::emulation::control::KeyboardEvent;
using ::android::emulation::control::MouseEvent;
using ::android::emulation::control::Pen;
using ::android::emulation::control::PenEvent;
using ::android::emulation::control::Touch;
using ::android::emulation::control::TouchEvent;
using ::android::emulation::v2::ButtonMask;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::KeyAction;
using ::android::emulation::v2::KeyEvent;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::ToolType;
using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::display::Dimensions;
using ::goldfish::display::DisplayId;
using ::goldfish::display::IMultiDisplay;
using ::goldfish::display::test::ActiveFakePixmanDisplay;
using ::goldfish::display::test::FakeEvDevEvent;
using ::goldfish::display::test::FakeMultiDisplay;
using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::IsEmpty;

constexpr float kPi = std::numbers::pi_v<float>;

class NoopKeyboardSink final : public KeyboardSink {
  public:
    void WriteKey(uint32_t /*evdev_code*/, bool /*down*/) override {}
    void TypeText(std::string_view /*utf8*/) override {}
    void Reset() override {}
};

class FoldableMultiDisplay : public FakeMultiDisplay {
  public:
    using FakeMultiDisplay::FakeMultiDisplay;
    using FakeMultiDisplay::GetDisplay;

    bool IsActive(DisplayId display_id) const override { return display_id != 0 || !folded; }

    absl::StatusOr<::goldfish::display::DisplayPtr> GetDisplay(
            DisplayId display_id) const override {
        if (display_id == 0 && display_0_override != nullptr) {
            return display_0_override;
        }
        return FakeMultiDisplay::GetDisplay(display_id);
    }

    bool folded = false;
    std::shared_ptr<ActiveFakePixmanDisplay> display_0_override;
};

class V1InputTranslatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Display(0)->evdev_reports.clear();
        Display(0)->evdevs.clear();
    }

    void TearDown() override {
        if (multi_display_.display_0_override != nullptr) {
            multi_display_.display_0_override->Stop();
        }
        for (const auto& weak : multi_display_.Displays()) {
            if (auto display = weak.lock()) {
                static_cast<ActiveFakePixmanDisplay*>(display.get())->Stop();
            }
        }
    }

    void AddDisplay(DisplayId id, uint32_t w = 800, uint32_t h = 600) {
        ASSERT_THAT(multi_display_.CreateDisplay(id, w, h, 160, 0), IsOk());
        loop_->RunAll();
    }

    std::shared_ptr<ActiveFakePixmanDisplay> Display(DisplayId id) {
        return multi_display_.GetDisplay<ActiveFakePixmanDisplay>(multi_display_.GetDisplay(id));
    }

    static Touch MakeTouch(int32_t id, int32_t x, int32_t y, int32_t pressure,
                           int32_t touch_major = 0, int32_t orientation = 0) {
        Touch t;
        t.set_identifier(id);
        t.set_x(x);
        t.set_y(y);
        t.set_pressure(pressure);
        t.set_touch_major(touch_major);
        if (orientation != 0) {
            t.set_orientation(orientation);
        }
        return t;
    }

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create("qemu");
    FoldableMultiDisplay multi_display_{loop_.get()};
    V1InputTranslator translator_{multi_display_};
};

TEST_F(V1InputTranslatorTest, TranslateTouchNormalizesCoordinatesPressureSizeAndOrientation) {
    TouchEvent event;
    event.set_display(0);
    *event.add_touches() = MakeTouch(/*id=*/0, /*x=*/320, /*y=*/240, /*pressure=*/512,
                                     /*touch_major=*/500, /*orientation=*/90);

    const std::vector<InputEvent> translated = translator_.TranslateTouch(event);
    ASSERT_EQ(translated.size(), 1U);
    ASSERT_TRUE(translated[0].has_pointer());
    const PointerEvent& pointer_event = translated[0].pointer();
    EXPECT_EQ(pointer_event.display_id(), 0);
    EXPECT_EQ(pointer_event.action(), PointerAction::POINTER_ACTION_DOWN);
    ASSERT_EQ(pointer_event.pointers_size(), 1);

    const PointerEvent::Pointer& p = pointer_event.pointers(0);
    EXPECT_EQ(p.pointer_id(), 0);
    EXPECT_EQ(p.tool_type(), ToolType::TOOL_TYPE_FINGER);
    EXPECT_THAT(p.x(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(p.y(), FloatNear(0.5F, 1e-5F));
    ASSERT_TRUE(p.has_pressure());
    EXPECT_THAT(p.pressure(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(p.size(), FloatNear(500.0F / 32767.0F, 1e-6F));
    EXPECT_THAT(p.orientation_radians(), FloatNear(kPi / 2.0F, 1e-5F));
}

TEST_F(V1InputTranslatorTest, TranslateTouchClampsOutOfRangeValuesAndOmitsZeroSizeAndOrientation) {
    TouchEvent event;
    event.set_display(0);
    *event.add_touches() = MakeTouch(/*id=*/2, /*x=*/-50, /*y=*/5000, /*pressure=*/2048,
                                     /*touch_major=*/0, /*orientation=*/0);

    const std::vector<InputEvent> translated = translator_.TranslateTouch(event);
    ASSERT_EQ(translated.size(), 1U);
    const PointerEvent::Pointer& p = translated[0].pointer().pointers(0);
    EXPECT_EQ(p.pointer_id(), 2);
    EXPECT_FLOAT_EQ(p.x(), 0.0F);
    EXPECT_FLOAT_EQ(p.y(), 1.0F);
    EXPECT_FLOAT_EQ(p.pressure(), 1.0F);
    EXPECT_FLOAT_EQ(p.size(), 0.0F);
    EXPECT_FLOAT_EQ(p.orientation_radians(), 0.0F);

    TouchEvent negative_orientation;
    negative_orientation.set_display(0);
    *negative_orientation.add_touches() = MakeTouch(/*id=*/2, /*x=*/540, /*y=*/960,
                                                    /*pressure=*/512,
                                                    /*touch_major=*/0, /*orientation=*/-45);
    const std::vector<InputEvent> moved = translator_.TranslateTouch(negative_orientation);
    ASSERT_EQ(moved.size(), 1U);
    EXPECT_EQ(moved[0].pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    EXPECT_THAT(moved[0].pointer().pointers(0).orientation_radians(),
                FloatNear(-kPi / 4.0F, 1e-5F));
}

TEST_F(V1InputTranslatorTest, TranslateTouchSinglePointerLifecycleAndIgnoresUnheldRelease) {
    TouchEvent unheld_up;
    *unheld_up.add_touches() = MakeTouch(/*id=*/0, /*x=*/100, /*y=*/200, /*pressure=*/0);
    EXPECT_THAT(translator_.TranslateTouch(unheld_up), IsEmpty());

    TouchEvent down;
    *down.add_touches() = MakeTouch(/*id=*/0, /*x=*/100, /*y=*/200, /*pressure=*/1024);
    const auto down_events = translator_.TranslateTouch(down);
    ASSERT_EQ(down_events.size(), 1U);
    EXPECT_EQ(down_events[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);

    TouchEvent move;
    *move.add_touches() = MakeTouch(/*id=*/0, /*x=*/200, /*y=*/300, /*pressure=*/1024);
    const auto move_events = translator_.TranslateTouch(move);
    ASSERT_EQ(move_events.size(), 1U);
    EXPECT_EQ(move_events[0].pointer().action(), PointerAction::POINTER_ACTION_MOVE);

    TouchEvent up;
    *up.add_touches() = MakeTouch(/*id=*/0, /*x=*/200, /*y=*/300, /*pressure=*/0);
    const auto up_events = translator_.TranslateTouch(up);
    ASSERT_EQ(up_events.size(), 1U);
    EXPECT_EQ(up_events[0].pointer().action(), PointerAction::POINTER_ACTION_UP);
    EXPECT_FLOAT_EQ(up_events[0].pointer().pointers(0).pressure(), 0.0F);

    EXPECT_THAT(translator_.TranslateTouch(up), IsEmpty());
}

TEST_F(V1InputTranslatorTest, TranslateTouchMultiFingerSimultaneousDownAndPartialRelease) {
    TouchEvent finger0_down;
    *finger0_down.add_touches() = MakeTouch(/*id=*/0, /*x=*/100, /*y=*/200, /*pressure=*/1024);
    ASSERT_EQ(translator_.TranslateTouch(finger0_down).size(), 1U);

    TouchEvent finger1_joins;
    *finger1_joins.add_touches() = MakeTouch(/*id=*/0, /*x=*/110, /*y=*/210, /*pressure=*/1024);
    *finger1_joins.add_touches() = MakeTouch(/*id=*/1, /*x=*/500, /*y=*/600, /*pressure=*/1024);
    const auto join_events = translator_.TranslateTouch(finger1_joins);
    ASSERT_EQ(join_events.size(), 1U);
    EXPECT_EQ(join_events[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    ASSERT_EQ(join_events[0].pointer().pointers_size(), 2);

    TouchEvent finger1_lifts;
    *finger1_lifts.add_touches() = MakeTouch(/*id=*/0, /*x=*/120, /*y=*/220, /*pressure=*/1024);
    *finger1_lifts.add_touches() = MakeTouch(/*id=*/1, /*x=*/500, /*y=*/600, /*pressure=*/0);
    const auto partial_up = translator_.TranslateTouch(finger1_lifts);
    ASSERT_EQ(partial_up.size(), 1U);
    EXPECT_EQ(partial_up[0].pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    ASSERT_EQ(partial_up[0].pointer().pointers_size(), 2);
    EXPECT_FLOAT_EQ(partial_up[0].pointer().pointers(0).pressure(), 1.0F);
    EXPECT_FLOAT_EQ(partial_up[0].pointer().pointers(1).pressure(), 0.0F);
}

TEST_F(V1InputTranslatorTest, TranslateTouchSplitsSimultaneousNewDownAndReleaseIntoDownThenUp) {
    TouchEvent finger0_down;
    *finger0_down.add_touches() = MakeTouch(/*id=*/0, /*x=*/100, /*y=*/200, /*pressure=*/1024);
    ASSERT_EQ(translator_.TranslateTouch(finger0_down).size(), 1U);

    TouchEvent mixed;
    *mixed.add_touches() = MakeTouch(/*id=*/0, /*x=*/100, /*y=*/200, /*pressure=*/0);
    *mixed.add_touches() = MakeTouch(/*id=*/1, /*x=*/300, /*y=*/400, /*pressure=*/1024);
    const auto events = translator_.TranslateTouch(mixed);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    ASSERT_EQ(events[0].pointer().pointers_size(), 1);
    EXPECT_EQ(events[0].pointer().pointers(0).pointer_id(), 1);
    EXPECT_FLOAT_EQ(events[0].pointer().pointers(0).pressure(), 1.0F);

    EXPECT_EQ(events[1].pointer().action(), PointerAction::POINTER_ACTION_UP);
    ASSERT_EQ(events[1].pointer().pointers_size(), 1);
    EXPECT_EQ(events[1].pointer().pointers(0).pointer_id(), 0);
    EXPECT_FLOAT_EQ(events[1].pointer().pointers(0).pressure(), 0.0F);
}

TEST_F(V1InputTranslatorTest, TranslateTouchMapsArbitraryIdentifiersToSessionRangeAndFreesOnUp) {
    TouchEvent down;
    *down.add_touches() = MakeTouch(/*id=*/100, /*x=*/540, /*y=*/960, /*pressure=*/1024);
    *down.add_touches() = MakeTouch(/*id=*/-5, /*x=*/270, /*y=*/480, /*pressure=*/1024);

    const auto down_events = translator_.TranslateTouch(down);
    ASSERT_EQ(down_events.size(), 1U);
    ASSERT_EQ(down_events[0].pointer().pointers_size(), 2);
    const int32_t id0 = down_events[0].pointer().pointers(0).pointer_id();
    const int32_t id1 = down_events[0].pointer().pointers(1).pointer_id();
    EXPECT_GE(id0, 0);
    EXPECT_LT(id0, 10);
    EXPECT_GE(id1, 0);
    EXPECT_LT(id1, 10);
    EXPECT_NE(id0, id1);

    TouchEvent up;
    *up.add_touches() = MakeTouch(/*id=*/100, /*x=*/540, /*y=*/960, /*pressure=*/0);
    *up.add_touches() = MakeTouch(/*id=*/-5, /*x=*/270, /*y=*/480, /*pressure=*/0);
    const auto up_events = translator_.TranslateTouch(up);
    ASSERT_EQ(up_events.size(), 1U);
    EXPECT_EQ(up_events[0].pointer().action(), PointerAction::POINTER_ACTION_UP);
    ASSERT_EQ(up_events[0].pointer().pointers_size(), 2);
    EXPECT_EQ(up_events[0].pointer().pointers(0).pointer_id(), id0);
    EXPECT_EQ(up_events[0].pointer().pointers(1).pointer_id(), id1);
}

TEST_F(V1InputTranslatorTest,
       TranslateTouchPrefersHighPointerIdsForExternalIdsAndAvoidsCurrentBatchCollisions) {
    // A batch with [10, 0] must not assign 0 to external ID 10 before seeing ID 0 in the batch;
    // instead external ID 10 prefers 9..1 and maps to 9 while ID 0 maps to 0.
    TouchEvent batch_with_zero;
    *batch_with_zero.add_touches() = MakeTouch(/*id=*/10, /*x=*/100, /*y=*/100, /*pressure=*/1024);
    *batch_with_zero.add_touches() = MakeTouch(/*id=*/0, /*x=*/200, /*y=*/200, /*pressure=*/1024);
    const auto batch_down = translator_.TranslateTouch(batch_with_zero);
    ASSERT_EQ(batch_down.size(), 1U);
    ASSERT_EQ(batch_down[0].pointer().pointers_size(), 2);
    EXPECT_EQ(batch_down[0].pointer().pointers(0).pointer_id(), 9);
    EXPECT_EQ(batch_down[0].pointer().pointers(1).pointer_id(), 0);

    TouchEvent batch_up;
    *batch_up.add_touches() = MakeTouch(/*id=*/10, /*x=*/100, /*y=*/100, /*pressure=*/0);
    *batch_up.add_touches() = MakeTouch(/*id=*/0, /*x=*/200, /*y=*/200, /*pressure=*/0);
    ASSERT_EQ(translator_.TranslateTouch(batch_up).size(), 1U);

    // A 10-finger TouchEvent with identifiers 1..10 maps 1..9 to 1..9 and 10 to 0.
    TouchEvent ten_fingers_down;
    for (int32_t id = 1; id <= 10; ++id) {
        *ten_fingers_down.add_touches() =
                MakeTouch(id, /*x=*/id * 50, /*y=*/id * 50, /*pressure=*/1024);
    }
    const auto ten_down = translator_.TranslateTouch(ten_fingers_down);
    ASSERT_EQ(ten_down.size(), 1U);
    ASSERT_EQ(ten_down[0].pointer().pointers_size(), 10);
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(ten_down[0].pointer().pointers(i).pointer_id(), i + 1);
    }
    EXPECT_EQ(ten_down[0].pointer().pointers(9).pointer_id(), 0);

    TouchEvent ten_fingers_up;
    for (int32_t id = 1; id <= 10; ++id) {
        *ten_fingers_up.add_touches() = MakeTouch(id, /*x=*/id * 50, /*y=*/id * 50, /*pressure=*/0);
    }
    const auto ten_up = translator_.TranslateTouch(ten_fingers_up);
    ASSERT_EQ(ten_up.size(), 1U);
    EXPECT_EQ(ten_up[0].pointer().action(), PointerAction::POINTER_ACTION_UP);
    ASSERT_EQ(ten_up[0].pointer().pointers_size(), 10);
}

TEST_F(V1InputTranslatorTest, TranslateTouchDropsDownOnMissingDisplayAndKeepsBoundDimensionsOnUp) {
    TouchEvent unknown_display;
    unknown_display.set_display(9);
    *unknown_display.add_touches() = MakeTouch(/*id=*/0, /*x=*/100, /*y=*/100, /*pressure=*/1024);
    EXPECT_THAT(translator_.TranslateTouch(unknown_display), IsEmpty());

    TouchEvent down_on_0;
    down_on_0.set_display(0);
    *down_on_0.add_touches() = MakeTouch(/*id=*/0, /*x=*/320, /*y=*/240, /*pressure=*/1024);
    ASSERT_EQ(translator_.TranslateTouch(down_on_0).size(), 1U);

    // Fold display 0 while pointer 0 is still held; UP still uses bound dimensions.
    multi_display_.folded = true;

    TouchEvent up_on_0;
    up_on_0.set_display(0);
    *up_on_0.add_touches() = MakeTouch(/*id=*/0, /*x=*/320, /*y=*/240, /*pressure=*/0);
    const auto up_events = translator_.TranslateTouch(up_on_0);
    ASSERT_EQ(up_events.size(), 1U);
    EXPECT_EQ(up_events[0].pointer().action(), PointerAction::POINTER_ACTION_UP);
    EXPECT_THAT(up_events[0].pointer().pointers(0).x(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(up_events[0].pointer().pointers(0).y(), FloatNear(0.5F, 1e-5F));
}

TEST_F(V1InputTranslatorTest, MultiDisplaySimultaneousPointerZeroScalesAndLiftsIndependently) {
    multi_display_.display_0_override =
            ActiveFakePixmanDisplay::CreateShared(loop_.get(), 0, 30, 1080, 2400);
    AddDisplay(1, 1920, 1080);

    // Press mouse (id=0) on display 0 (1080x2400) at center (540, 1200).
    MouseEvent down_0;
    down_0.set_display(0);
    down_0.set_x(540);
    down_0.set_y(1200);
    down_0.set_buttons(1);
    const auto out_down_0 = translator_.TranslateMouse(down_0);
    ASSERT_TRUE(out_down_0.has_value());
    EXPECT_EQ(out_down_0->pointer().display_id(), 0);
    EXPECT_EQ(out_down_0->pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    EXPECT_THAT(out_down_0->pointer().pointers(0).x(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(out_down_0->pointer().pointers(0).y(), FloatNear(0.5F, 1e-5F));

    // Simultaneously press mouse (id=0) on display 1 (1920x1080) at (480, 270):
    // must emit DOWN (not MOVE) and normalize against 1920x1080 -> (0.25, 0.25).
    MouseEvent down_1;
    down_1.set_display(1);
    down_1.set_x(480);
    down_1.set_y(270);
    down_1.set_buttons(1);
    const auto out_down_1 = translator_.TranslateMouse(down_1);
    ASSERT_TRUE(out_down_1.has_value());
    EXPECT_EQ(out_down_1->pointer().display_id(), 1);
    EXPECT_EQ(out_down_1->pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    EXPECT_THAT(out_down_1->pointer().pointers(0).x(), FloatNear(0.25F, 1e-5F));
    EXPECT_THAT(out_down_1->pointer().pointers(0).y(), FloatNear(0.25F, 1e-5F));

    // Lift display 1; display 0 must stay held and continue scaling against 1080x2400.
    MouseEvent up_1 = down_1;
    up_1.set_buttons(0);
    const auto out_up_1 = translator_.TranslateMouse(up_1);
    ASSERT_TRUE(out_up_1.has_value());
    EXPECT_EQ(out_up_1->pointer().display_id(), 1);
    EXPECT_EQ(out_up_1->pointer().action(), PointerAction::POINTER_ACTION_UP);

    MouseEvent drag_0;
    drag_0.set_display(0);
    drag_0.set_x(270);
    drag_0.set_y(600);
    drag_0.set_buttons(1);
    const auto out_drag_0 = translator_.TranslateMouse(drag_0);
    ASSERT_TRUE(out_drag_0.has_value());
    EXPECT_EQ(out_drag_0->pointer().display_id(), 0);
    EXPECT_EQ(out_drag_0->pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    EXPECT_THAT(out_drag_0->pointer().pointers(0).x(), FloatNear(0.25F, 1e-5F));
    EXPECT_THAT(out_drag_0->pointer().pointers(0).y(), FloatNear(0.25F, 1e-5F));

    // External touch identifiers are also scoped per display without colliding.
    TouchEvent ext_0;
    ext_0.set_display(0);
    *ext_0.add_touches() = MakeTouch(/*id=*/777, /*x=*/540, /*y=*/1200, /*pressure=*/1024);
    const auto out_ext_0 = translator_.TranslateTouch(ext_0);
    ASSERT_EQ(out_ext_0.size(), 1U);
    EXPECT_EQ(out_ext_0[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    EXPECT_EQ(out_ext_0[0].pointer().pointers(0).pointer_id(), 9);

    TouchEvent ext_1;
    ext_1.set_display(1);
    *ext_1.add_touches() = MakeTouch(/*id=*/777, /*x=*/960, /*y=*/540, /*pressure=*/1024);
    const auto out_ext_1 = translator_.TranslateTouch(ext_1);
    ASSERT_EQ(out_ext_1.size(), 1U);
    EXPECT_EQ(out_ext_1[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    EXPECT_EQ(out_ext_1[0].pointer().pointers(0).pointer_id(), 9);
}

TEST_F(V1InputTranslatorTest, ResetDisplayAndSyncResetTrackerScopeResetsProperly) {
    AddDisplay(1, 1920, 1080);
    SessionResetTracker tracker;

    // Hold mouse (id=0) and external touch (id=100 -> 9) on display 0,
    // and mouse (id=0) and external touch (id=200 -> 9) on display 1.
    MouseEvent down_0;
    down_0.set_display(0);
    down_0.set_x(100);
    down_0.set_y(100);
    down_0.set_buttons(1);
    ASSERT_TRUE(translator_.TranslateMouse(down_0).has_value());

    TouchEvent ext_0;
    ext_0.set_display(0);
    *ext_0.add_touches() = MakeTouch(/*id=*/100, /*x=*/200, /*y=*/200, /*pressure=*/1024);
    ASSERT_EQ(translator_.TranslateTouch(ext_0).size(), 1U);

    MouseEvent down_1;
    down_1.set_display(1);
    down_1.set_x(300);
    down_1.set_y(300);
    down_1.set_buttons(1);
    ASSERT_TRUE(translator_.TranslateMouse(down_1).has_value());

    TouchEvent ext_1;
    ext_1.set_display(1);
    *ext_1.add_touches() = MakeTouch(/*id=*/200, /*x=*/400, /*y=*/400, /*pressure=*/1024);
    ASSERT_EQ(translator_.TranslateTouch(ext_1).size(), 1U);

    // Per-display reset on display 1 clears display 1 without disturbing display 0.
    tracker.RecordDisplayReset(1);
    translator_.SyncResetTracker(tracker);

    MouseEvent hover_1 = down_1;
    hover_1.set_buttons(0);
    EXPECT_FALSE(translator_.TranslateMouse(hover_1).has_value());
    const auto fresh_down_1 = translator_.TranslateMouse(down_1);
    ASSERT_TRUE(fresh_down_1.has_value());
    EXPECT_EQ(fresh_down_1->pointer().action(), PointerAction::POINTER_ACTION_DOWN);

    TouchEvent stale_up_1;
    stale_up_1.set_display(1);
    *stale_up_1.add_touches() = MakeTouch(/*id=*/200, /*x=*/400, /*y=*/400, /*pressure=*/0);
    EXPECT_THAT(translator_.TranslateTouch(stale_up_1), IsEmpty());

    // Display 0's active mouse drag and external touch ID 100 are still intact.
    MouseEvent drag_0 = down_0;
    drag_0.set_x(150);
    const auto out_drag_0 = translator_.TranslateMouse(drag_0);
    ASSERT_TRUE(out_drag_0.has_value());
    EXPECT_EQ(out_drag_0->pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    const auto out_move_ext_0 = translator_.TranslateTouch(ext_0);
    ASSERT_EQ(out_move_ext_0.size(), 1U);
    EXPECT_EQ(out_move_ext_0[0].pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    EXPECT_EQ(out_move_ext_0[0].pointer().pointers(0).pointer_id(), 9);

    // Exhaust remaining external touch slots on display 0 (mouse holds 0, ext 100 holds 9).
    for (int32_t ext_id = 1001; ext_id <= 1008; ++ext_id) {
        TouchEvent leak;
        leak.set_display(0);
        *leak.add_touches() = MakeTouch(ext_id, /*x=*/50, /*y=*/50, /*pressure=*/1024);
        ASSERT_EQ(translator_.TranslateTouch(leak).size(), 1U);
    }
    TouchEvent overflow;
    overflow.set_display(0);
    *overflow.add_touches() = MakeTouch(/*id=*/2000, /*x=*/50, /*y=*/50, /*pressure=*/1024);
    EXPECT_THAT(translator_.TranslateTouch(overflow), IsEmpty());

    // Full reset (e.g. idle timeout or snapshot load) clears all held pointers and leaked IDs.
    tracker.RecordFullReset();
    translator_.SyncResetTracker(tracker);

    const auto recovered = translator_.TranslateTouch(overflow);
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    EXPECT_EQ(recovered[0].pointer().pointers(0).pointer_id(), 9);

    const auto fresh_down_0 = translator_.TranslateMouse(down_0);
    ASSERT_TRUE(fresh_down_0.has_value());
    EXPECT_EQ(fresh_down_0->pointer().action(), PointerAction::POINTER_ACTION_DOWN);
}

TEST_F(V1InputTranslatorTest, FoldableHingeRoutesInactiveDisplayZeroToCoverDisplay) {
    multi_display_.display_0_override =
            ActiveFakePixmanDisplay::CreateShared(loop_.get(), 0, 30, 1080, 2400);
    AddDisplay(1, 1080, 1080);
    Display(0)->evdev_reports.clear();
    Display(1)->evdev_reports.clear();
    multi_display_.folded = true;

    V1InputTranslator hinge_translator(multi_display_, /*has_hinge=*/true);

    // Touch on display=0 while folded normalizes using cover display 1 (1080x1080), not 1080x2400.
    TouchEvent down;
    down.set_display(0);
    *down.add_touches() = MakeTouch(/*id=*/0, /*x=*/540, /*y=*/540, /*pressure=*/1024);
    const auto out = hinge_translator.TranslateTouch(down);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].pointer().display_id(), 0);
    EXPECT_THAT(out[0].pointer().pointers(0).x(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(out[0].pointer().pointers(0).y(), FloatNear(0.5F, 1e-5F));

    // Raw AndroidEvent on display=0 while folded flushes to cover display 1.
    AndroidEvent key_ev;
    key_ev.set_display(0);
    key_ev.set_type(EV_KEY);
    key_ev.set_code(KEY_POWER);
    key_ev.set_value(1);
    EXPECT_THAT(hinge_translator.DispatchAndroidEvent(key_ev), IsOk());

    AndroidEvent syn_ev;
    syn_ev.set_display(0);
    syn_ev.set_type(EV_SYN);
    syn_ev.set_code(SYN_REPORT);
    syn_ev.set_value(0);
    EXPECT_THAT(hinge_translator.DispatchAndroidEvent(syn_ev), IsOk());

    EXPECT_THAT(Display(0)->evdev_reports, IsEmpty());
    ASSERT_EQ(Display(1)->evdev_reports.size(), 1U);
    EXPECT_THAT(Display(1)->evdev_reports[0], ElementsAre(FakeEvDevEvent{EV_KEY, KEY_POWER, 1},
                                                          FakeEvDevEvent{EV_SYN, SYN_REPORT, 0}));
}

TEST_F(V1InputTranslatorTest, RealClientQuirksLowPressureArbitraryTouchIdsAndMouseLeave) {
    // AndroidEnv, JniRtcReceiver, and XCID send binary pressure=1 for active touches.
    TouchEvent low_pressure_down;
    *low_pressure_down.add_touches() = MakeTouch(/*id=*/-1928374, /*x=*/320, /*y=*/240,
                                                 /*pressure=*/1);
    *low_pressure_down.add_touches() = MakeTouch(/*id=*/154328912, /*x=*/160, /*y=*/120,
                                                 /*pressure=*/1);
    const auto down = translator_.TranslateTouch(low_pressure_down);
    ASSERT_EQ(down.size(), 1U);
    EXPECT_EQ(down[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    ASSERT_EQ(down[0].pointer().pointers_size(), 2);
    EXPECT_EQ(down[0].pointer().pointers(0).pointer_id(), 9);
    EXPECT_EQ(down[0].pointer().pointers(1).pointer_id(), 8);
    EXPECT_GT(down[0].pointer().pointers(0).pressure(), 0.0F);
    EXPECT_THAT(down[0].pointer().pointers(0).pressure(), FloatNear(1.0F / 1024.0F, 1e-6F));

    TouchEvent low_pressure_up;
    *low_pressure_up.add_touches() = MakeTouch(/*id=*/-1928374, /*x=*/320, /*y=*/240,
                                               /*pressure=*/0);
    *low_pressure_up.add_touches() = MakeTouch(/*id=*/154328912, /*x=*/160, /*y=*/120,
                                               /*pressure=*/0);
    const auto up = translator_.TranslateTouch(low_pressure_up);
    ASSERT_EQ(up.size(), 1U);
    EXPECT_EQ(up[0].pointer().action(), PointerAction::POINTER_ACTION_UP);

    // XCID sendMouseLeave sends out-of-bounds negative coordinates with buttons=0.
    MouseEvent mouse_leave;
    mouse_leave.set_x(-15);
    mouse_leave.set_y(-4);
    mouse_leave.set_buttons(0);
    EXPECT_FALSE(translator_.TranslateMouse(mouse_leave).has_value());

    MouseEvent mouse_press;
    mouse_press.set_x(100);
    mouse_press.set_y(100);
    mouse_press.set_buttons(1);
    ASSERT_TRUE(translator_.TranslateMouse(mouse_press).has_value());

    const auto clamped_leave = translator_.TranslateMouse(mouse_leave);
    ASSERT_TRUE(clamped_leave.has_value());
    EXPECT_EQ(clamped_leave->pointer().action(), PointerAction::POINTER_ACTION_UP);
    EXPECT_FLOAT_EQ(clamped_leave->pointer().pointers(0).x(), 0.0F);
    EXPECT_FLOAT_EQ(clamped_leave->pointer().pointers(0).y(), 0.0F);
    EXPECT_FLOAT_EQ(clamped_leave->pointer().pointers(0).pressure(), 0.0F);
    EXPECT_FALSE(translator_.TranslateMouse(mouse_leave).has_value());
}

TEST_F(V1InputTranslatorTest, TranslateMouseSingleFingerLifecycleAndIgnoresHoverAndRightClick) {
    MouseEvent hover;
    hover.set_x(320);
    hover.set_y(240);
    hover.set_buttons(0);
    EXPECT_FALSE(translator_.TranslateMouse(hover).has_value());

    MouseEvent right_button;
    right_button.set_x(320);
    right_button.set_y(240);
    right_button.set_buttons(2);
    EXPECT_FALSE(translator_.TranslateMouse(right_button).has_value());

    MouseEvent press;
    press.set_x(320);
    press.set_y(240);
    press.set_buttons(1);
    const auto down = translator_.TranslateMouse(press);
    ASSERT_TRUE(down.has_value());
    EXPECT_EQ(down->pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    ASSERT_EQ(down->pointer().pointers_size(), 1);
    EXPECT_EQ(down->pointer().pointers(0).pointer_id(), 0);
    EXPECT_EQ(down->pointer().pointers(0).tool_type(), ToolType::TOOL_TYPE_FINGER);
    EXPECT_THAT(down->pointer().pointers(0).x(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(down->pointer().pointers(0).y(), FloatNear(0.5F, 1e-5F));
    EXPECT_FLOAT_EQ(down->pointer().pointers(0).pressure(), 1.0F);

    MouseEvent drag;
    drag.set_x(160);
    drag.set_y(120);
    drag.set_buttons(1);
    const auto move = translator_.TranslateMouse(drag);
    ASSERT_TRUE(move.has_value());
    EXPECT_EQ(move->pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    EXPECT_THAT(move->pointer().pointers(0).x(), FloatNear(0.25F, 1e-5F));
    EXPECT_THAT(move->pointer().pointers(0).y(), FloatNear(0.25F, 1e-5F));

    MouseEvent release;
    release.set_x(160);
    release.set_y(120);
    release.set_buttons(0);
    const auto up = translator_.TranslateMouse(release);
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->pointer().action(), PointerAction::POINTER_ACTION_UP);
    EXPECT_FLOAT_EQ(up->pointer().pointers(0).pressure(), 0.0F);

    EXPECT_FALSE(translator_.TranslateMouse(release).has_value());
}

TEST_F(V1InputTranslatorTest, StudioCtrlModeMouseToTouchAndBackSharesPointerZeroWithoutSlotLeaks) {
    MultiDisplayTouchDevices touch_devices(multi_display_, /*has_hinge=*/false);
    NoopKeyboardSink keyboard_sink;
    InputDispatcher dispatcher(*loop_, touch_devices, &keyboard_sink,
                               [](const KeyEvent& /*key*/) { return 0U; });
    const InputSessionId session = dispatcher.OpenSession({.reject_down_on_held = false});

    auto dispatch_all = [&](const std::vector<InputEvent>& events) {
        for (const InputEvent& ev : events) {
            absl::Status status = absl::UnknownError("not called");
            dispatcher.DispatchPointer(
                    session, ev.pointer(),
                    [&](IInputDispatcher::DispatchResult res) { status = res.status; });
            loop_->RunAll();
            ASSERT_THAT(status, IsOk());
        }
    };

    // Mouse button down at (540, 960) -> acquires pointer 0.
    MouseEvent mouse_down;
    mouse_down.set_x(540);
    mouse_down.set_y(960);
    mouse_down.set_buttons(1);
    const auto down_event = translator_.TranslateMouse(mouse_down);
    ASSERT_TRUE(down_event.has_value());
    dispatch_all({*down_event});

    // User presses Ctrl while dragging: Studio sends TouchEvent with id=0 and id=1 both down.
    TouchEvent pinch_move;
    *pinch_move.add_touches() = MakeTouch(/*id=*/0, /*x=*/600, /*y=*/1000, /*pressure=*/1024);
    *pinch_move.add_touches() = MakeTouch(/*id=*/1, /*x=*/480, /*y=*/920, /*pressure=*/1024);
    const auto pinch_events = translator_.TranslateTouch(pinch_move);
    ASSERT_EQ(pinch_events.size(), 1U);
    EXPECT_EQ(pinch_events[0].pointer().action(), PointerAction::POINTER_ACTION_DOWN);
    dispatch_all(pinch_events);

    // User releases mouse while Ctrl is still held: Studio sends TouchEvent with id=0,1 pressure=0.
    TouchEvent pinch_release;
    *pinch_release.add_touches() = MakeTouch(/*id=*/0, /*x=*/600, /*y=*/1000, /*pressure=*/0);
    *pinch_release.add_touches() = MakeTouch(/*id=*/1, /*x=*/480, /*y=*/920, /*pressure=*/0);
    const auto release_events = translator_.TranslateTouch(pinch_release);
    ASSERT_EQ(release_events.size(), 1U);
    EXPECT_EQ(release_events[0].pointer().action(), PointerAction::POINTER_ACTION_UP);
    ASSERT_EQ(release_events[0].pointer().pointers_size(), 2);
    dispatch_all(release_events);

    // User releases Ctrl and moves mouse with buttons=0: must be ignored because pointer 0 is up.
    MouseEvent mouse_hover;
    mouse_hover.set_x(500);
    mouse_hover.set_y(500);
    mouse_hover.set_buttons(0);
    EXPECT_FALSE(translator_.TranslateMouse(mouse_hover).has_value());

    // Conversely, Touch down on id=0 followed by Mouse release (buttons=0) releases pointer 0.
    TouchEvent touch_down;
    *touch_down.add_touches() = MakeTouch(/*id=*/0, /*x=*/540, /*y=*/960, /*pressure=*/1024);
    const auto touch_down_events = translator_.TranslateTouch(touch_down);
    ASSERT_EQ(touch_down_events.size(), 1U);
    dispatch_all(touch_down_events);

    const auto mouse_up = translator_.TranslateMouse(mouse_hover);
    ASSERT_TRUE(mouse_up.has_value());
    EXPECT_EQ(mouse_up->pointer().action(), PointerAction::POINTER_ACTION_UP);
    dispatch_all({*mouse_up});

    dispatcher.Shutdown();
}

TEST_F(V1InputTranslatorTest, CustomMouseEventTranslatorCanBeInjected) {
    class StubMouseTranslator final : public MouseEventTranslator {
      public:
        std::optional<InputEvent> Translate(
                const MouseEvent& event, IMultiDisplay& /*multi_display*/, bool /*has_hinge*/,
                absl::flat_hash_map<int32_t, Dimensions>& /*held_pointers*/) override {
            InputEvent out;
            out.mutable_pointer()->set_display_id(event.display());
            out.mutable_pointer()->set_action(PointerAction::POINTER_ACTION_MOVE);
            return out;
        }
    };

    V1InputTranslator custom(multi_display_, /*has_hinge=*/false,
                             std::make_unique<StubMouseTranslator>());
    MouseEvent mouse;
    mouse.set_display(3);
    const auto out = custom.TranslateMouse(mouse);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->pointer().display_id(), 3);
    EXPECT_EQ(out->pointer().action(), PointerAction::POINTER_ACTION_MOVE);
}

TEST_F(V1InputTranslatorTest, TranslatePenMapsStylusEraserButtonOrientationAndPointerTen) {
    PenEvent down;
    down.set_display(0);
    Pen* pen = down.add_events();
    *pen->mutable_location() =
            MakeTouch(/*id=*/0, /*x=*/320, /*y=*/240, /*pressure=*/512, /*touch_major=*/200,
                      /*orientation=*/90);
    pen->set_button_pressed(true);
    pen->set_rubber_pointer(false);

    const auto down_events = translator_.TranslatePen(down);
    ASSERT_EQ(down_events.size(), 1U);
    const PointerEvent& pe = down_events[0].pointer();
    EXPECT_EQ(pe.action(), PointerAction::POINTER_ACTION_DOWN);
    ASSERT_EQ(pe.pointers_size(), 1);
    const PointerEvent::Pointer& p = pe.pointers(0);
    EXPECT_EQ(p.pointer_id(), V1InputTranslator::kPenPointerId);
    EXPECT_EQ(p.tool_type(), ToolType::TOOL_TYPE_STYLUS);
    EXPECT_EQ(p.button_mask(), ButtonMask::BUTTON_MASK_STYLUS_PRIMARY);
    EXPECT_THAT(p.x(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(p.y(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(p.pressure(), FloatNear(0.5F, 1e-5F));
    EXPECT_THAT(p.orientation_radians(), FloatNear(kPi / 2.0F, 1e-5F));

    // Move with eraser and button released.
    pen->set_rubber_pointer(true);
    pen->set_button_pressed(false);
    const auto move_events = translator_.TranslatePen(down);
    ASSERT_EQ(move_events.size(), 1U);
    EXPECT_EQ(move_events[0].pointer().action(), PointerAction::POINTER_ACTION_MOVE);
    EXPECT_EQ(move_events[0].pointer().pointers(0).tool_type(), ToolType::TOOL_TYPE_ERASER);
    EXPECT_EQ(move_events[0].pointer().pointers(0).button_mask(), ButtonMask::BUTTON_MASK_NONE);

    // Release pen.
    pen->mutable_location()->set_pressure(0);
    const auto up_events = translator_.TranslatePen(down);
    ASSERT_EQ(up_events.size(), 1U);
    EXPECT_EQ(up_events[0].pointer().action(), PointerAction::POINTER_ACTION_UP);
    EXPECT_FLOAT_EQ(up_events[0].pointer().pointers(0).pressure(), 0.0F);
}

TEST_F(V1InputTranslatorTest, TranslateKeyboardDomKeyDomCodeAndShorthands) {
    KeyboardEvent key_a;
    key_a.set_key("KeyA");
    key_a.set_eventtype(KeyboardEvent::keydown);
    const auto a_events = translator_.TranslateKeyboard(key_a);
    ASSERT_EQ(a_events.size(), 1U);
    EXPECT_EQ(a_events[0].key().dom_code(), "KeyA");
    EXPECT_EQ(a_events[0].key().action(), KeyAction::KEY_ACTION_DOWN);

    KeyboardEvent enter_press;
    enter_press.set_key("Enter");
    enter_press.set_eventtype(KeyboardEvent::keypress);
    const auto enter_events = translator_.TranslateKeyboard(enter_press);
    ASSERT_EQ(enter_events.size(), 2U);
    EXPECT_EQ(enter_events[0].key().dom_code(), "Enter");
    EXPECT_EQ(enter_events[0].key().action(), KeyAction::KEY_ACTION_DOWN);
    EXPECT_EQ(enter_events[1].key().dom_code(), "Enter");
    EXPECT_EQ(enter_events[1].key().action(), KeyAction::KEY_ACTION_UP);

    KeyboardEvent vol_up;
    vol_up.set_key("VolumeUp");
    vol_up.set_eventtype(KeyboardEvent::keydown);
    const auto vol_events = translator_.TranslateKeyboard(vol_up);
    ASSERT_EQ(vol_events.size(), 1U);
    EXPECT_EQ(vol_events[0].key().dom_code(), "AudioVolumeUp");

    KeyboardEvent standby;
    standby.set_key("Standby");
    standby.set_eventtype(KeyboardEvent::keydown);
    const auto standby_events = translator_.TranslateKeyboard(standby);
    ASSERT_EQ(standby_events.size(), 1U);
    EXPECT_EQ(standby_events[0].key().dom_code(), "Sleep");

    KeyboardEvent power;
    power.set_key("Power");
    power.set_eventtype(KeyboardEvent::keydown);
    const auto power_events = translator_.TranslateKeyboard(power);
    ASSERT_EQ(power_events.size(), 1U);
    EXPECT_EQ(power_events[0].key().dom_code(), "Power");

    KeyboardEvent back;
    back.set_key("Back");
    back.set_eventtype(KeyboardEvent::keyup);
    const auto back_events = translator_.TranslateKeyboard(back);
    ASSERT_EQ(back_events.size(), 1U);
    EXPECT_EQ(back_events[0].key().dom_code(), "GoBack");
    EXPECT_EQ(back_events[0].key().action(), KeyAction::KEY_ACTION_UP);
}

TEST_F(V1InputTranslatorTest, TranslateKeyboardPrintableAsciiAndShiftedAscii) {
    KeyboardEvent lower_a;
    lower_a.set_key("a");
    lower_a.set_eventtype(KeyboardEvent::keypress);
    const auto lower_events = translator_.TranslateKeyboard(lower_a);
    ASSERT_EQ(lower_events.size(), 2U);
    EXPECT_EQ(lower_events[0].key().dom_code(), "KeyA");
    EXPECT_EQ(lower_events[0].key().action(), KeyAction::KEY_ACTION_DOWN);
    EXPECT_EQ(lower_events[1].key().dom_code(), "KeyA");
    EXPECT_EQ(lower_events[1].key().action(), KeyAction::KEY_ACTION_UP);

    KeyboardEvent upper_a;
    upper_a.set_key("A");
    upper_a.set_eventtype(KeyboardEvent::keypress);
    const auto upper_events = translator_.TranslateKeyboard(upper_a);
    ASSERT_EQ(upper_events.size(), 4U);
    EXPECT_EQ(upper_events[0].key().dom_code(), "ShiftLeft");
    EXPECT_EQ(upper_events[0].key().action(), KeyAction::KEY_ACTION_DOWN);
    EXPECT_EQ(upper_events[1].key().dom_code(), "KeyA");
    EXPECT_EQ(upper_events[1].key().action(), KeyAction::KEY_ACTION_DOWN);
    EXPECT_EQ(upper_events[2].key().dom_code(), "KeyA");
    EXPECT_EQ(upper_events[2].key().action(), KeyAction::KEY_ACTION_UP);
    EXPECT_EQ(upper_events[3].key().dom_code(), "ShiftLeft");
    EXPECT_EQ(upper_events[3].key().action(), KeyAction::KEY_ACTION_UP);
}

TEST_F(V1InputTranslatorTest, TranslateKeyboardKeyCodeTypesTextAndCombinedFields) {
    KeyboardEvent evdev_a;
    evdev_a.set_keycode(30);
    evdev_a.set_codetype(KeyboardEvent::Evdev);
    evdev_a.set_eventtype(KeyboardEvent::keyup);
    const auto evdev_events = translator_.TranslateKeyboard(evdev_a);
    ASSERT_EQ(evdev_events.size(), 1U);
    EXPECT_EQ(evdev_events[0].key().dom_code(), "KeyA");
    EXPECT_EQ(evdev_events[0].key().action(), KeyAction::KEY_ACTION_UP);

    // Bit 11 (0x400) on Evdev keyCode is stripped.
    KeyboardEvent home_bit11;
    home_bit11.set_keycode(102 | 0x400);
    home_bit11.set_codetype(KeyboardEvent::Evdev);
    home_bit11.set_eventtype(KeyboardEvent::keydown);
    const auto home_events = translator_.TranslateKeyboard(home_bit11);
    ASSERT_EQ(home_events.size(), 1U);
    EXPECT_EQ(home_events[0].key().dom_code(), "GoHome");

    // USB, Win, Mac, and XKB keycodes for KeyB (0x070005, 0x0030, 0x000b, 0x0038).
    KeyboardEvent usb_b;
    usb_b.set_keycode(0x070005);
    usb_b.set_codetype(KeyboardEvent::Usb);
    usb_b.set_eventtype(KeyboardEvent::keydown);
    const auto usb_events = translator_.TranslateKeyboard(usb_b);
    ASSERT_EQ(usb_events.size(), 1U);
    EXPECT_EQ(usb_events[0].key().dom_code(), "KeyB");

    KeyboardEvent win_b;
    win_b.set_keycode(0x0030);
    win_b.set_codetype(KeyboardEvent::Win);
    win_b.set_eventtype(KeyboardEvent::keydown);
    const auto win_events = translator_.TranslateKeyboard(win_b);
    ASSERT_EQ(win_events.size(), 1U);
    EXPECT_EQ(win_events[0].key().dom_code(), "KeyB");

    KeyboardEvent mac_b;
    mac_b.set_keycode(0x000b);
    mac_b.set_codetype(KeyboardEvent::Mac);
    mac_b.set_eventtype(KeyboardEvent::keydown);
    const auto mac_events = translator_.TranslateKeyboard(mac_b);
    ASSERT_EQ(mac_events.size(), 1U);
    EXPECT_EQ(mac_events[0].key().dom_code(), "KeyB");

    KeyboardEvent xkb_b;
    xkb_b.set_keycode(0x0038);
    xkb_b.set_codetype(KeyboardEvent::XKB);
    xkb_b.set_eventtype(KeyboardEvent::keydown);
    const auto xkb_events = translator_.TranslateKeyboard(xkb_b);
    ASSERT_EQ(xkb_events.size(), 1U);
    EXPECT_EQ(xkb_events[0].key().dom_code(), "KeyB");

    // Combined key + text + keyCode emits in order.
    KeyboardEvent combined;
    combined.set_key("Enter");
    combined.set_text("hi");
    combined.set_keycode(30);
    combined.set_codetype(KeyboardEvent::Evdev);
    combined.set_eventtype(KeyboardEvent::keydown);
    const auto combined_events = translator_.TranslateKeyboard(combined);
    ASSERT_EQ(combined_events.size(), 3U);
    EXPECT_EQ(combined_events[0].key().dom_code(), "Enter");
    EXPECT_EQ(combined_events[1].text().text(), "hi");
    EXPECT_EQ(combined_events[2].key().dom_code(), "KeyA");

    // Unknown key and zero keyCode drops cleanly.
    KeyboardEvent invalid;
    invalid.set_key("NotARealDomKey");
    EXPECT_THAT(translator_.TranslateKeyboard(invalid), IsEmpty());
}

TEST_F(V1InputTranslatorTest, TranslateInputEventDropsWheelAndUnknownEvents) {
    ::android::emulation::control::InputEvent wheel_env;
    wheel_env.mutable_wheel_event()->set_dy(120);
    EXPECT_THAT(translator_.TranslateInputEvent(wheel_env), IsEmpty());

    ::android::emulation::control::InputEvent empty_env;
    EXPECT_THAT(translator_.TranslateInputEvent(empty_env), IsEmpty());
}

TEST_F(V1InputTranslatorTest, DispatchAndroidEventBuffersUntilSynReportAndFiltersMtSlotState) {
    auto make_android = [](int32_t display, int32_t type, int32_t code, int32_t value) {
        AndroidEvent ev;
        ev.set_display(display);
        ev.set_type(type);
        ev.set_code(code);
        ev.set_value(value);
        return ev;
    };

    // Filtered MT slot state events + non-MT key event before SYN_REPORT.
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_ABS, ABS_MT_SLOT, 1)), IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_ABS, ABS_MT_TRACKING_ID, 42)),
                IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_ABS, ABS_MT_POSITION_X, 100)),
                IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_KEY, BTN_TOUCH, 1)), IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_KEY, BTN_TOOL_FINGER, 1)),
                IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_KEY, KEY_POWER, 1)), IsOk());
    EXPECT_THAT(Display(0)->evdev_reports, IsEmpty());

    // SYN_REPORT flushes only the non-MT event.
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_SYN, SYN_REPORT, 0)), IsOk());
    EXPECT_THAT(Display(0)->evdev_reports,
                ElementsAre(ElementsAre(FakeEvDevEvent{EV_KEY, KEY_POWER, 1},
                                        FakeEvDevEvent{EV_SYN, SYN_REPORT, 0})));

    // An all-MT batch followed by SYN_REPORT does not call SendEvDevReport.
    Display(0)->evdev_reports.clear();
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_ABS, ABS_MT_SLOT, 0)), IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_SYN, SYN_REPORT, 0)), IsOk());
    EXPECT_THAT(Display(0)->evdev_reports, IsEmpty());

    // Negative display ID returns InvalidArgument on flush.
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(-1, EV_KEY, KEY_POWER, 1)), IsOk());
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(-1, EV_SYN, SYN_REPORT, 0)),
                StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(V1InputTranslatorTest, DispatchAndroidEventPerDisplayBufferingAndBufferCap) {
    AddDisplay(1);
    auto make_android = [](int32_t display, int32_t type, int32_t code, int32_t value) {
        AndroidEvent ev;
        ev.set_display(display);
        ev.set_type(type);
        ev.set_code(code);
        ev.set_value(value);
        return ev;
    };

    for (size_t i = 0; i < V1InputTranslator::kMaxBufferedAndroidEvents + 50; ++i) {
        EXPECT_THAT(translator_.DispatchAndroidEvent(
                            make_android(0, EV_REL, REL_X, static_cast<int32_t>(i))),
                    IsOk());
    }
    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(1, EV_KEY, KEY_VOLUMEUP, 1)), IsOk());

    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(1, EV_SYN, SYN_REPORT, 0)), IsOk());
    ASSERT_EQ(Display(1)->evdev_reports.size(), 1U);
    EXPECT_THAT(Display(1)->evdev_reports[0], ElementsAre(FakeEvDevEvent{EV_KEY, KEY_VOLUMEUP, 1},
                                                          FakeEvDevEvent{EV_SYN, SYN_REPORT, 0}));
    EXPECT_THAT(Display(0)->evdev_reports, IsEmpty());

    EXPECT_THAT(translator_.DispatchAndroidEvent(make_android(0, EV_SYN, SYN_REPORT, 0)), IsOk());
    ASSERT_EQ(Display(0)->evdev_reports.size(), 1U);
    // 256 buffered events + 1 trailing SYN_REPORT added by FakePixmanDisplay::SendEvDevReport.
    EXPECT_EQ(Display(0)->evdev_reports[0].size(),
              V1InputTranslator::kMaxBufferedAndroidEvents + 1);
}

TEST_F(V1InputTranslatorTest, TranslateInputEventDispatchesAllEnvelopeVariants) {
    ::android::emulation::control::InputEvent touch_env;
    *touch_env.mutable_touch_event()->add_touches() =
            MakeTouch(/*id=*/0, /*x=*/540, /*y=*/960, /*pressure=*/1024);
    EXPECT_EQ(translator_.TranslateInputEvent(touch_env).size(), 1U);

    ::android::emulation::control::InputEvent mouse_env;
    mouse_env.mutable_mouse_event()->set_x(540);
    mouse_env.mutable_mouse_event()->set_y(960);
    mouse_env.mutable_mouse_event()->set_buttons(0);
    EXPECT_EQ(translator_.TranslateInputEvent(mouse_env).size(), 1U);

    ::android::emulation::control::InputEvent pen_env;
    *pen_env.mutable_pen_event()->add_events()->mutable_location() =
            MakeTouch(/*id=*/0, /*x=*/540, /*y=*/960, /*pressure=*/512);
    EXPECT_EQ(translator_.TranslateInputEvent(pen_env).size(), 1U);

    ::android::emulation::control::InputEvent key_env;
    key_env.mutable_key_event()->set_key("KeyA");
    key_env.mutable_key_event()->set_eventtype(KeyboardEvent::keydown);
    EXPECT_EQ(translator_.TranslateInputEvent(key_env).size(), 1U);

    ::android::emulation::control::InputEvent wheel_env;
    wheel_env.mutable_wheel_event()->set_dy(-60);
    EXPECT_THAT(translator_.TranslateInputEvent(wheel_env), IsEmpty());

    ::android::emulation::control::InputEvent android_env;
    android_env.mutable_android_event()->set_display(0);
    android_env.mutable_android_event()->set_type(EV_KEY);
    android_env.mutable_android_event()->set_code(KEY_POWER);
    android_env.mutable_android_event()->set_value(1);
    EXPECT_THAT(translator_.TranslateInputEvent(android_env), IsEmpty());

    android_env.mutable_android_event()->set_type(EV_SYN);
    android_env.mutable_android_event()->set_code(SYN_REPORT);
    android_env.mutable_android_event()->set_value(0);
    EXPECT_THAT(translator_.TranslateInputEvent(android_env), IsEmpty());
    EXPECT_EQ(Display(0)->evdev_reports.size(), 1U);
}

struct GuestContact {
    uint32_t slot = 0;
    int32_t tracking_id = -1;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t pressure = 0;
    uint32_t touch_major = 0;
    uint32_t touch_minor = 0;
    int32_t orientation = 0;
    uint32_t tool_type = MT_TOOL_FINGER;
};

class ProtocolBGuestState {
  public:
    void ApplyReport(const std::vector<FakeEvDevEvent>& report) {
        for (const FakeEvDevEvent& ev : report) {
            Apply(ev.type, ev.code, ev.value);
        }
    }

    void Apply(uint16_t type, uint16_t code, uint32_t value) {
        if (type == EV_SYN && code == SYN_REPORT) {
            reported_slots_ = slots_;
            return;
        }
        if (type != EV_ABS) {
            return;
        }
        if (code == ABS_MT_SLOT) {
            selected_slot_ = value;
            return;
        }
        ASSERT_LT(selected_slot_, slots_.size());
        GuestContact& slot = slots_[selected_slot_];
        switch (code) {
        case ABS_MT_TRACKING_ID:
            slot.tracking_id = static_cast<int32_t>(value);
            break;
        case ABS_MT_POSITION_X:
            slot.x = value;
            break;
        case ABS_MT_POSITION_Y:
            slot.y = value;
            break;
        case ABS_MT_PRESSURE:
            slot.pressure = value;
            break;
        case ABS_MT_TOUCH_MAJOR:
            slot.touch_major = value;
            break;
        case ABS_MT_TOUCH_MINOR:
            slot.touch_minor = value;
            break;
        case ABS_MT_ORIENTATION:
            slot.orientation = static_cast<int32_t>(value);
            break;
        case ABS_MT_TOOL_TYPE:
            slot.tool_type = value;
            break;
        default:
            break;
        }
    }

    std::vector<GuestContact> ActiveContacts() const {
        std::vector<GuestContact> active;
        for (const GuestContact& slot : reported_slots_) {
            if (slot.tracking_id != -1) {
                active.push_back(slot);
            }
        }
        return active;
    }

    const GuestContact& Slot(uint32_t slot) const { return reported_slots_.at(slot); }

  private:
    static std::array<GuestContact, kTouchDeviceSlots> InitialSlots() {
        std::array<GuestContact, kTouchDeviceSlots> slots{};
        for (uint32_t i = 0; i < slots.size(); ++i) {
            slots[i].slot = i;
        }
        return slots;
    }

    uint32_t selected_slot_ = 0;
    std::array<GuestContact, kTouchDeviceSlots> slots_ = InitialSlots();
    std::array<GuestContact, kTouchDeviceSlots> reported_slots_ = InitialSlots();
};

class StudioProtocolBGoldenTest : public V1InputTranslatorTest,
                                  public ::testing::WithParamInterface<int32_t> {
  protected:
    static constexpr uint32_t kWidth = 1080;
    static constexpr uint32_t kHeight = 1920;
    static constexpr int32_t kStudioPressure = 0x400;
    static constexpr uint32_t kDefaultTouchMajor = 0x500;

    void SetUp() override {
        V1InputTranslatorTest::SetUp();
        multi_display_.display_0_override =
                ActiveFakePixmanDisplay::CreateShared(loop_.get(), 0, 30, kWidth, kHeight);
        AddDisplay(1, kWidth, kHeight);
        Display(0)->evdev_reports.clear();
        Display(1)->evdev_reports.clear();

        touch_devices_ =
                std::make_unique<MultiDisplayTouchDevices>(multi_display_, /*has_hinge=*/false);
        dispatcher_ = std::make_shared<InputDispatcher>(*loop_, *touch_devices_, &keyboard_sink_,
                                                        [](const KeyEvent& /*key*/) { return 0U; });
        session_ = std::make_unique<DispatcherSession>(
                dispatcher_, IInputDispatcher::SessionOptions{
                                 .human = true, .source_id = 0, .reject_down_on_held = false});
    }

    void TearDown() override {
        session_.reset();
        loop_->RunAll();
        dispatcher_->Shutdown();
        V1InputTranslatorTest::TearDown();
    }

    static uint32_t ExpectedX(int32_t x) {
        const float norm =
                std::clamp(static_cast<float>(x) / static_cast<float>(kWidth), 0.0F, 1.0F);
        return static_cast<uint32_t>(norm * 32767.0F);
    }

    static uint32_t ExpectedY(int32_t y) {
        const float norm =
                std::clamp(static_cast<float>(y) / static_cast<float>(kHeight), 0.0F, 1.0F);
        return static_cast<uint32_t>(norm * 32767.0F);
    }

    void SyncReports() {
        const auto& reports = Reports();
        while (applied_reports_ < reports.size()) {
            guest_.ApplyReport(reports[applied_reports_++]);
        }
    }

    void SendV1(const ::android::emulation::control::InputEvent& v1_event) {
        translator_.SyncResetTracker(session_->ResetTracker());
        for (const InputEvent& v2_event : translator_.TranslateInputEvent(v1_event)) {
            session_->Dispatch(v2_event);
        }
        loop_->RunAll();
        SyncReports();
    }

    void SendMouse(int32_t x, int32_t y, int32_t buttons) {
        ::android::emulation::control::InputEvent ev;
        MouseEvent* mouse = ev.mutable_mouse_event();
        mouse->set_display(GetParam());
        mouse->set_x(x);
        mouse->set_y(y);
        mouse->set_buttons(buttons);
        SendV1(ev);
    }

    void SendTouch(std::initializer_list<Touch> touches) {
        ::android::emulation::control::InputEvent ev;
        TouchEvent* touch_event = ev.mutable_touch_event();
        touch_event->set_display(GetParam());
        for (const Touch& t : touches) {
            *touch_event->add_touches() = t;
        }
        SendV1(ev);
    }

    void SendPen(int32_t x, int32_t y, int32_t pressure, bool rubber, bool button_pressed,
                 int32_t orientation = 0) {
        ::android::emulation::control::InputEvent ev;
        PenEvent* pen_event = ev.mutable_pen_event();
        pen_event->set_display(GetParam());
        Pen* pen = pen_event->add_events();
        *pen->mutable_location() =
                MakeTouch(/*id=*/0, x, y, pressure, /*touch_major=*/200, orientation);
        pen->set_rubber_pointer(rubber);
        pen->set_button_pressed(button_pressed);
        SendV1(ev);
    }

    const std::vector<std::vector<FakeEvDevEvent>>& Reports() {
        return Display(static_cast<DisplayId>(GetParam()))->evdev_reports;
    }

    NoopKeyboardSink keyboard_sink_;
    std::unique_ptr<MultiDisplayTouchDevices> touch_devices_;
    std::shared_ptr<InputDispatcher> dispatcher_;
    std::unique_ptr<DispatcherSession> session_;
    ProtocolBGuestState guest_;
    size_t applied_reports_ = 0;
};

TEST_P(StudioProtocolBGoldenTest, HoverProducesZeroReports) {
    SendMouse(100, 200, /*buttons=*/0);
    SendMouse(150, 250, /*buttons=*/0);
    SendMouse(200, 300, /*buttons=*/0);

    EXPECT_THAT(Reports(), IsEmpty());
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());
}

TEST_P(StudioProtocolBGoldenTest, PressDragReleaseProducesThreeReportsOnSlotZero) {
    SendMouse(100, 200, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 1U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(100));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(200));
    EXPECT_EQ(guest_.Slot(0).pressure, static_cast<uint32_t>(kStudioPressure));
    EXPECT_EQ(guest_.Slot(0).touch_major, kDefaultTouchMajor);
    EXPECT_EQ(guest_.Slot(0).tool_type, static_cast<uint32_t>(MT_TOOL_FINGER));

    SendMouse(300, 400, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 2U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(300));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(400));
    EXPECT_EQ(guest_.Slot(0).pressure, static_cast<uint32_t>(kStudioPressure));

    SendMouse(300, 400, /*buttons=*/0);
    ASSERT_EQ(Reports().size(), 3U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, -1);
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());
}

TEST_P(StudioProtocolBGoldenTest, DragCrossesDisplayEdgeLiftsIgnoresRepeatsAndReenters) {
    SendMouse(500, 600, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 1U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);

    // Drag crosses display edge -> Studio sends buttons=0 at clamped edge.
    SendMouse(1080, 600, /*buttons=*/0);
    ASSERT_EQ(Reports().size(), 2U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, -1);
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());

    // Repeated buttons=0 moves while outside / on edge write nothing.
    SendMouse(1080, 650, /*buttons=*/0);
    SendMouse(1080, 700, /*buttons=*/0);
    EXPECT_EQ(Reports().size(), 2U);
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());

    // Re-entry with buttons=1 starts a new contact on slot 0.
    SendMouse(900, 700, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 3U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(900));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(700));
    EXPECT_EQ(guest_.Slot(0).pressure, static_cast<uint32_t>(kStudioPressure));
}

TEST_P(StudioProtocolBGoldenTest, LeavingViewMidDragMovesSlotZeroOnNextPressWithoutSecondSlot) {
    SendMouse(100, 200, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 1U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);

    // Next press arrives without an intermediate buttons=0 release.
    SendMouse(500, 600, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 2U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(500));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(600));
    EXPECT_EQ(guest_.Slot(0).pressure, static_cast<uint32_t>(kStudioPressure));
}

TEST_P(StudioProtocolBGoldenTest, CtrlPressedMidDragMovesSlotZeroAndPressesSlotOneInOneReport) {
    SendMouse(100, 200, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 1U);

    SendTouch({MakeTouch(/*id=*/0, /*x=*/120, /*y=*/220, kStudioPressure),
               MakeTouch(/*id=*/1, /*x=*/880, /*y=*/1780, kStudioPressure)});
    ASSERT_EQ(Reports().size(), 2U);

    // Verify that the TouchEvent report only assigns ABS_MT_TRACKING_ID once (for slot 1).
    int tracking_id_events = 0;
    for (const FakeEvDevEvent& ev : Reports()[1]) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
            ++tracking_id_events;
            EXPECT_EQ(ev.value, 1U);
        }
    }
    EXPECT_EQ(tracking_id_events, 1);

    ASSERT_EQ(guest_.ActiveContacts().size(), 2U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(120));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(220));
    EXPECT_EQ(guest_.Slot(0).pressure, static_cast<uint32_t>(kStudioPressure));
    EXPECT_EQ(guest_.Slot(1).tracking_id, 1);
    EXPECT_EQ(guest_.Slot(1).x, ExpectedX(880));
    EXPECT_EQ(guest_.Slot(1).y, ExpectedY(1780));
    EXPECT_EQ(guest_.Slot(1).pressure, static_cast<uint32_t>(kStudioPressure));
}

TEST_P(StudioProtocolBGoldenTest, CtrlReleasedMidDragLiftsBothSlotsThenMousePressesSlotZeroAgain) {
    SendMouse(100, 200, /*buttons=*/1);
    SendTouch({MakeTouch(/*id=*/0, /*x=*/120, /*y=*/220, kStudioPressure),
               MakeTouch(/*id=*/1, /*x=*/880, /*y=*/1780, kStudioPressure)});
    ASSERT_EQ(Reports().size(), 2U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 2U);

    // terminateDragging sends TouchEvent with id 0 and id 1 at pressure 0 -> one report lifts both.
    SendTouch({MakeTouch(/*id=*/0, /*x=*/120, /*y=*/220, /*pressure=*/0),
               MakeTouch(/*id=*/1, /*x=*/880, /*y=*/1780, /*pressure=*/0)});
    ASSERT_EQ(Reports().size(), 3U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, -1);
    EXPECT_EQ(guest_.Slot(1).tracking_id, -1);
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());

    // Continuing mouse drag with buttons=1 cleanly presses slot 0 again.
    SendMouse(140, 240, /*buttons=*/1);
    ASSERT_EQ(Reports().size(), 4U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(140));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(240));
    EXPECT_EQ(guest_.Slot(0).pressure, static_cast<uint32_t>(kStudioPressure));
}

TEST_P(StudioProtocolBGoldenTest, CtrlTappedWithoutMouseMoveMidDragRecoversSlotZeroCleanly) {
    // User holds mouse button at (200, 300), taps and releases Ctrl without moving the mouse
    // (Studio sends TouchEvent(id=0 p=0, id=1 p=0) where id=1 was never down), then drags.
    SendMouse(200, 300, /*buttons=*/1);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);

    SendTouch({MakeTouch(/*id=*/0, /*x=*/200, /*y=*/300, /*pressure=*/0),
               MakeTouch(/*id=*/1, /*x=*/880, /*y=*/1620, /*pressure=*/0)});
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());

    SendMouse(250, 350, /*buttons=*/1);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tracking_id, 0);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(250));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(350));
}

TEST_P(StudioProtocolBGoldenTest, StylusDownMoveUpIsolatesPointerTenFromMouseSlotZero) {
    SendMouse(100, 200, /*buttons=*/1);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tool_type, static_cast<uint32_t>(MT_TOOL_FINGER));

    SendPen(300, 500, /*pressure=*/512, /*rubber=*/false, /*button_pressed=*/false,
            /*orientation=*/-45);
    ASSERT_EQ(guest_.ActiveContacts().size(), 2U);
    EXPECT_EQ(guest_.Slot(1).tool_type, static_cast<uint32_t>(MT_TOOL_PEN));
    EXPECT_EQ(guest_.Slot(1).orientation, -45);
    EXPECT_EQ(guest_.Slot(1).x, ExpectedX(300));
    EXPECT_EQ(guest_.Slot(1).y, ExpectedY(500));

    SendPen(350, 550, /*pressure=*/512, /*rubber=*/false, /*button_pressed=*/false,
            /*orientation=*/30);
    EXPECT_EQ(guest_.Slot(1).orientation, 30);
    EXPECT_EQ(guest_.Slot(1).x, ExpectedX(350));
    EXPECT_EQ(guest_.Slot(1).y, ExpectedY(550));

    SendPen(350, 550, /*pressure=*/0, /*rubber=*/false, /*button_pressed=*/false);
    ASSERT_EQ(guest_.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_.Slot(0).tool_type, static_cast<uint32_t>(MT_TOOL_FINGER));

    SendMouse(100, 200, /*buttons=*/0);
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());
}

TEST_P(StudioProtocolBGoldenTest, TwoFingerMirrorDownMoveUpInThreeReports) {
    SendTouch({MakeTouch(/*id=*/0, /*x=*/200, /*y=*/400, kStudioPressure),
               MakeTouch(/*id=*/1, /*x=*/880, /*y=*/1520, kStudioPressure)});
    ASSERT_EQ(Reports().size(), 1U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 2U);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(200));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(400));
    EXPECT_EQ(guest_.Slot(1).x, ExpectedX(880));
    EXPECT_EQ(guest_.Slot(1).y, ExpectedY(1520));

    SendTouch({MakeTouch(/*id=*/0, /*x=*/250, /*y=*/450, kStudioPressure),
               MakeTouch(/*id=*/1, /*x=*/830, /*y=*/1470, kStudioPressure)});
    ASSERT_EQ(Reports().size(), 2U);
    ASSERT_EQ(guest_.ActiveContacts().size(), 2U);
    EXPECT_EQ(guest_.Slot(0).x, ExpectedX(250));
    EXPECT_EQ(guest_.Slot(0).y, ExpectedY(450));
    EXPECT_EQ(guest_.Slot(1).x, ExpectedX(830));
    EXPECT_EQ(guest_.Slot(1).y, ExpectedY(1470));

    SendTouch({MakeTouch(/*id=*/0, /*x=*/250, /*y=*/450, /*pressure=*/0),
               MakeTouch(/*id=*/1, /*x=*/880, /*y=*/1470, /*pressure=*/0)});
    ASSERT_EQ(Reports().size(), 3U);
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());
}

TEST_P(StudioProtocolBGoldenTest, RightAndMiddleMouseButtonsProduceZeroReports) {
    SendMouse(300, 400, /*buttons=*/2);
    SendMouse(300, 400, /*buttons=*/4);
    SendMouse(300, 400, /*buttons=*/6);

    EXPECT_THAT(Reports(), IsEmpty());
    EXPECT_THAT(guest_.ActiveContacts(), IsEmpty());
}

INSTANTIATE_TEST_SUITE_P(PrimaryAndSecondaryDisplays, StudioProtocolBGoldenTest,
                         ::testing::Values(0, 1));

TEST_F(V1InputTranslatorTest, StudioSingleStreamMultiDisplaySimultaneousDragAndResetRecovery) {
    multi_display_.display_0_override =
            ActiveFakePixmanDisplay::CreateShared(loop_.get(), 0, 30, 1080, 1920);
    AddDisplay(1, 1920, 1080);
    Display(0)->evdev_reports.clear();
    Display(1)->evdev_reports.clear();

    MultiDisplayTouchDevices touch_devices(multi_display_, /*has_hinge=*/false);
    NoopKeyboardSink keyboard_sink;
    auto dispatcher = std::make_shared<InputDispatcher>(*loop_, touch_devices, &keyboard_sink,
                                                        [](const KeyEvent& /*key*/) { return 0U; });
    DispatcherSession session(dispatcher,
                              IInputDispatcher::SessionOptions{
                                  .human = true, .source_id = 0, .reject_down_on_held = false});

    ProtocolBGuestState guest_0;
    ProtocolBGuestState guest_1;
    size_t applied_0 = 0;
    size_t applied_1 = 0;
    auto sync_guests = [&]() {
        while (applied_0 < Display(0)->evdev_reports.size()) {
            guest_0.ApplyReport(Display(0)->evdev_reports[applied_0++]);
        }
        if (multi_display_.GetDisplay(1).ok()) {
            while (applied_1 < Display(1)->evdev_reports.size()) {
                guest_1.ApplyReport(Display(1)->evdev_reports[applied_1++]);
            }
        }
    };

    auto send_mouse = [&](int32_t display_id, int32_t x, int32_t y, int32_t buttons) {
        ::android::emulation::control::InputEvent ev;
        MouseEvent* mouse = ev.mutable_mouse_event();
        mouse->set_display(display_id);
        mouse->set_x(x);
        mouse->set_y(y);
        mouse->set_buttons(buttons);
        translator_.SyncResetTracker(session.ResetTracker());
        for (const InputEvent& v2_event : translator_.TranslateInputEvent(ev)) {
            session.Dispatch(v2_event);
        }
        loop_->RunAll();
        sync_guests();
    };

    // Simultaneous mouse drag on Display 0 and Display 1 over one shared session.
    send_mouse(/*display_id=*/0, 540, 960, /*buttons=*/1);
    send_mouse(/*display_id=*/1, 960, 540, /*buttons=*/1);
    ASSERT_EQ(guest_0.ActiveContacts().size(), 1U);
    ASSERT_EQ(guest_1.ActiveContacts().size(), 1U);

    // Removing Display 1 mid-drag records a per-display reset for Display 1 only:
    // Display 0 continues dragging seamlessly, while recreating Display 1 and continuing the
    // drag re-acquires a fresh DOWN contact on Display 1.
    Display(1)->Stop();
    ASSERT_THAT(multi_display_.EraseDisplay(1), IsOk());
    dispatcher->OnDisplayRemoved(1);
    loop_->RunAll();

    send_mouse(/*display_id=*/0, 560, 980, /*buttons=*/1);
    ASSERT_EQ(guest_0.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_0.ActiveContacts()[0].x, static_cast<uint32_t>((560.0F / 1080.0F) * 32767.0F));

    guest_1 = ProtocolBGuestState{};
    applied_1 = 0;
    AddDisplay(1, 1920, 1080);
    Display(1)->evdev_reports.clear();
    dispatcher->OnDisplayAdded(1);
    loop_->RunAll();
    sync_guests();
    EXPECT_THAT(guest_1.ActiveContacts(), IsEmpty());

    send_mouse(/*display_id=*/1, 1000, 600, /*buttons=*/1);
    ASSERT_EQ(guest_1.ActiveContacts().size(), 1U);
    EXPECT_EQ(guest_1.ActiveContacts()[0].x, static_cast<uint32_t>((1000.0F / 1920.0F) * 32767.0F));

    // Mid-drag snapshot load cancels guest contacts and notifies ResetTracker; the next mouse
    // drag frame on Display 0 re-acquires a fresh DOWN contact instead of failing as unheld MOVE.
    dispatcher->OnSnapshotLoaded();
    loop_->RunAll();
    sync_guests();
    EXPECT_THAT(guest_0.ActiveContacts(), IsEmpty());
    EXPECT_THAT(guest_1.ActiveContacts(), IsEmpty());

    send_mouse(/*display_id=*/0, 600, 1000, /*buttons=*/1);
    ASSERT_EQ(guest_0.ActiveContacts().size(), 1U);
    EXPECT_THAT(guest_1.ActiveContacts(), IsEmpty());

    dispatcher->Shutdown();
}

}  // namespace
}  // namespace goldfish::grpc::v2
