// Copyright 2024 The Android Open Source Project
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

#include "goldfish/grpc/grpc.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>
#include <type_traits>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/strings/escaping.h"

#include "android/control/interceptor/breadcrumb_interceptor.h"
#include "android/control/interceptor/idle_interceptor.h"
#include "android/control/interceptor/logging_interceptor.h"
#include "android/control/interceptor/metrics_interceptor.h"
#include "android/control/interceptor/perfetto_interceptor.h"
#include "android/emulation/control/allow_list.h"
#include "android/emulation/control/basic_token_auth.h"
#include "android/emulation/control/emulator_controller.h"
#include "android/emulation/control/incubating/modem_service.h"
#include "android/emulation/control/incubating/screen_recording_impl.h"
#include "android/emulation/control/incubating/sensor_service_incubating.h"
#include "android/emulation/control/incubating/vehicle_service.h"
#include "android/emulation/control/jwt_token_auth.h"
#include "android/emulation/control/service_forwarder_impl.h"
#include "android/emulation/control/snapshot_service_impl.h"
#include "android/emulation/control/ui_controller_forwarder.h"
#include "android/sockets/scoped_socket.h"
#include "android/sockets/socket_utils.h"
#include "android/status/status_macros.h"
#include "emulator/plugin/grpc/grpc_display.h"
#include "emulator/plugin/vminterface/vm_lock.h"
#include "emulator/plugin/webrtc/webrtc_device.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/async/qemu_event_loop.h"
#include "goldfish/avd/avd_info.h"
#include "goldfish/avd/global_event_loop.h"
#include "goldfish/discovery/emulator_advertisement.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/file/file.h"
#include "goldfish/grpc/grpc_key_utils.h"
#include "goldfish/grpc/v2/v2_services.h"
#include "goldfish/modem_simulator/modem_simulator_client.h"
#include "goldfish/tools/aemu_version.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
extern "C" {
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/notify.h"
#include "qom/object.h"
#include "system/runstate.h"
#include "ui/console.h"
}
// IWYU pragma: end_keep
// clang-format on

#ifdef send
#undef send
#endif

namespace fs = std::filesystem;
namespace file = ::android::base::file;
using namespace ::android::emulation::control;
using ::android::goldfish::VmOperations;
using ::goldfish::async::EventLoop;
using ::goldfish::async::QemuEventLoop;
using ::goldfish::discovery::EmulatorAdvertisement;
using ::goldfish::discovery::EmulatorProperties;
using ::goldfish::display::IMultiDisplay;
using ::goldfish::modem_simulator::ModemSimulatorClient;

namespace goldfish::grpc {

namespace {

struct GrpcConfig {
    std::string addr{"127.0.0.1"};
    fs::path tls_key_path;
    fs::path tls_cert_path;
    fs::path tls_ca_path;
    fs::path allow_list_path;
    fs::path discovery_path;
    fs::path launcher_dir;
    std::vector<std::string> custom_jwt_public_keys;
    bool enable_logging{false};
    bool enable_embedded{false};
    bool use_token{false};
    bool use_jwt{false};
    int idle_timeout{0};
    int port{0};
    int modem_simulator_port{0};

    std::unique_ptr<AllowList> allow_list;
    std::vector<std::shared_ptr<::grpc::Service>> grpc_services;
    std::unique_ptr<::grpc::Server> grpc_server;
    std::unique_ptr<EmulatorAdvertisement> advertiser;
};

struct GrpcDev {
    DeviceState parent_obj;

