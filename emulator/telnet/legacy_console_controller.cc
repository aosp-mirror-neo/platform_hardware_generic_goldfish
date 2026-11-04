// Copyright (C) 2026 The Android Open Source Project
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
#include "legacy_console_controller.h"

#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include "console_server.h"
#include "goldfish/network/endpoint.h"
#include "legacy_console_bridge.h"
#include "telnet_auth.h"

namespace goldfish::telnet {

LegacyConsoleController::LegacyConsoleController(
        goldfish::async::AsyncSocketFactory& factory, goldfish::async::EventLoop* main_loop,
        goldfish::metrics::MetricsReporter& metrics_reporter)
        : factory_(factory), main_loop_(main_loop), metrics_reporter_(metrics_reporter) {}

absl::Status LegacyConsoleController::Start(int port, LoopbackRequirement requirement) {
    using ::goldfish::network::Endpoint;
    using ::goldfish::network::kIPv4LoopbackAddress;
    using ::goldfish::network::kIPv6LoopbackAddress;
    using ::goldfish::network::ToEndpoint;

    struct ServerConfig {
        std::string name;
        Endpoint endpoint;
    };

    std::vector<ServerConfig> configs = {{"IPv4", ToEndpoint(kIPv4LoopbackAddress, port)},
                                         {"IPv6", ToEndpoint(kIPv6LoopbackAddress, port)}};

    servers_.clear();
    bool any_success = false;
    auto handler = std::make_shared<LegacyConsoleBridge>(port, TelnetAuth::GetTokenPath(),
                                                         metrics_reporter_);

    for (const auto& config : configs) {
        ServerInstance inst;
        inst.name = config.name;
        inst.server =
                std::make_shared<ConsoleServer>(factory_, main_loop_, config.endpoint, handler);
        auto status = inst.server->Start();

        if (status.ok()) {
            any_success = true;
        } else {
            // TODO(jansene): Free port selection requires IPv4 to be available.
            // See b/505949436.
            if (inst.name == "IPv4" && requirement == LoopbackRequirement::kRequireIpv4) {
                servers_.clear();
                return absl::NotFoundError("Unable to bind to IPv4 port");
            }
            // The port was chosen explicitly, so a host with only one loopback
            // family (e.g. an IPv6-only sandbox has no 127.0.0.1) can still run
            // as long as the other family is listening.
            LOG(WARNING) << "Unable to start " << inst.name << " console server on port " << port
                         << ": " << status;
            inst.server.reset();
        }
        servers_.push_back(std::move(inst));
    }

    if (!any_success) {
        servers_.clear();
        return absl::NotFoundError(
                absl::StrCat("Unable to bind the console to IPv4 or IPv6 loopback port ", port));
    }

    handler_ = std::move(handler);
    return absl::OkStatus();
}

absl::Status LegacyConsoleController::Stop() {
    absl::Status status = absl::OkStatus();

    for (auto& inst : servers_) {
        if (inst.server) {
            auto s = inst.server->Stop();
            if (!s.ok()) {
                status = s;
                LOG(ERROR) << "Failed to stop " << inst.name << " console server: " << s;
            }
            inst.server.reset();
        }
    }

    if (handler_) {
        handler_->ReportMetrics();
        handler_.reset();
    }

    return status;
}

}  // namespace goldfish::telnet
