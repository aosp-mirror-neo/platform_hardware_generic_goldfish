// Copyright 2026 The Android Open Source Project
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

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"

#include "goldfish/async/async_socket_factory.h"
#include "goldfish/async/async_socket_server.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/network/dns_resolver.h"
#include "goldfish/network/endpoint.h"
#include "goldfish/network/ip_endpoint.h"

namespace goldfish::vsock {

inline bool IsIpv4Endpoint(const goldfish::network::Endpoint& endpoint) {
    return std::holds_alternative<goldfish::network::Ipv4Endpoint>(endpoint);
}

// Resolves `address` into network endpoints, ordering IPv4 endpoints before
// non-IPv4 (IPv6) endpoints while retaining all resolved candidates for
// fallback binding in IPv6-only environments.
inline absl::StatusOr<std::vector<goldfish::network::Endpoint>> ResolveForwarderEndpoints(
        const std::string& address) {
    auto endpoints = goldfish::network::ResolveEndpoints(address);
    if (!endpoints.ok()) {
        return absl::Status(
                endpoints.status().code(),
                absl::StrFormat("Failed to resolve vsock port forwarder address '%s': %s; verify "
                                "the hostname or IP address syntax and DNS/hosts configuration",
                                address, endpoints.status().message()));
    }
    if (endpoints->empty()) {
        return absl::NotFoundError(absl::StrFormat(
                "No network endpoints found for vsock port forwarder address '%s': verify the "
                "hostname or IP address resolves to at least one IPv4 or IPv6 address",
                address));
    }

    std::stable_partition(endpoints->begin(), endpoints->end(), IsIpv4Endpoint);
    if (!IsIpv4Endpoint(endpoints->front())) {
        LOG(WARNING) << "Vsock port forwarder address '" << address
                     << "' did not resolve to an IPv4 endpoint (resolved: ["
                     << absl::StrJoin(*endpoints, ", ", goldfish::network::EndpointFormatter())
                     << "]); falling back to non-IPv4 endpoint " << ToString(endpoints->front());
    }
    return *std::move(endpoints);
}

struct BoundForwarderServer {
    std::shared_ptr<goldfish::async::AsyncSocketServer> server;
    goldfish::network::Endpoint endpoint;
};

// Attempts to bind an AsyncSocketServer to each endpoint in `endpoints` in
// order until one succeeds.
inline absl::StatusOr<BoundForwarderServer> BindForwarderServer(
        goldfish::async::AsyncSocketFactory& socket_factory,
        goldfish::async::EventLoop* client_loop,
        absl::Span<const goldfish::network::Endpoint> endpoints, int guest_port,
        const goldfish::async::AsyncSocketServer::ConnectCallback& connect_callback) {
    if (endpoints.empty()) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Cannot bind vsock port forwarder for guest port %d: no host endpoints provided; "
                "pass at least one resolved host endpoint",
                guest_port));
    }

    for (const auto& endpoint : endpoints) {
        VLOG(1) << "Trying to bind to " << ToString(endpoint);
        if (auto server = socket_factory.CreateServer(client_loop, endpoint, connect_callback)) {
            VLOG(1) << "Successfully bound to " << ToString(endpoint);
            return BoundForwarderServer{
                .server = std::move(server),
                .endpoint = endpoint,
            };
        }
        LOG(WARNING) << "Failed to bind vsock port forwarder for guest port " << guest_port
                     << " to host endpoint " << ToString(endpoint) << " (resolved candidates: ["
                     << absl::StrJoin(endpoints, ", ", goldfish::network::EndpointFormatter())
                     << "]); trying next endpoint if available";
    }

    return absl::UnavailableError(absl::StrFormat(
            "The VSockProxy that forwards the guest port: %d to the host: [%s] could not be "
            "created: all resolved host endpoints failed to bind; verify the host port is not "
            "already in use and the address family is enabled",
            guest_port, absl::StrJoin(endpoints, ", ", goldfish::network::EndpointFormatter())));
}

}  // namespace goldfish::vsock
