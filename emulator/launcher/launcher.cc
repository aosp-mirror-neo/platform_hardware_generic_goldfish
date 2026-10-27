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

#include "launcher.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"

#include "android/base/system.h"
#include "android/cmdline_option.h"
#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/emulator_grpc_client.h"
#include "android/goldfish/avd.h"
#include "android/goldfish/input_paths.h"
#include "android/status/status_macros.h"
#include "emulator_controller.grpc.pb.h"
#include "goldfish/async/async_socket_server.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/libuv_process_launcher.h"
#include "goldfish/async/libuv_signal_handlers.h"
#include "goldfish/async/libuv_socket_factory.h"
#include "goldfish/async/when_all.h"
#include "goldfish/file/file.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/snapshot_metrics.h"
#include "goldfish/metrics/studio_file_metrics_writer.h"
#include "goldfish/modem_simulator/modem_simulator_service.h"
#include "goldfish/network/endpoint.h"
#include "goldfish/timezone/host_timezone.h"
#include "host_info.h"
#include "launch_fishtank.h"
#include "launch_netsimd.h"
#include "launch_qemu/emulator_config.h"
#include "launch_qemu/gpu_device.h"
#include "launch_qemu/launch_qemu.h"
#include "launch_qemu/snapshot_device.h"
#include "legacy_console_controller.h"
#include "logging.h"

namespace android::goldfish {
namespace {

absl::Status send_emulator_grpc_shutdown(int serial_number) {
    // TODO it would be good to share this grpc connection with the telnet console.
    ASSIGN_OR_RETURN(auto client, android::emulation::control::EmulatorGrpcClientBuilder()
                                          .ForDiscoveredEmulator(
                                                  {{"port.serial", std::to_string(serial_number)}})
                                          .BuildBlocking());
    RETURN_IF_ERROR(client->Connect(absl::Seconds(2)));

    ASSIGN_OR_RETURN(auto stub, client->Stub<android::emulation::control::EmulatorController>());
    ASSIGN_OR_RETURN(auto context, client->NewContext());
    context->set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));

    android::emulation::control::VmRunState request;
    request.set_state(android::emulation::control::VmRunState::SHUTDOWN);
    google::protobuf::Empty response;
    return android::emulation::control::GrpcStatusToAbslStatus(
            stub->setVmState(context.get(), request, &response));
}

using ::goldfish::async::WhenAll;
using WhenAllChardevEndpoints = std::shared_ptr<WhenAll<ChardevEndpoints>>;

using ::goldfish::modem_simulator::ModemSimulatorService;

bool ShouldUseNetsim(const LauncherConfig& config) {
    if (config.opts.no_netsim) {
        return false;
    }
    const bool has_endpoint_override = config.opts.packet_streamer_endpoint != nullptr &&
                                       *config.opts.packet_streamer_endpoint != '\0';
    return has_endpoint_override || config.emulator_paths.HasNetsim();
}

class Launcher {
  public:
    Launcher(LauncherConfig config)
            : config_(std::move(config))
            , use_netsim_(ShouldUseNetsim(config_))
            , host_timezone_(::goldfish::timezone::GetHostTimezone()) {
        config_.signal_handlers->SetCallback(
                [this](int signal) { forwarding_signal_handler(signal); });
        config_.event_loop
                .Post([this] {
                    if (auto s = setup_emulator_ports(config_.opts, config_.event_loop); !s.ok()) {
                        fail_startup(absl::Status(
                                s.code(), absl::StrCat("Failed to set ports: ", s.message())));
                        return;
                    }

                    if (ShouldLaunchFishtank(config_.opts)) {
                        config_.event_loop.Post([this] { launch_fishtank(); }).IgnoreError();
                    }

                    auto chardevs = std::make_shared<WhenAll<ChardevEndpoints>>(
                            &config_.event_loop,
                            [this](ChardevEndpoints ce) { launch_emulator(ce); });

                    if (!config_.opts.no_netsim && !use_netsim_) {
                        LOG(WARNING)
                                << "netsimd binary not found in '"
                                << config_.emulator_paths.binary_directory.string()
                                << "'; emulated Wi-Fi, Bluetooth, UWB, and NFC radios are "
                                   "unavailable, and guest networking/telephony will fall back to "
                                   "QEMU SLIRP (eth0) and the host modem simulator. Pass "
                                   "'-packet-streamer-endpoint <uri>' to connect to an external "
                                   "netsimd instance, or '-no-netsim' to suppress this warning.";
                    }

                    if (use_netsim_) {
                        netsimd_connector_thread_ = std::thread([this, chardevs]() {
                            std::optional<std::string> endpoint_override;
                            if (const char* ep = config_.opts.packet_streamer_endpoint; ep && *ep) {
                                endpoint_override = ep;
                            }
                            netsim::NetsimConnector connector([this] { return launch_netsimd(); },
                                                              shutting_down_,
                                                              std::move(endpoint_override));

                            if (auto connection = connector.Run(); connection.ok()) {
                                config_.event_loop
                                        .Post([this, conn = *std::move(connection),
                                               chardevs = std::move(chardevs)]() mutable {
                                            VLOG(1) << "Launcher connection to netsim established";
                                            if (netsimd_connector_thread_.joinable()) {
                                                netsimd_connector_thread_.join();
                                            }
                                            netsimd_connection_ = std::move(conn);
                                            chardevs->MutableResults().netsim =
                                                    netsimd_connection_->GetEndpoint().target();
                                        })
                                        .IgnoreError();
                            } else if (!shutting_down_) {
                                config_.event_loop
                                        .Post([this, status = connection.status()]() {
                                            fail_startup(absl::Status(
                                                    status.code(),
                                                    absl::StrCat("Emulator launch aborted: Failed "
                                                                 "to connect to netsimd (required "
                                                                 "for Wi-Fi, Bluetooth, and "
                                                                 "Telephony). Status: ",
                                                                 status.message())));
                                        })
                                        .IgnoreError();
                            }
                        });
                    }
                    if (!use_netsim_ || config_.opts.no_netsim_modem) {
                        config_.event_loop
                                .Post([this, chardevs]() { init_modem_simulator(chardevs); })
                                .IgnoreError();
                    }
                })
                .IgnoreError();
    }