    GrpcConfig* config;
    Notifier shutdown_notifier;
    Notifier main_loop_exit_notifier;
};
static_assert(std::is_standard_layout_v<GrpcDev>);

#define TYPE_GRPC "grpc"
#define GRPC_DEV(obj) OBJECT_CHECK(GrpcDev, (obj), TYPE_GRPC)

std::vector<std::shared_ptr<::grpc::Service>> CreateServices(avd::AvdUniverse& avd_universe,
                                                             int modem_simulator_port) {
    std::vector<std::shared_ptr<::grpc::Service>> services;

    auto* vm_operations = &avd_universe.GetVmOperations();

    services.emplace_back(::android::emulation::control::getEmulatorController(
            vm_operations, &avd_universe, &avd_universe.GetMultiDisplay()));
    services.emplace_back(std::make_shared<SnapshotServiceImpl>(*vm_operations));
    services.emplace_back(std::make_shared<::android::emulation::control::VehicleServiceImpl>(
            avd_universe.GetVehicleChannel()));
    auto service_forwarder =
            std::make_shared<::android::emulation::forwarding::ServiceForwarderImpl>();
    services.emplace_back(service_forwarder);
    services.emplace_back(std::make_shared<::android::emulation::forwarding::UiControllerForwarder>(
            service_forwarder));
    services.emplace_back(
            std::make_shared<::android::emulation::control::incubating::ScreenRecordingServiceImpl>(
                    &avd_universe.GetMultiDisplay()));
    services.emplace_back(std::make_shared<
                          ::android::emulation::control::incubating::SensorServiceIncubatingImpl>(
            avd_universe.GetSensorsPhysicalModel()));

    if (modem_simulator_port > 0) {
        services.emplace_back(
                std::make_shared<::android::emulation::control::incubating::ModemServiceImpl>(
                        std::make_unique<ModemSimulatorClient>(modem_simulator_port)));
    } else {
        LOG(WARNING) << "No valid modem_simulator_port. Not enabling gRPC ModemService.";
    }

    for (const auto& webrtc_service : WebrtcGetServices()) {
        services.emplace_back(webrtc_service);
    }

    // Register AEMU v2 services
    auto v2_services = ::goldfish::grpc::v2::CreateV2Services(avd_universe, vm_operations);
    services.insert(services.end(), v2_services.begin(), v2_services.end());

    return services;
}

std::vector<std::unique_ptr<::grpc::experimental::ServerInterceptorFactoryInterface>>
CreateInterceptors(bool enable_logging, int idle_timeout) {
    std::vector<std::unique_ptr<::grpc::experimental::ServerInterceptorFactoryInterface>> creators;
    if (enable_logging) {
        creators.emplace_back(
                std::make_unique<android::control::interceptor::StdOutLoggingInterceptorFactory>());
    }

    creators.emplace_back(
            std::make_unique<android::control::interceptor::BreadcrumbInterceptorFactory>());
    creators.emplace_back(
            std::make_unique<android::control::interceptor::MetricsInterceptorFactory>(
                    ::goldfish::avd::GetAvd().GetMetricsReporter(),
                    goldfish::avd::globalEventLoop()));
    creators.emplace_back(
            std::make_unique<android::control::interceptor::PerfettoInterceptorFactory>());

    if (idle_timeout > 0) {
        LOG(INFO) << "Terminating emulator if no activity after " << idle_timeout << " seconds.";
        creators.emplace_back(
                std::make_unique<android::control::interceptor::IdleInterceptorFactory>(
                        std::chrono::seconds(idle_timeout), goldfish::avd::globalEventLoop()));
    }
    return creators;
}

bool IsLocalAddress(const std::string& addr) {
    return addr == "[::1]" || addr == "127.0.0.1" || addr == "localhost";
}

struct PortRes {
    int port = -1;
    bool ipv6 = false;
};

PortRes ValidatePort(int port) {
    PortRes res;
    android::base::ScopedSocket s0(android::base::socketTcp4LoopbackServer(port));
    if (s0.valid()) {
        res.port = android::base::socketGetPort(s0.get());
    } else {
        // Try ipv6 port
        s0 = android::base::socketTcp6LoopbackServer(port);
        if (s0.valid()) {
            res.port = android::base::socketGetPort(s0.get());
            res.ipv6 = true;
        }
    }
    return res;
}

std::unique_ptr<AllowList> loadAllowlist(const fs::path& path) {
    auto emulator_access = std::ifstream(path);

    if (!emulator_access.good()) {
        LOG(WARNING) << "Cannot find access file " << path << ", blocking all access.";
        return std::make_unique<DisableAccess>();
    }

    LOG(INFO) << "Using security allow list from: " << path;
    auto list = AllowList::FromStream(emulator_access);
    list->SetSource(path.string());

    return list;
}

// Generates a secure base64 encoded token of |cnt| bytes.
std::string generateToken(int cnt) {
    absl::BitGen gen;
    std::string buf(cnt, 0);  // Initialize a string of cnt bytes with 0s
    for (int i = 0; i < cnt; ++i) {
        auto byte = absl::Uniform<uint8_t>(gen);
        buf[i] = static_cast<char>(byte);
    }

    std::string encoded;
    absl::WebSafeBase64Escape(buf, &encoded);
    return encoded;
}

struct CredConf {
    std::string tls_key;
    std::string tls_cert;
    std::string tls_ca;

