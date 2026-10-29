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

#include "goldfish/system/virtual_clock.h"

namespace goldfish::system {

// Returns a VirtualClock backed by QEMU's QEMU_CLOCK_VIRTUAL timer domain.
// Only targets linked into the QEMU plugin binary may depend on this library.
const VirtualClock& GetQemuVirtualClock();

}  // namespace goldfish::system
