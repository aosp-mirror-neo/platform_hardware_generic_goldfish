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
#include <string>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "grpcpp/support/interceptor.h"

#include "android/control/interceptor/logging_interceptor.h"
#include "android/control/interceptor/percentiles.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/metrics/metrics_reporter.h"

namespace android::control::interceptor {

class MetricsInterceptorFactory : public grpc::experimental::ServerInterceptorFactoryInterface {
  public:
    static constexpr absl::Duration kFlushInterval = absl::Minutes(5);

    // Metrics for a single gRPC method.
    struct MethodMetrics {
        // Tracks bytes received for streaming reads.
        Percentiles rcv_bytes{32, {0.9}};

        // Tracks bytes sent for streaming writes.
        Percentiles snd_bytes{32, {0.9}};

        // Duration of the total call length.
        Percentiles duration{32, {0.9}};

        // Messages received.
        Percentiles rcv_msg{32, {0.9}};

        // Messages sent.
        Percentiles snd_msg{32, {0.9}};

        int total_requests = 0;
        int failed_requests = 0;
    };

    MetricsInterceptorFactory(::goldfish::metrics::MetricsReporter& reporter,
                              ::goldfish::async::EventLoop* event_loop);
    ~MetricsInterceptorFactory() override;

    grpc::experimental::Interceptor* CreateServerInterceptor(
            grpc::experimental::ServerRpcInfo* info) override {
        return new LoggingInterceptor(info,
                                      [this](const InvocationRecord& record) { Record(record); });
    }

    void Record(const InvocationRecord& invocation);

    void ReportMetrics();

  private:
    void ReportMethodMetrics(const std::string& name, const MethodMetrics& metrics, bool is_server)
            ABSL_LOCKS_EXCLUDED(mutex_);

    ::goldfish::metrics::MetricsReporter& reporter_;

    absl::Mutex mutex_;
    absl::flat_hash_map<std::string, MethodMetrics> server_metrics_ ABSL_GUARDED_BY(mutex_);
    absl::flat_hash_map<std::string, MethodMetrics> client_metrics_ ABSL_GUARDED_BY(mutex_);
    ::goldfish::async::EventLoop::ScopedTimer flush_timer_;
};

}  // namespace android::control::interceptor
