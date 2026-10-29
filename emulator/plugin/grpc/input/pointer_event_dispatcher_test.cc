// Copyright (C) 2025 The Android Open Source Project
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
#include "pointer_event_dispatcher.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <ostream>
#include <thread>

#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"

#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/display/test/mock_display.h"
#include "slot_registry.h"
#include "standard-headers/linux/input-event-codes.h"
#include "standard-headers/linux/input.h"

using android::emulation::control::EvDevEvent;
using ::goldfish::async::testing::TestEventLoop;
using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Field;
using ::testing::Mock;
using ::testing::NiceMock;
using ::testing::UnorderedElementsAre;

namespace goldfish::display {

// Helper function to print EvDevEvent objects in case of assertion failure.
// Defined in the namespace of the aliased type so that gtest finds it.
void PrintTo(const android::emulation::control::EvDevEvent& event, std::ostream* os) {
    *os << "{type: " << std::hex << event.type << ", code: " << event.code
        << ", value: " << event.value << std::dec << "}";
}

}  // namespace goldfish::display

namespace android {
namespace emulation {
namespace control {

namespace internal {

using ::goldfish::display::test::MockDisplay;
using namespace std::chrono_literals;

// Matches an EvDevEvent with the given type and code whose value matches `value`.
auto IsEvDev(uint16_t type, uint16_t code, ::testing::Matcher<uint32_t> value) {
    return AllOf(Field(&EvDevEvent::type, type), Field(&EvDevEvent::code, code),
                 Field(&EvDevEvent::value, value));
}

TEST(PenTouchEventTest, EvDevEventsConversion) {
    SlotRegistry registry;
    PenTouchEvent event;
    Pen pen;
    pen.button_pressed = true;
    pen.rubber_pointer = true;
    pen.x = 1;
    pen.y = 2;
    pen.identifier = 0x0a;
    pen.pressure = 50;
    pen.touch_major = 0;
    pen.touch_minor = 0;
    pen.orientation = 45;

    event.touches.push_back(pen);
    EvDevEvents events = event.touches[0].ToEvDevEvents(1024, 768, &registry);
    // Expected events.
    EvDevEvents expected_events = {{EV_KEY, BTN_TOOL_RUBBER, 1},
                                   {EV_KEY, BTN_STYLUS, 1},
                                   {EV_ABS, ABS_MT_SLOT, 0},
                                   {EV_ABS, ABS_MT_TRACKING_ID, 0},
                                   {EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_MAX},
                                   {EV_ABS, ABS_MT_ORIENTATION, 45},
                                   {EV_ABS, ABS_MT_PRESSURE, 50},
                                   {EV_ABS, ABS_MT_POSITION_X, 31},
                                   {EV_ABS, ABS_MT_POSITION_Y, 85}};

    EXPECT_EQ(events.size(), expected_events.size());
    EXPECT_THAT(events, Eq(expected_events));
}

TEST(PenTouchEventTest, SendEvents) {
    // Create a mock display.
    auto evloop = TestEventLoop::Create();
    MockDisplay display(evloop.get(), 0, 1024, 768);

    // Create a PointerEventDispatcher.
    PointerEventDispatcher dispatcher;

    // Create a PenTouchEvent.
    PenTouchEvent pen_event;
    Pen pen;
    pen.button_pressed = true;
    pen.rubber_pointer = true;
    pen.x = 100;
    pen.y = 200;
    pen.identifier = 0x0a;
    pen.pressure = 50;
    pen.touch_major = 10;
    pen.touch_minor = 5;
    pen.orientation = 45;
    pen_event.touches.push_back(pen);

    // Expect the whole report in one call on the mock display.
    EXPECT_CALL(
            display,
            SendEvDevReport(ElementsAre(
                    IsEvDev(EV_KEY, BTN_TOOL_RUBBER, 1), IsEvDev(EV_KEY, BTN_STYLUS, 1),
                    IsEvDev(EV_ABS, ABS_MT_SLOT, 0), IsEvDev(EV_ABS, ABS_MT_TRACKING_ID, 0),
                    IsEvDev(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_MAX),
                    IsEvDev(EV_ABS, ABS_MT_TOUCH_MAJOR, 10), IsEvDev(EV_ABS, ABS_MT_TOUCH_MINOR, 5),
                    IsEvDev(EV_ABS, ABS_MT_ORIENTATION, 45), IsEvDev(EV_ABS, ABS_MT_PRESSURE, 50),
                    IsEvDev(EV_ABS, ABS_MT_POSITION_X, _), IsEvDev(EV_ABS, ABS_MT_POSITION_Y, _))));

    // Call SendEvents.
    dispatcher.SendEvents(display, pen_event);
}

struct PointerEventDispatcherUnderTest : public PointerEventDispatcher {
    using PointerEventDispatcher::PointerEventDispatcher;
    SlotRegistry* registry() { return &registry_; }
};

TEST(PenTouchEventTest, SendEventsWithOldSlots) {
    // Create a mock display, note we are using a nicemock
    // as we will make a series of calls to our mock that we
    // do not care about.
    auto evloop = TestEventLoop::Create();
    ::testing::NiceMock<MockDisplay> display(evloop.get(), 0, 1024, 768);

    // Create a PointerEventDispatcher.
    absl::SimulatedClock clock;
    PointerEventDispatcherUnderTest dispatcher(clock);
    dispatcher.registry()->SetSlotExpiration(absl::Milliseconds(100));

    // Create a PenTouchEvent.
    PenTouchEvent pen_event;
    Pen pen;
    pen.button_pressed = true;
    pen.rubber_pointer = true;
    pen.x = 100;
    pen.y = 200;
    pen.identifier = 0x0a;
    pen.pressure = 50;
    pen.touch_major = 10;
    pen.touch_minor = 5;
    pen.orientation = 45;
    pen_event.touches.push_back(pen);

    // Call SendEvents.
    dispatcher.SendEvents(display, pen_event);
    clock.AdvanceTime(absl::Milliseconds(500));

    // Expect the release of the expired slot as one report on the mock display.
    EXPECT_CALL(display,
                SendEvDevReport(ElementsAre(IsEvDev(EV_ABS, ABS_MT_SLOT, _),
                                            IsEvDev(EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp))));
    // Call SendEvents with no events, which will cleanup the old ones.
    PenTouchEvent pen_empty;
    dispatcher.SendEvents(display, pen_empty);
}

TEST(TouchEvDevTest, toEvDevEventsPress) {
    SlotRegistry registry;
    Touch touch;
    touch.x = 100;
    touch.y = 200;
    touch.identifier = 1;
    touch.pressure = 50;
    touch.touch_major = 10;
    touch.touch_minor = 5;
    touch.orientation = 45;

    EvDevEvents events = touch.ToEvDevEvents(1024, 768, &registry);

    // Expected events for a touch press.
    EvDevEvents expected_events = {
        {EV_ABS, ABS_MT_SLOT, 0},          {EV_ABS, ABS_MT_TRACKING_ID, 0},
        {EV_ABS, ABS_MT_TOUCH_MAJOR, 10},  {EV_ABS, ABS_MT_TOUCH_MINOR, 5},
        {EV_ABS, ABS_MT_ORIENTATION, 45},  {EV_ABS, ABS_MT_PRESSURE, 50},
        {EV_ABS, ABS_MT_POSITION_X, 3199}, {EV_ABS, ABS_MT_POSITION_Y, 8533}};

    EXPECT_EQ(events.size(), expected_events.size());
    EXPECT_THAT(events, Eq(expected_events));
    EXPECT_TRUE(registry.IsSlotRegistered(0));
    EXPECT_TRUE(registry.IsIdentifierRegistered(1));
}

TEST(TouchEvDevTest, toEvDevEventsRelease) {
    SlotRegistry registry;
    // First create a press event
    Touch touch;
    touch.x = 100;
    touch.y = 200;
    touch.identifier = 1;
    touch.pressure = 50;
    touch.touch_major = 10;
    touch.touch_minor = 5;
    touch.orientation = 45;
    EvDevEvents events = touch.ToEvDevEvents(1024, 768, &registry);

    // Now release it
    touch.pressure = 0;
    touch.touch_major = 0;
    touch.touch_minor = 0;
    touch.orientation = 0;

    EvDevEvents release_events = touch.ToEvDevEvents(1024, 768, &registry);

    EvDevEvents expected_release_events = {{EV_ABS, ABS_MT_SLOT, 0},
                                           {EV_ABS, ABS_MT_PRESSURE, 0},
                                           {EV_ABS, ABS_MT_POSITION_X, 0xc7f},
                                           {EV_ABS, ABS_MT_POSITION_Y, 0x2155},
                                           {EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp},
                                           {EV_ABS, ABS_MT_ORIENTATION, 0}};
    EXPECT_EQ(release_events.size(), expected_release_events.size());
    EXPECT_THAT(release_events, Eq(expected_release_events));
    EXPECT_FALSE(registry.IsSlotRegistered(0));
    EXPECT_FALSE(registry.IsIdentifierRegistered(1));
}

TEST(TouchEvDevTest, toEvDevEventsNoPressure) {
    SlotRegistry registry;
    Touch touch;
    touch.x = 100;
    touch.y = 200;
    touch.identifier = 1;
    touch.pressure = 0;
    touch.touch_major = 10;
    touch.touch_minor = 5;
    touch.orientation = 45;

    EvDevEvents events = touch.ToEvDevEvents(1024, 768, &registry);
    EXPECT_EQ(events.size(), 0);
    EXPECT_FALSE(registry.IsIdentifierRegistered(1));
}

TEST(TouchEvDevTest, NoReleaseTwice) {
    SlotRegistry registry;
    // First create a press event
    Touch touch;
    touch.x = 100;
    touch.y = 200;
    touch.identifier = 1;
    touch.pressure = 50;
    touch.touch_major = 10;
    touch.touch_minor = 5;
    touch.orientation = 45;
    EvDevEvents events = touch.ToEvDevEvents(1024, 768, &registry);

    // Now release it
    touch.pressure = 0;
    EvDevEvents release_events = touch.ToEvDevEvents(1024, 768, &registry);

    // Now release again
    EvDevEvents release_again_events = touch.ToEvDevEvents(1024, 768, &registry);
    EXPECT_EQ(release_again_events.size(), 0);
    EXPECT_FALSE(registry.IsSlotRegistered(0));
    EXPECT_FALSE(registry.IsIdentifierRegistered(1));
}

TEST(MultiTouchEventTest, SendEventsPress) {
    // Create a mock display.
    auto evloop = TestEventLoop::Create();
    MockDisplay display(evloop.get(), 0, 1024, 768);

    // Create a PointerEventDispatcher.
    PointerEventDispatcher dispatcher;

    // Create a MultiTouchEvent.
    MultiTouchEvent touch_event;
    Touch touch;
    touch.x = 100;
    touch.y = 200;
    touch.identifier = 1;
    touch.pressure = 50;
    touch.touch_major = 10;
    touch.touch_minor = 5;
    touch.orientation = 45;
    touch_event.touches.push_back(touch);

    // Expect the whole report in one call on the mock display.
    EXPECT_CALL(
            display,
            SendEvDevReport(ElementsAre(
                    IsEvDev(EV_ABS, ABS_MT_SLOT, 0), IsEvDev(EV_ABS, ABS_MT_TRACKING_ID, 0),
                    IsEvDev(EV_ABS, ABS_MT_TOUCH_MAJOR, 10), IsEvDev(EV_ABS, ABS_MT_TOUCH_MINOR, 5),
                    IsEvDev(EV_ABS, ABS_MT_ORIENTATION, 45), IsEvDev(EV_ABS, ABS_MT_PRESSURE, 50),
                    IsEvDev(EV_ABS, ABS_MT_POSITION_X, _), IsEvDev(EV_ABS, ABS_MT_POSITION_Y, _))));

    // Call SendEvents.
    dispatcher.SendEvents(display, touch_event);
}

TEST(MultiTouchEventTest, SendEventsWithOldSlots) {
    // Create a mock display, note we are using a nicemock
    // as we will make a series of calls to our mock that we
    // do not care about.
    auto evloop = TestEventLoop::Create();
    ::testing::NiceMock<MockDisplay> display(evloop.get(), 0, 1024, 768);

    // Create a PointerEventDispatcher.
    absl::SimulatedClock clock;
    PointerEventDispatcherUnderTest dispatcher(clock);
    dispatcher.registry()->SetSlotExpiration(absl::Milliseconds(100));

    // Create a MultiTouchEvent.
    MultiTouchEvent touch_event;
    Touch touch;
    touch.x = 100;
    touch.y = 200;
    touch.identifier = 1;
    touch.pressure = 50;
    touch.touch_major = 10;
    touch.touch_minor = 5;
    touch.orientation = 45;
    touch_event.touches.push_back(touch);

    // Call SendEvents.
    dispatcher.SendEvents(display, touch_event);
    clock.AdvanceTime(absl::Milliseconds(500));

    // Expect the release of the expired slot as one report on the mock display.
    EXPECT_CALL(display,
                SendEvDevReport(ElementsAre(IsEvDev(EV_ABS, ABS_MT_SLOT, _),
                                            IsEvDev(EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp))));

    // Call SendEvents with no events, which will cleanup the old ones.
    MultiTouchEvent touch_empty;
    dispatcher.SendEvents(display, touch_empty);
}

TEST(MultiTouchEventTest, SendEventsMultipleTouch) {
    // Create a mock display.
    auto evloop = TestEventLoop::Create();
    MockDisplay display(evloop.get(), 0, 1024, 768);

    // Create a PointerEventDispatcher.
    PointerEventDispatcher dispatcher;

    // Create a MultiTouchEvent.
    MultiTouchEvent touch_event;
    Touch touch1;
    touch1.x = 100;
    touch1.y = 200;
    touch1.identifier = 1;
    touch1.pressure = 50;
    touch1.touch_major = 10;
    touch1.touch_minor = 5;
    touch1.orientation = 45;
    touch_event.touches.push_back(touch1);

    Touch touch2;
    touch2.x = 300;
    touch2.y = 400;
    touch2.identifier = 2;
    touch2.pressure = 50;
    touch2.touch_major = 12;
    touch2.touch_minor = 6;
    touch2.orientation = 90;
    touch_event.touches.push_back(touch2);

    // Expect both touches in one report on the mock display.
    EXPECT_CALL(
            display,
            SendEvDevReport(ElementsAre(
                    // First touch events.
                    IsEvDev(EV_ABS, ABS_MT_SLOT, 0), IsEvDev(EV_ABS, ABS_MT_TRACKING_ID, 0),
                    IsEvDev(EV_ABS, ABS_MT_TOUCH_MAJOR, 10), IsEvDev(EV_ABS, ABS_MT_TOUCH_MINOR, 5),
                    IsEvDev(EV_ABS, ABS_MT_ORIENTATION, 45), IsEvDev(EV_ABS, ABS_MT_PRESSURE, 50),
                    IsEvDev(EV_ABS, ABS_MT_POSITION_X, _), IsEvDev(EV_ABS, ABS_MT_POSITION_Y, _),
                    // Second touch events.
                    IsEvDev(EV_ABS, ABS_MT_SLOT, 1), IsEvDev(EV_ABS, ABS_MT_TRACKING_ID, 1),
                    IsEvDev(EV_ABS, ABS_MT_TOUCH_MAJOR, 12), IsEvDev(EV_ABS, ABS_MT_TOUCH_MINOR, 6),
                    IsEvDev(EV_ABS, ABS_MT_ORIENTATION, 90), IsEvDev(EV_ABS, ABS_MT_PRESSURE, 50),
                    IsEvDev(EV_ABS, ABS_MT_POSITION_X, _), IsEvDev(EV_ABS, ABS_MT_POSITION_Y, _))));

    // Call SendEvents.
    dispatcher.SendEvents(display, touch_event);
}

// Each finger's events must not interleave with another sender's, so the whole
// multi-pointer report goes to the display in a single call.
TEST(MultiTouchEventTest, SendEventsSendsMultiPointerReportInOneCall) {
    auto evloop = TestEventLoop::Create();
    MockDisplay display(evloop.get(), 0, 1024, 768);
    PointerEventDispatcher dispatcher;

    MultiTouchEvent touch_event;
    Touch touch1;
    touch1.x = 100;
    touch1.y = 200;
    touch1.identifier = 1;
    touch1.pressure = 50;
    touch1.touch_major = 0;
    touch1.touch_minor = 0;
    touch1.orientation = 0;
    touch_event.touches.push_back(touch1);
    Touch touch2 = touch1;
    touch2.x = 300;
    touch2.y = 400;
    touch2.identifier = 2;
    touch_event.touches.push_back(touch2);

    const EvDevEvents expected = {
        {EV_ABS, ABS_MT_SLOT, 0},          {EV_ABS, ABS_MT_TRACKING_ID, 0},
        {EV_ABS, ABS_MT_PRESSURE, 50},     {EV_ABS, ABS_MT_POSITION_X, 3199},
        {EV_ABS, ABS_MT_POSITION_Y, 8533}, {EV_ABS, ABS_MT_SLOT, 1},
        {EV_ABS, ABS_MT_TRACKING_ID, 1},   {EV_ABS, ABS_MT_PRESSURE, 50},
        {EV_ABS, ABS_MT_POSITION_X, 9599}, {EV_ABS, ABS_MT_POSITION_Y, 17066},
    };
    EXPECT_CALL(display, SendEvDevEvent).Times(0);
    EXPECT_CALL(display, SendEvDevReport(ElementsAreArray(expected)));

    dispatcher.SendEvents(display, touch_event);
}

// A multi-touch contact as the guest kernel sees it after a SYN_REPORT.
struct GuestContact {
    uint32_t slot;
    int32_t tracking_id;
    uint32_t x;
    uint32_t y;
    uint32_t tool_type;
};

void PrintTo(const GuestContact& contact, std::ostream* os) {
    *os << "{slot: " << contact.slot << ", tracking_id: " << contact.tracking_id
        << ", x: " << contact.x << ", y: " << contact.y << ", tool_type: " << contact.tool_type
        << "}";
}

// Interprets an evdev stream the way the Linux input core applies multi-touch
// protocol B: ABS_MT_SLOT selects a slot, every following ABS_MT_* value
// updates the selected slot, and SYN_REPORT publishes the frame. A slot holds
// an active contact while its tracking ID is not -1.
class GuestMultiTouchState {
  public:
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
        ASSERT_LT(selected_slot_, slots_.size()) << "ABS_MT_* value written to an invalid slot";
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
        case ABS_MT_TOOL_TYPE:
            slot.tool_type = value;
            break;
        default:
            break;
        }
    }

