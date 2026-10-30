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

#include "mouse_button_mask.h"

#include <gtest/gtest.h>

namespace goldfish::videobridge::java {
namespace {

TEST(MouseButtonMaskTest, NoneMapsToNoButtons) {
    EXPECT_EQ(MouseButtonOrdinalToMask(0), 0);
}

TEST(MouseButtonMaskTest, LeftMapsToPrimaryBit) {
    EXPECT_EQ(MouseButtonOrdinalToMask(1), 1);
}

TEST(MouseButtonMaskTest, RightMapsToSecondaryBit) {
    EXPECT_EQ(MouseButtonOrdinalToMask(2), 2);
}

TEST(MouseButtonMaskTest, MiddleMapsToMiddleBitOnly) {
    EXPECT_EQ(MouseButtonOrdinalToMask(3), 4);
}

TEST(MouseButtonMaskTest, UnknownOrdinalsMapToNoButtons) {
    EXPECT_EQ(MouseButtonOrdinalToMask(-1), 0);
    EXPECT_EQ(MouseButtonOrdinalToMask(4), 0);
}

}  // namespace
}  // namespace goldfish::videobridge::java
