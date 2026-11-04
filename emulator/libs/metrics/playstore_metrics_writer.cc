/* Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "goldfish/metrics/playstore_metrics_writer.h"

#include <functional>
#include <sstream>
#include <string_view>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"

#include "android/base/system.h"
#include "goldfish/network/http_client.h"
#include "goldfish/tools/aemu_version.h"
#include "google_logs_response.pb.h"
#include "gzip_ostream.h"

namespace goldfish::metrics {

using wireless_android_play_playlog::LogRequest;
using wireless_android_play_playlog::LogResponse;

namespace {

// Upper bound for one upload. The batch is small (at most kMaxStorage bytes before gzip), and the
// last upload runs while the emulator exits.
constexpr absl::Duration kUploadTimeout = absl::Seconds(15);

const char* GetOsType() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macosx";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

LogRequest BuildBaseRequest(const std::string& user_id) {
    LogRequest request;
    request.set_request_time_ms(android::base::System::Get()->GetUnixTimeUs() / 1000);
    request.set_log_source(LogRequest::ANDROID_STUDIO);

    auto& client = *request.mutable_client_info();
    client.set_client_type(wireless_android_play_playlog::ClientInfo::DESKTOP);

    auto& desktop = *client.mutable_desktop_client_info();
    desktop.set_application_build(std::string(goldfish::version::GetEmulatorVersion()));
    desktop.set_os(GetOsType());
    desktop.set_os_full_version(android::base::System::Get()->GetOsName());
    desktop.set_os_major_version(android::base::System::Get()->GetMajorOsVersion());
    desktop.set_logging_id(user_id);
    return request;
}

absl::StatusOr<std::string> SerializeAndGzip(const LogRequest& request) {
    std::ostringstream buff;
    {
        GzipOutputStream gos(buff);
        if (!request.SerializeToOstream(&gos)) {
            return absl::InternalError("Failed to serialize metrics request.");
        }
    }
    return buff.str();
}

absl::StatusOr<absl::Duration> ParseResp(std::string resp) {
    LogResponse response;
    if (!response.ParseFromString(resp)) {
        return absl::InternalError(absl::StrCat("Failed to parse Clearcut LogResponse protobuf (",
                                                resp.size(), " bytes)"));
    }
    if (!response.has_next_request_wait_millis()) {
        return absl::ZeroDuration();
    }
    auto wait = response.next_request_wait_millis();
    if (wait <= 0) {
        return absl::ZeroDuration();
    }
    return absl::Milliseconds(wait);
}

// `cancel` is called during the upload, which stops once it returns true.
absl::StatusOr<absl::Duration> SendToPlaystore(const std::string& url, const LogRequest& req,
                                               const std::function<bool()>& cancel) {
    VLOG(1) << "Making Clearcut POST: " << url << " - " << req.ShortDebugString();
    auto serialized = SerializeAndGzip(req);
    if (!serialized.ok()) {
        return serialized.status();
    }

    auto http_client = goldfish::network::CreateCurlHttpClient();
    constexpr std::string_view kHeaders[] = {
        "Content-Encoding: gzip",
        "Content-Type: application/x-gzip",
    };
    auto resp = http_client->Post(url, *serialized, kHeaders, kUploadTimeout, cancel);
    if (!resp.ok()) {
        return resp.status();
    }

    LOG(INFO) << "Metrics written to playstore.";
    VLOG(2) << "Clearcut response: 200 - '" << *resp << "'";
    return ParseResp(*std::move(resp));
}

}  // namespace

PlaystoreMetricsWriter::PlaystoreMetricsWriter(const std::string& playstore_url,
                                               const std::string& user_id,
                                               goldfish::async::EventLoop& event_loop)
        : playstore_url_(playstore_url)
        , user_id_(user_id)
        , commit_timer_(event_loop.ScheduleRepeating(
                  [this] {
                      Commit();
                      return true;
                  },
                  kCommitInterval, kCommitInterval)) {
    // GetOsName() caches result on first call
    android::base::System::Get()->GetOsName();
}

PlaystoreMetricsWriter::~PlaystoreMetricsWriter() {
    commit_timer_.reset();
    BeginShutdown(kShutdownUploadBudget);
    Commit();
}

void PlaystoreMetricsWriter::Write(MetricsEvent event) {
    wireless_android_play_playlog::LogEvent log_event;
    log_event.set_event_time_ms(event.time_ms);
    if (!event.as_event.SerializeToString(log_event.mutable_source_extension())) {
        LOG(ERROR) << "Failed to serialize metrics event.";
        return;
    }

    absl::MutexLock lock(&mutex_);
    PushEvent(std::move(log_event));
}

void PlaystoreMetricsWriter::PushEvent(wireless_android_play_playlog::LogEvent event) {
    const size_t message_length = event.ByteSizeLong();
    if (message_length > kMaxStorage) {
        return;
    }
    while (current_bytes_ + message_length > kMaxStorage && !events_.empty()) {
        current_bytes_ -= events_.front().ByteSizeLong();
        events_.pop();
    }

    events_.push(std::move(event));
    current_bytes_ += message_length;
}

void PlaystoreMetricsWriter::Commit() {
    LogRequest request;
    {
        absl::MutexLock lock(&mutex_);
        if (events_.empty() ||
            absl::FromUnixMicros(android::base::System::Get()->GetUnixTimeUs()) < send_after_) {
            return;
        }

        // Once shutting down, upload only while time is left and no upload has failed. Otherwise
        // drop the events.
        if (shutdown_upload_failed_ || absl::Now() >= shutdown_deadline_) {
            VLOG(1) << "Shutting down: dropping " << events_.size() << " metrics events.";
            events_ = {};
            current_bytes_ = 0;
            return;
        }

        request = BuildBaseRequest(user_id_);
        while (!events_.empty()) {
            request.add_log_event()->CopyFrom(events_.front());
            events_.pop();
        }
        current_bytes_ = 0;
    }

    const auto wait_response =
            SendToPlaystore(playstore_url_, request, [this] { return PastShutdownDeadline(); });

    absl::MutexLock lock(&mutex_);
    if (!wait_response.ok()) {
        if (shutdown_deadline_ != absl::InfiniteFuture()) {
            // Shutting down: no retry, so a bad network can't hold up the exit.
            shutdown_upload_failed_ = true;
            VLOG(1) << "Shutting down: dropping " << request.log_event_size()
                    << " metrics events, failed to send them: " << wait_response.status();
        } else {
            LOG(ERROR) << "Failed to upload metrics to Playstore (" << playstore_url_
                       << "): " << wait_response.status();
            // Queue the events again, so that the next Commit() retries them.
            for (auto& event : *request.mutable_log_event()) {
                PushEvent(std::move(event));
            }
        }
    } else if (*wait_response > absl::ZeroDuration()) {
        send_after_ = absl::FromUnixMicros(android::base::System::Get()->GetUnixTimeUs()) +
                      *wait_response;
        VLOG(1) << "Updated backoff until " << *wait_response;
    }
}

void PlaystoreMetricsWriter::BeginShutdown(absl::Duration upload_budget) {
    absl::MutexLock lock(&mutex_);
    if (shutdown_deadline_ == absl::InfiniteFuture()) {
        shutdown_deadline_ = absl::Now() + upload_budget;
        VLOG(1) << "Shutting down: uploads get " << upload_budget << " in total, no retries.";
    }
}

bool PlaystoreMetricsWriter::PastShutdownDeadline() {
    absl::MutexLock lock(&mutex_);
    return absl::Now() >= shutdown_deadline_;
}

}  // namespace goldfish::metrics
