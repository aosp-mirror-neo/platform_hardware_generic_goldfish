// Copyright (C) 2019 The Android Open Source Project
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
#include "android/control/interceptor/idle_interceptor.h"

#include <functional>

#include "absl/log/log.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/process/process.h"
#include "goldfish/async/event_loop.h"

namespace android::control::interceptor {

using goldfish::async::EventLoop;
using grpc::experimental::Interceptor;
using grpc::experimental::InterceptorBatchMethods;
using grpc::experimental::ServerRpcInfo;

IdleInterceptor::IdleInterceptor(std::chrono::seconds timeout,
                                 std::atomic<uint64_t>* termination_unix_time,
                                 std::atomic<uint64_t>* active_requests)
        : timeout_(timeout)
        , termination_unix_time_(termination_unix_time)
        , active_requests_(active_requests) {}

IdleInterceptor::~IdleInterceptor() {
    const uint64_t idle_time = absl::ToUnixSeconds(absl::Now() + absl::FromChrono(timeout_));
    termination_unix_time_->store(idle_time);
    active_requests_->fetch_sub(1);
}

void IdleInterceptor::Intercept(InterceptorBatchMethods* methods) {
    methods->Proceed();
}

IdleInterceptorFactory::IdleInterceptorFactory(std::chrono::seconds timeout, EventLoop* event_loop)
        : timeout_(timeout)
        , termination_unix_time_(static_cast<uint64_t>(
                  absl::ToUnixSeconds(absl::Now() + absl::FromChrono(timeout)))) {
    timeout_checker_ = event_loop->ScheduleRepeating(
            [this]() {
                CheckIdleTimeout();
                return true;
            },
            absl::FromChrono(timeout_), absl::FromChrono(timeout_));
}

Interceptor* IdleInterceptorFactory::CreateServerInterceptor(ServerRpcInfo* /* info */) {
    active_requests_++;
    return new IdleInterceptor(timeout_, &termination_unix_time_, &active_requests_);
}

bool IdleInterceptorFactory::CheckIdleTimeout() {
    auto epoch = absl::ToUnixSeconds(absl::Now());
    if (active_requests_ > 0U || static_cast<uint64_t>(epoch) < termination_unix_time_.load()) {
        return true;
    }

    LOG(WARNING) << "Idled to long, shutting down. " << epoch << " > "
                 << termination_unix_time_.load();
    if (shutdown_attempt_ == 0) {
        LOG(WARNING) << "Trying nicely is not yet implemented..";
    } else {
        LOG(INFO) << "Terminating the emulator.";
        auto me = android::base::Process::Me();
        if (me) {
            me->Terminate();
        }
    }

    shutdown_attempt_++;
    return true;
}

}  // namespace android::control::interceptor
