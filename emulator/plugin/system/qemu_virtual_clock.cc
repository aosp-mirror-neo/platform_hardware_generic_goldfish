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
#include "goldfish/system/qemu_virtual_clock.h"

#include "absl/base/no_destructor.h"
#include "absl/time/time.h"

extern "C" {
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qemu/timer.h"
// IWYU pragma: end_keep
// clang-format on
}

namespace goldfish::system {
namespace {

class QemuVirtualClock final : public VirtualClock {
  public:
    absl::Duration Now() const override {
        return absl::Nanoseconds(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
};

}  // namespace

const VirtualClock& GetQemuVirtualClock() {
    static const absl::NoDestructor<QemuVirtualClock> kClock;
    return *kClock;
}

}  // namespace goldfish::system
