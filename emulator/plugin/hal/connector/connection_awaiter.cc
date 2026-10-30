// Copyright (C) 2024 The Android Open Source Project
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
#include "goldfish/devices/connection_awaiter.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>

#include "absl/log/check.h"

#include "goldfish/devices/cable/cable.h"

namespace goldfish::devices {

using cable::IPlug;
using cable::PlugPtr;
using cable::SocketPtr;
namespace async = goldfish::async;

ConnectionAwaiter::~ConnectionAwaiter() {
    DCHECK(!socket_);
}

void ConnectionAwaiter::OnConnect() {
    const std::lock_guard<std::mutex> lock(connection_mutex_);
    VLOG(1) << "Received onConnect: "
            << (is_connected_ ? "already connected" : "not connected yet");
    if (is_connected_) {
        return;
    }
    is_connected_ = true;
    connection_retry_task_.reset();
    DCHECK(socket_);
    on_connected_(std::move(socket_));
    DCHECK(!socket_);
}

bool ConnectionAwaiter::OnReceive(const void* /*data*/, size_t /*size*/) {
    return false;
}

SocketPtr ConnectionAwaiter::OnUnplug() {
    const std::lock_guard<std::mutex> lock(connection_mutex_);
    return std::move(socket_);
}

std::shared_ptr<ConnectionAwaiter> ConnectionAwaiter::RetryUntilConnected(
        async::EventLoop* event_loop, CreateConnection create_connection,
        ConnectionCallback on_connected, std::chrono::milliseconds interval) {
    return std::make_shared<ConnectionAwaiter>(event_loop, std::move(create_connection),
                                               std::move(on_connected), interval, Private());
}

ConnectionAwaiter::ConnectionAwaiter(async::EventLoop* event_loop,
                                     CreateConnection create_connection,
                                     ConnectionCallback on_connected,
                                     std::chrono::milliseconds interval, Private)
        : create_connection_(std::move(create_connection)), on_connected_(std::move(on_connected)) {
    VLOG(1) << "Scheduling retry task with interval: " << interval;
    connection_retry_task_ = event_loop->ScheduleRepeating(
            [this]() {
                AttemptConnection();
                return true;
            },
            absl::FromChrono(interval), absl::FromChrono(interval));
}

bool ConnectionAwaiter::AttemptConnection() {
    const std::lock_guard<std::mutex> lock(connection_mutex_);
    VLOG(2) << "attempting a connection: "
            << (is_connected_ ? "already connected" : "not connected");
    if (is_connected_) {
        return false;
    }
    socket_ = create_connection_(this->shared_from_this());
    return true;
}
}  // namespace goldfish::devices
