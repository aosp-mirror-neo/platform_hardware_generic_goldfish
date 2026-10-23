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
    EXPECT_THAT(table.Acquire({kAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Acquire({kAgent, 1}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(1));
    EXPECT_THAT(table.Acquire({kOtherAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(2));
    EXPECT_THAT(table.Find({kAgent, 1}), Optional(Contact{kDisplay0, 1, kRequested}));
}

TEST(ContactTableTest, ReacquireReturnsSameSlot) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 7}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Acquire({kAgent, 7}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    // The second acquire must not have consumed another slot.
    EXPECT_THAT(table.Acquire({kAgent, 8}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(1));
}

TEST(ContactTableTest, ContactStaysBoundToFirstDevice) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kHuman, 0}, kDisplay0, kRequested, kIsHuman), IsOkAndHolds(0));
    // A MOVE that crosses onto another display still targets the device the
    // contact went DOWN on.
    EXPECT_THAT(table.Acquire({kHuman, 0}, kDisplay1, kRequested, kIsHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Find({kHuman, 0}), Optional(Contact{kDisplay0, 0, kRequested}));
    // Nothing was allocated on the other device.
    EXPECT_THAT(table.Acquire({kHuman, 1}, kDisplay1, kRequested, kIsHuman), IsOkAndHolds(0));
}

TEST(ContactTableTest, ContactKeepsDisplayOfFirstAcquire) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kHuman, 0}, kDisplay1, /*display_id=*/0, kIsHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, 0}, kDisplay0, /*display_id=*/1, kIsHuman), IsOkAndHolds(0));
    EXPECT_THAT(table.Release({kHuman, 0}), Optional(Contact{kDisplay1, 0, /*display_id=*/0}));
}

TEST(ContactTableTest, ReleaseFreesSlotForReuse) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kAgent, 1}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(1));

    EXPECT_THAT(table.Release({kAgent, 0}), Optional(Contact{kDisplay0, 0, kRequested}));
    EXPECT_FALSE(table.IsHeld({kAgent, 0}));
    EXPECT_EQ(table.Find({kAgent, 0}), std::nullopt);
    EXPECT_EQ(table.Release({kAgent, 0}), std::nullopt);

    EXPECT_THAT(table.Acquire({kOtherAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
}

TEST(ContactTableTest, NonHumanLimitLeavesReservedSlotsForHuman) {
    ContactTable table;
    for (int i = 0; i < kNonHumanSlots; ++i) {
        // Spread across two sessions: the limit is per device, not per
        // session.
        InputSessionId session = i % 2 == 0 ? kAgent : kOtherAgent;
        ASSERT_THAT(table.Acquire({session, i}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(i));
    }

    EXPECT_THAT(table.Acquire({kAgent, 100}, kDisplay0, kRequested, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted, HasSubstr("reserved")));
    EXPECT_FALSE(table.IsHeld({kAgent, 100}));

    EXPECT_THAT(table.Acquire({kHuman, 0}, kDisplay0, kRequested, kIsHuman),
                IsOkAndHolds(kNonHumanSlots));
    EXPECT_THAT(table.Acquire({kHuman, 1}, kDisplay0, kRequested, kIsHuman),
                IsOkAndHolds(kNonHumanSlots + 1));

    // The failed acquire changed nothing: freeing one agent slot lets exactly
    // one more agent contact in, at the freed slot.
    ASSERT_THAT(table.Release({kAgent, 4}), Optional(Contact{kDisplay0, 4, kRequested}));
    EXPECT_THAT(table.Acquire({kAgent, 100}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(4));
    EXPECT_THAT(table.Acquire({kAgent, 101}, kDisplay0, kRequested, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST(ContactTableTest, HumanExhaustsAllSlots) {
    ContactTable table;
    for (int i = 0; i < kTouchDeviceSlots; ++i) {
        ASSERT_THAT(table.Acquire({kHuman, i}, kDisplay0, kRequested, kIsHuman), IsOkAndHolds(i));
    }
    EXPECT_THAT(table.Acquire({kHuman, kTouchDeviceSlots}, kDisplay0, kRequested, kIsHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));
    EXPECT_FALSE(table.IsHeld({kHuman, kTouchDeviceSlots}));
    // A human holding every slot also blocks non-human sessions.
    EXPECT_THAT(table.Acquire({kAgent, 0}, kDisplay0, kRequested, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST(ContactTableTest, LimitsArePerDevice) {
    ContactTable table;
    for (int i = 0; i < kNonHumanSlots; ++i) {
        ASSERT_THAT(table.Acquire({kAgent, i}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(i));
    }
    ASSERT_THAT(table.Acquire({kAgent, 100}, kDisplay0, kRequested, kNotHuman),
                StatusIs(absl::StatusCode::kResourceExhausted));

    for (int i = 0; i < kNonHumanSlots; ++i) {
        EXPECT_THAT(table.Acquire({kOtherAgent, i}, kDisplay1, kRequested, kNotHuman),
                    IsOkAndHolds(i));
    }
}

TEST(ContactTableTest, ReleaseSessionFreesOnlyThatSessionSorted) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 5}, kDisplay1, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kOtherAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kAgent, 3}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kAgent, 1}, kDisplay1, kRequested, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kAgent, 9}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(2));

    EXPECT_THAT(table.ReleaseSession(kAgent),
                ElementsAre(Pair(Eq(ContactKey{kAgent, 3}), Contact{kDisplay0, 1, kRequested}),
                            Pair(Eq(ContactKey{kAgent, 9}), Contact{kDisplay0, 2, kRequested}),
                            Pair(Eq(ContactKey{kAgent, 5}), Contact{kDisplay1, 0, kRequested}),
                            Pair(Eq(ContactKey{kAgent, 1}), Contact{kDisplay1, 1, kRequested})));

    EXPECT_FALSE(table.IsHeld({kAgent, 3}));
    EXPECT_FALSE(table.IsHeld({kAgent, 5}));
    EXPECT_THAT(table.Find({kOtherAgent, 0}), Optional(Contact{kDisplay0, 0, kRequested}));
    EXPECT_THAT(table.ReleaseSession(kAgent), ElementsAre());
    // Freed slots are reusable.
    EXPECT_THAT(table.Acquire({kHuman, 0}, kDisplay0, kRequested, kIsHuman), IsOkAndHolds(1));
}

TEST(ContactTableTest, ReleaseSessionOnFreesOnlyThatSessionOnThatDevice) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 5}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kOtherAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kAgent, 1}, kDisplay1, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kAgent, 2}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(2));

    EXPECT_THAT(table.ReleaseSessionOn(kAgent, kDisplay0),
                ElementsAre(Pair(Eq(ContactKey{kAgent, 5}), Contact{kDisplay0, 0, kRequested}),
                            Pair(Eq(ContactKey{kAgent, 2}), Contact{kDisplay0, 2, kRequested})));

    EXPECT_TRUE(table.IsHeld({kOtherAgent, 0}));
    EXPECT_TRUE(table.IsHeld({kAgent, 1}));
    EXPECT_THAT(table.ReleaseSessionOn(kAgent, kDisplay0), ElementsAre());
}

