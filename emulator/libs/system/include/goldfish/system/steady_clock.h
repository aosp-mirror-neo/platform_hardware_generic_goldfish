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
#pragma once

#include "absl/time/time.h"

namespace goldfish::system {

// Interface for a host monotonic (steady) clock unaffected by wall-clock or NTP
// jumps. Returns elapsed time since the host's steady-clock epoch (e.g.,
// CLOCK_MONOTONIC on Linux) as an absl::Duration rather than absl::Time, which
// is reserved for Unix wall-clock timestamps.
class SteadyClock {
  public:
    virtual ~SteadyClock() = default;

    virtual absl::Duration Now() const = 0;

    // Returns a process-wide SteadyClock backed by std::chrono::steady_clock.
    static const SteadyClock& GetRealClock();
};

}  // namespace goldfish::system
