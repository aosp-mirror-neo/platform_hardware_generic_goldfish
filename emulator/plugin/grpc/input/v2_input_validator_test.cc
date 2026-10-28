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
#include "emulator/plugin/grpc/input/v2_input_validator.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"

#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2 {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::android::emulation::v2::AxisType;
using ::android::emulation::v2::ButtonMask;
using ::android::emulation::v2::InputEvent;
using ::android::emulation::v2::KeyAction;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::android::emulation::v2::ToolType;
using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::display::test::FakeMultiDisplay;

InputEvent FingerDown(float x = 0.5F, float y = 0.5F) {
    InputEvent event;
    PointerEvent* pointer = event.mutable_pointer();
    pointer->set_display_id(0);
    pointer->set_action(PointerAction::POINTER_ACTION_DOWN);
    PointerEvent::Pointer* p = pointer->add_pointers();
    p->set_pointer_id(0);
    p->set_tool_type(ToolType::TOOL_TYPE_FINGER);
    p->set_x(x);
    p->set_y(y);
    return event;
}

class V2InputValidatorTest : public ::testing::Test {
  protected:
    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    FakeMultiDisplay multi_display_{loop_.get()};
    V2InputValidator validator_{multi_display_};
};

TEST_F(V2InputValidatorTest, AcceptsValidFingerAndStylusEvents) {
    EXPECT_THAT(validator_.Validate(FingerDown()), IsOk());

    InputEvent stylus = FingerDown();
    PointerEvent::Pointer* p = stylus.mutable_pointer()->mutable_pointers(0);
    p->set_tool_type(ToolType::TOOL_TYPE_STYLUS);
    p->set_button_mask(ButtonMask::BUTTON_MASK_STYLUS_PRIMARY);
    p->set_tilt_radians(0.4F);
    p->set_orientation_radians(-0.2F);
    EXPECT_THAT(validator_.Validate(stylus), IsOk());

    p->set_tool_type(ToolType::TOOL_TYPE_ERASER);
    p->set_button_mask(ButtonMask::BUTTON_MASK_NONE);
    EXPECT_THAT(validator_.Validate(stylus), IsOk());
}

