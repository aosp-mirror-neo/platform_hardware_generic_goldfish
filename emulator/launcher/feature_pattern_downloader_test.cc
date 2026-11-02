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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/base/testing/test_system.h"
#include "android/goldfish/feature_flags.h"
#include "android/status/status_matcher_macros.h"
#include "emulator_feature_patterns.pb.h"
#include "goldfish/file/file.h"
#include "goldfish/http/web_server.h"
#include "goldfish/network/http_client.h"

namespace android::goldfish {
namespace {

namespace fs = std::filesystem;
using S = android_studio::EmulatorFeatureFlagState;
using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::AllOf;
using ::testing::HasSubstr;

void WriteTextFile(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << path;
    out << content;
}

class FakeHttpClient final : public ::goldfish::network::HttpClient {
  public:
    explicit FakeHttpClient(absl::StatusOr<std::string> response)
        : response_(std::move(response)) {}

    absl::StatusOr<std::string> Get(std::string_view url, absl::Duration timeout,
                                    const std::function<bool()>& cancel) override {
        last_url_ = std::string(url);
        last_timeout_ = timeout;
        ++call_count_;
        if (cancel && cancel()) {
            return absl::CancelledError("cancelled");
        }
        return response_;
    }

    absl::StatusOr<std::string> Post(std::string_view /*url*/, std::string_view /*body*/,
                                     absl::Span<const std::string_view> /*headers*/,
                                     absl::Duration /*timeout*/,
                                     const std::function<bool()>& /*cancel*/) override {
        return absl::UnimplementedError("Post not used");
    }

    int call_count() const { return call_count_; }
    const std::string& last_url() const { return last_url_; }

  private:
    absl::StatusOr<std::string> response_;
    int call_count_ = 0;
    std::string last_url_;
    absl::Duration last_timeout_ = absl::ZeroDuration();
};

TEST(FeaturePatternDownloaderTest, CheckAndDownloadWritesBinaryAndThrottlesFor24Hours) {
    base::TestSystem sys("bin", "myhome");
    const fs::path cached_path = sys.GetTempRoot()->Path() / "emu-last-feature-flags.protobuf";

    const std::string server_textproto = R"pb(
        pattern {
            hwconfig {
                hostinfo {
                    os_platform: "Linux"
                }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
        })pb";

    FakeHttpClient client(server_textproto);
    const absl::Time t0 = absl::FromUnixSeconds(1700000000);
    const FeaturePatternDownloadOptions opts{
        .cached_patterns_path = cached_path,
    };

    // First check downloads and writes binary protobuf with last_download_time = t0.
    ASSERT_OK_AND_ASSIGN(const bool downloaded1,
                         CheckAndDownloadFeaturePatterns(opts, client, t0));
    EXPECT_TRUE(downloaded1);
    EXPECT_EQ(client.call_count(), 1);
    EXPECT_EQ(client.last_url(), kFeaturePatternsUrl);

    // Verify the cached binary protobuf is loaded and applied by FeatureFlags.
    ASSERT_OK_AND_ASSIGN(FeatureFlags flags1,
                         FeatureFlags::Create({
                             .cached_patterns_path = cached_path,
                         }));
    flags1.ApplyHostFeaturePatterns(HostInfo{.os_platform = "Linux"});
    EXPECT_TRUE(flags1.IsEnabled(S::FORCE_LAVAPIPE));

    // Second check 12 hours later is throttled without making an HTTP request.
    ASSERT_OK_AND_ASSIGN(const bool downloaded2,
                         CheckAndDownloadFeaturePatterns(opts, client, t0 + absl::Hours(12)));
    EXPECT_FALSE(downloaded2);
    EXPECT_EQ(client.call_count(), 1);

    // Third check 25 hours later downloads again and updates last_download_time.
    const absl::Time t1 = t0 + absl::Hours(25);
    ASSERT_OK_AND_ASSIGN(const bool downloaded3,
                         CheckAndDownloadFeaturePatterns(opts, client, t1));
    EXPECT_TRUE(downloaded3);
    EXPECT_EQ(client.call_count(), 2);

    // Fourth check 1 hour after t1 is throttled against the updated timestamp.
    ASSERT_OK_AND_ASSIGN(const bool downloaded4,
                         CheckAndDownloadFeaturePatterns(opts, client, t1 + absl::Hours(1)));
    EXPECT_FALSE(downloaded4);
    EXPECT_EQ(client.call_count(), 2);
}

TEST(FeaturePatternDownloaderTest, CheckAndDownloadRejectsInvalidOrEmptyTextproto) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();
    const fs::path cached_path = dir / "emu-last-feature-flags.protobuf";
    const fs::path default_path = dir / "emu-original-feature-flags.protobuf";
    WriteTextFile(default_path, R"pb(
        pattern {
            hwconfig {
                hostinfo {
                    os_platform: "Linux"
                }
            }
            featureaction {
                feature: ForceSwiftshader
                enable: true
            }
        })pb");

