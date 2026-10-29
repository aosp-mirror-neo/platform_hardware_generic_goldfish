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
#include "touch_encoding.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <ostream>

#include "slot_registry.h"
#include "standard-headers/linux/input-event-codes.h"
#include "standard-headers/linux/input.h"

namespace goldfish::display {

void PrintTo(const EvDevEvent& ev, std::ostream* os) {
    *os << "{type=" << ev.type << ", code=" << ev.code << ", value=" << ev.value << "}";
}

}  // namespace goldfish::display

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::control::EvDevEvent;
using ::android::emulation::control::EvDevEvents;
using ::android::emulation::control::kMtsPointerUp;
using ::android::emulation::control::kMtsPressureMax;
using ::android::emulation::v2::ButtonMask;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::ToolType;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Not;

constexpr uint32_t kDefaultTouchMajor = 0x500;

PointerEvent::Pointer MakePointer(ToolType tool_type = ToolType::TOOL_TYPE_FINGER) {
    PointerEvent::Pointer pointer;
    pointer.set_x(0.0F);
    pointer.set_y(1.0F);
    pointer.set_tool_type(tool_type);
    return pointer;
}

EvDevEvents DownOrMove(const PointerEvent::Pointer& pointer, PointerAction action, bool new_contact,
                       int slot = 3) {
    EvDevEvents events;
    AppendContactDownOrMove(pointer, action, slot, new_contact, events);
    return events;
}

TEST(TouchEncodingTest, NewContactGetsTrackingIdEqualToSlot) {
    EXPECT_THAT(DownOrMove(MakePointer(), PointerAction::POINTER_ACTION_DOWN, true),
                ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 3},
                            EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, 3},
                            EvDevEvent{EV_ABS, ABS_MT_POSITION_X, 0},
                            EvDevEvent{EV_ABS, ABS_MT_POSITION_Y, 0x7FFF},
                            EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER},
                            EvDevEvent{EV_ABS, ABS_MT_PRESSURE, kMtsPressureMax},
                            EvDevEvent{EV_ABS, ABS_MT_TOUCH_MAJOR, kDefaultTouchMajor},
                            EvDevEvent{EV_ABS, ABS_MT_TOUCH_MINOR, kDefaultTouchMajor}));
}

TEST(TouchEncodingTest, ExistingContactOmitsTrackingId) {
    EXPECT_THAT(DownOrMove(MakePointer(), PointerAction::POINTER_ACTION_MOVE, false),
                ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 3},
                            EvDevEvent{EV_ABS, ABS_MT_POSITION_X, 0},
                            EvDevEvent{EV_ABS, ABS_MT_POSITION_Y, 0x7FFF},
                            EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER},
                            EvDevEvent{EV_ABS, ABS_MT_PRESSURE, kMtsPressureMax},
                            EvDevEvent{EV_ABS, ABS_MT_TOUCH_MAJOR, kDefaultTouchMajor},
                            EvDevEvent{EV_ABS, ABS_MT_TOUCH_MINOR, kDefaultTouchMajor}));
}

TEST(TouchEncodingTest, HoverEncodesZeroPressure) {
    auto pointer = MakePointer();
    pointer.set_pressure(0.5F);
    EXPECT_THAT(DownOrMove(pointer, PointerAction::POINTER_ACTION_HOVER_ENTER, true),
                Contains(EvDevEvent{EV_ABS, ABS_MT_PRESSURE, 0}));
    EXPECT_THAT(DownOrMove(pointer, PointerAction::POINTER_ACTION_HOVER_MOVE, false),
                Contains(EvDevEvent{EV_ABS, ABS_MT_PRESSURE, 0}));
}

TEST(TouchEncodingTest, PressureScalesToDeviceRange) {
    auto pointer = MakePointer();
    pointer.set_pressure(0.5F);
    EXPECT_THAT(DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false),
                Contains(EvDevEvent{EV_ABS, ABS_MT_PRESSURE, kMtsPressureMax / 2}));
}

