// Copyright 2024 The Android Open Source Project
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

#include "machine.h"

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "android/goldfish/avd.h"
#include "android/goldfish/constants.h"
#include "android/goldfish/hardware_config.h"

namespace android::goldfish {

namespace {
absl::StatusOr<std::string> machine(const EmulatorConfig& emulator) {
    const Avd& avd = emulator.avd();
    std::string machine_arg;

    switch (auto a = avd.Arch(); a) {
    case Avd::CpuArchitecture::kArm:
        machine_arg = "goldfish-arm";
        break;
    case Avd::CpuArchitecture::kX86:
        machine_arg = "goldfish";
        break;
    default:
        return absl::UnimplementedError(absl::StrCat("Machine type not supported: ", a));
    }

    if (avd.ApiLevel() < kVirtioReadyApiLevel) {
        absl::StrAppend(&machine_arg, ",goldfish-pipe=on");
        absl::StrAppend(&machine_arg, ",goldfish-sync=on");
    }

    return machine_arg;
}
}  // namespace

absl::Status Machine::initialize(const EmulatorConfig& emulator) {
    if (auto m = machine(emulator); m.ok()) {
        mMachine = *m;
        return absl::OkStatus();
    } else {
        return m.status();
    }
}

// TODO(jansene) add kernel versioning magic to add/subtract parameters,
std::vector<std::string> Machine::getQemuParameters(const EmulatorConfig& emulator) const {
    return {"-machine", mMachine};
}

}  // namespace android::goldfish