    const FeaturePatternDownloadOptions opts{
        .cached_patterns_path = cached_path,
    };

    for (std::string_view bad_payload :
         {std::string_view("not a valid textproto {{{"), std::string_view(""),
          std::string_view("# comment-only HTTP 200 payload\n")}) {
        FakeHttpClient client{std::string(bad_payload)};
        const auto res =
                CheckAndDownloadFeaturePatterns(opts, client, absl::FromUnixSeconds(1000));
        EXPECT_THAT(res, StatusIs(absl::StatusCode::kDataLoss,
                                  AllOf(HasSubstr(kFeaturePatternsUrl),
                                        HasSubstr(std::to_string(bad_payload.size()) + " bytes"))));
        EXPECT_FALSE(android::base::file::exists(cached_path));
    }

    // Bundled default patterns still apply because no empty cache file was stamped.
    ASSERT_OK_AND_ASSIGN(FeatureFlags flags,
                         FeatureFlags::Create({
                             .cached_patterns_path = cached_path,
                             .default_patterns_path = default_path,
                         }));
    flags.ApplyHostFeaturePatterns(HostInfo{.os_platform = "Linux"});
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_SWIFTSHADER));

    // A pre-existing 0-pattern binary cache file with last_download_time set must also be
    // rejected so LoadPatternsWithFallback still uses default_path and refreshes are not throttled.
    {
        emulator_features::EmulatorFeaturePatterns empty_stamped;
        empty_stamped.set_last_download_time(1000);
        std::ofstream out(cached_path, std::ios_base::binary | std::ios_base::trunc);
        ASSERT_TRUE(out.is_open());
        ASSERT_TRUE(empty_stamped.SerializeToOstream(&out));
    }

    ASSERT_OK_AND_ASSIGN(FeatureFlags fallback_flags,
                         FeatureFlags::Create({
                             .cached_patterns_path = cached_path,
                             .default_patterns_path = default_path,
                         }));
    fallback_flags.ApplyHostFeaturePatterns(HostInfo{.os_platform = "Linux"});
    EXPECT_TRUE(fallback_flags.IsEnabled(S::FORCE_SWIFTSHADER));

    FakeHttpClient valid_client(R"pb(
        pattern {
            hwconfig {
                hostinfo {
                    os_platform: "Linux"
                }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
        })pb");
    ASSERT_OK_AND_ASSIGN(
            const bool redownloaded,
            CheckAndDownloadFeaturePatterns(opts, valid_client,
                                            absl::FromUnixSeconds(1000) + absl::Hours(1)));
    EXPECT_TRUE(redownloaded);
    EXPECT_EQ(valid_client.call_count(), 1);
}

TEST(FeaturePatternDownloaderTest, CheckAndDownloadSupportsNonAsciiDirectory) {
    base::TestSystem sys("bin", "myhome");
    const fs::path unicode_dir = sys.GetTempRoot()->Path() / "täst_中文_🚀";
    const fs::path cached_path = unicode_dir / "emu-last-feature-flags.protobuf";

    FakeHttpClient client(R"pb(
        pattern {
            hwconfig {
                hostinfo {
                    os_platform: "Linux"
                }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
        })pb");
    const FeaturePatternDownloadOptions opts{
        .cached_patterns_path = cached_path,
    };

    ASSERT_OK_AND_ASSIGN(const bool downloaded,
                         CheckAndDownloadFeaturePatterns(opts, client, absl::FromUnixSeconds(2000)));
    EXPECT_TRUE(downloaded);
    EXPECT_TRUE(android::base::file::exists(cached_path));

    ASSERT_OK_AND_ASSIGN(FeatureFlags flags,
                         FeatureFlags::Create({
                             .cached_patterns_path = cached_path,
                         }));
    flags.ApplyHostFeaturePatterns(HostInfo{.os_platform = "Linux"});
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_LAVAPIPE));
}

