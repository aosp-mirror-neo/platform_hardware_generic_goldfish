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
#include "contact_table.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"

namespace goldfish::grpc::v2 {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Optional;
using ::testing::Pair;

constexpr TouchDeviceId kDisplay0 = 0;
constexpr TouchDeviceId kDisplay1 = 1;
// The requested display: not a device ID, so tests see it is stored as given.
constexpr int32_t kRequested = 7;
constexpr InputSessionId kAgent = 10;
constexpr InputSessionId kOtherAgent = 11;
constexpr InputSessionId kHuman = 20;
constexpr bool kIsHuman = true;
constexpr bool kNotHuman = false;
constexpr int kNonHumanSlots = kTouchDeviceSlots - kReservedHumanSlots;

TEST(ContactTableTest, AllocatesLowestFreeSlot) {
    ContactTable table;
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 1}, kDisplay0, kNotHuman), IsOkAndHolds(1));
    EXPECT_THAT(table.Acquire({kOtherAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(2));
    EXPECT_THAT(table.Find({kAgent, kRequested, 1}), Optional(Contact{kDisplay0, 1}));
}

TEST(ContactTableTest, ReacquireReturnsSameSlot) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 7}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 7}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    // The second acquire must not have consumed another slot.
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 8}, kDisplay0, kNotHuman), IsOkAndHolds(1));
}

TEST(ContactTableTest, ContactStaysBoundToFirstDevice) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kHuman, kRequested, 0}, kDisplay0, kIsHuman), IsOkAndHolds(0));
    // When a foldable posture change remaps `kRequested` to another physical
    // device mid-gesture, the held contact stays bound to the device it went
    // DOWN on.
    EXPECT_THAT(table.Acquire({kHuman, kRequested, 0}, kDisplay1, kIsHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Find({kHuman, kRequested, 0}), Optional(Contact{kDisplay0, 0}));
    // Nothing was allocated on the other device.
    EXPECT_THAT(table.Acquire({kHuman, kRequested, 1}, kDisplay1, kIsHuman), IsOkAndHolds(0));
}

TEST(ContactTableTest, SamePointerIdOnDifferentDisplaysAcquiresIndependentContacts) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kHuman, /*display_id=*/0, /*pointer_id=*/0}, kDisplay0, kIsHuman),
                IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, /*display_id=*/1, /*pointer_id=*/0}, kDisplay1, kIsHuman),
                IsOkAndHolds(0));
    // Even if two logical displays resolve to the same physical touch device,
    // each `(session, display_id, pointer_id)` gets its own slot.
    ASSERT_THAT(table.Acquire({kHuman, /*display_id=*/2, /*pointer_id=*/0}, kDisplay1, kIsHuman),
                IsOkAndHolds(1));

    EXPECT_THAT(table.Release({kHuman, /*display_id=*/0, /*pointer_id=*/0}),
                Optional(Contact{kDisplay0, 0}));
    EXPECT_THAT(table.Find({kHuman, /*display_id=*/1, /*pointer_id=*/0}),
                Optional(Contact{kDisplay1, 0}));
    EXPECT_THAT(table.Find({kHuman, /*display_id=*/2, /*pointer_id=*/0}),
                Optional(Contact{kDisplay1, 1}));
}