TEST(TouchEncodingTest, DownWithZeroPressureEncodesFullPressure) {
    auto pointer = MakePointer();
    pointer.set_pressure(0.0F);
    EXPECT_THAT(DownOrMove(pointer, PointerAction::POINTER_ACTION_DOWN, true),
                Contains(EvDevEvent{EV_ABS, ABS_MT_PRESSURE, kMtsPressureMax}));
}

TEST(TouchEncodingTest, PositionAndPressureClampToDeviceRange) {
    auto pointer = MakePointer();
    pointer.set_x(-0.5F);
    pointer.set_y(1.5F);
    pointer.set_pressure(2.0F);
    const auto events = DownOrMove(pointer, PointerAction::POINTER_ACTION_DOWN, true);
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_MT_POSITION_X, 0}));
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_MT_POSITION_Y, 0x7FFF}));
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_MT_PRESSURE, kMtsPressureMax}));
}

TEST(TouchEncodingTest, SizeScalesToAbsRange) {
    auto pointer = MakePointer();
    pointer.set_size(0.5F);
    EXPECT_THAT(DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false),
                Contains(EvDevEvent{EV_ABS, ABS_MT_TOUCH_MAJOR, 0x3FFF}));
}

TEST(TouchEncodingTest, OrientationIsDegreesClampedToNinety) {
    const auto encode = [](float orientation_radians) {
        auto pointer = MakePointer();
        pointer.set_orientation_radians(orientation_radians);
        return DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false);
    };
    EXPECT_THAT(encode(0.785398F), Contains(EvDevEvent{EV_ABS, ABS_MT_ORIENTATION, 45}));
    EXPECT_THAT(encode(-0.785398F),
                Contains(EvDevEvent{EV_ABS, ABS_MT_ORIENTATION, static_cast<uint32_t>(-45)}));
    EXPECT_THAT(encode(3.14159F), Contains(EvDevEvent{EV_ABS, ABS_MT_ORIENTATION, 90}));
    EXPECT_THAT(encode(-3.14159F),
                Contains(EvDevEvent{EV_ABS, ABS_MT_ORIENTATION, static_cast<uint32_t>(-90)}));
}

TEST(TouchEncodingTest, OrientationClampsToDegreeBoundsAndOmitsZeroOrNan) {
    auto nan_pointer = MakePointer();
    nan_pointer.set_orientation_radians(std::numeric_limits<float>::quiet_NaN());
    EXPECT_THAT(DownOrMove(nan_pointer, PointerAction::POINTER_ACTION_MOVE, false),
                Not(Contains(Field(&EvDevEvent::code, ABS_MT_ORIENTATION))));

    auto inf_pointer = MakePointer();
    inf_pointer.set_orientation_radians(std::numeric_limits<float>::infinity());
    EXPECT_THAT(DownOrMove(inf_pointer, PointerAction::POINTER_ACTION_MOVE, false),
                Contains(EvDevEvent{EV_ABS, ABS_MT_ORIENTATION, 90}));

    auto neg_large_pointer = MakePointer();
    neg_large_pointer.set_orientation_radians(-1e20F);
    EXPECT_THAT(DownOrMove(neg_large_pointer, PointerAction::POINTER_ACTION_MOVE, false),
                Contains(EvDevEvent{EV_ABS, ABS_MT_ORIENTATION, static_cast<uint32_t>(-90)}));
}

TEST(TouchEncodingTest, PalmEmitsPalmTool) {
    EXPECT_THAT(DownOrMove(MakePointer(ToolType::TOOL_TYPE_PALM),
                           PointerAction::POINTER_ACTION_DOWN, true),
                Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM}));
}