    // TODO(whollins): update libs/grpc_security to take this as const.
    AllowList* allow_list;

    std::string auth_token;
    fs::path jwk_file;

    bool is_tls() const { return !tls_key.empty() && !tls_cert.empty(); }
    bool has_auth() const { return !auth_token.empty() || !jwk_file.empty(); }
};

absl::StatusOr<CredConf> GetCredConf(const GrpcConfig& config) {
    CredConf cred_conf;
    if (!config.tls_key_path.empty()) {
        ASSIGN_OR_RETURN(cred_conf.tls_key,
                         android::base::file::read_whole_file(config.tls_key_path,
                                                              /*binary=*/false));
    }
    if (!config.tls_cert_path.empty()) {
        ASSIGN_OR_RETURN(cred_conf.tls_cert,
                         android::base::file::read_whole_file(config.tls_cert_path,
                                                              /*binary=*/false));
    }
    if (!config.tls_ca_path.empty()) {
        ASSIGN_OR_RETURN(cred_conf.tls_ca, android::base::file::read_whole_file(config.tls_ca_path,
                                                                                /*binary=*/false));
    }

    cred_conf.allow_list = config.allow_list.get();

    if (config.use_token) {
        constexpr int of64Bytes = 64;
        cred_conf.auth_token = generateToken(of64Bytes);
    }

    if (config.use_jwt || !config.custom_jwt_public_keys.empty()) {
        ASSIGN_OR_RETURN(fs::path jwk_dir,
                         config.advertiser->CreateJwkDirectory(generateToken(16)));

        if (config.use_jwt) {
            cred_conf.jwk_file = jwk_dir / "active.jwk";
        }

        if (!config.custom_jwt_public_keys.empty()) {
            RETURN_IF_ERROR(ProcessCustomJwtKeys(config.custom_jwt_public_keys, jwk_dir));
            if (cred_conf.jwk_file.empty()) {
                cred_conf.jwk_file = jwk_dir / "custom_key_0.jwk";
            }
        }
    }

    return cred_conf;
}

std::shared_ptr<::grpc::ServerCredentials> CreateCredentials(const CredConf& cred_conf,
                                                             bool local_host) {
    std::shared_ptr<::grpc::ServerCredentials> creds;
    if (cred_conf.is_tls()) {
        ::grpc::SslServerCredentialsOptions::PemKeyCertPair keycert(cred_conf.tls_key,
                                                                    cred_conf.tls_cert);
        ::grpc::SslServerCredentialsOptions ssl_opts;
        ssl_opts.pem_key_cert_pairs.push_back(keycert);

        // Register the certificate authority if one exists.
        if (!cred_conf.tls_ca.empty()) {
            ssl_opts.pem_root_certs = cred_conf.tls_ca;
            ssl_opts.client_certificate_request =
                    GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
        }
        creds = ::grpc::SslServerCredentials(ssl_opts);
    } else if (local_host) {
        creds = ::grpc::experimental::LocalServerCredentials(LOCAL_TCP);
    } else {
        creds = ::grpc::InsecureServerCredentials();
    }

    if (cred_conf.has_auth()) {
        auto anyauth = std::vector<std::unique_ptr<BasicTokenAuth>>();
        if (!cred_conf.auth_token.empty()) {
            anyauth.emplace_back(std::make_unique<StaticTokenAuth>(
                    cred_conf.auth_token, "android-studio", cred_conf.allow_list));
        }
        if (!cred_conf.jwk_file.empty()) {
            anyauth.emplace_back(std::make_unique<JwtTokenAuth>(
                    cred_conf.jwk_file.parent_path(), cred_conf.jwk_file, cred_conf.allow_list));
        }
        creds->SetAuthMetadataProcessor(
                std::make_shared<AnyTokenAuth>(std::move(anyauth), cred_conf.allow_list));
    } else {
        LOG(WARNING) << "*** No gRPC protection active ***";
    }
    return creds;
}

EmulatorProperties CreateProps(const GrpcConfig* config, const avd::AvdUniverse& avd_universe,
                               const CredConf& cred_conf) {
    const auto& avdprops = avd_universe.Props();
    EmulatorProperties props{
        {"port.serial", std::to_string(avdprops.serial_number)},
        {"emulator.build", std::string(goldfish::version::GetEmulatorBuildId())},
        {"emulator.version", std::string(goldfish::version::GetEmulatorVersion())},
        {"port.adb", std::to_string(avdprops.adb_port)},
        {"avd.name", avdprops.avd_name},
        {"avd.id", avdprops.avd_id},
        {"avd.dir", avdprops.avd_content_path.string()},
        {"launcher.dir", config->launcher_dir.string()},
        {"cmdline", absl::StrJoin({"\"qemu-system-x86_64\"", "\"@dummy\"",
                                   (config->enable_embedded ? "\"-qt-hide-window\"" : ""),
                                   (config->use_token ? "\"-grpc-use-token\"" : "")},
                                  " ")},
        {"grpc.port", std::to_string(config->port)},
        {"grpc.allowlist", config->allow_list_path.string()},
    };

    if (auto endpoint = avd_universe.GetNetsimEndpoint(); !endpoint.empty()) {
        props["netsim.endpoint"] = endpoint;
    }

    if (!cred_conf.jwk_file.empty()) {
        props["grpc.jwks"] = cred_conf.jwk_file.parent_path().string();
        props["grpc.jwk_active"] = cred_conf.jwk_file.string();
    }

    if (!config->tls_cert_path.empty()) {
        props["grpc.server_cert"] = config->tls_cert_path.string();
    }
    if (!config->tls_ca_path.empty()) {
        props["grpc.ca_root"] = config->tls_ca_path.string();
    }
    if (!cred_conf.auth_token.empty()) {
        props["grpc.token"] = cred_conf.auth_token;
    }

    return props;
}

#ifdef _WIN32
std::string ShutdownCauseToString(ShutdownCause cause) {
    // referencing &ShutdownCause_lookup dereferences an invalid pointer at runtime (b/553508439).
    return absl::StrCat(static_cast<int>(cause));
}
#else
std::string ShutdownCauseToString(ShutdownCause cause) {
    return ShutdownCause_str(cause);
}
#endif

// Runs after the exit snapshot and before qemu_cleanup() tears down QEMU's subsystems, so clients
// can follow the exit snapshot while no RPC or service outlives the QEMU state it uses. Some
// services, like the WebRTC switchboard, outlive their RPC streams and release QEMU resources such
// as audio capture when destroyed. We do not want to do this at program exit either, as we may be
// holding on to loopers whose threads have likely been destroyed at that point.
void grpc_main_loop_exit_notify(Notifier* notifier, void* data) {
    GrpcDev* grpc_device = container_of(notifier, GrpcDev, main_loop_exit_notifier);
    GrpcConfig* config = grpc_device->config;
    if (!config || !config->grpc_server) {
        return;
    }

    if (data) {
        auto cause = *static_cast<const ShutdownCause*>(data);
        LOG(INFO) << "Shutdown requested (cause=" << ShutdownCauseToString(cause)
                  << "), terminating gRPC service on " << config->addr << ":" << config->port
                  << ".";
    } else {
        LOG(INFO) << "Shutdown requested, terminating gRPC service on " << config->addr << ":"
                  << config->port << ".";
    }

    // The QEMU main loop has exited, so stop the AVD objects that RPCs may wait
    // on before any service: the QEMU event loop and the input dispatcher.
    // Called here, with the BQL held, and not from an exit notifier of the AVD,
    // because QEMU runs those in reverse registration order.
    goldfish::avd::GetAvd().OnPreGrpcShutdown();

    // RPC handlers, loop callbacks, and service destructors may need the BQL.
    // Flush the global event loop before shutting down the server so that terminal
    // exit-snapshot progress and run-state events reach active streams and LROs.
    android::goldfish::ScopedVmUnlock unlock;
    goldfish::avd::globalEventLoop()->WaitUntilIdle();
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
    config->grpc_server->Shutdown(deadline);
    config->grpc_server.reset();
    config->grpc_services.clear();
}

// Stops advertising the endpoint so that no new clients discover this emulator. Connected clients
// are still served while the exit snapshot is saved.
void grpc_shutdown_notify(Notifier* notifier, void* /*data*/) {
    GrpcDev* grpc_device = container_of(notifier, GrpcDev, shutdown_notifier);
    GrpcConfig* config = grpc_device->config;
    if (!config) {
        return;
    }
    config->advertiser.reset();
}

void grpc_realize(DeviceState* dev, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(dev);
    auto* config = grpc_device->config;

    if (!config->port) {
        error_setg(errp, "port attribute not set");
        return;
    }

    if (config->discovery_path.empty()) {
        error_setg(errp, "discovery_dir attribute not set");
        return;
    }

    if (!android::base::file::exists(config->discovery_path)) {
        LOG(WARNING) << "Discovery directory: " << config->discovery_path.string()
                     << ", does not exist. creating";
        if (auto s = file::mkdir_recursive(config->discovery_path, 0700); !s.ok()) {
            LOG(ERROR) << "Failed to create discovery directory: "
                       << config->discovery_path.string() << " - " << s;
        }
    }

    if (!config->allow_list_path.empty()) {
        config->allow_list = loadAllowlist(config->allow_list_path);
    }

    config->advertiser = std::make_unique<EmulatorAdvertisement>(config->discovery_path);
    auto cred_conf = GetCredConf(*config);
    if (!cred_conf.ok()) {
        LOG(ERROR) << "failed to load grpc credentials config: " << cred_conf.status();
        error_setg(errp, "failed to load grpc credentials config");
        return;
    }

    PortRes port_res = ValidatePort(config->port);
    config->port = port_res.port;
    bool is_local_address = IsLocalAddress(config->addr);
    if (!is_local_address && cred_conf->has_auth() && !cred_conf->is_tls()) {
        is_local_address = true;
        LOG(WARNING) << "Token/JWT auth requested without tls, restricting access "
                        "to localhost.";
    }
    if (is_local_address) {
        // Translate loopback Ipv4/Ipv6 preference ourselves. gRPC resolver can
        // do it slightly differently than us, leading to unexpected results.
        config->addr = port_res.ipv6 ? "[::1]" : "127.0.0.1";
    }

    auto credentials = CreateCredentials(*cred_conf, is_local_address);

    avd::AvdUniverse& avd_universe = goldfish::avd::GetAvd();
    config->grpc_services = CreateServices(avd_universe, config->modem_simulator_port);
    auto interceptors = CreateInterceptors(config->enable_logging, config->idle_timeout);

    ::grpc::ServerBuilder builder;
    std::string server_address = absl::StrCat(config->addr, ":", config->port);
    builder.AddListeningPort(server_address, std::move(credentials));

    for (const auto& service : config->grpc_services) {
        builder.RegisterService(service.get());
    }
    builder.experimental().SetInterceptorCreators(std::move(interceptors));
    config->grpc_server = builder.BuildAndStart();
    if (!config->grpc_server) {
        error_setg(errp, "failed to initialize grpc server");
        return;
    }

    auto props = CreateProps(config, avd_universe, *cred_conf);
    if (auto s = config->advertiser->Write(props); !s.ok()) {
        LOG(WARNING) << "Failed to write the emulator discovery file. As a result, Android "
                        "Studio and other user interfaces will not be able to automatically "
                        "detect or connect to this running emulator. Reason: "
                     << s;
    }

    grpc_device->shutdown_notifier.notify = grpc_shutdown_notify;
    qemu_register_shutdown_notifier(&grpc_device->shutdown_notifier);
    grpc_device->main_loop_exit_notifier.notify = grpc_main_loop_exit_notify;
    qemu_register_main_loop_exit_notifier(&grpc_device->main_loop_exit_notifier);
}

void grpc_unrealize(DeviceState* dev) {
    VLOG(1) << "Finalizing gRPC endpoint";
    GrpcDev* grpc_device = GRPC_DEV(dev);

    notifier_remove(&grpc_device->shutdown_notifier);
    notifier_remove(&grpc_device->main_loop_exit_notifier);

    // QEMU only unrealizes this device in qemu_cleanup(), after grpc_main_loop_exit_notify().
    DCHECK(!grpc_device->config->grpc_server)
            << "gRPC server must be gone when the main loop exits";
}

void grpc_set_tls_cer(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->tls_cert_path = value;
}

void grpc_set_tls_key(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->tls_key_path = value;
}

void grpc_set_tls_ca(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->tls_ca_path = value;
}

void grpc_set_allowlist(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->allow_list_path = value;
}

void grpc_set_addr(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->addr = value;
}

void grpc_set_port(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    // Check for invalid input or overflow
    if (value < 0 || value > 65535) {
        error_setg(errp, "Port number should be between 0 and 65535, not: %d", value);
        return;
    }

    grpc_device->config->port = value;
}

void grpc_set_idle_timeout(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    grpc_device->config->idle_timeout = value;
}

void grpc_set_modem_simulator_port(Object* obj, Visitor* v, const char* name, void* opaque,
                                   Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    grpc_device->config->modem_simulator_port = value;
}

void grpc_set_enable_token(Object* obj, bool v, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->use_token = v;
}

void grpc_set_discovery_dir(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->discovery_path = value;
}

void grpc_set_launcher_dir(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->launcher_dir = value;
}

void grpc_set_enable_logging(Object* obj, bool v, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->enable_logging = v;
}

void grpc_set_enable_embedded(Object* obj, bool v, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->enable_embedded = v;
}

void grpc_set_enable_jwt(Object* obj, bool v, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config->use_jwt = v;
}

void grpc_set_jwt_public_key(Object* obj, const char* value, Error** errp) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    if (value && *value) {
        grpc_device->config->custom_jwt_public_keys.emplace_back(value);
    }
}

void grpc_instance_init(Object* obj) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    grpc_device->config = new GrpcConfig{};
    add_deletable_object(obj);
}

