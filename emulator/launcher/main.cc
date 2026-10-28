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

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"

#include "android/base/system.h"
#include "android/cmdline_option.h"
#include "android/crashreport/crash_system.h"
#include "android/goldfish/avd.h"
#include "android/goldfish/feature_flags.h"
#include "android/goldfish/input_paths.h"
#include "android/main_help.h"
#include "android/status/status_macros.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/libuv_process_launcher.h"
#include "goldfish/async/libuv_signal_handlers.h"
#include "goldfish/async/libuv_socket_factory.h"
#include "goldfish/fsimage/qcow2.h"
#include "goldfish/metrics/configure_metrics_writer.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/studio_config.h"
#include "goldfish/tools/aemu_version.h"
#include "goldfish/update_check/update_checker.h"
#include "host_info.h"
#include "launcher.h"
#include "logging.h"
#include "qemu-version.h"
#include "trampoline.h"
#include "uv.h"

namespace {
namespace fs = std::filesystem;

constexpr int EMULATOR_COMPATIBLE_QEMU_VERSION = 10;

constexpr std::string_view kUpdateRepositoryUrl =
        "https://dl.google.com/android/repository/repository2-3.xml";
constexpr std::string_view kEmulatorPreviewPackagePrefix = "emulators;";
constexpr std::string_view kEmulatorPreviewDisplayName = "Android Emulator (Preview)";
constexpr absl::Duration kUpdateCheckInterval = absl::Hours(4);
constexpr absl::Duration kUpdateCheckHttpTimeout = absl::Seconds(20);

goldfish::update_check::UpdateChannel ResolveUpdateCheckChannel(
        goldfish::metrics::studio::UpdateChannel studio_channel) {
    using goldfish::metrics::studio::UpdateChannel;
    switch (studio_channel) {
    case UpdateChannel::kBeta:
        return goldfish::update_check::UpdateChannel::kBeta;
    case UpdateChannel::kDev:
        return goldfish::update_check::UpdateChannel::kDev;
    case UpdateChannel::kCanary:
        return goldfish::update_check::UpdateChannel::kCanary;
    case UpdateChannel::kStable:
    case UpdateChannel::kUnknown:
        break;
    }
    return goldfish::update_check::UpdateChannel::kStable;
}

// clang-format off

void ShowBanner() {
    const std::string_view platform = goldfish::version::GetPlatformString();

    // Check if stdout is a terminal
    const bool use_color = isatty(fileno(stdout));

    // Dynamically assign ANSI codes or empty strings based on the TTY check
    const std::string_view c_tail  = use_color ? "\033[95m"   : ""; // Magenta
    const std::string_view c_body  = use_color ? "\033[96m"   : ""; // Cyan
    const std::string_view c_face  = use_color ? "\033[93m"   : ""; // Yellow
    const std::string_view c_beta  = use_color ? "\033[1;32m" : ""; // Bold Green
    const std::string_view c_reset = use_color ? "\033[0m"    : ""; // Reset

    std::cout << absl::Substitute(
R"(                           Welcome to goldfish
         $3_/\_$7              The android emulator
    $3~><>>$4<(((($5o>$7  $6[BETA]$7   Version: $0-$1
         $3\/\/$7              Platform: $2
                           Copyright 2026 The Android Open Source Project
                           ----------------------------------------------
                           DISCLAIMER: This is a beta release. Features are
                           feature-complete and stabilizing, but may still
                           contain bugs or not work exactly as expected.
)",
            goldfish::version::GetEmulatorVersion(),
            goldfish::version::GetEmulatorBuildId(),
            platform, c_tail, c_body, c_face, c_beta, c_reset);
}
// clang-format on

void WarnAboutNoMetricsConsentInput() {
    printf("##############################################################################\n");
    printf("##                        WARNING - ACTION REQUIRED                         ##\n");
    printf("##  Consider using the '-metrics-collection' flag to help improve the       ##\n");
    printf("##  emulator by sending anonymized usage data. Or use the '-no-metrics'     ##\n");
    printf("##  flag to bypass this warning and turn off the metrics collection.        ##\n");
    printf("##  In a future release this warning will turn into a one-time blocking     ##\n");
    printf("##  prompt to ask for explicit user input regarding metrics collection.     ##\n");
    printf("##                                                                          ##\n");
    printf("##  Please see '-help-metrics-collection' for more details. You can use     ##\n");
    printf("##  '-metrics-to-file' or '-metrics-to-console' flags to see what type of   ##\n");
    printf("##  data is being collected by emulator as part of usage statistics.        ##\n");
    printf("##############################################################################\n");
}

std::string EmulatorMetricsUserId(const fs::path& user_directory) {
    auto path = user_directory / "userid";
    if (android::base::file::exists(path)) {
        auto id = android::base::file::read_whole_file(path, /*binary=*/false);
        if (id.ok() && !id->empty()) {
            return *id;
        } else {
            LOG(ERROR) << "failed to read emulator metrics user id, re-generating...";
        }
    }
    auto uuid = ::goldfish::metrics::Uuid::Generate().ToString();
    std::ofstream f(path);
    f << uuid;
    return uuid;
}

::goldfish::metrics::MetricsWriterConfig GetMetricsWriterConfig(const AndroidOptions& opts,
                                                                const fs::path& user_dir) {
    using enum ::goldfish::metrics::MetricsWriterType;
    auto user_id = ::goldfish::metrics::studio::GetMetricsUserId(user_dir);
    if (user_id.empty()) {
        // create our own one if there's no studio config
        user_id = EmulatorMetricsUserId(user_dir);
    }
    if (opts.no_metrics) {
        // do nothing
        LOG(WARNING) << "Metrics disabled by user";
        return {.type = kNone, .user_id = user_id};
    } else if (opts.metrics_to_console) {
        LOG(INFO) << "Metrics will be written to console";
        return {.type = kConsole, .user_id = user_id};
    } else if (opts.metrics_collection) {
        LOG(INFO) << "Metrics will be uploaded directly by the emulator";
        return {.type = kPlaystore,
                .playstore_url = "https://play.googleapis.com/log?format=raw",
                .user_id = user_id,
                .user_upload_consent = true};
    } else if (opts.metrics_to_file) {
        LOG(INFO) << "Metrics will be written to: " << opts.metrics_to_file;
        return {.type = kFile, .file_path = opts.metrics_to_file, .user_id = user_id};
    } else {
        // No CLI overrides, use studio settings.
        switch (::goldfish::metrics::studio::GetUserMetricsOptIn(user_dir)) {
            using enum ::goldfish::metrics::studio::OptInState;
        case kOptedIn:
            LOG(INFO) << "Metrics will be written to file and uploaded by Studio";
            return {.type = kStudio,
                    .studio_spool_dir = ::goldfish::metrics::studio::GetSpoolDirectory(user_dir),
                    .user_id = user_id,
                    .user_upload_consent = true};
        case kOptedOut:
            LOG(INFO) << "Studio user opted out of metrics";
            return {.type = kNone, .user_id = user_id};
        case kUnknown:
            WarnAboutNoMetricsConsentInput();
            return {.type = kNone, .user_id = user_id};
        }
    }
}

void ListAvds(const AndroidOptions& opts, const android::goldfish::UserPaths& user_paths,
              bool verbose) {
    auto avds = android::goldfish::Avd::List(user_paths.avd_directory);
    for (const auto& name : avds) {
        if (verbose) {
            // Only parse the AVD when -verbose is specified.
            auto a = android::goldfish::Avd::FromName(opts, user_paths, name, /*wipe_data=*/false,
                                                      /*content_override=*/{},
                                                      /*sysdir_override=*/{});
            if (a.ok()) {
                std::cout << (*a)->Details(/*verbose=*/true) << '\n';
            } else {
                std::cout << name << " is not valid: " << a.status();
            }
        } else {
            std::cout << name << "\n";
        }
    }
}

std::string FormatSnapshots(const std::vector<goldfish::fsimage::Qcow2Snapshot>& snapshots) {
    if (snapshots.empty()) {
        return "";
    }
    std::string out = absl::StrFormat("%-7s %-16s %12s %19s %15s\n", "ID", "TAG", "VM_SIZE", "DATE",
                                      "VM_CLOCK");
    for (const auto& s : snapshots) {
        const int64_t clock_ms = absl::ToInt64Milliseconds(s.vm_clock);
        const std::string clock_str =
                absl::StrFormat("%04d:%02d:%02d.%03d", clock_ms / 3600000, (clock_ms / 60000) % 60,
                                (clock_ms / 1000) % 60, clock_ms % 1000);
        const std::string date_str =
                absl::FormatTime("%Y-%m-%d %H:%M:%S", s.date, absl::LocalTimeZone());
        absl::StrAppendFormat(&out, "%-7s %-16s %10u B %19s %15s\n", s.id, s.name, s.vm_state_size,
                              date_str, clock_str);
    }
    return out;
}

void ListSnapshots(const AndroidOptions& opts, const android::goldfish::UserPaths& user_paths,
                   bool verbose) {
    std::vector<std::string> avd_names;
    if (opts.avd) {
        avd_names.push_back(opts.avd);
    } else {
        avd_names = android::goldfish::Avd::List(user_paths.avd_directory);
    }

    if (avd_names.empty()) {
        std::cout << "No AVDs found.\n";
        return;
    }

    android::goldfish::Avd::ImageInspector inspector = nullptr;
    if (verbose) {
        inspector = [](const fs::path& p) -> std::string {
            const auto snapshots = goldfish::fsimage::ListQcow2Snapshots(p);
            return snapshots.ok() ? FormatSnapshots(*snapshots) : "";
        };
    }

    for (const auto& name : avd_names) {
        auto a = android::goldfish::Avd::FromName(opts, user_paths, name, /*wipe_data=*/false,
                                                  /*content_override=*/{}, /*sysdir_override=*/{});
        if (!a.ok()) {
            std::cout << "AVD " << name << " is not valid: " << a.status() << "\n";
            continue;
        }
        auto snapshots = (*a)->ListSnapshots(inspector);
        if (verbose) {
            std::cout << "AVD: " << (*a)->Name() << "\n";
            if (snapshots.empty()) {
                std::cout << "  (no snapshots found)\n";
            } else {
                for (const auto& snap : snapshots) {
                    std::cout << "  Snapshot: " << snap.name << "\n";
                    std::cout << "    Path: " << snap.path.string() << "\n";
                    std::cout << "    Size: " << (snap.size_bytes / (1024 * 1024)) << " MB ("
                              << snap.size_bytes << " bytes)\n";
                    if (!snap.last_modified.empty()) {
                        std::cout << "    Last Modified: " << snap.last_modified << "\n";
                    }
                    std::cout << "    Has snapshot.pb: " << (snap.has_snapshot_pb ? "yes" : "no")
                              << "\n";
                    std::cout << "    Has RAM file: " << (snap.has_ram_file ? "yes" : "no") << "\n";
                    if (!snap.image_info.empty()) {
                        std::cout << "    Image Info:\n" << snap.image_info;
                    }
                }
            }
            std::cout << "\n";
        } else {
            std::cout << (*a)->Name();
            if (!snapshots.empty()) {
                std::cout << "\t: ";
                for (const auto& snap : snapshots) {
                    std::cout << snap.name << ", ";
                }
            }
            std::cout << "\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    if (android::base::System::GetEnvironmentVariable("ANDROID_CLI") == "1") {
        if (setsid() == -1 && errno != EPERM) {
            std::cerr << "emulator-launcher: Warning: setsid() failed: " << strerror(errno) << ".\n"
                      << "emulator-launcher: Failed to detach from parent Process Group (PGID).\n"
                      << "emulator-launcher: If running inside a CLI tool like gemini-cli, the "
                         "emulator may be terminated unexpectedly when the launcher exits.\n";
        }
    }
#endif
    absl::InitializeSymbolizer(argv[0]);

    absl::InitializeLog();

    // Take a copy of the args before the parser modifies them.
    std::vector<std::string> args_copy(argv + 1, argv + argc);

    for (int nn = 1; nn < argc; nn++) {
        const char* opt = argv[nn];
        int helpStatus = emulator_parseHelpOption(opt);
        if (helpStatus >= 0) {
            return helpStatus;
        }
    }

    AndroidOptions opts;
    if (android_parse_options(&argc, &argv, &opts) < 0) {
        return 1;
    }

#ifdef _WIN32
    if (!opts.no_boot_anim && !opts.boot_anim) {
        opts.no_boot_anim = 1;
        LOG(WARNING) << "On windows boot animation slows down boot time significantly. It has been "
                        "disabled by default. If you really want to see boot animation, you can "
                        "pass -boot-anim to the command line.";
    }
#endif
    if (opts.boot_anim) {
        opts.no_boot_anim = 0;
    }

    configureLogging(opts);
    android_log_parse_warnings();

    if (!android::base::System::IsUtf8CodePage()) {
        LOG(ERROR) << "Active system code page is not UTF-8. The Android Emulator requires "
                      "Windows 11 or later with UTF-8 code page manifest support; non-ASCII "
                      "file paths and environment variables may not work correctly.";
    }

#if defined(__linux__) || defined(__APPLE__)
    constexpr std::string_view kXDG_RUNTIME_DIR_NAME = "XDG_RUNTIME_DIR";
    auto* sys = android::base::System::Get();
    std::string xdg_runtime_dir_val = sys->EnvGet(kXDG_RUNTIME_DIR_NAME);
    if (xdg_runtime_dir_val.empty()) {
        std::string default_runtime_dir = "/tmp";
#if defined(__APPLE__)
        if (std::string darwin_runtime_dir = sys->EnvGet("DARWIN_USER_TEMP_DIR");
            !darwin_runtime_dir.empty()) {
            default_runtime_dir = std::move(darwin_runtime_dir);
        } else if (std::string darwin_temp_dir = sys->EnvGet("TMPDIR"); !darwin_temp_dir.empty()) {
            default_runtime_dir = std::move(darwin_temp_dir);
        }
#endif
        sys->SetEnvironmentVariable(std::string(kXDG_RUNTIME_DIR_NAME), default_runtime_dir);
    } else {
#if defined(__linux__)
        // Bug: 454403989
        // when systme has XDG_RUNTIME_DIR set, we need to pass it
        // to ANDROID_EMULATOR_DISCOVERY_DIR; do nothing otherwise
        sys->EnvSet("ANDROID_EMULATOR_DISCOVERY_DIR", xdg_runtime_dir_val);
#endif
    }
#endif

    // Check that things exist so that we can error out early if necessary.
    auto emulator_paths = android::goldfish::ResolveEmulatorPaths(opts.verbose);
    if (!emulator_paths.ok()) {
        LOG(ERROR) << "Failed to resolve emulator paths: " << emulator_paths.status();
        return 1;
    }

    auto user_paths =
            android::goldfish::ResolveUserPaths(emulator_paths->launcher_directory, opts.verbose);
    if (!user_paths.ok()) {
        LOG(ERROR) << "Failed to resolve user paths: " << user_paths.status();
        return 1;
    }

    if (opts.list_avds) {
        ListAvds(opts, *user_paths, opts.verbose);
        return 0;
    }

    if (opts.snapshot_list) {
        ListSnapshots(opts, *user_paths, opts.verbose);
        return 0;
    }

    ShowBanner();

    if (opts.verbose) {
        LOG(INFO) << std::boolalpha << "Emulator launcher environment: "
                  << "continuous_integration=" << android::goldfish::IsRunningInCi() << " "
                  << "launched_by_android_cli=" << android::goldfish::IsAndroidCliDefined();
    }

    if (android::goldfish::ShouldLaunchFishtank(opts) && !emulator_paths->HasFishtank()) {
        LOG(ERROR) << "Fishtank (UI) is not available in the AOSP build. "
                      "Please use the '-no-window' flag to run in headless mode.";
        return 1;
    }

    // CrashSystem::get().initialize() reads AEMU_CRASHPAD_HANDLER from the
    // launcher's own environment to locate the crashpad_handler executable.
    android::base::System::SetEnvironmentVariable("AEMU_CRASHPAD_HANDLER",
                                                  emulator_paths->crashpad_handler_binary.string());
    if (auto status = android::crashreport::CrashSystem::get().initialize(); !status.ok()) {
        LOG(WARNING) << "Failed to initialize crashreporting: " << status.message();
    }

    absl::FailureSignalHandlerOptions options;
    // Call crashpad after printing stack trace.
    options.call_previous_handler = true;
    absl::InstallFailureSignalHandler(options);

    auto event_loop = goldfish::async::LibuvEventLoop::Create("LauncherLoop");

    auto reporter = std::make_unique<::goldfish::metrics::MetricsReporter>();
    auto metrics_writer_config = GetMetricsWriterConfig(opts, user_paths->user_directory);
    std::vector<std::string> crashed_metrics_sessions;
    if (metrics_writer_config.type == goldfish::metrics::MetricsWriterType::kStudio) {
        if (!::android::base::file::exists(metrics_writer_config.studio_spool_dir)) {
            if (auto s = ::android::base::file::mkdir_recursive(
                        metrics_writer_config.studio_spool_dir, 0755);
                !s.ok()) {
                LOG(ERROR)
                        << "Failed to create metrics spool directory, reporting will be disabled: "
                        << metrics_writer_config.studio_spool_dir << " - " << s;
                metrics_writer_config.type = goldfish::metrics::MetricsWriterType::kNone;
            }
        } else if (!::android::base::file::is_dir(metrics_writer_config.studio_spool_dir)) {
            LOG(ERROR) << "Metrics spool path is not a directory, reporting will be disabled: "
                       << metrics_writer_config.studio_spool_dir;
            metrics_writer_config.type = goldfish::metrics::MetricsWriterType::kNone;
        }
        crashed_metrics_sessions =
                ::goldfish::metrics::StudioFileMetricsWriter::FinalizeAbandonedSessionFiles(
                        metrics_writer_config.studio_spool_dir);
    }
    ::goldfish::metrics::ConfigureMetricsWriter(*reporter, metrics_writer_config, *event_loop);
    for (const auto& session_id : crashed_metrics_sessions) {
        LOG(WARNING) << "Reporting crashed metrics session: " << session_id;
        reporter->Report([&session_id](android_studio::AndroidStudioEvent& event) {
            event.set_studio_session_id(session_id);
            event.mutable_emulator_details()->set_crashes(
                    ::android::goldfish::kMetricsCrashesAbandoned);
        });
    }

    auto crash_consent = metrics_writer_config.user_upload_consent
                                 ? android::crashreport::Consent::ALWAYS
                                 : android::crashreport::Consent::NEVER;
    android::crashreport::CrashSystem::get().uploadEntries(crash_consent);

    // This is needed for gfxstream to be able to load GL libs.
    // TODO: consider moving this to gfxstream itself via the ANDROID_EMULATOR_LIBRARY_DIR env var.
    android::base::System::Get()->AddLibrarySearchDir(emulator_paths->library_directory.string());
    android::base::System::Get()->AddLibrarySearchDir(emulator_paths->lib64_directory.string());

    std::string avd_name;
    fs::path android_build_out;
    if (opts.avd) {
        avd_name = opts.avd;
    } else {
        // Root not used: auto android_build_root =
        // android::base::System::GetEnvironmentVariable("ANDROID_BUILD_TOP"); e.g.
        // <root>/out/target/product/emu64xa
        auto out = android::base::System::GetEnvironmentVariable("ANDROID_PRODUCT_OUT");
        if (!out.empty()) {
            avd_name = "<build>";
            android_build_out = out;
            if (!android::base::file::exists(android_build_out)) {
                LOG(ERROR) << "ANDROID_PRODUCT_OUT specified but does not exist: "
                           << android_build_out;
                return 1;
            }
            if (!android::base::file::is_dir(android_build_out)) {
                LOG(ERROR) << "ANDROID_PRODUCT_OUT is not a directory: " << android_build_out;
                return 1;
            }
        }
        // TODO also support -sysdir without -avd
    }
    if (avd_name.empty()) {
        LOG(ERROR) << "No AVD specified. Use '@foo' or '-avd foo' to launch a virtual device named "
                      "'foo'";
        return 1;
    }

    fs::path writable_content_override;
    if (opts.read_only) {
        writable_content_override = android::base::System::Get()->GetTempDir();
        VLOG(1) << "Content path overridden to: " << writable_content_override;
        android::base::file::mkdir_recursive(writable_content_override, 0755).IgnoreError();
    } else if (opts.datadir) {
        writable_content_override = fs::path(opts.datadir);
        if (!android::base::file::exists(writable_content_override)) {
            LOG(ERROR) << "-datadir specified does not exist: " << writable_content_override;
            return 1;
        }
        if (!android::base::file::is_dir(writable_content_override)) {
            LOG(ERROR) << "-datadir specified is not a directory: " << writable_content_override;
            return 1;
        }
        VLOG(1) << "Content path overridden to: " << writable_content_override;
    }

    absl::StatusOr<std::unique_ptr<android::goldfish::Avd>> avd;
    if (!android_build_out.empty()) {
        avd = android::goldfish::Avd::FromAndroidBuild(opts, *user_paths, avd_name,
                                                       android_build_out, opts.wipe_data,
                                                       writable_content_override);
    } else {
        avd = android::goldfish::Avd::FromName(opts, *user_paths, avd_name, opts.wipe_data,
                                               writable_content_override,
                                               opts.sysdir ? opts.sysdir : fs::path());
    }

    // Always error for missing AVDs, don't try to trampoline.
    if (absl::IsNotFound(avd.status())) {
        LOG(ERROR) << "Unknown AVD name [" << avd_name << "], use -list-avds to see valid list.";
        for (const auto line : absl::StrSplit(avd.status().message(), '\n')) {
            LOG(ERROR) << line;
        }
        return 1;
    }

    absl::StatusOr<android::goldfish::FeatureFlags> feature_flags;
    if (avd.ok()) {
        std::vector<std::string> cli_features;
        for (const ParamList* f = opts.feature; f != nullptr; f = f->next) {
            if (f->param != nullptr) {
                cli_features.emplace_back(f->param);
            }
        }
        feature_flags = android::goldfish::FeatureFlags::Create({
            .host_ini_path = emulator_paths->advanced_features,
            .guest_ini_path = (*avd)->GetSystemImagePaths().advanced_features,
            .user_ini_path = user_paths->user_directory / "advancedFeatures.ini",
            .guest_api_level = (*avd)->ApiLevel(),
            .avd_feature_flags = (*avd)->Hw().hw_featureflags,
            .env_feature_flags = android::base::System::Get()->EnvGet("ANDROID_EMULATOR_FEATURES"),
            .cli_feature_flags = cli_features,
        });
    }

    if (android::goldfish::ShouldTrampolineToQemu2(avd, feature_flags)) {
        auto s = android::goldfish::TrampolineToQemu2(emulator_paths->launcher_directory,
                                                      std::move(args_copy));
        LOG(ERROR) << s;
        return 1;
    }

    // We currently expect AVD load failures to trigger trampoline and not reach this point.
    if (!avd.ok()) {
        LOG(ERROR) << "Bug: AVD loading failed: " << avd.status();
        return 1;
    }
    if (!feature_flags.ok()) {
        LOG(ERROR) << "Failed to load feature flags: " << feature_flags.status();
        return 1;
    }

    // Runs a one-shot update check on a background thread (throttled via
    // emu-update-last-check.ini) and logs a notice if a newer version is available.
    // If still in flight when main() exits, ScopedUpdateCheck cancels and joins the thread.
    goldfish::update_check::ScopedUpdateCheck update_check;
    if (android_build_out.empty()) {
        update_check = goldfish::update_check::StartAsyncUpdateCheck({
            .user_directory = user_paths->user_directory,
            .base_url = std::string(kUpdateRepositoryUrl),
            .package_prefix = std::string(kEmulatorPreviewPackagePrefix),
            .package_display_name = std::string(kEmulatorPreviewDisplayName),
            .emulator_version = std::string(goldfish::version::GetEmulatorVersion()),
            .build_id = std::string(goldfish::version::GetEmulatorBuildId()),
            .platform = std::string(goldfish::version::GetEmulatorPlatform()),
            .target_cpu = std::string(goldfish::version::GetEmulatorTargetCpu()),
            .core_version = QEMU_FULL_VERSION,
            .user_id = metrics_writer_config.user_id,
            .channel = ResolveUpdateCheckChannel(goldfish::metrics::studio::GetUpdateChannel()),
            .interval = kUpdateCheckInterval,
            .http_timeout = kUpdateCheckHttpTimeout,
        });
    }

    bool set_qemu_version = true;
    auto last_run_qemu_version = (*avd)->GetLastRunQemuVersion();
    if (!last_run_qemu_version.ok()) {
        LOG(ERROR) << "Error reading last used QEMU version for AVD " << avd_name << " due to "
                   << last_run_qemu_version.status().message();
    } else if (std::optional<int> version = last_run_qemu_version.value()) {
        if (version.value() != EMULATOR_COMPATIBLE_QEMU_VERSION) {
            LOG(ERROR) << "AVD " << avd_name
                       << " is not compatible with this emulator. Last run QEMU version: "
                       << version.value()
                       << ", compatible QEMU version: " << EMULATOR_COMPATIBLE_QEMU_VERSION
                       << ". Use -wipe-data option to reset the AVD data and use this emulator.";
            return 1;
        } else {
            VLOG(1) << "AVD last run QEMU version is compatible.";
            set_qemu_version = false;
        }
    }

    if (set_qemu_version) {
        LOG(INFO) << "Setting AVD last run compatible QEMU version to "
                  << EMULATOR_COMPATIBLE_QEMU_VERSION;
        auto s = (*avd)->SetLastRunQemuVersion(EMULATOR_COMPATIBLE_QEMU_VERSION);
        if (!s.ok()) {
            LOG(ERROR) << "Could not save last run QEMU version, error: " << s;
        }
    }

    LOG(INFO) << "Launching AVD: " << (*avd)->Details(opts.verbose);
    auto exit_status = android::goldfish::RunLauncher({
        .event_loop = *event_loop,
        .process_launcher = std::make_unique<::goldfish::async::UvProcessLauncher>(*event_loop),
        .signal_handlers = std::make_unique<::goldfish::async::UvSignalHandlers>(*event_loop),
        .metrics_reporter = std::move(reporter),
        .socket_factory = std::make_unique<::goldfish::async::LibuvAsyncSocketFactory>(),
        .user_paths = *std::move(user_paths),
        .emulator_paths = *std::move(emulator_paths),
        .avd = *std::move(avd),
        .opts = std::move(opts),
        .feature_flags = *std::move(feature_flags),
        .metrics_writer_config = std::move(metrics_writer_config),
    });
    if (!exit_status.ok()) {
        LOG(ERROR) << exit_status.status();
        return 1;
    }
    return *exit_status;
}
