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
#include "goldfish/system/steady_clock.h"

#include <chrono>

#include "absl/base/no_destructor.h"
#include "absl/time/time.h"

namespace goldfish::system {
namespace {

class RealSteadyClock final : public SteadyClock {
  public:
    absl::Duration Now() const override {
        const auto elapsed =
            std::chrono::steady_clock::now().time_since_epoch();
        return absl::Nanoseconds(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count());
    }
};

}  // namespace

const SteadyClock& SteadyClock::GetRealClock() {
    static const absl::NoDestructor<RealSteadyClock> kClock;
    return *kClock;
}

}  // namespace goldfish::system