    int emulator_exit_status() const { return emulator_exit_status_; }
    const absl::Status& startup_status() const { return startup_status_; }

    void join_shutdown_thread() {
        if (netsimd_connector_thread_.joinable()) {
            netsimd_connector_thread_.join();
        }
        if (shutdown_thread_.joinable()) {
            shutdown_thread_.join();
        }
    }

  private:
    void fail_startup(absl::Status status) {
        if (startup_status_.ok()) {
            startup_status_ = std::move(status);
        }
        shutdown();
    }

    void forwarding_signal_handler(int signum) {
        VLOG(1) << "forwarding_signal_handler called with signum: " << signum;
        if (shutting_down_) {
            return;
        }
        if (auto* p = emulator_process_.get()) {
            if (qemu_shutdown_timeout_) {
                // If the user has sent another signal, e.g. another INT or TERM, then they
                // must really want us to exit immediately so don't wait any longer.
                LOG(WARNING)
                        << "Signal " << signum
                        << " received while already shutting down; sending SIGKILL to emulator";
                qemu_shutdown_timeout_->Cancel();
                p->Kill(SIGKILL);
                return;
            }
            LOG(ERROR) << "Signal received, sending shutdown command to emulator: " << signum;
            if (auto s = send_emulator_grpc_shutdown(ports_.serial_number); !s.ok()) {
                LOG(WARNING) << "Graceful gRPC shutdown failed; falling back to forceful kill with "
                                "signal: "
                             << signum << " - " << s;
                // Note that: on Windows, this does not send a signal but instead calls
                // TerminateProcess().
                p->Kill(signum);
            }
            qemu_shutdown_timeout_ = config_.event_loop.ScheduleDelayed(
                    [this] {
                        if (auto* proc = emulator_process_.get()) {
                            LOG(WARNING) << "Emulator did not exit within "
                                         << config_.qemu_shutdown_timeout
                                         << " after shutdown signal; sending SIGKILL";
                            proc->Kill(SIGKILL);
                        }
                    },
                    config_.qemu_shutdown_timeout);
        } else {
            // If there is no emulator process yet then we want to shutdown directly.
            shutdown();
        }
    }

    void init_modem_simulator(const WhenAllChardevEndpoints& chardevs) {
        std::optional<std::filesystem::path> icc_profile_override;
        if (const char* icc_profile = config_.opts.icc_profile) {
            icc_profile_override = std::filesystem::path(icc_profile);
        }

        modem_simulator_service_ = ModemSimulatorService::Create(
                *config_.avd, icc_profile_override,
                ::goldfish::timezone::GetGuestTimezone(config_.opts.timezone, host_timezone_)
                        .value_or(""));
        if (modem_simulator_service_) {
            chardevs->MutableResults().modem_simulator =
                    modem_simulator_service_->ChardevEndpoint();
            chardevs->MutableResults().modem_simulator_host_id = modem_simulator_service_->HostId();
        }
    }

