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

#include "android/control/interceptor/metrics_interceptor.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/log/log.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/studio_stats_wrapper.h"

namespace android::control::interceptor {

namespace {

void FillPercentiles(const Percentiles& perc, android_studio::PercentileEstimator* event) {
    if (!perc.IsBucketized()) {
        std::vector<double> sorted = perc.RawSamples();
        std::sort(sorted.begin(), sorted.end());
        for (double val : sorted) {
            event->add_raw_sample(val);
        }
    } else {
        for (const auto& b : perc.Buckets()) {
            auto* bucket = event->add_bucket();
            bucket->set_target_percentile(b.p);
            bucket->set_value(b.value);
            bucket->set_count(static_cast<uint64_t>(b.count));
        }
    }
}

}  // namespace

MetricsInterceptorFactory::MetricsInterceptorFactory(::goldfish::metrics::MetricsReporter& reporter,
                                                     ::goldfish::async::EventLoop* event_loop)
        : reporter_(reporter) {
    if (event_loop != nullptr) {
        flush_timer_ = event_loop->ScheduleRepeating(
                [this]() {
                    ReportMetrics();
                    return true;
                },
                kFlushInterval, kFlushInterval);
    }
}

MetricsInterceptorFactory::~MetricsInterceptorFactory() {
    flush_timer_.reset();
    ReportMetrics();
}

void MetricsInterceptorFactory::ReportMetrics() {
    // Move the interval's metrics out under the lock and report them after releasing it, so
    // Record() on gRPC threads never waits for the reporter. Every moved entry was created by
    // Record() in this interval, so idle methods are not reported.
    absl::flat_hash_map<std::string, MethodMetrics> server_metrics;
    absl::flat_hash_map<std::string, MethodMetrics> client_metrics;
    {
        absl::MutexLock lock(mutex_);
        server_metrics.swap(server_metrics_);
        client_metrics.swap(client_metrics_);
    }
    for (const auto& [name, metrics] : server_metrics) {
        ReportMethodMetrics(name, metrics, /*is_server=*/true);
    }
    for (const auto& [name, metrics] : client_metrics) {
        ReportMethodMetrics(name, metrics, /*is_server=*/false);
    }
}

void MetricsInterceptorFactory::ReportMethodMetrics(const std::string& name,
                                                    const MethodMetrics& metrics, bool is_server) {
    reporter_.Report([name, &metrics, is_server](android_studio::AndroidStudioEvent& event) {
        // TODO we should add a new event kind enum for grpc metrics.
        event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_PING);

        auto& grpc = *event.mutable_emulator_details()->mutable_grpc();
        grpc.set_type(is_server ? android_studio::EmulatorGrpc::SERVER
                                : android_studio::EmulatorGrpc::CLIENT);

        uint32_t call_hash = crc32(0, reinterpret_cast<const uint8_t*>(name.data()),
                                   static_cast<uInt>(name.size()));
        grpc.set_call_id(call_hash);

        grpc.set_requests(metrics.total_requests);
        grpc.set_failures(metrics.failed_requests);

        FillPercentiles(metrics.rcv_bytes, grpc.mutable_rcv_bytes_estimate());
        FillPercentiles(metrics.snd_bytes, grpc.mutable_snd_bytes_estimate());
        FillPercentiles(metrics.snd_msg, grpc.mutable_snd());
        FillPercentiles(metrics.rcv_msg, grpc.mutable_rcv());
        FillPercentiles(metrics.duration, grpc.mutable_duration());

        VLOG(1) << "Reporting aggregated gRPC metrics for [" << name
                << "]: " << grpc.ShortDebugString();
    });
}

void MetricsInterceptorFactory::Record(const InvocationRecord& invocation) {
    absl::MutexLock lock(&mutex_);
    auto& map = (invocation.direction == Direction::kIncoming) ? server_metrics_ : client_metrics_;
    MethodMetrics& metrics = map[invocation.method];

    // Byte counts are only collected for streaming requests to keep data volume low.
    if (invocation.type != CallType::kUnary) {
        if (invocation.type != CallType::kServerStreaming) {
            metrics.rcv_bytes.AddSample(static_cast<double>(invocation.rcv_bytes));
        }
        if (invocation.type != CallType::kClientStreaming) {
            metrics.snd_bytes.AddSample(static_cast<double>(invocation.snd_bytes));
        }
    }

    // Always track duration and message counts.
    metrics.duration.AddSample(static_cast<double>(invocation.duration) / 1000.0);
    metrics.rcv_msg.AddSample(static_cast<double>(invocation.rcv_messages));
    metrics.snd_msg.AddSample(static_cast<double>(invocation.snd_messages));

    metrics.total_requests++;
    if (!invocation.status.ok()) {
        metrics.failed_requests++;
    }
}

}  // namespace android::control::interceptor