TEST(TouchEncodingTest, StylusEmitsPenToolRubberZeroAndStylusButtonState) {
    auto unpressed = MakePointer(ToolType::TOOL_TYPE_STYLUS);
    const auto unpressed_events = DownOrMove(unpressed, PointerAction::POINTER_ACTION_DOWN, true);
    EXPECT_THAT(unpressed_events, Contains(EvDevEvent{EV_KEY, BTN_TOOL_RUBBER, 0}));
    EXPECT_THAT(unpressed_events, Contains(EvDevEvent{EV_KEY, BTN_STYLUS, 0}));
    EXPECT_THAT(unpressed_events, Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PEN}));

    auto pressed = MakePointer(ToolType::TOOL_TYPE_STYLUS);
    pressed.set_button_mask(ButtonMask::BUTTON_MASK_STYLUS_PRIMARY);
    const auto pressed_events = DownOrMove(pressed, PointerAction::POINTER_ACTION_MOVE, false);
    EXPECT_THAT(pressed_events, Contains(EvDevEvent{EV_KEY, BTN_TOOL_RUBBER, 0}));
    EXPECT_THAT(pressed_events, Contains(EvDevEvent{EV_KEY, BTN_STYLUS, 1}));
    EXPECT_THAT(pressed_events, Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PEN}));
}

TEST(TouchEncodingTest, EraserEmitsMaxToolRubberOneAndStylusButtonState) {
    auto unpressed = MakePointer(ToolType::TOOL_TYPE_ERASER);
    const auto unpressed_events = DownOrMove(unpressed, PointerAction::POINTER_ACTION_DOWN, true);
    EXPECT_THAT(unpressed_events, Contains(EvDevEvent{EV_KEY, BTN_TOOL_RUBBER, 1}));
    EXPECT_THAT(unpressed_events, Contains(EvDevEvent{EV_KEY, BTN_STYLUS, 0}));
    EXPECT_THAT(unpressed_events, Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_MAX}));

    auto pressed = MakePointer(ToolType::TOOL_TYPE_ERASER);
    pressed.set_button_mask(ButtonMask::BUTTON_MASK_STYLUS_PRIMARY);
    const auto pressed_events = DownOrMove(pressed, PointerAction::POINTER_ACTION_MOVE, false);
    EXPECT_THAT(pressed_events, Contains(EvDevEvent{EV_KEY, BTN_TOOL_RUBBER, 1}));
    EXPECT_THAT(pressed_events, Contains(EvDevEvent{EV_KEY, BTN_STYLUS, 1}));
    EXPECT_THAT(pressed_events, Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_MAX}));
}

TEST(TouchEncodingTest, FingerOmitsStylusAndRubberKeys) {
    const auto events = DownOrMove(MakePointer(ToolType::TOOL_TYPE_FINGER),
                                   PointerAction::POINTER_ACTION_DOWN, true);
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER}));
    EXPECT_THAT(events, Not(Contains(Field(&EvDevEvent::code, BTN_TOOL_RUBBER))));
    EXPECT_THAT(events, Not(Contains(Field(&EvDevEvent::code, BTN_STYLUS))));
}

TEST(TouchEncodingTest, StylusTiltIsSplitAlongOrientation) {
    auto pointer = MakePointer(ToolType::TOOL_TYPE_STYLUS);
    pointer.set_orientation_radians(0.785398F);  // 45 degrees.
    pointer.set_tilt_radians(0.523599F);         // 30 degrees.
    const auto events = DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false);
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_TILT_X, 21}));
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_TILT_Y, static_cast<uint32_t>(-21)}));
}

TEST(TouchEncodingTest, TiltedStylusAndEraserEmitTilt) {
    for (ToolType tool_type : {ToolType::TOOL_TYPE_STYLUS, ToolType::TOOL_TYPE_ERASER}) {
        auto pointer = MakePointer(tool_type);
        pointer.set_tilt_radians(0.785398163F);  // 45 degrees, pointing up (orientation 0).
        const auto events = DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false);
        const uint32_t expected_tool =
                tool_type == ToolType::TOOL_TYPE_ERASER ? MT_TOOL_MAX : MT_TOOL_PEN;
        EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, expected_tool}));
        EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_TILT_X, 0}));
        EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_TILT_Y, static_cast<uint32_t>(-45)}));
    }
}