    static absl::StatusOr<std::shared_ptr<::goldfish::async::AsyncSocketServer>>
    open_tcp_server_port(::goldfish::async::EventLoop& event_loop,
                         ::goldfish::async::AsyncSocketFactory& factory, int port) {
        using ::goldfish::network::ToEndpoint;
        using ::goldfish::network::ToIpv4Address;
        const auto k_ipv4_loopback = ToIpv4Address(127, 0, 0, 1);

        auto e = ToEndpoint(k_ipv4_loopback, port);
        auto sock = factory.CreateServer(&event_loop, std::move(e), [](auto) {
            VLOG(1) << "Ignoring connection to serial port reservation server";
            return false;
        });
        if (sock != nullptr) {
            return sock;
        }
        return absl::UnavailableError("unable to open server socket");
    }

    absl::Status hunt_for_free_telnet_port(
            ::goldfish::telnet::LegacyConsoleController& console_controller) {
        constexpr int kStartingPort = 5554;
        for (int port = kStartingPort; port < 5585; port += 2) {
            if (auto s = console_controller.Start(port); s.ok()) {
                ports_.serial_number = port;
                ports_.adb_port = port + 1;
                return absl::OkStatus();
            }
        }
        return absl::UnavailableError("No available emulator serial console port (5554-5584)");
    }

    absl::Status setup_emulator_ports(const AndroidOptions& opts,
                                      ::goldfish::async::EventLoop& event_loop) {
        auto console_controller = std::make_unique<::goldfish::telnet::LegacyConsoleController>(
                *config_.socket_factory, &event_loop, *config_.metrics_reporter);
        if (opts.ports) {
            // Format should be console_port,adb_port
            std::vector<std::string_view> parts = absl::StrSplit(opts.ports, ',');
            if (parts.size() != 2) {
                return absl::InvalidArgumentError(
                        absl::StrCat("Failed to parse -ports: ", opts.ports));
            }
            if (!absl::SimpleAtoi(parts[0], &ports_.serial_number)) {
                return absl::InvalidArgumentError(absl::StrCat(
                        "Failed to parse serial port number from -ports: ", opts.ports));
            }
            if (!absl::SimpleAtoi(parts[1], &ports_.adb_port)) {
                return absl::InvalidArgumentError(
                        absl::StrCat("Failed to parse ADB port number from -ports: ", opts.ports));
            }
            if (auto s = console_controller->Start(ports_.serial_number); !s.ok()) {
                return s;
            }
        } else if (opts.port) {
            // opts.port specifies the telnet console port and by default ADB port is that +1
            int port;
            if (!absl::SimpleAtoi(opts.port, &port)) {
                return absl::InvalidArgumentError(
                        absl::StrCat("Failed to parse serial port number from -port ", opts.port));
            }
            ports_.serial_number = port;
            ports_.adb_port = port + 1;
            if (auto s = console_controller->Start(port); !s.ok()) {
                return s;
            }
        } else {
            RETURN_IF_ERROR(hunt_for_free_telnet_port(*console_controller));
        }

        if (ports_.adb_port < 5555 || ports_.adb_port > 5585) {
            LOG(WARNING)
                    << "ADB port specified is out of range [5555,5585], adb may not work properly: "
                    << ports_.adb_port;
        }
        if (ports_.adb_port % 2 != 1) {
            LOG(WARNING) << "ADB port specified is not an odd number, adb may not work properly: "
                         << ports_.adb_port;
        }

        console_controller_ = std::move(console_controller);
        return absl::OkStatus();
    }

    void fishtank_exit(int64_t exit_status, int term_signal) {
        LOG(INFO) << "Fishtank exited with status " << exit_status << ", signal " << term_signal;
        // TODO(whollins): Should we sigterm the emulator when the UI is closed?
        fishtank_process_.reset();
    }

    void launch_fishtank() {
        if (shutting_down_) {
            return;
        }
        if (auto fishtank_config = ::goldfish::launcher::fishtank::launch_config(
                    config_.emulator_paths.fishtank_binary, config_.avd->Name(),
                    ports_.serial_number, config_.opts);
            fishtank_config.ok()) {
            if (auto s = config_.process_launcher->Launch(
                        *std::move(fishtank_config),
                        [this](int64_t status, int signal) { fishtank_exit(status, signal); });
                s.ok()) {
                fishtank_process_ = *std::move(s);
                LOG(INFO) << "Running fishtank as pid: " << fishtank_process_->GetPid();
            } else {
                fail_startup(absl::Status(s.status().code(),
                                          absl::StrCat("Fatal error whilst launching fishtank: ",
                                                       s.status().message())));
            }
        } else {
            fail_startup(absl::Status(fishtank_config.status().code(),
                                      absl::StrCat("Fatal error whilst launching fishtank: ",
                                                   fishtank_config.status().message())));
        }
    }

