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
#pragma once

#include <vector>

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace goldfish {

/**
 * @brief A utility class for calculating frames per second (FPS).
 *
 * This class uses a sliding window of timestamps to calculate the average FPS
 * over a specified number of frames. This approach provides a stable and
 * accurate FPS measurement.
 */
class FpsCalculator {
  public:
    /**
     * @brief Constructs an FpsCalculator with a given window size.
     *
     * The accuracy of the FPS calculation is controlled by the window size. A
     * larger window size will result in a more stable but less responsive FPS
     * measurement.
     *
     * @param windowSize The number of frames to include in the sliding window.
     */
    explicit FpsCalculator(int window_size);

    /**
     * @brief Informs the calculator that a new frame has arrived.
     *
     * This method should be called for each new frame. It records the timestamp
     * of the frame, which is used to calculate the FPS.
     *
     * @param timestamp The time at which the frame arrived. Defaults to the
     * current time.
     */
    void AddFrame(absl::Time timestamp = absl::Now());

    /**
     * @brief Returns the current FPS.
     *
     * This method calculates the FPS based on the timestamps of the frames in
     * the current window.
     *
     * @return The current FPS, or 0 if not enough data is available.
     */
    double GetFps() const;

  private:
    std::vector<absl::Time> timestamps_;
    int window_size_;
    int current_frame_count_ = 0;
    int next_frame_index_ = 0;
};

}  // namespace goldfish
