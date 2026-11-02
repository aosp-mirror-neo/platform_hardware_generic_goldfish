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
#include <string_view>
#include <thread>

#include "absl/status/statusor.h"
#include "absl/time/time.h"

#include "goldfish/network/http_client.h"

namespace android::goldfish {

inline constexpr std::string_view kFeaturePatternsUrl =
        "https://dl.google.com/dl/android/studio/metadata/emulator-feature-flags.protobuf";
inline constexpr absl::Duration kFeaturePatternDownloadInterval = absl::Hours(24);
inline constexpr absl::Duration kFeaturePatternHttpTimeout = absl::Seconds(5);

struct FeaturePatternDownloadOptions {
    std::filesystem::path cached_patterns_path;
    std::string url = std::string(kFeaturePatternsUrl);
    absl::Duration interval = kFeaturePatternDownloadInterval;
    absl::Duration http_timeout = kFeaturePatternHttpTimeout;
};

// Downloads and caches the latest server feature patterns to |options.cached_patterns_path|
// if the cached file does not exist or its last_download_time is older than |options.interval|.
// Returns true if a new patterns file was downloaded and written, or false if throttled.
absl::StatusOr<bool> CheckAndDownloadFeaturePatterns(const FeaturePatternDownloadOptions& options,
                                                     ::goldfish::network::HttpClient& http_client,
                                                     absl::Time now);

// Runs CheckAndDownloadFeaturePatterns asynchronously on a background thread.
std::jthread StartAsyncFeaturePatternDownload(FeaturePatternDownloadOptions options);

}  // namespace android::goldfish