    void netsimd_exit(int64_t exit_status, int term_signal) {
        LOG(INFO) << "Netsimd exited with status " << exit_status << ", signal " << term_signal;
        netsimd_process_.reset();
    }

    absl::Status launch_netsimd() {
        return config_.event_loop.PostAndWait([this]() -> absl::Status {
            if (shutting_down_) {
                return absl::CancelledError(
                        "Cancelled netsimd launch: emulator launcher is shutting down");
            }

            ASSIGN_OR_RETURN(auto netsim_config,
                             netsim::netsimd_launch_config(config_.emulator_paths.netsim_binary,
                                                           config_.opts, config_.feature_flags));
            ASSIGN_OR_RETURN(auto proc, config_.process_launcher->Launch(
                                                netsim_config, [this](int64_t status, int signal) {
                                                    netsimd_exit(status, signal);
                                                }));
            netsimd_process_ = std::move(proc);
            VLOG(1) << "Running netsimd as pid: " << netsimd_process_->GetPid();
            config_.process_launcher->ForgetProcess(*netsimd_process_);
            return absl::OkStatus();
        });
    }

    void emulator_exit(int64_t exit_status, int term_signal) {
        LOG(INFO) << "emulator exited with status " << exit_status << ", signal " << term_signal;
        qemu_shutdown_timeout_.reset();
        emulator_exit_status_ = static_cast<int>(exit_status);
        emulator_process_.reset();

        // Send a final ping with the crash status and finalize QEMU's .open
        // spool files on clean exit.
        // NOTE(b/476380758): Temporarily gate FinalizeProcessFiles on clean
        // QEMU exits here so crashed QEMU .open spool files stay on disk for
        // next-launch crash recovery until live crash classification lands in a
        // follow-up CL in this stack.
        if (exit_status == 0 && term_signal == 0 && config_.metrics_reporter) {
            if (config_.metrics_writer_config.type ==
                        ::goldfish::metrics::MetricsWriterType::kStudio &&
                qemu_pid_ > 0 && !config_.metrics_writer_config.studio_spool_dir.empty()) {
                ::goldfish::metrics::StudioFileMetricsWriter::FinalizeProcessFiles(
                        config_.metrics_writer_config.studio_spool_dir,
                        config_.metrics_reporter->session_id(), static_cast<uint32_t>(qemu_pid_));
            }
            config_.metrics_reporter->Report([](android_studio::AndroidStudioEvent& event) {
                event.mutable_emulator_details()->set_crashes(kMetricsCrashesNone);
            });
        }
        // TODO(b/476380758): Temporarily disable reporting crashes = 2 (kMetricsCrashesUncleanExit)
        // on unclean QEMU exit to stop misleading crash counts on dashboards (which sum `crashes`
        // and conflate real crashes with kills/error exits). Follow up with accurate crash
        // reporting.
        // else {
        //     config_.metrics_reporter->Report([](android_studio::AndroidStudioEvent& event) {
        //         event.mutable_emulator_details()->set_crashes(kMetricsCrashesUncleanExit);
        //     });
        // }

        shutdown();
    }

    void launch_emulator(ChardevEndpoints chardev_endpoints) {
        if (shutting_down_) {
            return;
        }
        AndroidOptions qemu_opts = config_.opts;
        if (!use_netsim_) {
            qemu_opts.no_netsim = true;
        }
        LaunchQemu emulator{EmulatorConfig{ports_,
                                           chardev_endpoints,
                                           {
                                               .session_id = config_.metrics_reporter->session_id(),
                                               .writer_config = config_.metrics_writer_config,
                                           },
                                           config_.user_paths,
                                           config_.emulator_paths,
                                           *config_.avd,
                                           qemu_opts,
                                           config_.feature_flags,
                                           host_timezone_}};

        if (auto emulator_config = emulator.launch_config(); emulator_config.ok()) {
            if (auto s = config_.process_launcher->Launch(
                        *std::move(emulator_config),
                        [this](int64_t status, int signal) { emulator_exit(status, signal); });
                s.ok()) {
                emulator_process_ = *std::move(s);
                qemu_pid_ = emulator_process_->GetPid();
                LOG(INFO) << "Running emulator as pid: " << qemu_pid_;
                report_host_info_metrics(emulator);
                report_quickboot_metrics(emulator);
            } else {
                fail_startup(
                        absl::Status(s.status().code(),
                                     absl::StrCat("Fatal error whilst launching the emulator: ",
                                                  s.status().message())));
            }
        } else {
            fail_startup(absl::Status(emulator_config.status().code(),
                                      absl::StrCat("Fatal error whilst launching the emulator: ",
                                                   emulator_config.status().message())));
        }
    }