TEST(ContactTableTest, ReleaseFreesSlotForReuse) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 1}, kDisplay0, kNotHuman), IsOkAndHolds(1));

    EXPECT_THAT(table.Release({kAgent, kRequested, 0}), Optional(Contact{kDisplay0, 0}));
    EXPECT_FALSE(table.IsHeld({kAgent, kRequested, 0}));
    EXPECT_EQ(table.Find({kAgent, kRequested, 0}), std::nullopt);
    EXPECT_EQ(table.Release({kAgent, kRequested, 0}), std::nullopt);

    EXPECT_THAT(table.Acquire({kOtherAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(0));
}

TEST(ContactTableTest, NonHumanLimitLeavesReservedSlotsForHuman) {
    ContactTable table;
    for (int i = 0; i < kNonHumanSlots; ++i) {
        // Spread across two sessions: the limit is per device, not per
        // session.
        InputSessionId session = i % 2 == 0 ? kAgent : kOtherAgent;
        ASSERT_THAT(table.Acquire({session, kRequested, i}, kDisplay0, kNotHuman), IsOkAndHolds(i));
    }

    EXPECT_THAT(table.Acquire({kAgent, kRequested, 100}, kDisplay0, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted, HasSubstr("reserved")));
    EXPECT_FALSE(table.IsHeld({kAgent, kRequested, 100}));

    EXPECT_THAT(table.Acquire({kHuman, kRequested, 0}, kDisplay0, kIsHuman),
                IsOkAndHolds(kNonHumanSlots));
    EXPECT_THAT(table.Acquire({kHuman, kRequested, 1}, kDisplay0, kIsHuman),
                IsOkAndHolds(kNonHumanSlots + 1));

    // The failed acquire changed nothing: freeing one agent slot lets exactly
    // one more agent contact in, at the freed slot.
    ASSERT_THAT(table.Release({kAgent, kRequested, 4}), Optional(Contact{kDisplay0, 4}));
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 100}, kDisplay0, kNotHuman), IsOkAndHolds(4));
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 101}, kDisplay0, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST(ContactTableTest, HumanExhaustsAllSlots) {
    ContactTable table;
    for (int i = 0; i < kTouchDeviceSlots; ++i) {
        ASSERT_THAT(table.Acquire({kHuman, kRequested, i}, kDisplay0, kIsHuman), IsOkAndHolds(i));
    }
    EXPECT_THAT(table.Acquire({kHuman, kRequested, kTouchDeviceSlots}, kDisplay0, kIsHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));
    EXPECT_FALSE(table.IsHeld({kHuman, kRequested, kTouchDeviceSlots}));
    // A human holding every slot also blocks non-human sessions.
    EXPECT_THAT(table.Acquire({kAgent, kRequested, 0}, kDisplay0, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST(ContactTableTest, LimitsArePerDevice) {
    ContactTable table;
    for (int i = 0; i < kNonHumanSlots; ++i) {
        ASSERT_THAT(table.Acquire({kAgent, kRequested, i}, kDisplay0, kNotHuman), IsOkAndHolds(i));
    }
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 100}, kDisplay0, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));

    for (int i = 0; i < kNonHumanSlots; ++i) {
        EXPECT_THAT(table.Acquire({kOtherAgent, kRequested, i}, kDisplay1, kNotHuman),
                    IsOkAndHolds(i));
    }
}

TEST(ContactTableTest, ReleaseSessionFreesOnlyThatSessionSorted) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 1, 5}, kDisplay1, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kOtherAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 3}, kDisplay0, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kAgent, 1, 1}, kDisplay1, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 9}, kDisplay0, kNotHuman), IsOkAndHolds(2));

    EXPECT_THAT(table.ReleaseSession(kAgent),
                ElementsAre(Pair(Eq(ContactKey{kAgent, kRequested, 3}), Contact{kDisplay0, 1}),
                            Pair(Eq(ContactKey{kAgent, kRequested, 9}), Contact{kDisplay0, 2}),
                            Pair(Eq(ContactKey{kAgent, 1, 5}), Contact{kDisplay1, 0}),
                            Pair(Eq(ContactKey{kAgent, 1, 1}), Contact{kDisplay1, 1})));

    EXPECT_FALSE(table.IsHeld({kAgent, kRequested, 3}));
    EXPECT_FALSE(table.IsHeld({kAgent, 1, 5}));
    EXPECT_THAT(table.Find({kOtherAgent, kRequested, 0}), Optional(Contact{kDisplay0, 0}));
    EXPECT_THAT(table.ReleaseSession(kAgent), ElementsAre());
    // Freed slots are reusable.
    EXPECT_THAT(table.Acquire({kHuman, kRequested, 0}, kDisplay0, kIsHuman), IsOkAndHolds(1));
}