    // Returns the active contacts of the last reported frame.
    std::vector<GuestContact> ActiveContacts() const {
        std::vector<GuestContact> active;
        for (const auto& slot : reported_slots_) {
            if (slot.tracking_id != -1) {
                active.push_back(slot);
            }
        }
        return active;
    }

  private:
    static std::array<GuestContact, kMtsPointersNum> InitialSlots() {
        std::array<GuestContact, kMtsPointersNum> slots{};
        for (uint32_t i = 0; i < slots.size(); ++i) {
            slots[i] = {.slot = i, .tracking_id = -1, .x = 0, .y = 0, .tool_type = MT_TOOL_FINGER};
        }
        return slots;
    }

    uint32_t selected_slot_ = 0;
    std::array<GuestContact, kMtsPointersNum> slots_ = InitialSlots();
    std::array<GuestContact, kMtsPointersNum> reported_slots_ = InitialSlots();
};

auto IsContactAt(uint32_t x, uint32_t y, uint32_t tool_type = MT_TOOL_FINGER) {
    return AllOf(Field("x", &GuestContact::x, x), Field("y", &GuestContact::y, y),
                 Field("tool_type", &GuestContact::tool_type, tool_type));
}

class GuestMultiTouchTest : public ::testing::Test {
  protected:
    static constexpr uint32_t kWidth = 1080;
    static constexpr uint32_t kHeight = 1920;
    static constexpr uint32_t kStudioPressure = 0x400;