    void report_quickboot_metrics(const LaunchQemu& emulator) {
        if (const auto* snapshot = emulator.get<SnapshotDevice>("snapshot")) {
            ::goldfish::metrics::ReportQuickbootSkippedMetrics(
                    *config_.metrics_reporter, snapshot->load_status(), snapshot->save_status());
        }
    }

    void report_host_info_metrics(const LaunchQemu& emulator) {
        std::string vk_icd;
        std::vector<VulkanGpuDeviceInfo> host_gpus;
        std::optional<VulkanGpuDeviceInfo> selected_gpu;
        if (const auto* gpu = emulator.get<GpuDevice>("gpu0")) {
            vk_icd = gpu->getVulkanIcd();
            host_gpus.assign(gpu->getHostGpus().begin(), gpu->getHostGpus().end());
            selected_gpu = gpu->getSelectedGpu();
        }
        config_.metrics_reporter->Report([this, vk_icd = std::move(vk_icd),
                                          host_gpus = std::move(host_gpus),
                                          selected_gpu = std::move(selected_gpu)](
                                                 android_studio::AndroidStudioEvent& event) {
            android::goldfish::FillEmulatorHostEvent(
                    event, *config_.avd, config_.feature_flags,
                    /*launcher_pid=*/android::base::System::GetCurrentProcessPid(),
                    /*qemu_pid=*/emulator_process_->GetPid(), config_.opts.metrics_collection,
                    config_.opts.fuchsia, vk_icd, host_gpus, selected_gpu);
        });
    }

    void shutdown() {
        if (shutting_down_.exchange(true)) {
            return;
        }

        // Shutdown fishtank if it's running.
        if (auto* p = fishtank_process_.get()) {
            p->Kill(SIGTERM);
        }

        if (console_controller_) {
            if (auto s = console_controller_->Stop(); !s.ok()) {
                LOG(WARNING) << "Failed to gracefully stop console controller: " << s;
            }
            console_controller_.reset();
        }

        if (config_.metrics_reporter) {
            config_.metrics_reporter->Shutdown();
        }

        if (netsimd_connection_) {
            netsimd_connection_->Disconnect();
            netsimd_connection_.reset();
        }

        VLOG(1) << "Shutting down";
        shutdown_thread_ = std::thread([this] {
            // Shut down the signal handlers before the loop.
            config_.signal_handlers->close();
            // This can't run on the loop itself.
            if (auto s = config_.event_loop.ShutdownAndWait(absl::Seconds(10)); !s.ok()) {
                LOG(ERROR) << "Event loop shutdown error: " << s;
            } else {
                VLOG(1) << "Event loop shutdown succeeded";
            }
        });
    }

    const LauncherConfig config_;
    const bool use_netsim_;
    // The zoneinfo name of the host timezone, or std::nullopt if it is unknown.
    const std::optional<std::string> host_timezone_;

    EmulatorPorts ports_;
    std::unique_ptr<::goldfish::telnet::LegacyConsoleController> console_controller_;

    // Keep a handle open from the launcher to keep netsimd alive.
    // This should avoid any races between discovery and qemu device connection.
    netsim::NetsimConnection_ptr netsimd_connection_;

    std::thread netsimd_connector_thread_;

    std::unique_ptr<::goldfish::async::ManagedProcess> fishtank_process_;
    std::unique_ptr<::goldfish::async::ManagedProcess> netsimd_process_;
    std::unique_ptr<::goldfish::async::ManagedProcess> emulator_process_;
    std::shared_ptr<::goldfish::async::EventLoop::Timer> qemu_shutdown_timeout_;
    std::shared_ptr<ModemSimulatorService> modem_simulator_service_;

    int emulator_exit_status_ = 0;
    int64_t qemu_pid_ = 0;
    absl::Status startup_status_ = absl::OkStatus();

    std::thread shutdown_thread_;

    std::atomic<bool> shutting_down_{false};
};

}  // namespace

absl::StatusOr<int> RunLauncher(LauncherConfig config) {
    auto& event_loop = config.event_loop;

    Launcher l(std::move(config));

    auto run_status = event_loop.Run();
    l.join_shutdown_thread();

    if (!run_status.ok()) {
        return absl::Status(run_status.code(), absl::StrCat("Event loop run failed with error: ",
                                                            run_status.message()));
    }

    if (!l.startup_status().ok()) {
        return l.startup_status();
    }

    return l.emulator_exit_status();
}

}  // namespace android::goldfish
