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

#include "goldfish/videobridge/in_process_input_sender.h"

#include <gtest/gtest.h>

namespace goldfish::videobridge {
namespace {

TEST(InProcessInputSenderTest, StartSendStopFlow) {
    InputEvent last_event;
    int event_count = 0;

    auto dispatcher = [&](const InputEvent& event) -> absl::Status {
        last_event = event;
        event_count++;
        return absl::OkStatus();
    };

    InProcessInputSender sender(dispatcher);

    // Send before Start -> should be ignored
    InputEvent event1;
    event1.mutable_key_event()->set_key("A");
    sender.SendEvent(event1);
    EXPECT_EQ(event_count, 0);

    // Start sender
    EXPECT_TRUE(sender.Start().ok());

    // Send after Start -> should be dispatched
    sender.SendEvent(event1);
    EXPECT_EQ(event_count, 1);
    EXPECT_EQ(last_event.key_event().key(), "A");

    // Stop sender
    sender.Stop();
    InputEvent event2;
    event2.mutable_key_event()->set_key("B");
    sender.SendEvent(event2);
    EXPECT_EQ(event_count, 1);
}

TEST(InProcessInputSenderTest, DispatcherErrorStatusHandling) {
    auto error_dispatcher = [](const InputEvent& /*event*/) -> absl::Status {
        return absl::InternalError("Dispatch failed");
    };

    InProcessInputSender sender(error_dispatcher);
    EXPECT_TRUE(sender.Start().ok());

    InputEvent event;
    // Should gracefully handle error return without crashing
    sender.SendEvent(event);
}

TEST(InProcessInputSenderTest, V2EventDispatching) {
    V2InputEvent last_v2_event;
    int v2_count = 0;

    auto v2_dispatcher = [&](const V2InputEvent& event) -> absl::Status {
        last_v2_event = event;
        v2_count++;
        return absl::OkStatus();
    };

    InProcessInputSender sender(nullptr, v2_dispatcher);

    V2InputEvent event1;
    event1.mutable_pointer()->set_display_id(0);
    event1.mutable_pointer()->set_action(
            ::android::emulation::v2::PointerAction::POINTER_ACTION_DOWN);

    // Before Start -> ignored
    sender.SendV2Event(event1);
    EXPECT_EQ(v2_count, 0);

    // Start -> dispatched
    EXPECT_TRUE(sender.Start().ok());
    sender.SendV2Event(event1);
    EXPECT_EQ(v2_count, 1);
    EXPECT_EQ(last_v2_event.pointer().display_id(), 0);
    EXPECT_EQ(last_v2_event.pointer().action(),
              ::android::emulation::v2::PointerAction::POINTER_ACTION_DOWN);

    // Stop -> ignored
    sender.Stop();
    sender.SendV2Event(event1);
    EXPECT_EQ(v2_count, 1);
}

TEST(InProcessInputSenderTest, StopCallbackFiresOncePerActiveCycle) {
    int stop_count = 0;
    InProcessInputSender sender([](const InputEvent& /*event*/) { return absl::OkStatus(); },
                                /*v2_dispatcher=*/nullptr,
                                /*v2_sensors_dispatcher=*/nullptr,
                                /*on_stop=*/[&stop_count]() { ++stop_count; });

    // Stop before Start must not invoke on_stop.
    sender.Stop();
    EXPECT_EQ(stop_count, 0);

    // Start followed by Closing + Closed Stop() calls invokes on_stop once.
    ASSERT_TRUE(sender.Start().ok());
    sender.Stop();
    EXPECT_EQ(stop_count, 1);
    sender.Stop();
    EXPECT_EQ(stop_count, 1);

    // Re-starting and stopping fires on_stop once more.
    ASSERT_TRUE(sender.Start().ok());
    sender.Stop();
    EXPECT_EQ(stop_count, 2);
}

}  // namespace
}  // namespace goldfish::videobridge
