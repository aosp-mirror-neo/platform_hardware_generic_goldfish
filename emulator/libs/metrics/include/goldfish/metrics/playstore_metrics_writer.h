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

#pragma once

#include <queue>
#include <string>

#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/metrics/metrics_writer.h"
#include "google_logs_publishing.pb.h"

namespace goldfish::metrics {

class PlaystoreMetricsWriter : public MetricsWriter {
  public:
    PlaystoreMetricsWriter(const std::string& playstore_url, const std::string& user_id,
                           goldfish::async::EventLoop& event_loop);
    ~PlaystoreMetricsWriter() override;

    void Write(MetricsEvent event) override;

  private:
    void Commit();
    // Queues the event, dropping the oldest ones to stay within kMaxStorage. An event bigger than
    // kMaxStorage is dropped.
    void PushEvent(wireless_android_play_playlog::LogEvent event)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    // Whether the shutdown deadline has passed. A running upload checks it, to cancel itself.
    bool PastShutdownDeadline() ABSL_LOCKS_EXCLUDED(mutex_);
    void BeginShutdown(absl::Duration upload_budget) ABSL_LOCKS_EXCLUDED(mutex_);

    const std::string playstore_url_;
    const std::string user_id_;

    goldfish::async::EventLoop::ScopedTimer commit_timer_;

    absl::Mutex mutex_;
    std::queue<wireless_android_play_playlog::LogEvent> events_ ABSL_GUARDED_BY(mutex_);
    size_t current_bytes_ ABSL_GUARDED_BY(mutex_) = 0;
    absl::Time send_after_ ABSL_GUARDED_BY(mutex_) = absl::InfinitePast();
    absl::Time shutdown_deadline_ ABSL_GUARDED_BY(mutex_) = absl::InfiniteFuture();
    bool shutdown_upload_failed_ ABSL_GUARDED_BY(mutex_) = false;

    static constexpr size_t kMaxStorage = 1024 * 128;
    static constexpr absl::Duration kCommitInterval = absl::Minutes(10);
    static constexpr absl::Duration kShutdownUploadBudget = absl::Seconds(2);
};

}  // namespace goldfish::metrics