TEST_F(V2InputValidatorTest, RejectsMouseToolTypeWithFailedPrecondition) {
    InputEvent event = FingerDown();
    event.mutable_pointer()->mutable_pointers(0)->set_tool_type(ToolType::TOOL_TYPE_MOUSE);
    EXPECT_THAT(validator_.Validate(event), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(V2InputValidatorTest, RejectsRelativeMoveAndDeltasWithFailedPrecondition) {
    InputEvent relative_action = FingerDown();
    relative_action.mutable_pointer()->set_action(PointerAction::POINTER_ACTION_RELATIVE_MOVE);
    EXPECT_THAT(validator_.Validate(relative_action),
                StatusIs(absl::StatusCode::kFailedPrecondition));

    InputEvent delta_x = FingerDown();
    delta_x.mutable_pointer()->mutable_pointers(0)->set_delta_x(4.0F);
    EXPECT_THAT(validator_.Validate(delta_x), StatusIs(absl::StatusCode::kFailedPrecondition));

    InputEvent delta_y = FingerDown();
    delta_y.mutable_pointer()->mutable_pointers(0)->set_delta_y(-2.0F);
    EXPECT_THAT(validator_.Validate(delta_y), StatusIs(absl::StatusCode::kFailedPrecondition));

    InputEvent nan_delta = FingerDown();
    nan_delta.mutable_pointer()->mutable_pointers(0)->set_delta_x(
            std::numeric_limits<float>::quiet_NaN());
    EXPECT_THAT(validator_.Validate(nan_delta), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(V2InputValidatorTest, RejectsScrollActionAndScrollDeltasWithFailedPrecondition) {
    InputEvent scroll_action = FingerDown();
    scroll_action.mutable_pointer()->set_action(PointerAction::POINTER_ACTION_SCROLL);
    EXPECT_THAT(validator_.Validate(scroll_action),
                StatusIs(absl::StatusCode::kFailedPrecondition));

    InputEvent scroll_x = FingerDown();
    scroll_x.mutable_pointer()->set_scroll_x(1.0F);
    EXPECT_THAT(validator_.Validate(scroll_x), StatusIs(absl::StatusCode::kFailedPrecondition));

    InputEvent scroll_y = FingerDown();
    scroll_y.mutable_pointer()->set_scroll_y(-1.0F);
    EXPECT_THAT(validator_.Validate(scroll_y), StatusIs(absl::StatusCode::kFailedPrecondition));

    InputEvent inf_scroll = FingerDown();
    inf_scroll.mutable_pointer()->set_scroll_y(std::numeric_limits<float>::infinity());
    EXPECT_THAT(validator_.Validate(inf_scroll), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(V2InputValidatorTest, RejectsUnsupportedButtonMaskBitsWithFailedPrecondition) {
    const ButtonMask unsupported_buttons[] = {
        ButtonMask::BUTTON_MASK_PRIMARY,  ButtonMask::BUTTON_MASK_SECONDARY,
        ButtonMask::BUTTON_MASK_TERTIARY, ButtonMask::BUTTON_MASK_BACK,
        ButtonMask::BUTTON_MASK_FORWARD,  ButtonMask::BUTTON_MASK_STYLUS_SECONDARY,
    };
    for (ButtonMask button : unsupported_buttons) {
        InputEvent event = FingerDown();
        event.mutable_pointer()->mutable_pointers(0)->set_button_mask(button);
        EXPECT_THAT(validator_.Validate(event), StatusIs(absl::StatusCode::kFailedPrecondition))
                << ButtonMask_Name(button);
    }

    InputEvent combined = FingerDown();
    combined.mutable_pointer()->mutable_pointers(0)->set_button_mask(
            ButtonMask::BUTTON_MASK_STYLUS_PRIMARY | ButtonMask::BUTTON_MASK_STYLUS_SECONDARY);
    EXPECT_THAT(validator_.Validate(combined), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(V2InputValidatorTest, RejectsUnknownOrEmptyDomCodeWithInvalidArgument) {
    InputEvent valid_key;
    valid_key.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
    valid_key.mutable_key()->set_dom_code("KeyA");
    EXPECT_THAT(validator_.Validate(valid_key), IsOk());

    InputEvent ascii_char;
    ascii_char.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
    ascii_char.mutable_key()->set_dom_code("a");
    EXPECT_THAT(validator_.Validate(ascii_char), StatusIs(absl::StatusCode::kInvalidArgument));

    InputEvent empty_dom;
    empty_dom.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
    empty_dom.mutable_key()->set_dom_code("");
    EXPECT_THAT(validator_.Validate(empty_dom), StatusIs(absl::StatusCode::kInvalidArgument));

    InputEvent unicode_dom;
    unicode_dom.mutable_key()->set_action(KeyAction::KEY_ACTION_PRESS);
    unicode_dom.mutable_key()->set_dom_code("测试_🚀_é");
    EXPECT_THAT(validator_.Validate(unicode_dom), StatusIs(absl::StatusCode::kInvalidArgument));

    InputEvent unicode_text;
    unicode_text.mutable_text()->set_text("测试_🚀_é");
    EXPECT_THAT(validator_.Validate(unicode_text), IsOk());
}

TEST_F(V2InputValidatorTest, RejectsRotaryEncoderByDefaultWithFailedPrecondition) {
    InputEvent rotary;
    rotary.mutable_axis()->set_axis(AxisType::AXIS_TYPE_ROTARY_ENCODER);
    rotary.mutable_axis()->set_value(1.0F);
    EXPECT_THAT(validator_.Validate(rotary),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         ::testing::HasSubstr("no guest rotary device is registered")));

    V2InputValidator enabled_validator(multi_display_,
                                       V2InputValidator::Options{.hw_rotary_input = true});
    EXPECT_THAT(enabled_validator.Validate(rotary), IsOk());

    InputEvent unknown_axis;
    unknown_axis.mutable_axis()->set_axis(static_cast<AxisType>(99));
    unknown_axis.mutable_axis()->set_value(1.0F);
    EXPECT_THAT(validator_.Validate(unknown_axis),
                StatusIs(absl::StatusCode::kInvalidArgument, ::testing::HasSubstr("'99'")));
}

}  // namespace
}  // namespace goldfish::grpc::v2
