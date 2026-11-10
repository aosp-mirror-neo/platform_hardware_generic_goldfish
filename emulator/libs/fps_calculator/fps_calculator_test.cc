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

#include "goldfish/fps_calculator.h"

#include <gtest/gtest.h>

namespace goldfish {

TEST(FpsCalculatorTest, InitialState) {
    FpsCalculator calculator(10);
    EXPECT_DOUBLE_EQ(0.0, calculator.GetFps());
}

TEST(FpsCalculatorTest, CalculatesFpsCorrectly) {
    FpsCalculator calculator(10);
    absl::Time now = absl::Now();

    calculator.AddFrame(now);
    EXPECT_DOUBLE_EQ(0.0, calculator.GetFps());

    calculator.AddFrame(now + absl::Seconds(1));
    EXPECT_DOUBLE_EQ(1.0, calculator.GetFps());

    calculator.AddFrame(now + absl::Seconds(2));
    EXPECT_NEAR(1.0, calculator.GetFps(), 1e-9);
}

TEST(FpsCalculatorTest, WindowWrapsAround) {
    FpsCalculator calculator(3);
    absl::Time now = absl::Now();

    calculator.AddFrame(now);
    calculator.AddFrame(now + absl::Seconds(1));
    calculator.AddFrame(now + absl::Seconds(2));
    EXPECT_NEAR(1.0, calculator.GetFps(), 1e-9);

    calculator.AddFrame(now + absl::Seconds(3));
    EXPECT_NEAR(1.0, calculator.GetFps(), 1e-9);
}

TEST(FpsCalculatorTest, ZeroDuration) {
    FpsCalculator calculator(10);
    absl::Time now = absl::Now();

    calculator.AddFrame(now);
    calculator.AddFrame(now);
    calculator.AddFrame(now);

    EXPECT_DOUBLE_EQ(0.0, calculator.GetFps());
}

}  // namespace goldfish