TEST(TouchEncodingTest, TiltedStylusClampsExtremeTiltValues) {
    auto pointer = MakePointer(ToolType::TOOL_TYPE_STYLUS);
    pointer.set_tilt_radians(1e20F);
    pointer.set_orientation_radians(0.0F);
    const auto events = DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false);
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_TILT_X, 0}));
    EXPECT_THAT(events, Contains(EvDevEvent{EV_ABS, ABS_TILT_Y, static_cast<uint32_t>(-90)}));
}

TEST(TouchEncodingTest, TiltedFingerOmitsTilt) {
    auto pointer = MakePointer(ToolType::TOOL_TYPE_FINGER);
    pointer.set_tilt_radians(0.785398163F);
    const auto events = DownOrMove(pointer, PointerAction::POINTER_ACTION_MOVE, false);
    EXPECT_THAT(events, Not(Contains(Field(&EvDevEvent::code, ABS_TILT_X))));
    EXPECT_THAT(events, Not(Contains(Field(&EvDevEvent::code, ABS_TILT_Y))));
}

TEST(TouchEncodingTest, ContactUpSelectsSlotAndClearsTrackingId) {
    EvDevEvents events;
    AppendContactUp(7, events);
    EXPECT_THAT(events, ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 7},
                                    EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp}));

    EvDevEvents stylus_up_events;
    AppendContactUp(7, stylus_up_events, /*clear_stylus_keys=*/true);
    EXPECT_THAT(stylus_up_events, ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 7},
                                              EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp},
                                              EvDevEvent{EV_KEY, BTN_TOOL_RUBBER, 0},
                                              EvDevEvent{EV_KEY, BTN_STYLUS, 0}));
}

TEST(TouchEncodingTest, ContactCancelTurnsToPalmBeforeLifting) {
    EvDevEvents palm_events;
    EvDevEvents lift_events;
    AppendContactCancel(7, palm_events, lift_events);
    EXPECT_THAT(palm_events, ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 7},
                                         EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM}));
    EXPECT_THAT(lift_events, ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 7},
                                         EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp}));

    EvDevEvents stylus_palm_events;
    EvDevEvents stylus_lift_events;
    AppendContactCancel(7, stylus_palm_events, stylus_lift_events, /*clear_stylus_keys=*/true);
    EXPECT_THAT(stylus_palm_events,
                ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 7},
                            EvDevEvent{EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM}));
    EXPECT_THAT(
            stylus_lift_events,
            ElementsAre(EvDevEvent{EV_ABS, ABS_MT_SLOT, 7},
                        EvDevEvent{EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp},
                        EvDevEvent{EV_KEY, BTN_TOOL_RUBBER, 0}, EvDevEvent{EV_KEY, BTN_STYLUS, 0}));
}

TEST(TouchEncodingTest, IsContactReleasedFollowsActionAndPressure) {
    const auto pointer = MakePointer();
    auto lifted = MakePointer();
    lifted.set_pressure(0.0F);
    auto pressed = MakePointer();
    pressed.set_pressure(0.5F);

    EXPECT_TRUE(IsContactReleased(PointerAction::POINTER_ACTION_CANCEL, pressed));
    EXPECT_TRUE(IsContactReleased(PointerAction::POINTER_ACTION_HOVER_EXIT, pointer));
    EXPECT_TRUE(IsContactReleased(PointerAction::POINTER_ACTION_UP, pointer));
    EXPECT_FALSE(IsContactReleased(PointerAction::POINTER_ACTION_UP, pressed));
    EXPECT_FALSE(IsContactReleased(PointerAction::POINTER_ACTION_DOWN, lifted));
    EXPECT_FALSE(IsContactReleased(PointerAction::POINTER_ACTION_HOVER_MOVE, lifted));
    EXPECT_FALSE(IsContactReleased(PointerAction::POINTER_ACTION_MOVE, pointer));
    EXPECT_TRUE(IsContactReleased(PointerAction::POINTER_ACTION_MOVE, lifted));
}

}  // namespace
}  // namespace goldfish::grpc::v2
