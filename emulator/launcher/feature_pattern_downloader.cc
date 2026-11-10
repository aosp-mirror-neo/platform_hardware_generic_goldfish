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

#include "feature_pattern_downloader.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stop_token>
#include <thread>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "google/protobuf/text_format.h"

#include "android/base/system.h"
#include "android/goldfish/feature_flags.h"
#include "emulator_feature_patterns.pb.h"
#include "goldfish/file/file.h"
#include "goldfish/network/http_client.h"

namespace android::goldfish {
namespace {

absl::StatusOr<bool> CheckAndDownloadFeaturePatternsImpl(
        const FeaturePatternDownloadOptions& options, ::goldfish::network::HttpClient& http_client,
        absl::Time now, const std::function<bool()>& cancel) {
    if (options.cached_patterns_path.empty()) {
        return absl::InvalidArgumentError("cached_patterns_path must not be empty");
    }

    if (const auto existing = ParseFeaturePatternsFile(options.cached_patterns_path);
        existing.has_value() && existing->last_download_time() > 0) {
        const absl::Time last_download =
                absl::FromUnixSeconds(static_cast<int64_t>(existing->last_download_time()));
        if (now >= last_download && (now - last_download) < options.interval) {
            VLOG(1) << "FeatureFlags: skipping feature pattern download (last downloaded "
                    << (now - last_download) << " ago)";
            return false;
        }
    }

    const auto response = http_client.Get(options.url, options.http_timeout, cancel);
    if (!response.ok()) {
        return response.status();
    }

    emulator_features::EmulatorFeaturePatterns downloaded;
    if (!google::protobuf::TextFormat::ParseFromString(*response, &downloaded) ||
        downloaded.pattern_size() == 0) {
        return absl::DataLossError(absl::StrCat(
                "Failed to parse non-empty feature patterns textproto from ", options.url, " (",
                response->size(), " bytes)"));
    }

    const int64_t unix_seconds = std::max<int64_t>(0, absl::ToUnixSeconds(now));
    downloaded.set_last_download_time(static_cast<uint32_t>(unix_seconds));

    if (const auto parent = options.cached_patterns_path.parent_path(); !parent.empty()) {
        android::base::file::mkdir_recursive(parent, 0755).IgnoreError();
    }

    std::filesystem::path tmp_path = options.cached_patterns_path;
    tmp_path += absl::StrCat(".", android::base::System::GetCurrentProcessPid(), ".tmp");
    bool wrote_ok = false;
    {
        std::ofstream out(tmp_path, std::ios_base::binary | std::ios_base::trunc);
        wrote_ok = out.is_open() && downloaded.SerializeToOstream(&out) && out.flush().good();
    }
    if (!wrote_ok) {
        android::base::file::rm(tmp_path).IgnoreError();
        return absl::InternalError(absl::StrCat("Failed to write cached feature patterns to ",
                                                options.cached_patterns_path.string()));
    }

    if (auto status = android::base::file::mv_file(tmp_path, options.cached_patterns_path);
        !status.ok()) {
        android::base::file::rm(tmp_path).IgnoreError();
        return status;
    }

    return true;
}

}  // namespace

absl::StatusOr<bool> CheckAndDownloadFeaturePatterns(const FeaturePatternDownloadOptions& options,
                                                     ::goldfish::network::HttpClient& http_client,
                                                     absl::Time now) {
    return CheckAndDownloadFeaturePatternsImpl(options, http_client, now, /*cancel=*/{});
}

std::jthread StartAsyncFeaturePatternDownload(FeaturePatternDownloadOptions options) {
    if (options.cached_patterns_path.empty()) {
        return {};
    }
    return std::jthread([opts = std::move(options)](std::stop_token stop_token) {
        auto http_client = ::goldfish::network::CreateCurlHttpClient();
        auto res = CheckAndDownloadFeaturePatternsImpl(
                opts, *http_client, absl::Now(),
                [stop_token] { return stop_token.stop_requested(); });
        if (!res.ok()) {
            VLOG(1) << "FeatureFlags: failed to download server feature patterns: " << res.status();
        }
    });
}

}  // namespace android::goldfish
