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

#include "goldfish/metrics/metrics_reporter.h"

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/log/vlog_is_on.h"

#include "android/base/system.h"
#include "goldfish/tools/aemu_version.h"
#include "qemu-version.h"

namespace goldfish::metrics {
namespace {

thread_local const MetricsReporter* g_active_worker_reporter = nullptr;

}  // namespace

MetricsReporter::MetricsReporter(Uuid session_id) : session_id_(session_id.ToString()) {
    debug_printer_.SetSingleLineMode(true);
    absl::MutexLock shutdown_lock(shutdown_mutex_);
    {
        absl::MutexLock lock(mutex_);
        running_ = true;
    }
    worker_thread_ = std::thread(&MetricsReporter::Run, this);
    LOG(INFO) << "Metrics session started: " << session_id_;
}

MetricsReporter::~MetricsReporter() {
    Shutdown();
    LOG(INFO) << "Metrics session stopped.";
}

void MetricsReporter::Shutdown() {
    DCHECK_NE(g_active_worker_reporter, this)
            << "MetricsReporter::Shutdown() must not be called from its worker thread";
    absl::MutexLock shutdown_lock(shutdown_mutex_);
    DCHECK_NE(std::this_thread::get_id(), worker_thread_.get_id())
            << "MetricsReporter::Shutdown() must not be called from its worker thread";
    {
        absl::MutexLock lock(mutex_);
        running_ = false;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::shared_ptr<MetricsWriter> writer_to_destroy;
    {
        absl::MutexLock lock(mutex_);
        writer_to_destroy = std::move(writer_);
    }
    writer_to_destroy.reset();
}

void MetricsReporter::SetWriter(std::unique_ptr<MetricsWriter> writer) {
    std::shared_ptr<MetricsWriter> next_writer(std::move(writer));
    std::shared_ptr<MetricsWriter> old_writer;
    {
        absl::MutexLock lock(mutex_);
        if (!running_) {
            old_writer = std::move(next_writer);
        } else {
            old_writer = std::move(writer_);
            writer_ = std::move(next_writer);
        }
    }
    old_writer.reset();
}

void MetricsReporter::SetBaseFields(android_studio::AndroidStudioEvent& event) {
    // Default to PING but most reporters should replace this.
    event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_PING);

    // This can be overridden if needed e.g. reports for previous processes that crashed.
    event.set_studio_session_id(session_id_);

    auto& product = *event.mutable_product_details();
    auto& details = *event.mutable_emulator_details();
    product.set_product(android_studio::ProductDetails::EMULATOR);
    product.set_version(std::string(goldfish::version::GetEmulatorVersion()));
    product.set_build(std::string(goldfish::version::GetEmulatorFullVersion()));
    details.set_core_version(QEMU_FULL_VERSION);

    const auto times = android::base::System::Get()->GetProcessTimes();
    details.set_system_time(times.system_ms);
    details.set_user_time(times.user_ms);
    details.set_wall_time(times.wall_clock_ms);
}

void MetricsReporter::Report(const Callback& callback) {
    absl::MutexLock lock(mutex_);
    if (!running_) {
        LOG(WARNING) << "Metric being skipped because reporter isn't running";
        return;
    }

    MetricsEvent event;
    event.time_ms = android::base::System::Get()->GetUnixTimeUs() / 1000;
    SetBaseFields(event.as_event);
    callback(event.as_event);
    event_queue_.push(std::move(event));
}

void MetricsReporter::Run() {
    VLOG(1) << "Metrics reporter Run enter";
    g_active_worker_reporter = this;
    while (true) {
        MetricsEvent event;
        std::shared_ptr<MetricsWriter> writer;
        {
            auto has_event = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
                return !event_queue_.empty() || !running_;
            };
            absl::MutexLock lock(mutex_);
            mutex_.Await(absl::Condition(&has_event));

            if (event_queue_.empty() && !running_) {
                break;
            }

            event = std::move(event_queue_.front());
            event_queue_.pop();
            writer = writer_;
        }

        if (VLOG_IS_ON(kMetricsWriterDebugVlogLevel)) {
            std::string buf;
            LOG_IF(ERROR, !debug_printer_.PrintToString(event.as_event, &buf))
                    << "Failed to format metrics event";
            VLOG(kMetricsWriterDebugVlogLevel)
                    << "event time " << event.time_ms << "ms\n{ " << buf << "}";
        }

        if (writer) {
            writer->Write(std::move(event));
        }
    }
    g_active_worker_reporter = nullptr;
    VLOG(1) << "Metrics reporter Run exit";
}

}  // namespace goldfish::metrics
