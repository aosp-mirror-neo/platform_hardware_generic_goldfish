// Copyright 2026 The Android Open Source Project
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

#include "goldfish/update_check/update_checker.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/charset.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/goldfish/ini_file.h"
#include "goldfish/update_check/repository_manifest.h"

namespace goldfish::update_check {
namespace {

namespace fs = std::filesystem;

struct HostKey {
    std::string os;
    std::string arch;
    std::string url_os;
    std::string url_arch;
};

std::optional<HostKey> MapHost(std::string_view platform, std::string_view target_cpu) {
    if (platform.empty() || target_cpu.empty()) {
        return std::nullopt;
    }

    const bool is_arm64 =
            absl::StartsWith(target_cpu, "arm") || absl::StartsWith(target_cpu, "aarch") ||
            absl::StrContains(target_cpu, "arm64") || absl::StrContains(target_cpu, "aarch64");
    const bool is_x64 = absl::StartsWith(target_cpu, "x86") ||
                        absl::StrContains(target_cpu, "x86_64") ||
                        absl::StrContains(target_cpu, "x64") ||
                        absl::StrContains(target_cpu, "amd64") || target_cpu == "k8";
    if (!is_arm64 && !is_x64) {
        return std::nullopt;
    }

    std::string os;
    std::string url_os;
    if (platform == "linux" || absl::StartsWith(platform, "linux_")) {
        os = "linux";
        url_os = "Linux";
    } else if (platform == "windows" || absl::StartsWith(platform, "windows_")) {
        os = "windows";
        url_os = "Windows";
    } else if (platform == "darwin" || platform == "macosx" ||
               absl::StartsWith(platform, "darwin_")) {
        os = "macosx";
        url_os = "Mac";
    } else {
        return std::nullopt;
    }

    return HostKey{
        .os = std::move(os),
        .arch = is_arm64 ? "aarch64" : "x64",
        .url_os = std::move(url_os),
        .url_arch = is_arm64 ? "arm64" : "x86_64",
    };
}

std::string NormalizeFourPartVersion(std::string_view version) {
    const std::string_view trimmed = absl::StripAsciiWhitespace(version);
    const std::vector<std::string_view> parts = absl::StrSplit(trimmed, '.');
    if (parts.size() == 3) {
        return absl::StrCat(trimmed, ".0");
    }
    return std::string(trimmed);
}

std::string UriEncode(std::string_view input) {
    constexpr absl::CharSet kUnreserved =
            absl::CharSet::AsciiAlphanumerics() | absl::CharSet("-_.~");
    std::string encoded;
    encoded.reserve(input.size());
    for (const char c : input) {
        if (kUnreserved.contains(c)) {
            encoded.push_back(c);
        } else {
            absl::StrAppendFormat(&encoded, "%%%02X", static_cast<unsigned char>(c));
        }
    }
    return encoded;
}

std::string BuildVersionDataKey(std::string_view emulator_version, std::string_view core_version) {
    return absl::StrCat("version=", NormalizeFourPartVersion(emulator_version),
                        "&coreVersion=", UriEncode(core_version));
}

std::string BuildUpdateCheckUrl(std::string_view base_url, std::string_view uid,
                                std::string_view os_name, std::string_view os_arch,
                                std::string_view emulator_version, std::string_view core_version) {
    if (core_version.empty()) {
        return std::string(base_url);
    }
    return absl::StrCat(base_url, "?tool=emulator&uid=", UriEncode(uid), "&os=", UriEncode(os_name),
                        "&osa=", UriEncode(os_arch), "&",
                        BuildVersionDataKey(emulator_version, core_version));
}

std::string FormatUpdateNotice(std::string_view package_display_name,
                               std::string_view package_prefix, const SdkPackage& running,
                               const SdkPackage& newer) {
    std::string_view normalized_prefix = absl::StripAsciiWhitespace(package_prefix);
    while (!normalized_prefix.empty() && normalized_prefix.back() == ';') {
        normalized_prefix.remove_suffix(1);
    }
    return absl::StrFormat(
            "A newer %s is available: %s. You are running %s.\n"
            "Update from Android Studio: Tools > SDK Manager > SDK Tools > \"%s\",\n"
            "or from the command line: sdkmanager \"%s;latest\"",
            package_display_name, newer.ToString(), running.ToString(), package_display_name,
            normalized_prefix);
}

bool NeedsUpdateCheck(const fs::path& user_directory, std::string_view data_key,
                      absl::Duration interval, absl::Time now) {
    if (user_directory.empty()) {
        return true;
    }
    android::goldfish::IniFile ini_file(user_directory / kUpdateCheckIniFileName);
    if (!ini_file.Read()) {
        return true;
    }
    const std::string valid_key = android::goldfish::IniFile::MakeValidKey(data_key);
    const int64_t last_check_seconds = ini_file.GetInt64(valid_key, 0);
    if (last_check_seconds <= 0) {
        return true;
    }
    const absl::Time last_check = absl::FromUnixSeconds(last_check_seconds);
    return now < last_check || (now - last_check) >= interval;
}

void RecordUpdateCheckTime(const fs::path& user_directory, std::string_view data_key,
                           absl::Time check_time) {
    if (user_directory.empty() || data_key.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(user_directory, ec);

    const fs::path ini_path = user_directory / kUpdateCheckIniFileName;
    android::goldfish::IniFile ini_file(ini_path);
    ini_file.Read();
    const std::string valid_key = android::goldfish::IniFile::MakeValidKey(data_key);
    ini_file.SetInt64(valid_key, absl::ToUnixSeconds(check_time));
    if (!ini_file.Write()) {
        LOG(WARNING) << "UpdateCheck: failed to save update check timestamp to "
                     << ini_path.string();
    }
}

// Parses the running emulator version and build ID if it is a release build.
std::optional<SdkPackage> ParseRunningPackage(const UpdateCheckOptions& options) {
    auto running = SdkPackage::Parse(options.emulator_version, options.build_id);
    if (!running.has_value()) {
        VLOG(1) << "UpdateCheck: skipping check for non-release build (" << options.emulator_version
                << "-" << options.build_id << ")";
    }
    return running;
}

absl::StatusOr<std::optional<std::string>> CheckForUpdateImpl(
        const UpdateCheckOptions& options, goldfish::network::HttpClient& http_client,
        absl::Time now, const std::function<bool()>& cancel) {
    const auto running = ParseRunningPackage(options);
    if (!running.has_value()) {
        return std::nullopt;
    }

    const auto host = MapHost(options.platform, options.target_cpu);
    if (!host.has_value()) {
        return absl::FailedPreconditionError(
                absl::StrCat("Unsupported host platform/CPU for update check: platform='",
                             options.platform, "', target_cpu='", options.target_cpu, "'"));
    }

    const std::string data_key =
            BuildVersionDataKey(options.emulator_version, options.core_version);
    if (!NeedsUpdateCheck(options.user_directory, data_key, options.interval, now)) {
        VLOG(1) << "UpdateCheck: skipping version check (checked within the last "
                << options.interval << ")";
        return std::nullopt;
    }

    const std::string url =
            BuildUpdateCheckUrl(options.base_url, options.user_id, host->url_os, host->url_arch,
                                options.emulator_version, options.core_version);

    const auto xml = http_client.Get(url, options.http_timeout, cancel);
    if (!xml.ok()) {
        return xml.status();
    }

    const auto latest =
            FindLatestPackage(*xml, host->os, host->arch, options.package_prefix, options.channel);
    if (!latest.ok()) {
        return latest.status();
    }

    VLOG(1) << "UpdateCheck: current version '" << running->ToString() << "', latest version '"
            << (latest->has_value() ? (*latest)->ToString() : "none") << "'";

    RecordUpdateCheckTime(options.user_directory, data_key, now);

    if (latest->has_value() && **latest > *running) {
        return FormatUpdateNotice(options.package_display_name, options.package_prefix, *running,
                                  **latest);
    }
    return std::nullopt;
}

}  // namespace

absl::StatusOr<std::optional<std::string>> CheckForUpdate(
        const UpdateCheckOptions& options, goldfish::network::HttpClient& http_client,
        absl::Time now) {
    return CheckForUpdateImpl(options, http_client, now, /*cancel=*/{});
}

ScopedUpdateCheck StartAsyncUpdateCheck(UpdateCheckOptions options) {
    if (!ParseRunningPackage(options).has_value()) {
        return ScopedUpdateCheck{};
    }

    std::jthread worker([opts = std::move(options)](std::stop_token stop_token) {
        auto http_client = goldfish::network::CreateCurlHttpClient();
        auto notice = CheckForUpdateImpl(opts, *http_client, absl::Now(),
                                         [stop_token] { return stop_token.stop_requested(); });
        if (!notice.ok()) {
            VLOG(1) << "UpdateCheck: failed to check for updates: " << notice.status();
            return;
        }
        if (notice->has_value()) {
            LOG(WARNING) << **notice;
        }
    });
    return ScopedUpdateCheck(std::move(worker));
}

}  // namespace goldfish::update_check
