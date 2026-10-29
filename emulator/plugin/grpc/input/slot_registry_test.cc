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
#include "slot_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "absl/time/clock.h"
#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"

#include "standard-headers/linux/input-event-codes.h"

namespace android {
namespace emulation {
namespace control {

using namespace std::chrono_literals;

TEST(SlotRegistryTest, AcquireReleaseSlot) {
    absl::SimulatedClock clock;
    SlotRegistry registry(absl::Milliseconds(10), clock);
    int slot1 = registry.AcquireSlot(1);
    ASSERT_GE(slot1, 0);
    ASSERT_TRUE(registry.IsSlotRegistered(slot1));
    ASSERT_TRUE(registry.IsIdentifierRegistered(1));

    registry.ReleaseSlot(1);
    ASSERT_FALSE(registry.IsSlotRegistered(slot1));
    ASSERT_FALSE(registry.IsIdentifierRegistered(1));
}

TEST(SlotRegistryTest, AcquireMultipleSlots) {
    absl::SimulatedClock clock;
    SlotRegistry registry(absl::Milliseconds(10), clock);
    std::vector<int> slots;
    for (int i = 0; i < kMtsPointersNum; ++i) {
        int slot = registry.AcquireSlot(i);
        ASSERT_GE(slot, 0);
        slots.push_back(slot);
        ASSERT_TRUE(registry.IsSlotRegistered(slot));
        ASSERT_TRUE(registry.IsIdentifierRegistered(i));
    }

    // Check that we can't acquire any more slots.
    ASSERT_EQ(registry.AcquireSlot(kMtsPointersNum), -1);

    // Release all slots.
    for (int i = 0; i < kMtsPointersNum; ++i) {
        registry.ReleaseSlot(i);
        ASSERT_FALSE(registry.IsSlotRegistered(slots[i]));
        ASSERT_FALSE(registry.IsIdentifierRegistered(i));
    }
}

TEST(SlotRegistryTest, ReacquireSlot) {
    absl::SimulatedClock clock;
    SlotRegistry registry(absl::Milliseconds(10), clock);
    int slot1 = registry.AcquireSlot(1);
    ASSERT_GE(slot1, 0);
    registry.ReleaseSlot(1);

    // Reacquire the same slot.
    int slot2 = registry.AcquireSlot(1);
    ASSERT_EQ(slot2, slot1);
}

TEST(SlotRegistryTest, ExpireOldSlots) {
    absl::SimulatedClock clock;
    SlotRegistry registry(absl::Milliseconds(10), clock);

    int slot1 = registry.AcquireSlot(1);
    int slot2 = registry.AcquireSlot(2);

    registry.UpdateSlotExpiration(1);
    registry.UpdateSlotExpiration(2);
    // Advance time past expiration
    clock.AdvanceTime(absl::Milliseconds(20));

    std::vector<EvDevEvent> events = registry.ExpireOldSlots();
    ASSERT_EQ(events.size(), 4);

    // Note that expiration of slot events depends on the hashmap ordering.

    // check the first events are the slot events
    ASSERT_EQ(events[0].type, EV_ABS);
    ASSERT_EQ(events[0].code, ABS_MT_SLOT);
    // Assert that the first slot event value is either slot1 or slot2
    ASSERT_TRUE(events[0].value == slot1 || events[0].value == slot2);

    ASSERT_EQ(events[1].type, EV_ABS);
    ASSERT_EQ(events[1].code, ABS_MT_TRACKING_ID);
    ASSERT_EQ(events[1].value, kMtsPointerUp);

    // check the second events are the slot events
    ASSERT_EQ(events[2].type, EV_ABS);
    ASSERT_EQ(events[2].code, ABS_MT_SLOT);
    // Assert that the first slot event value is either slot1 or slot2
    ASSERT_TRUE(events[0].value == slot1 || events[0].value == slot2);
    ASSERT_EQ(events[3].type, EV_ABS);
    ASSERT_EQ(events[3].code, ABS_MT_TRACKING_ID);
    ASSERT_EQ(events[3].value, kMtsPointerUp);

    ASSERT_FALSE(registry.IsSlotRegistered(slot1));
    ASSERT_FALSE(registry.IsIdentifierRegistered(1));
    ASSERT_FALSE(registry.IsSlotRegistered(slot2));
    ASSERT_FALSE(registry.IsIdentifierRegistered(2));
}

TEST(SlotRegistryTest, ExpireOldSlotsOne) {
    absl::SimulatedClock clock;
    SlotRegistry registry(absl::Milliseconds(10), clock);

    int slot1 = registry.AcquireSlot(1);

    registry.UpdateSlotExpiration(1);
    // Advance time past expiration
    clock.AdvanceTime(absl::Milliseconds(20));

    std::vector<EvDevEvent> events = registry.ExpireOldSlots();
    ASSERT_EQ(events.size(), 2);

    // check the first events are the slot events
    ASSERT_EQ(events[0].type, EV_ABS);
    ASSERT_EQ(events[0].code, ABS_MT_SLOT);
    ASSERT_EQ(events[0].value, slot1);
    ASSERT_EQ(events[1].type, EV_ABS);
    ASSERT_EQ(events[1].code, ABS_MT_TRACKING_ID);
    ASSERT_EQ(events[1].value, kMtsPointerUp);

    ASSERT_FALSE(registry.IsSlotRegistered(slot1));
    ASSERT_FALSE(registry.IsIdentifierRegistered(1));
}
TEST(SlotRegistryTest, AcquireExtendsExpiration) {
    absl::SimulatedClock clock;
    // 100ms expiration
    SlotRegistry registry(absl::Milliseconds(100), clock);

    int slot1 = registry.AcquireSlot(1);

    // Advance time by 50ms
    clock.AdvanceTime(absl::Milliseconds(50));

    // Re-acquire (should extend expiration)
    registry.AcquireSlot(1);

    // Advance time by another 60ms.
    // Total 110ms since first acquire, but only 60ms since second.
    // If second acquire extended expiration, it should NOT be expired.
    clock.AdvanceTime(absl::Milliseconds(60));

    std::vector<EvDevEvent> events = registry.ExpireOldSlots();
    EXPECT_EQ(events.size(), 0);

    // Advance another 50ms (total 110ms since second acquire)
    clock.AdvanceTime(absl::Milliseconds(50));
    events = registry.ExpireOldSlots();
    EXPECT_EQ(events.size(), 2);
}

TEST(SlotRegistryTest, ReleaseAllSlots) {
    absl::SimulatedClock clock;
    SlotRegistry registry(absl::Seconds(120), clock);

    int s1 = registry.AcquireSlot(10);
    int s2 = registry.AcquireSlot(20);
    ASSERT_GE(s1, 0);
    ASSERT_GE(s2, 0);

    auto events = registry.ReleaseAllSlots();
    EXPECT_EQ(events.size(), 4);
    EXPECT_FALSE(registry.IsSlotRegistered(s1));
    EXPECT_FALSE(registry.IsSlotRegistered(s2));
    EXPECT_FALSE(registry.IsIdentifierRegistered(10));
    EXPECT_FALSE(registry.IsIdentifierRegistered(20));
}

}  // namespace control
}  // namespace emulation
}  // namespace android
