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

#include <filesystem>
#include <string>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"

#include "android/goldfish/avd.h"
#include "android/goldfish/feature_flags.h"

namespace android::goldfish {

bool ShouldTrampolineToQemu2(const absl::StatusOr<std::unique_ptr<android::goldfish::Avd>>& avd,
                             const absl::StatusOr<FeatureFlags>& feature_flags);

absl::Status TrampolineToQemu2(
        const fs::path& launcher_directory, std::vector<std::string> args,
        absl::FunctionRef<void()> before_exec = [] {});

}  // namespace android::goldfish
