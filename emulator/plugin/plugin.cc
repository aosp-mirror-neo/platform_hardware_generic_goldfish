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

#include <cstring>

#include "absl/base/log_severity.h"
#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/log/structured.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"

#include "android/base/color_log_sink.h"
#include "android/base/system.h"
#include "android/crashreport/breadcrumb.h"
#include "android/crashreport/crash_reporter.h"
#include "android/crashreport/crash_system.h"
#include "android/crashreport/debug.h"
#include "emulator/plugin/webrtc/webrtc_device.h"
#include "goldfish/adb_device/adb_device.h"
#include "goldfish/avd/avd_finalize.h"
#include "goldfish/avd/avd_start.h"
#include "goldfish/avd/global_event_loop.h"
#include "goldfish/battery/goldfish_battery.h"
#include "goldfish/devices/virtio_gpu_gfxstream/virtio_gpu_gfxstream.h"
#include "goldfish/grpc/grpc.h"
#include "goldfish/input/virtio_input_android.h"
#include "goldfish/legacy/goldfish_address_space_low_level.h"
#include "goldfish/legacy/goldfish_pipe_low_level.h"
#include "goldfish/legacy/goldfish_sync_low_level.h"
#include "goldfish/net/virtio_wifi.h"
#include "goldfish/netsim/netsim_chardev.h"
#include "goldfish/netsim/netsim_connection.h"
#include "goldfish/netsim/netsim_netdev.h"
#include "goldfish/perfetto/perfetto.h"
#include "goldfish/perfetto/perfetto_categories.h"
#include "goldfish/perfetto/qemu_trace_bridge.h"
#include "goldfish/tools/aemu_version.h"
#include "goldfish/vsock/vsock_low_level.h"
#include "goldfish/vsock/vsock_port_fwd.h"
#if defined(_WIN32)
#include "goldfish/wasapi/audio_wasapi.h"
#endif

// library and initialize the crashpad crash engine upon launch.
#include "google/system/aemu_func_defs.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#if defined(CONFIG_TRACE_GOLDFISH)
    #include "google/drivers/trace/goldfish.h"
#endif
    #include "qemu/error-report.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace {

using android::base::System;

void qemu_absl_logger(int severity, const char* file, int line, const char* fmt, va_list ap) {
    const auto log_impl = [](const int severity, const char* file, int line, std::string_view msg) {
        while (!msg.empty()) {
            if (msg.back() == '\n') {
                msg.remove_suffix(1);
            } else {
                break;
            }
        }

        if (msg.empty()) {
            return;
        }

        // Log error messages as breadcrumbs so they show up in crash reports
        if (severity >= static_cast<int>(absl::LogSeverity::kError)) {
            CRUMB(kQemu) << absl::StrFormat("%s:%d %s\n", file ? file : "QEMU", line, msg);
        }

        LOG(LEVEL(severity)).AtLocation(file ? file : "QEMU", line) << absl::LogAsLiteral(msg);
    };

    va_list ap_long;
    va_copy(ap_long, ap);

    char short_buf[256];
    const int size = vsnprintf(short_buf, sizeof(short_buf), fmt, ap);

    if (size <= 0) {
        // nothing: ignore errors and empty strings.
    } else if (size < sizeof(short_buf)) {  // + terminating zero
        log_impl(severity, file, line, std::string_view(short_buf, size));
    } else {
        // vsnprintf returns the untruncated size
        const int required_size = std::min(size + 1, 4096);  // + terminating zero
        std::string long_buf(required_size, '\0');
        vsnprintf(long_buf.data(), required_size, fmt, ap_long);
        long_buf.resize(required_size - 1);  // drop the terminating zero

        if (size > long_buf.size()) {
            long_buf.replace(long_buf.size() - 3, 3, "...");  // indicate truncation
        }

        log_impl(severity, file, line, long_buf);
    }

    va_end(ap_long);
}

void setup_debug_logging() {
    std::string v_str = System::Get()->GetEnvironmentVariable("AEMU_VLOG_LEVEL");
    if (!v_str.empty()) {
        if (int v_level; !absl::SimpleAtoi(v_str, &v_level)) {
            LOG(ERROR) << "AEMU_VLOG_LEVEL was set to an invalid value: " << v_str;
        } else {
            absl::SetGlobalVLogLevel(v_level);
        }
    }

    if (std::string vmodule = System::Get()->GetEnvironmentVariable("AEMU_VMODULE");
        !vmodule.empty()) {
        // TODO share this with launcher.cpp / logging.cpp
        for (const absl::string_view glob_level : absl::StrSplit(vmodule, ',')) {
            const size_t eq = glob_level.rfind('=');
            if (eq == glob_level.npos) continue;
            const absl::string_view glob = glob_level.substr(0, eq);
            int level;
            if (!absl::SimpleAtoi(glob_level.substr(eq + 1), &level)) continue;

            absl::SetVLogLevel(glob, level);
            LOG(INFO) << "Setting module verbosity for " << glob << " to " << level;
        }
    }
}

int get_log_level() {
    // Default to logging only error and fatal.
    static const int default_log_level = 2;
    std::string log_level_str = System::Get()->GetEnvironmentVariable("AEMU_LOG_LEVEL");
    if (log_level_str.empty()) {
        return default_log_level;
    }
    int log_level;
    if (!absl::SimpleAtoi(log_level_str, &log_level)) {
        LOG(ERROR) << "AEMU_LOG_LEVEL was set to an invalid value: " << log_level_str;
        return default_log_level;
    }

    if (log_level < 0 || log_level > 4) {
        LOG(ERROR) << "AEMU_LOG_LEVEL should be in the range [0, 3] (info, "
                      "warning, error, fatal), "
                      "not: "
                   << log_level;
        return default_log_level;
    }
    return log_level;
}