TEST(ContactTableTest, ClearDeviceClearsOnlyThatDeviceSorted) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, 0}, kDisplay1, kRequested, kIsHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, 1}, kDisplay0, kRequested, kIsHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kOtherAgent, 0}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(2));

    EXPECT_THAT(
            table.ClearDevice(kDisplay0),
            ElementsAre(Pair(Eq(ContactKey{kAgent, 0}), Contact{kDisplay0, 0, kRequested}),
                        Pair(Eq(ContactKey{kHuman, 1}), Contact{kDisplay0, 1, kRequested}),
                        Pair(Eq(ContactKey{kOtherAgent, 0}), Contact{kDisplay0, 2, kRequested})));

    EXPECT_FALSE(table.IsHeld({kAgent, 0}));
    EXPECT_FALSE(table.IsHeld({kHuman, 1}));
    EXPECT_THAT(table.Find({kHuman, 0}), Optional(Contact{kDisplay1, 0, kRequested}));
    // The device's non-human budget is fully restored.
    for (int i = 0; i < kNonHumanSlots; ++i) {
        EXPECT_THAT(table.Acquire({kAgent, i}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(i));
    }
}

TEST(ContactTableTest, ClearFreesAllDevicesSorted) {
    ContactTable table;
    ASSERT_THAT(table.Acquire({kAgent, 5}, kDisplay1, kRequested, kNotHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kHuman, 0}, kDisplay0, kRequested, kIsHuman), IsOkAndHolds(0));
    ASSERT_THAT(table.Acquire({kOtherAgent, 3}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(1));
    ASSERT_THAT(table.Acquire({kHuman, 1}, kDisplay1, kRequested, kIsHuman), IsOkAndHolds(1));

    EXPECT_THAT(table.Clear(),
                ElementsAre(Pair(Eq(ContactKey{kHuman, 0}), Contact{kDisplay0, 0, kRequested}),
                            Pair(Eq(ContactKey{kOtherAgent, 3}), Contact{kDisplay0, 1, kRequested}),
                            Pair(Eq(ContactKey{kAgent, 5}), Contact{kDisplay1, 0, kRequested}),
                            Pair(Eq(ContactKey{kHuman, 1}), Contact{kDisplay1, 1, kRequested})));

    EXPECT_FALSE(table.IsHeld({kHuman, 0}));
    EXPECT_FALSE(table.IsHeld({kAgent, 5}));
    EXPECT_THAT(table.Clear(), ElementsAre());
    for (int i = 0; i < kNonHumanSlots; ++i) {
        EXPECT_THAT(table.Acquire({kAgent, i}, kDisplay0, kRequested, kNotHuman), IsOkAndHolds(i));
        EXPECT_THAT(table.Acquire({kAgent, i}, kDisplay1, kRequested, kNotHuman), IsOkAndHolds(i));
    }
}

}  // namespace
}  // namespace goldfish::grpc::v2
