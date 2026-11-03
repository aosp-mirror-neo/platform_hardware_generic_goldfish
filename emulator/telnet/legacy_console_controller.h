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
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"

#include "console_server.h"
#include "goldfish/async/async_socket_factory.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/metrics/metrics_reporter.h"

namespace goldfish::telnet {

class LegacyConsoleBridge;

/**
 * @class LegacyConsoleController
 * @brief Manages the lifecycle of legacy telnet console servers for both IPv4 and IPv6.
 */
class LegacyConsoleController {
  public:
    /**
     * @brief Constructs the controller.
     *
     * @param factory The socket factory used to create listeners.
     * @param main_loop The core thread event loop driving the listener sockets.
     * @param metrics_reporter Reporter for sending telnet command usage metrics.
     */
    LegacyConsoleController(goldfish::async::AsyncSocketFactory& factory,
                            goldfish::async::EventLoop* main_loop,
                            goldfish::metrics::MetricsReporter& metrics_reporter);

    /**
     * @brief Which loopback families must be bound for Start() to succeed.
     */
    enum class LoopbackRequirement {
        // IPv4 must bind; IPv6 is best effort. Required when the console port is
        // picked by hunting for a free port, because serial number selection
        // relies on every emulator on the host sharing the IPv4 port space.
        kRequireIpv4,
        // Either family is enough. Only safe when the caller chose the port
        // explicitly (-port/-ports), so no free port search can race with
        // emulators bound to the other family. Allows IPv6-only hosts.
        kAnyFamily,
    };

    /**
     * @brief Starts the console servers for both IPv4 and IPv6 on the specified port.
     *
     * @param port The port to listen on.
     * @param requirement Which loopback families must bind successfully.
     * @return absl::Status Ok if the required server(s) started, or an error.
     */
    absl::Status Start(int port,
                       LoopbackRequirement requirement = LoopbackRequirement::kRequireIpv4);

    /**
     * @brief Stops all running console servers.
     *
     * @return absl::Status Ok if all servers stopped successfully.
     */
    absl::Status Stop();

  private:
    struct ServerInstance {
        std::string name;
        std::shared_ptr<ConsoleServer> server;
    };

    goldfish::async::AsyncSocketFactory& factory_;
    goldfish::async::EventLoop* main_loop_;
    goldfish::metrics::MetricsReporter& metrics_reporter_;
    std::shared_ptr<LegacyConsoleBridge> handler_;

    std::vector<ServerInstance> servers_;
};

}  // namespace goldfish::telnet