class CrashBreadcrumbSink : public absl::LogSink {
  public:
    void Send(const absl::LogEntry& entry) override {
        if (entry.log_severity() == absl::LogSeverity::kFatal) {
            CRUMB(kQemu) << "FATAL: " << entry.text_message();
        }
    }
};

void setup_logging() {
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
    int log_level = get_log_level();
    absl::SetMinLogLevel(static_cast<absl::LogSeverityAtLeast>(log_level));
    setup_debug_logging();

    if (System::Get()->GetEnvironmentVariable("AEMU_NO_LOG_SINK").empty()) {
        static android::base::ColorLogSink logSink(&std::cout, isatty(fileno(stdout)));
        logSink.SetVerbosity(System::Get()->GetEnvironmentVariable("AEMU_LOG_DETAILED") == "true");
        absl::AddLogSink(&logSink);
        absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
    }

    static CrashBreadcrumbSink crashBreadcrumbSink;
    absl::AddLogSink(&crashBreadcrumbSink);

    // Switch QEMU logging to ABSL.
    set_logger(&qemu_absl_logger);
}

}  // namespace

extern "C" void GF_REGISTER_TYPES_FUNC(void) {
    VLOG(1) << "Enter GF_REGISTER_TYPES";
    goldfish_battery_register_types();
    vsock_port_fwd_register_types();
    vsock_low_level_register_types();
    goldfish::avd::avd_info_register_types();
    goldfish::avd::avd_finalize_register_types();
    goldfish::adb_device::adb_device_register_types();
    virtio_input_android_register_types();
    virtio_gpu_gfxstream_register_types();
    goldfish::grpc::grpc_register_types();
    goldfish::grpc::webrtc_register_types();
    virtio_wifi_register_types();
    goldfish::netsim::netsim_connection_register_types();
    goldfish::netsim::netsim_netdev_register_types();
    goldfish::netsim::netsim_chardev_register_types();
    goldfish_pipe_register_types();
    goldfish_sync_register_types();
    goldfish_address_space_register_types();
#if defined(_WIN32)
    goldfish::wasapi::WasapiRegisterTypes();
#endif
    VLOG(1) << "Exit GF_REGISTER_TYPES";
}

#if defined(CONFIG_TRACE_GOLDFISH)
static std::unique_ptr<goldfish::perfetto::EmulatorTracingSession> sTracingSession;

static void start_tracing() {
    goldfish::perfetto::Initialize();

    const std::string trace_path =
            System::Get()->GetEnvironmentVariable("ANDROID_EMU_PERFETTO_TRACE");
    if (!trace_path.empty()) {
        LOG(INFO) << "Starting Perfetto tracing session to: " << trace_path;
        qemu_goldfish_set_trace_callback(&goldfish::perfetto::OnQemuGoldfishTrace);
        sTracingSession = std::make_unique<goldfish::perfetto::EmulatorTracingSession>(
                trace_path, "qemu,rendering,grpc,display,async");
    }
}

static void stop_tracing() {
    if (sTracingSession) {
        LOG(INFO) << "Stopping Perfetto tracing session";
        qemu_goldfish_set_trace_callback(nullptr);
        sTracingSession->Stop();
        sTracingSession.reset();
    }
}
#else
static void start_tracing() {}
static void stop_tracing() {}
#endif

extern "C" void GF_STARTUP_FUNC(int argc, char** argv) {
    absl::InitializeSymbolizer(argv[0]);

    setup_logging();
    android::base::System::DisableAppNap();
    start_tracing();

    VLOG(1) << "Goldfish plugin version: " << goldfish::version::GetEmulatorFullVersion();
    // The plugin crash system should never try to upload - that should only be
    // done by the launcher.
    if (auto status = android::crashreport::CrashSystem::get().initialize(); !status.ok()) {
        LOG(WARNING) << "Failed to initialize crashreporting: " << status.message();
    }

    absl::FailureSignalHandlerOptions options;
    // Call crashpad after printing stack trace.
    options.call_previous_handler = true;
    absl::InstallFailureSignalHandler(options);

    auto* client_loop = goldfish::avd::globalEventLoop();

    // The global event loop is the main thread of the plugin and should always
    // be responsive, regardless of the vm state.
    android::crashreport::CrashReporter::GetCrashingHangDetector().AddWatchedLooper(
            "GlobalEventLoop", *client_loop, absl::Seconds(15), []() { return true; });

    LOG(INFO) << "goldfish plugin initialization completed";

    // Allow waiting for a debugger through a command line argument
    if (System::Get()->GetEnvironmentVariable("ANDROID_EMU_WAIT_FOR_DEBUGGER") == "1") {
        LOG(WARNING) << "Waiting for a debugger...";
        android::base::WaitForDebugger();
        LOG(WARNING) << "Debugger has attached, resuming";
    }
}

extern "C" void GF_SHUTDOWN_FUNC(void) {
    stop_tracing();

    auto* client_loop = goldfish::avd::globalEventLoop();
    android::crashreport::CrashReporter::GetCrashingHangDetector().RemoveWatchedLooper(
            *client_loop);
    LOG_IF(QFATAL, !client_loop->ShutdownAndWait(absl::Seconds(10)).ok())
            << "global event loop shutdown failed within 10s";
    LOG(INFO) << "goldfish plugin shutdown completed";
}