TEST(FeaturePatternDownloaderTest, StartAsyncDownloadFetchesAndCancelsCleanly) {
    base::TestSystem sys("bin", "myhome");
    const fs::path dir = sys.GetTempRoot()->Path();

    // Empty cached_patterns_path is a no-op and returns a non-joinable jthread.
    {
        std::jthread empty_handle = StartAsyncFeaturePatternDownload({});
        EXPECT_FALSE(empty_handle.joinable());
    }

    constexpr std::string_view kServerTextproto = R"pb(
        pattern {
            hwconfig {
                hostinfo {
                    os_platform: "Linux"
                }
            }
            featureaction {
                feature: ForceLavapipe
                enable: true
            }
        })pb";

    std::atomic<int> pattern_requests{0};
    std::atomic<bool> stall_notified{false};
    absl::Notification stall_entered;

    ::goldfish::http::WebServer server{
        ::goldfish::http::CreateWebServer(0).BindAddress("127.0.0.1")};
    server.OnGet("/emulator-feature-flags.protobuf",
                 [&](const ::goldfish::http::HttpRequest&) {
                     pattern_requests.fetch_add(1, std::memory_order_relaxed);
                     return ::goldfish::http::HttpResponse::String(
                             std::string(kServerTextproto), ::goldfish::http::HttpStatus::kOk);
                 });
    server.OnStream(
            ::goldfish::http::HttpMethod::kGet, "/stall.protobuf",
            [&](const ::goldfish::http::HttpRequest&,
                std::shared_ptr<::goldfish::http::HttpResponseWriter> /*writer*/) {
                if (!stall_notified.exchange(true, std::memory_order_relaxed)) {
                    stall_entered.Notify();
                }
            });
    ASSERT_THAT(server.Start(), IsOk());

    const std::string server_base = absl::StrCat(
            "http://127.0.0.1:",
            ::goldfish::network::GetPortFromEndpoint(server.GetEndpoint()));

    // 1. Successful background download writes and applies the cached binary protobuf.
    const fs::path cached_path = dir / "async_ok" / "emu-last-feature-flags.protobuf";
    {
        std::jthread handle = StartAsyncFeaturePatternDownload({
            .cached_patterns_path = cached_path,
            .url = absl::StrCat(server_base, "/emulator-feature-flags.protobuf"),
        });
        ASSERT_TRUE(handle.joinable());
        handle.join();
    }
    EXPECT_EQ(pattern_requests.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(android::base::file::exists(cached_path));

    ASSERT_OK_AND_ASSIGN(FeatureFlags flags,
                         FeatureFlags::Create({
                             .cached_patterns_path = cached_path,
                         }));
    flags.ApplyHostFeaturePatterns(HostInfo{.os_platform = "Linux"});
    EXPECT_TRUE(flags.IsEnabled(S::FORCE_LAVAPIPE));

    // 2. Destroying the jthread while a request is stalled cancels the HTTP transfer via
    // std::stop_token promptly instead of blocking for the full http_timeout.
    const fs::path stalled_cache_path = dir / "async_stall" / "emu-last-feature-flags.protobuf";
    const absl::Time start = absl::Now();
    {
        std::jthread handle = StartAsyncFeaturePatternDownload({
            .cached_patterns_path = stalled_cache_path,
            .url = absl::StrCat(server_base, "/stall.protobuf"),
            .http_timeout = absl::Seconds(10),
        });
        ASSERT_TRUE(stall_entered.WaitForNotificationWithTimeout(absl::Seconds(2)));
    }
    const absl::Duration elapsed = absl::Now() - start;
    server.Stop();

    EXPECT_LT(elapsed, absl::Seconds(4));
    EXPECT_FALSE(android::base::file::exists(stalled_cache_path));
}

}  // namespace
}  // namespace android::goldfish
