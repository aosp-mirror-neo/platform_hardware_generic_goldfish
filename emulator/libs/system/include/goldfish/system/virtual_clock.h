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

// Interface for the guest virtual clock (e.g., QEMU_CLOCK_VIRTUAL), which
// advances only while the guest VM is running and pauses while the VM is
// suspended. Returns guest virtual uptime as an absl::Duration.
class VirtualClock {
  public:
    virtual ~VirtualClock() = default;

    virtual absl::Duration Now() const = 0;
};

}  // namespace goldfish::system
