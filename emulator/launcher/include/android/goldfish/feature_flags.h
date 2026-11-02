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

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"

#include "emulator_feature_patterns.pb.h"
#include "goldfish/metrics/studio_stats_wrapper.h"

namespace android::goldfish {

using EmulatorFeatureFlag = android_studio::EmulatorFeatureFlagState::EmulatorFeatureFlag;

inline constexpr std::string_view kCachedFeaturePatternsFileName =
        "emu-last-feature-flags.protobuf";

struct HostInfo {
    std::string cpu_manufacturer;
    std::string os_platform;
    uint32_t cpu_model_name = 0;
    bool virt_support = false;
    bool running_in_vm = false;
};

struct HostGpuInfo {
    std::string make;
    std::string model;
    std::string device_id;
    std::string revision_id;
    std::string version;
    std::string renderer;
};

// Parses a non-empty binary or textproto EmulatorFeaturePatterns file from |filepath|,
// returning std::nullopt if the file cannot be read, fails to parse, or has zero patterns.
std::optional<emulator_features::EmulatorFeaturePatterns> ParseFeaturePatternsFile(
        const std::filesystem::path& filepath);

// Evaluates and stores emulator feature flags from host/guest INI files, overrides,
// and server-side hardware feature patterns.
class FeatureFlags {
  public:
    struct Options {
        std::filesystem::path host_ini_path;
        std::filesystem::path guest_ini_path;
        std::filesystem::path user_ini_path;
        std::filesystem::path cached_patterns_path;
        std::filesystem::path default_patterns_path;
        std::string_view emulator_version;
        int guest_api_level = 0;
        std::string_view avd_feature_flags;
        std::string_view env_feature_flags;
        std::span<const std::string> cli_feature_flags;
    };

    static absl::StatusOr<FeatureFlags> Create(const Options& options);
    static FeatureFlags CreateForTesting(
            std::initializer_list<EmulatorFeatureFlag> baseline_enabled = {});

    FeatureFlags();

    bool IsEnabled(EmulatorFeatureFlag feature) const;
    bool IsOverridden(EmulatorFeatureFlag feature) const;
    bool IsEnabledByGuest(EmulatorFeatureFlag feature) const;

    void SetEnabledOverride(EmulatorFeatureFlag feature, bool enabled);
    void SetIfNotOverridden(EmulatorFeatureFlag feature, bool enabled);

    // Updates the host CPU/OS info and applies any matching server patterns.
    void ApplyHostFeaturePatterns(const HostInfo& host_info);

    // Updates the host GPU info with probed |gpus| and applies any matching server patterns.
    void ApplyGpuFeaturePatterns(std::span<const HostGpuInfo> gpus);

    std::vector<EmulatorFeatureFlag> GetEnabled() const;
    std::vector<EmulatorFeatureFlag> GetEnabledNonOverride() const;
    std::vector<EmulatorFeatureFlag> GetEnabledOverride() const;
    std::vector<EmulatorFeatureFlag> GetDisabledOverride() const;

  private:
    struct FeatureState {
        bool baseline_val = false;
        bool current_val = false;
        bool is_overridden = false;
        bool is_enabled_by_guest = false;
    };

    void ApplyOverrides(std::string_view overrides, std::string_view source = "");
    void ApplyFeatureAction(std::string_view name, bool enable);
    void ApplyFeaturePatterns();

    template <typename Predicate>
    std::vector<EmulatorFeatureFlag> CollectFeatures(Predicate predicate) const;

    absl::flat_hash_map<EmulatorFeatureFlag, FeatureState> features_;
    std::optional<emulator_features::EmulatorFeaturePatterns> patterns_;
    std::string emulator_version_;
    std::optional<HostInfo> host_info_;
    std::vector<HostGpuInfo> host_gpus_;
};

}  // namespace android::goldfish