TEST(ContactTableTest, ReleaseSessionOnFreesOnlyThatSessionOnThatDisplayAcrossDevices) {
    constexpr int32_t kOtherDisplay = 8;
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 5}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kOtherAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(1));
    // Same session and same physical device `kDisplay0`, but a different requested display.
    ASSERT_THAT(table.Acquire({kAgent, kOtherDisplay, 1}, kDisplay0, kNotHuman), IsOkAndHolds(2));
    // Same session and requested display `kRequested`, bound to `kDisplay1` after posture remap.
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 2}, kDisplay1, kNotHuman), IsOkAndHolds(0));

    EXPECT_THAT(table.FindSessionOn(kAgent, kRequested),
                ElementsAre(Pair(Eq(ContactKey{kAgent, kRequested, 5}), Contact{kDisplay0, 0}),
                            Pair(Eq(ContactKey{kAgent, kRequested, 2}), Contact{kDisplay1, 0})));

    EXPECT_THAT(table.ReleaseSessionOn(kAgent, kRequested),
                ElementsAre(Pair(Eq(ContactKey{kAgent, kRequested, 5}), Contact{kDisplay0, 0}),
                            Pair(Eq(ContactKey{kAgent, kRequested, 2}), Contact{kDisplay1, 0})));

    EXPECT_TRUE(table.IsHeld({kOtherAgent, kRequested, 0}));
    EXPECT_TRUE(table.IsHeld({kAgent, kOtherDisplay, 1}));
    EXPECT_THAT(table.FindSessionOn(kAgent, kRequested), ElementsAre());
    EXPECT_THAT(table.ReleaseSessionOn(kAgent, kRequested), ElementsAre());
}

TEST(ContactTableTest, ClearDeviceClearsOnlyThatDeviceSorted) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, 1, 0}, kDisplay1, kIsHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, kRequested, 1}, kDisplay0, kIsHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kOtherAgent, kRequested, 0}, kDisplay0, kNotHuman), IsOkAndHolds(2));

    EXPECT_THAT(
            table.ClearDevice(kDisplay0),
            ElementsAre(Pair(Eq(ContactKey{kAgent, kRequested, 0}), Contact{kDisplay0, 0}),
                        Pair(Eq(ContactKey{kHuman, kRequested, 1}), Contact{kDisplay0, 1}),
                        Pair(Eq(ContactKey{kOtherAgent, kRequested, 0}), Contact{kDisplay0, 2})));

    EXPECT_FALSE(table.IsHeld({kAgent, kRequested, 0}));
    EXPECT_FALSE(table.IsHeld({kHuman, kRequested, 1}));
    EXPECT_THAT(table.Find({kHuman, 1, 0}), Optional(Contact{kDisplay1, 0}));
    // The device's non-human budget is fully restored.
    for (int i = 0; i < kNonHumanSlots; ++i) {
        EXPECT_THAT(table.Acquire({kAgent, kRequested, i}, kDisplay0, kNotHuman), IsOkAndHolds(i));
    }
}

TEST(ContactTableTest, ClearFreesAllDevicesSorted) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 1, 5}, kDisplay1, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, kRequested, 0}, kDisplay0, kIsHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kOtherAgent, kRequested, 3}, kDisplay0, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kHuman, 1, 1}, kDisplay1, kIsHuman), IsOkAndHolds(1));

    EXPECT_THAT(table.Clear(),
                ElementsAre(Pair(Eq(ContactKey{kHuman, kRequested, 0}), Contact{kDisplay0, 0}),
                            Pair(Eq(ContactKey{kOtherAgent, kRequested, 3}), Contact{kDisplay0, 1}),
                            Pair(Eq(ContactKey{kAgent, 1, 5}), Contact{kDisplay1, 0}),
                            Pair(Eq(ContactKey{kHuman, 1, 1}), Contact{kDisplay1, 1})));

    EXPECT_FALSE(table.IsHeld({kHuman, kRequested, 0}));
    EXPECT_FALSE(table.IsHeld({kAgent, 1, 5}));
    EXPECT_THAT(table.Clear(), ElementsAre());
    for (int i = 0; i < kNonHumanSlots; ++i) {
        EXPECT_THAT(table.Acquire({kAgent, kRequested, i}, kDisplay0, kNotHuman), IsOkAndHolds(i));
        EXPECT_THAT(table.Acquire({kAgent, 1, i}, kDisplay1, kNotHuman), IsOkAndHolds(i));
    }
}

}  // namespace
}  // namespace goldfish::grpc::v2