void grpc_instance_finalize(Object* obj) {
    GrpcDev* grpc_device = GRPC_DEV(obj);
    delete grpc_device->config;
    grpc_device->config = nullptr;
}

void grpc_class_init(ObjectClass* oc, const void* data) {
    object_class_property_add_str(oc, "tls_cer", NULL, grpc_set_tls_cer);
    object_class_property_set_description(oc, "tls_cer",
                                          "PEM file with a X.509 public key certificate.");

    object_class_property_add_str(oc, "tls_key", NULL, grpc_set_tls_key);
    object_class_property_set_description(oc, "tls_key",
                                          "PEM file containing a private key used for TLS.");

    object_class_property_add_str(oc, "tls_ca", NULL, grpc_set_tls_ca);
    object_class_property_set_description(
            oc, "tls_ca",
            "PEM file containing a series of certificate authorities for client "
            "validation.");

    object_class_property_add_str(oc, "allowlist", NULL, grpc_set_allowlist);
    object_class_property_set_description(oc, "allowlist",
                                          "A json file describing the access rules.");

    object_class_property_add_str(oc, "addr", NULL, grpc_set_addr);
    object_class_property_set_description(oc, "addr",
                                          "Address to which the grpc service should be bound.");

    object_class_property_add(oc, "port", "int", NULL, grpc_set_port, NULL, NULL);
    object_class_property_set_description(oc, "port",
                                          "The port to which the grpc service should be bound.");

    object_class_property_add(oc, "idle_timeout", "int", NULL, grpc_set_idle_timeout, NULL, NULL);
    object_class_property_set_description(
            oc, "idle_timeout",
            "Shutdown the emulator after idle_timeout seconds of inactivity from the "
            "gRPC endpoint.");

    object_class_property_add(oc, "modem_simulator_port", "int", NULL,
                              grpc_set_modem_simulator_port, NULL, NULL);
    object_class_property_set_description(oc, "modem_simulator_port",
                                          "The port to connect to the modem simulator service.");

    object_class_property_add_bool(oc, "token", NULL, grpc_set_enable_token);
    object_class_property_set_description(oc, "token",
                                          "Require an authorization header with "
                                          "a valid token for every grpc call.");

    object_class_property_add_bool(oc, "jwt", NULL, grpc_set_enable_jwt);
    object_class_property_set_description(oc, "jwt",
                                          "Require an authorization header with "
                                          "a valid signed JWT token for every grpc call.");

    object_class_property_add_str(oc, "jwt_public_key", NULL, grpc_set_jwt_public_key);
    object_class_property_set_description(
            oc, "jwt_public_key",
            "Pass a custom JWT public key as a file path or raw JWK JSON string. "
            "Can be specified multiple times.");

    object_class_property_add_str(oc, "discovery_dir", NULL, grpc_set_discovery_dir);
    object_class_property_add_str(oc, "launcher_dir", NULL, grpc_set_launcher_dir);

    object_class_property_add_bool(oc, "logging", NULL, grpc_set_enable_logging);
    object_class_property_add_bool(oc, "embedded", NULL, grpc_set_enable_embedded);

    // TODO should this be done on instance realization rather than setup?
    grpc_display_register();

    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = grpc_realize;
    dc->unrealize = grpc_unrealize;
}

const TypeInfo grpc_type_info = {
    .name = TYPE_GRPC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GrpcDev),
    .instance_init = grpc_instance_init,
    .instance_finalize = grpc_instance_finalize,
    .class_init = grpc_class_init,
};

}  // namespace

void grpc_register_types() {
    type_register_static(&grpc_type_info);
}

}  // namespace goldfish::grpc
