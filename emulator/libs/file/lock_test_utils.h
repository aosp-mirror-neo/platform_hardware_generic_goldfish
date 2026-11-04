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

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "android/base/bazel_info.h"
#include "android/process/process.h"

namespace android::base::file::testing {

inline std::string SleepExecutablePath() {
#ifdef _WIN32
    constexpr std::string_view kSuffix = ".exe";
#else
    constexpr std::string_view kSuffix = "";
#endif
    return Bazel::RunfilesPath(absl::StrCat("goldfish+/emulator/libs/process/sleep_emu", kSuffix));
}

inline void WaitForChildExec(int pid, std::string_view expected_name) {
    const absl::Time deadline = absl::Now() + absl::Seconds(10);
    auto proc = Process::FromPid(pid);
    ASSERT_NE(proc, nullptr);
    while (absl::Now() < deadline) {
        if (absl::StrContains(proc->Exe(), expected_name)) {
            return;
        }
        absl::SleepFor(absl::Milliseconds(5));
    }
    FAIL() << "Timed out waiting for PID " << pid << " to exec " << expected_name
           << " (current exe: " << proc->Exe() << ")";
}

}  // namespace android::base::file::testing