    void SetUp() override {
        ON_CALL(display_, SendEvDevReport(_)).WillByDefault([this](const EvDevEvents& events) {
            for (const EvDevEvent& ev : events) {
                guest_.Apply(ev.type, ev.code, ev.value);
            }
            guest_.Apply(EV_SYN, SYN_REPORT, 0);
        });
    }

    static void AddTouch(TouchEvent& event, int32_t identifier, int32_t x, int32_t y,
                         int32_t pressure) {
        auto* touch = event.add_touches();
        touch->set_identifier(identifier);
        touch->set_x(x);
        touch->set_y(y);
        touch->set_pressure(pressure);
    }

    static PenEvent MakePenEvent(int32_t x, int32_t y, int32_t pressure) {
        PenEvent event;
        auto* location = event.add_events()->mutable_location();
        location->set_x(x);
        location->set_y(y);
        location->set_pressure(pressure);
        return event;
    }

    std::unique_ptr<TestEventLoop> evloop_ = TestEventLoop::Create();
    NiceMock<MockDisplay> display_{evloop_.get(), 0, kWidth, kHeight};
    PointerEventDispatcher dispatcher_;
    GuestMultiTouchState guest_;
};

// Mirrors the TouchEvent sequence Android Studio sends in its Ctrl two-finger
// mode: both fingers travel together in a single TouchEvent.
TEST_F(GuestMultiTouchTest, StudioTwoFingerGestureReportsBothContacts) {
    TouchEvent press;
    AddTouch(press, 0, 300, 800, kStudioPressure);
    AddTouch(press, 1, 700, 1100, kStudioPressure);
    dispatcher_.SendEvents(display_, MultiTouchEvent::FromProto(press));

    const auto pressed = guest_.ActiveContacts();
    EXPECT_THAT(pressed, UnorderedElementsAre(IsContactAt(9101, 13652), IsContactAt(21237, 18772)));
    ASSERT_EQ(pressed.size(), 2);
    EXPECT_NE(pressed[0].tracking_id, pressed[1].tracking_id);

    TouchEvent move;
    AddTouch(move, 0, 350, 850, kStudioPressure);
    AddTouch(move, 1, 750, 1150, kStudioPressure);
    dispatcher_.SendEvents(display_, MultiTouchEvent::FromProto(move));

    EXPECT_THAT(guest_.ActiveContacts(),
                UnorderedElementsAre(IsContactAt(10618, 14506), IsContactAt(22754, 19626)));

    TouchEvent release;
    AddTouch(release, 0, 350, 850, 0);
    AddTouch(release, 1, 750, 1150, 0);
    dispatcher_.SendEvents(display_, MultiTouchEvent::FromProto(release));

    EXPECT_THAT(guest_.ActiveContacts(), ::testing::IsEmpty());
}

TEST_F(GuestMultiTouchTest, PenDownWhileFingerIsDownReportsBothContacts) {
    TouchEvent finger_press;
    AddTouch(finger_press, 0, 300, 800, kStudioPressure);
    dispatcher_.SendEvents(display_, MultiTouchEvent::FromProto(finger_press));

    dispatcher_.SendEvents(display_, PenTouchEvent::FromProto(MakePenEvent(540, 960, 50)));

    const auto pressed = guest_.ActiveContacts();
    EXPECT_THAT(pressed, UnorderedElementsAre(IsContactAt(9101, 13652, MT_TOOL_FINGER),
                                              IsContactAt(16383, 16383, MT_TOOL_PEN)));
    ASSERT_EQ(pressed.size(), 2);
    EXPECT_NE(pressed[0].tracking_id, pressed[1].tracking_id);

    dispatcher_.SendEvents(display_, PenTouchEvent::FromProto(MakePenEvent(540, 960, 0)));

    EXPECT_THAT(guest_.ActiveContacts(),
                UnorderedElementsAre(IsContactAt(9101, 13652, MT_TOOL_FINGER)));

    TouchEvent finger_release;
    AddTouch(finger_release, 0, 300, 800, 0);
    dispatcher_.SendEvents(display_, MultiTouchEvent::FromProto(finger_release));

    EXPECT_THAT(guest_.ActiveContacts(), ::testing::IsEmpty());
}

}  // namespace internal
}  // namespace control
}  // namespace emulation
}  // namespace android
