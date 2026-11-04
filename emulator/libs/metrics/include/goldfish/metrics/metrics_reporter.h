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

#include <memory>
#include <queue>
#include <string>
#include <thread>

#include "absl/synchronization/mutex.h"
#include "google/protobuf/text_format.h"

#include "goldfish/metrics/metrics_writer.h"
#include "goldfish/metrics/uuid.h"
#include "studio_stats_wrapper.h"

namespace goldfish::metrics {

// VLOG level for debugging metrics writer events without changing the backend writer.
constexpr int kMetricsWriterDebugVlogLevel = 5;

class MetricsReporter final {
  public:
    MetricsReporter() : MetricsReporter(Uuid::Generate()) {}
    explicit MetricsReporter(Uuid session_id);

    ~MetricsReporter();
    MetricsReporter(const MetricsReporter&) = delete;
    MetricsReporter(MetricsReporter&&) = delete;
    MetricsReporter& operator=(const MetricsReporter&) = delete;
    MetricsReporter& operator=(MetricsReporter&&) = delete;

    void SetWriter(std::unique_ptr<MetricsWriter> writer);

    // Drains queued events, stops the background worker thread, and destroys the
    // active MetricsWriter synchronously (flushing/finalizing any open spool file).
    // After Shutdown(), Report() drops events and SetWriter() destroys the new
    // writer immediately. Safe to call multiple times, but must not be called from
    // the worker thread (it would join itself).
    void Shutdown();

    using Callback = std::function<void(android_studio::AndroidStudioEvent&)>;
    void Report(const Callback& callback);

    const std::string& session_id() const { return session_id_; }

  private:
    void SetBaseFields(android_studio::AndroidStudioEvent& event);

    void Run();

    const std::string session_id_;
    google::protobuf::TextFormat::Printer debug_printer_;

    absl::Mutex shutdown_mutex_;
    absl::Mutex mutex_ ABSL_ACQUIRED_AFTER(shutdown_mutex_);
    std::shared_ptr<MetricsWriter> writer_ ABSL_GUARDED_BY(mutex_);
    bool running_ ABSL_GUARDED_BY(mutex_) = false;
    std::queue<MetricsEvent> event_queue_ ABSL_GUARDED_BY(mutex_);
    std::thread worker_thread_ ABSL_GUARDED_BY(shutdown_mutex_);
};

}  // namespace goldfish::metrics
