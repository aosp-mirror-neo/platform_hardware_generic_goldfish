// Copyright (C) 2020 The Android Open Source Project
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
#pragma once
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "goldfish/async/event_loop.h"

namespace android::control::interceptor {

using ::goldfish::async::EventLoop;
using grpc::experimental::Interceptor;
using grpc::experimental::InterceptorBatchMethods;
using grpc::experimental::ServerInterceptorFactoryInterface;
using grpc::experimental::ServerRpcInfo;

// An IdleInterceptor can be installed if you wish to terminate the emulator
// when there is no gRPC activity within the given timeout.
class IdleInterceptor : public Interceptor {
  public:
    IdleInterceptor(std::chrono::seconds timeout, std::atomic<uint64_t>* termination_unix_time,
                    std::atomic<uint64_t>* active_requests);
    ~IdleInterceptor() override;
    void Intercept(InterceptorBatchMethods* methods) override;

  private:
    std::chrono::seconds timeout_;
    std::atomic<uint64_t>* termination_unix_time_;
    std::atomic<uint64_t>* active_requests_;
};

// The factory class that needs to be registered with the gRPC server/client.
//
// The interceptor will schedule a check every timeout seconds to see if any
// gRPC activity took place. If no activity took place the emulator will be
// shutdown in an orderly fashion.
class IdleInterceptorFactory : public ServerInterceptorFactoryInterface {
  public:
    IdleInterceptorFactory(std::chrono::seconds timeout, EventLoop* event_loop);
    ~IdleInterceptorFactory() override = default;
    Interceptor* CreateServerInterceptor(ServerRpcInfo* info) override;
    bool CheckIdleTimeout();

  private:
    int shutdown_attempt_{0};
    std::chrono::seconds timeout_;
    std::atomic<uint64_t> termination_unix_time_;
    std::atomic<uint64_t> active_requests_;
    EventLoop::ScopedTimer timeout_checker_;
};

}  // namespace android::control::interceptor
