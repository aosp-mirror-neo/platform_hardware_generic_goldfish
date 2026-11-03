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
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "android/cpu/cpu_accelerator.h"
#include "android/goldfish/avd.h"
#include "android/goldfish/constants.h"
#include "android/goldfish/input_paths.h"
#include "goldfish/async/async_socket_server.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/libuv_socket_factory.h"
#include "goldfish/async/process_launcher.h"
#include "goldfish/async/signal_handlers.h"
#include "goldfish/async/testing/fake_async_socket.h"
#include "goldfish/file/file.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/metrics_writer.h"
#include "goldfish/metrics/studio_file_metrics_writer.h"
#include "goldfish/network/endpoint.h"
#include "launch_qemu/launch_qemu.h"
#include "mock_avd.h"
#include "uv.h"

namespace android::goldfish {
namespace {

using testing::_;
using testing::Return;

MATCHER_P(IsPort, port, "") {
    return ExplainMatchResult(port, ::goldfish::network::GetPortFromEndpoint(arg), result_listener);
}

MATCHER(IsIpv4Endpoint, "") {
    return std::holds_alternative<::goldfish::network::Ipv4Endpoint>(arg);
}

class MockAsyncSocketServer : public ::goldfish::async::AsyncSocketServer {
  public:
    MOCK_METHOD(void, Close, (), (override));
    MOCK_METHOD(::goldfish::network::Endpoint, GetEndpoint, (), (const, override));
    MOCK_METHOD(::goldfish::async::EventLoop*, GetLoop, (), (const, override));
};

class MockAsyncSocketFactory : public ::goldfish::async::AsyncSocketFactory {
  public:
    MOCK_METHOD(std::shared_ptr<::goldfish::async::AsyncSocket>, CreateSocket,
                (::goldfish::async::EventLoop * loop,
                 const ::goldfish::network::Endpoint& endpoint),
                (override));
    MOCK_METHOD(std::shared_ptr<::goldfish::async::AsyncSocketServer>, CreateServer,
                (::goldfish::async::EventLoop * loop, const ::goldfish::network::Endpoint& endpoint,
                 ::goldfish::async::AsyncSocketServer::ConnectCallback connect_callback,
                 ::goldfish::async::AsyncSocketServer::LoopProvider loop_provider),
                (override));
};

class MockManagedProcess : public ::goldfish::async::ManagedProcess {
  public:
    MOCK_METHOD(int, GetPid, (), (const, override));
    MOCK_METHOD(void, Kill, (int signum), (override));
};

class MockProcessLauncher : public ::goldfish::async::ProcessLauncher {
  public:
    MOCK_METHOD(absl::StatusOr<std::unique_ptr<::goldfish::async::ManagedProcess>>, Launch,
                (const ::goldfish::async::LaunchConfig& config, ExitCallback exit_cb), (override));
    MOCK_METHOD(void, ForgetProcess, (const ::goldfish::async::ManagedProcess& process),
                (override));
};

class MockSignalHandlers : public ::goldfish::async::SignalHandlers {
  public:
    MOCK_METHOD(void, SetCallback, (Callback signal_cb), (override));
    MOCK_METHOD(void, close, (), (override));
};

class MockMetricsWriter : public ::goldfish::metrics::MetricsWriter {
  public:
    explicit MockMetricsWriter(std::atomic<bool>* destroyed_flag = nullptr)
            : destroyed_flag_(destroyed_flag) {}
    ~MockMetricsWriter() override {
        if (destroyed_flag_) {
            destroyed_flag_->store(true, std::memory_order_release);
        }
    }
    MOCK_METHOD(void, Write, (::goldfish::metrics::MetricsEvent event), (override));

  private:
    std::atomic<bool>* destroyed_flag_;
};

class LauncherTest : public testing::Test {
  protected:
    void SetUp() override {
        temp_dir = fs::path(testing::TempDir()) / "launcher_test";
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir / "bin");
        fs::create_directories(temp_dir / "lib");
        fs::create_directories(temp_dir / "lib64");
        fs::create_directories(temp_dir / "share" / "qemu");

        emulator_paths.launcher_binary = temp_dir / "emulator";
        emulator_paths.launcher_directory = temp_dir;
        emulator_paths.binary_directory = temp_dir / "bin";
        emulator_paths.library_directory = temp_dir / "lib";
        emulator_paths.lib64_directory = temp_dir / "lib64";
        emulator_paths.bios_directory = temp_dir / "share" / "qemu";
        // No need to have .exe for Windows on these binaries as they aren't executed.
        emulator_paths.qemu_system_x86_binary = temp_dir / "bin" / "qemu-system-x86_64";
        emulator_paths.qemu_system_arm_binary = temp_dir / "bin" / "qemu-system-aarch64";
        emulator_paths.netsim_binary = temp_dir / "bin" / "netsimd";
        emulator_paths.crashpad_handler_binary = temp_dir / "bin" / "crashpad_handler";
        emulator_paths.fishtank_binary = temp_dir / "fishtank" / "fishtank";
        emulator_paths.advanced_features = temp_dir / "lib" / "advancedFeatures.ini";

        user_paths.user_directory = temp_dir / "user";
        user_paths.avd_directory = temp_dir / "avd";
        user_paths.sdk_directory = temp_dir / "sdk";
        user_paths.discovery_directory = temp_dir / "discovery";

        std::memset(&opts, 0, sizeof(opts));
        opts.no_window = true;
        opts.no_netsim = true;
        opts.avd = "test_avd";

        using S = android_studio::EmulatorFeatureFlagState;
        feature_flags = FeatureFlags::CreateForTesting({
            S::MINIGBM,
            S::GUEST_ANGLE,
            S::DYNAMIC_PARTITION,
            S::MAC80211HWSIM_USERSPACE_MANAGED,
            S::VULKAN_SNAPSHOTS,
            S::VULKAN_VIRTUAL_QUEUE,
            S::VULKAN_USE_DEDICATED_AHB_MEMORY_TYPE,
            S::NETSIMCLIUI,
            S::NETSIMWEBUI,
        });
#ifdef _WIN32
        // Windows VMs don't support nested virt, mock WHPX availability.
        SetCurrentCpuAcceleratorForTesting(CpuAccelerator::CPU_ACCELERATOR_WHPX,
                                           AndroidCpuAcceleration::ANDROID_CPU_ACCELERATION_READY,
                                           "");
#endif

        owned_launcher = std::make_unique<MockProcessLauncher>();
        mock_launcher = owned_launcher.get();
        owned_signal_handlers = std::make_unique<MockSignalHandlers>();
        mock_signal_handlers = owned_signal_handlers.get();
        owned_socket_factory = std::make_unique<MockAsyncSocketFactory>();
        mock_socket_factory = owned_socket_factory.get();

        EXPECT_CALL(*mock_socket_factory, CreateServer(_, _, _, _))
                .WillRepeatedly([](::goldfish::async::EventLoop* loop,
                                   const ::goldfish::network::Endpoint& endpoint,
                                   ::goldfish::async::AsyncSocketServer::ConnectCallback,
                                   ::goldfish::async::AsyncSocketServer::LoopProvider) {
                    auto server = std::make_shared<MockAsyncSocketServer>();
                    ON_CALL(*server, GetEndpoint()).WillByDefault(Return(endpoint));
                    return server;
                });

        event_loop = ::goldfish::async::LibuvEventLoop::Create("MainLoop");

        ON_CALL(*mock_launcher, Launch(_, _))
                .WillByDefault([&](const ::goldfish::async::LaunchConfig&,
                                   ::goldfish::async::ProcessLauncher::ExitCallback exit_cb) {
                    auto process = std::make_unique<MockManagedProcess>();
                    ON_CALL(*process, GetPid()).WillByDefault(Return(1234));
                    return absl::StatusOr<std::unique_ptr<::goldfish::async::ManagedProcess>>(
                            std::move(process));
                });
    }

    void TearDown() override { fs::remove_all(temp_dir); }

    absl::StatusOr<int> RunLauncherStatus(
            std::unique_ptr<MockAvd> avd,
            std::unique_ptr<::goldfish::metrics::MetricsReporter> reporter = nullptr) {
        if (!reporter) {
            reporter = std::make_unique<::goldfish::metrics::MetricsReporter>();
        }
        return android::goldfish::RunLauncher({
            .event_loop = *event_loop,
            .process_launcher = std::move(owned_launcher),
            .signal_handlers = std::move(owned_signal_handlers),
            .metrics_reporter = std::move(reporter),
            .socket_factory = std::move(owned_socket_factory),
            .user_paths = std::move(user_paths),
            .emulator_paths = std::move(emulator_paths),
            .avd = std::move(avd),
            .opts = std::move(opts),
            .feature_flags = std::move(feature_flags),
            .metrics_writer_config = metrics_writer_config,
            .qemu_shutdown_timeout = qemu_shutdown_timeout,
        });
    }

    int RunLauncher(std::unique_ptr<MockAvd> avd,
                    std::unique_ptr<::goldfish::metrics::MetricsReporter> reporter = nullptr) {
        auto status = RunLauncherStatus(std::move(avd), std::move(reporter));
        EXPECT_TRUE(status.ok()) << status.status();
        return status.value_or(1);
    }

    std::thread TriggerEmulatorScript(absl::Notification& launched, std::function<void()> script,
                                      absl::Notification* wait_for_extra = nullptr) {
        return std::thread([&, script = std::move(script), wait_for_extra]() {
            launched.WaitForNotification();
            if (wait_for_extra) {
                wait_for_extra->WaitForNotification();
            }
            // Small sleep to ensure launcher has actually finished setup.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            script();
        });
    }

    std::thread TriggerEmulatorExit(absl::Notification& launched,
                                    ::goldfish::async::ProcessLauncher::ExitCallback& exit_cb,
                                    int exit_code = 0,
                                    absl::Notification* wait_for_extra = nullptr) {
        return TriggerEmulatorScript(
                launched,
                [this, &exit_cb, exit_code]() {
                    event_loop
                            ->Post([exit_cb, exit_code]() {
                                if (exit_cb) exit_cb(exit_code, 0);
                            })
                            .IgnoreError();
                },
                wait_for_extra);
    }

    std::unique_ptr<MockAvd> CreateMockAvd() {
        auto avd = std::make_unique<testing::NiceMock<MockAvd>>();
        fs::path avd_dir = temp_dir / "avd_content";
        fs::create_directories(avd_dir);
        EXPECT_CALL(*avd, Name()).WillRepeatedly(Return("test_avd"));
        EXPECT_CALL(*avd, GetContentPath()).WillRepeatedly(Return(avd_dir));
        EXPECT_CALL(*avd, Details(_)).WillRepeatedly(Return("test_details"));
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
        EXPECT_CALL(*avd, Arch()).WillRepeatedly(Return(Avd::CpuArchitecture::kArm));
        EXPECT_CALL(*avd, Abi()).WillRepeatedly(Return("arm64-v8a"));
#else
        EXPECT_CALL(*avd, Arch()).WillRepeatedly(Return(Avd::CpuArchitecture::kX86));
        EXPECT_CALL(*avd, Abi()).WillRepeatedly(Return("x86_64"));
#endif
        EXPECT_CALL(*avd, GetDeviceType()).WillRepeatedly(Return(DeviceType::kPhone));
        EXPECT_CALL(*avd, GetLastRunQemuVersion()).WillRepeatedly(Return(std::optional<int>(10)));
        EXPECT_CALL(*avd, Hw()).WillRepeatedly(testing::ReturnRef(hw));
        EXPECT_CALL(*avd, ApiLevel()).WillRepeatedly(testing::Return(kVirtioReadyApiLevel));

        system_image_paths.system_image = temp_dir / "system.img";
        system_image_paths.vendor_image = temp_dir / "vendor.img";
        system_image_paths.ramdisk_image = temp_dir / "ramdisk.img";
        system_image_paths.data_dir = temp_dir / "data";
        system_image_paths.kernel_image = temp_dir / "kernel-ranchu";
        system_image_paths.kernel_cmdline = temp_dir / "kernel_cmdline.txt";
        system_image_paths.build_properties = temp_dir / "build.prop";
        system_image_paths.advanced_features = temp_dir / "advancedFeatures.ini";
        system_image_paths.verified_boot_params = temp_dir / "VerifiedBootParams.textproto";
        system_image_paths.encryption_key_image = temp_dir / "encryptionkey.img";

        // Create dummy images
        std::ofstream(system_image_paths.build_properties).close();
        std::ofstream(system_image_paths.advanced_features).close();
        std::ofstream(system_image_paths.verified_boot_params).close();
        std::ofstream(system_image_paths.kernel_cmdline).close();
        std::ofstream(system_image_paths.kernel_image).close();
        std::ofstream(system_image_paths.ramdisk_image).close();
        std::ofstream(system_image_paths.encryption_key_image).close();
        std::ofstream(system_image_paths.system_image).close();
        std::ofstream(system_image_paths.vendor_image).close();
        fs::create_directories(system_image_paths.data_dir);
        std::ofstream(system_image_paths.data_dir / "empty_data_disk").close();

        EXPECT_CALL(*avd, GetSystemImagePaths())
                .WillRepeatedly(testing::ReturnRef(system_image_paths));
        return avd;
    }

    fs::path ExpectedQemuBinary() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
        return emulator_paths.qemu_system_arm_binary;
#else
        return emulator_paths.qemu_system_x86_binary;
#endif
    }

    void ExpectEmulatorLaunch(
            absl::Notification& launched,
            ::goldfish::async::ProcessLauncher::ExitCallback& exit_cb_out,
            MockManagedProcess** process_out = nullptr,
            std::optional<testing::Matcher<const ::goldfish::async::LaunchConfig&>> config_matcher =
                    std::nullopt) {
        auto matcher = config_matcher.has_value()
                               ? *config_matcher
                               : testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                                ExpectedQemuBinary());
        EXPECT_CALL(*mock_launcher, Launch(matcher, _))
                .WillOnce([&, process_out](
                                  const ::goldfish::async::LaunchConfig&,
                                  ::goldfish::async::ProcessLauncher::ExitCallback exit_cb) {
                    exit_cb_out = std::move(exit_cb);
                    auto process = std::make_unique<MockManagedProcess>();
                    EXPECT_CALL(*process, GetPid()).WillRepeatedly(Return(1234));
                    if (process_out) {
                        *process_out = process.get();
                    }
                    launched.Notify();
                    return absl::StatusOr<std::unique_ptr<::goldfish::async::ManagedProcess>>(
                            std::move(process));
                });
    }

    fs::path temp_dir;
    UserPaths user_paths;
    EmulatorPaths emulator_paths;
    SystemImagePaths system_image_paths;
    AndroidOptions opts;
    FeatureFlags feature_flags;
    HardwareConfig hw;
    ::goldfish::metrics::MetricsWriterConfig metrics_writer_config;
    absl::Duration qemu_shutdown_timeout = absl::Seconds(60);
    std::unique_ptr<::goldfish::async::LibuvEventLoop> event_loop;
    std::unique_ptr<MockProcessLauncher> owned_launcher;
    MockProcessLauncher* mock_launcher;
    std::unique_ptr<MockSignalHandlers> owned_signal_handlers;
    MockSignalHandlers* mock_signal_handlers;
    std::unique_ptr<MockAsyncSocketFactory> owned_socket_factory;
    MockAsyncSocketFactory* mock_socket_factory;
};

TEST_F(LauncherTest, CanLaunchEmulator) {
    auto avd = CreateMockAvd();
    feature_flags.SetEnabledOverride(android_studio::EmulatorFeatureFlagState::FORCE_SWIFTSHADER,
                                     true);

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    absl::Notification launched;

    ExpectEmulatorLaunch(
            launched, emulator_exit_cb, nullptr,
            testing::AllOf(
                    testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                   ExpectedQemuBinary()),
                    testing::Field(
                            &::goldfish::async::LaunchConfig::environment,
                            testing::AllOf(testing::Contains(testing::Pair("AEMU_LOG_LEVEL", "1")),
                                           testing::Not(testing::Contains(
                                                   testing::Key("AEMU_VLOG_LEVEL")))))));

    EXPECT_CALL(*mock_signal_handlers, SetCallback(_)).Times(1);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    auto reporter = std::make_unique<::goldfish::metrics::MetricsReporter>();
    auto mock_writer = std::make_unique<MockMetricsWriter>();
    bool saw_vulkan_icd_metric = false;
    bool saw_clean_exit_crashes_metric = false;
    bool saw_feature_flag_metric = false;
    EXPECT_CALL(*mock_writer, Write(_))
            .Times(testing::AtLeast(1))
            .WillRepeatedly([&](::goldfish::metrics::MetricsEvent event) {
                if (event.as_event.emulator_details().vulkan_icd() ==
                    android_studio::EmulatorDetails::LAVAPIPE_VK) {
                    saw_vulkan_icd_metric = true;
                }
                if (event.as_event.emulator_details().has_crashes() &&
                    event.as_event.emulator_details().crashes() == kMetricsCrashesNone) {
                    saw_clean_exit_crashes_metric = true;
                }
                const auto& ff_state = event.as_event.emulator_details().feature_flag_state();
                if (ff_state.user_overridden_enabled_features_size() == 1 &&
                    ff_state.user_overridden_enabled_features(0) ==
                            android_studio::EmulatorFeatureFlagState::FORCE_SWIFTSHADER) {
                    saw_feature_flag_metric = true;
                }
            });
    reporter->SetWriter(std::move(mock_writer));

    int status = RunLauncher(std::move(avd), std::move(reporter));

    trigger.join();
    EXPECT_EQ(status, 0);
    EXPECT_TRUE(saw_vulkan_icd_metric);
    EXPECT_TRUE(saw_clean_exit_crashes_metric);
    EXPECT_TRUE(saw_feature_flag_metric);
}

TEST_F(LauncherTest, ForwardsSignalToEmulator) {
    auto avd = CreateMockAvd();

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ::goldfish::async::SignalHandlers::Callback launcher_signal_cb;
    absl::Notification launched;
    absl::Notification callback_set;

    // We need to capture the process object to verify Kill()
    MockManagedProcess* mock_process_ptr = nullptr;
    ExpectEmulatorLaunch(launched, emulator_exit_cb, &mock_process_ptr);

    EXPECT_CALL(*mock_signal_handlers, SetCallback(_))
            .WillOnce([&](::goldfish::async::SignalHandlers::Callback signal_cb) {
                launcher_signal_cb = std::move(signal_cb);
                callback_set.Notify();
            });

    std::thread trigger = TriggerEmulatorScript(
            launched,
            [&]() {
                // Verify Kill is called on the process
                EXPECT_CALL(*mock_process_ptr, Kill(SIGINT)).Times(1);
                event_loop->Post([&]() { launcher_signal_cb(SIGINT); }).IgnoreError();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                event_loop->Post([&]() { emulator_exit_cb(0, 0); }).IgnoreError();
            },
            &callback_set);

    auto reporter = std::make_unique<::goldfish::metrics::MetricsReporter>();
    auto mock_writer = std::make_unique<MockMetricsWriter>();
    EXPECT_CALL(*mock_writer, Write(_)).Times(testing::AtLeast(1));
    reporter->SetWriter(std::move(mock_writer));

    RunLauncher(std::move(avd), std::move(reporter));

    trigger.join();
}

TEST_F(LauncherTest, LaunchesFishtankWhenWindowIsEnabled) {
    auto avd = CreateMockAvd();
    opts.no_window = false;

    absl::Notification fishtank_launched;
    ::goldfish::async::ProcessLauncher::ExitCallback fishtank_exit_cb;
    absl::Notification emulator_launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    EXPECT_CALL(*mock_launcher, Launch(testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                                      emulator_paths.fishtank_binary),
                                       _))
            .WillOnce([&](const ::goldfish::async::LaunchConfig&,
                          ::goldfish::async::ProcessLauncher::ExitCallback exit_cb) {
                fishtank_exit_cb = std::move(exit_cb);
                auto process = std::make_unique<MockManagedProcess>();
                MockManagedProcess* process_ptr = process.get();
                EXPECT_CALL(*process_ptr, GetPid()).WillRepeatedly(Return(5678));
                EXPECT_CALL(*process_ptr, Kill(SIGTERM)).Times(testing::AtMost(1));
                fishtank_launched.Notify();
                return absl::StatusOr<std::unique_ptr<::goldfish::async::ManagedProcess>>(
                        std::move(process));
            });

    ExpectEmulatorLaunch(emulator_launched, emulator_exit_cb);

    std::thread trigger = TriggerEmulatorScript(
            emulator_launched,
            [&]() {
                event_loop
                        ->Post([&]() {
                            if (fishtank_exit_cb) fishtank_exit_cb(0, 0);
                            emulator_exit_cb(0, 0);
                        })
                        .IgnoreError();
            },
            &fishtank_launched);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, DoesNotLaunchFishtankWhenWindowIsDisabled) {
    auto avd = CreateMockAvd();
    opts.no_window = true;

    // Verify fishtank is NOT launched.
    EXPECT_CALL(*mock_launcher, Launch(testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                                      emulator_paths.fishtank_binary),
                                       _))
            .Times(0);

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    absl::Notification launched;
    ExpectEmulatorLaunch(launched, emulator_exit_cb);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));
    trigger.join();
}

TEST_F(LauncherTest, HandlesEmulatorExitFailure) {
    auto avd = CreateMockAvd();

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    absl::Notification launched;
    ExpectEmulatorLaunch(launched, emulator_exit_cb);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb, 1);

    auto reporter = std::make_unique<::goldfish::metrics::MetricsReporter>();
    auto mock_writer = std::make_unique<MockMetricsWriter>();
    bool saw_crashes_metric = false;
    EXPECT_CALL(*mock_writer, Write(_))
            .Times(testing::AtLeast(1))
            .WillRepeatedly([&](::goldfish::metrics::MetricsEvent event) {
                if (event.as_event.emulator_details().has_crashes()) {
                    saw_crashes_metric = true;
                }
            });
    reporter->SetWriter(std::move(mock_writer));

    int status = RunLauncher(std::move(avd), std::move(reporter));

    trigger.join();
    EXPECT_EQ(status, 1);
    EXPECT_FALSE(saw_crashes_metric);
}

TEST_F(LauncherTest, ForwardsMultipleSignalsToEmulator) {
    auto avd = CreateMockAvd();

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ::goldfish::async::SignalHandlers::Callback launcher_signal_cb;
    absl::Notification launched;
    absl::Notification callback_set;
    MockManagedProcess* mock_process_ptr = nullptr;

    ExpectEmulatorLaunch(launched, emulator_exit_cb, &mock_process_ptr);

    EXPECT_CALL(*mock_signal_handlers, SetCallback(_))
            .WillOnce([&](::goldfish::async::SignalHandlers::Callback signal_cb) {
                launcher_signal_cb = std::move(signal_cb);
                callback_set.Notify();
            });

    std::thread trigger = TriggerEmulatorScript(
            launched,
            [&]() {
                testing::InSequence seq;
                EXPECT_CALL(*mock_process_ptr, Kill(SIGINT)).Times(1);
                EXPECT_CALL(*mock_process_ptr, Kill(SIGKILL)).Times(2);

                event_loop->Post([&]() { launcher_signal_cb(SIGINT); }).IgnoreError();
                event_loop->Post([&]() { launcher_signal_cb(SIGTERM); }).IgnoreError();
                event_loop->Post([&]() { launcher_signal_cb(SIGHUP); }).IgnoreError();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                event_loop->Post([&]() { emulator_exit_cb(0, 0); }).IgnoreError();
            },
            &callback_set);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, EscalatesToSigkillAfterTimeout) {
    auto avd = CreateMockAvd();
    qemu_shutdown_timeout = absl::Milliseconds(50);

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ::goldfish::async::SignalHandlers::Callback launcher_signal_cb;
    absl::Notification launched;
    absl::Notification callback_set;
    absl::Notification sigkill_sent;
    MockManagedProcess* mock_process_ptr = nullptr;

    ExpectEmulatorLaunch(launched, emulator_exit_cb, &mock_process_ptr);

    EXPECT_CALL(*mock_signal_handlers, SetCallback(_))
            .WillOnce([&](::goldfish::async::SignalHandlers::Callback signal_cb) {
                launcher_signal_cb = std::move(signal_cb);
                callback_set.Notify();
            });

    std::thread trigger = TriggerEmulatorScript(
            launched,
            [&]() {
                testing::InSequence seq;
                EXPECT_CALL(*mock_process_ptr, Kill(SIGTERM)).Times(1);
                EXPECT_CALL(*mock_process_ptr, Kill(SIGKILL)).WillOnce([&]() {
                    sigkill_sent.Notify();
                });

                event_loop->Post([&]() { launcher_signal_cb(SIGTERM); }).IgnoreError();
                sigkill_sent.WaitForNotification();
                event_loop->Post([&]() { emulator_exit_cb(0, SIGKILL); }).IgnoreError();
            },
            &callback_set);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, HandlesPortOption) {
    auto avd = CreateMockAvd();
    opts.port = "5562";

    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5562), _, _))
            .Times(2)
            .WillRepeatedly(Return(std::make_shared<MockAsyncSocketServer>()));

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ExpectEmulatorLaunch(launched, emulator_exit_cb);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));
    trigger.join();
}

TEST_F(LauncherTest, PortOptionStartsWithOnlyIpv6Loopback) {
    auto avd = CreateMockAvd();
    opts.port = "5562";

    // An IPv6-only host: the IPv4 console fails, IPv6 binds, and the emulator
    // still launches because -port skips the free port search.
    EXPECT_CALL(*mock_socket_factory,
                CreateServer(_, testing::AllOf(IsPort(5562), IsIpv4Endpoint()), _, _))
            .WillOnce(Return(nullptr));
    EXPECT_CALL(*mock_socket_factory,
                CreateServer(_, testing::AllOf(IsPort(5562), testing::Not(IsIpv4Endpoint())), _, _))
            .WillOnce(Return(std::make_shared<MockAsyncSocketServer>()));

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ExpectEmulatorLaunch(launched, emulator_exit_cb);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));
    trigger.join();
}

TEST_F(LauncherTest, HandlesValidPortsOption) {
    auto avd = CreateMockAvd();
    opts.ports = "5560,5561";

    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5560), _, _))
            .Times(2)
            .WillRepeatedly(Return(std::make_shared<MockAsyncSocketServer>()));

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    auto config_matcher = testing::AllOf(
            testing::Field(&::goldfish::async::LaunchConfig::exe_path, ExpectedQemuBinary()),
            testing::Field(
                    &::goldfish::async::LaunchConfig::args,
                    testing::AllOf(testing::Contains(testing::HasSubstr("avdstart")),
                                   testing::Contains(testing::HasSubstr("serial_number=5560")),
                                   testing::Contains(testing::HasSubstr("adb_port=5561")))));

    ExpectEmulatorLaunch(launched, emulator_exit_cb, nullptr, config_matcher);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, HuntsForFreePort) {
    auto avd = CreateMockAvd();

    // Mock port 5554 and 5556 as busy, 5558 as free.
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5554), _, _))
            .WillOnce(Return(nullptr));
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5556), _, _))
            .WillOnce(Return(nullptr));
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5558), _, _))
            .Times(2)
            .WillRepeatedly(Return(std::make_shared<MockAsyncSocketServer>()));

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    auto config_matcher = testing::AllOf(
            testing::Field(&::goldfish::async::LaunchConfig::exe_path, ExpectedQemuBinary()),
            testing::Field(
                    &::goldfish::async::LaunchConfig::args,
                    testing::AllOf(testing::Contains(testing::HasSubstr("avdstart")),
                                   testing::Contains(testing::HasSubstr("serial_number=5558")))));

    ExpectEmulatorLaunch(launched, emulator_exit_cb, nullptr, config_matcher);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, RespectsNoSnapshotUpdateTimeFlag) {
    opts.no_snapshot_update_time = true;
    auto avd = CreateMockAvd();

    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5554), _, _))
            .Times(2)
            .WillRepeatedly(Return(std::make_shared<MockAsyncSocketServer>()));

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    auto config_matcher = testing::AllOf(
            testing::Field(&::goldfish::async::LaunchConfig::exe_path, ExpectedQemuBinary()),
            testing::Field(&::goldfish::async::LaunchConfig::args,
                           testing::AllOf(testing::Contains(testing::HasSubstr("avdstart")),
                                          testing::Contains(testing::HasSubstr(
                                                  "snapshot_update_time=false")))));

    ExpectEmulatorLaunch(launched, emulator_exit_cb, nullptr, config_matcher);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, EscapesCommasInLogcatOutputAndAvdName) {
    char logcat_output[] = "/tmp/log,cat.txt";
    opts.logcat_output = logcat_output;
    auto avd = CreateMockAvd();
    EXPECT_CALL(*avd, Name()).WillRepeatedly(Return("test,avd"));

    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5554), _, _))
            .Times(2)
            .WillRepeatedly(Return(std::make_shared<MockAsyncSocketServer>()));

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    auto config_matcher = testing::AllOf(
            testing::Field(&::goldfish::async::LaunchConfig::exe_path, ExpectedQemuBinary()),
            testing::Field(
                    &::goldfish::async::LaunchConfig::args,
                    testing::AllOf(testing::Contains("test,,avd,debug-threads=on"),
                                   testing::Contains("file,id=forhvc1,path=/tmp/log,,cat.txt"))));

    ExpectEmulatorLaunch(launched, emulator_exit_cb, nullptr, config_matcher);

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    RunLauncher(std::move(avd));

    trigger.join();
}

TEST_F(LauncherTest, ShutsDownDirectlyWhenNoEmulatorProcess) {
    auto avd = CreateMockAvd();

    ::goldfish::async::SignalHandlers::Callback launcher_signal_cb;
    absl::Notification callback_set;
    EXPECT_CALL(*mock_signal_handlers, SetCallback(_))
            .WillOnce([&](::goldfish::async::SignalHandlers::Callback signal_cb) {
                launcher_signal_cb = std::move(signal_cb);
                callback_set.Notify();
            });

    EXPECT_CALL(*mock_signal_handlers, close()).Times(1);

    // Ensure emulator is NEVER launched.
    EXPECT_CALL(*mock_launcher, Launch(_, _)).Times(0);

    absl::Notification port_hunt_blocked;
    absl::Notification port_hunt_can_continue;
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5554), _, _))
            .WillOnce([&](auto, auto, auto, auto) {
                port_hunt_blocked.Notify();
                port_hunt_can_continue.WaitForNotification();
                return std::make_shared<MockAsyncSocketServer>();
            })
            .WillOnce(Return(std::make_shared<MockAsyncSocketServer>()));

    std::thread trigger = TriggerEmulatorScript(callback_set, [&]() {
        port_hunt_blocked.WaitForNotification();
        // Signal tasks are posted while port hunt is blocked.
        // They will be queued on the event loop and run after the current init task.
        event_loop->Post([&]() { launcher_signal_cb(SIGINT); }).IgnoreError();
        event_loop->Post([&]() { launcher_signal_cb(SIGTERM); }).IgnoreError();
        // Now let port hunt continue.
        port_hunt_can_continue.Notify();
    });

    int status = RunLauncher(std::move(avd));
    trigger.join();
    EXPECT_EQ(status, 0);
}

TEST_F(LauncherTest, NonSignalledEmulatorExitKillsFishtankAndIgnoresSubsequentSignals) {
    auto avd = CreateMockAvd();
    opts.no_window = false;

    absl::Notification fishtank_launched;
    absl::Notification emulator_launched;
    absl::Notification callback_set;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ::goldfish::async::SignalHandlers::Callback launcher_signal_cb;
    MockManagedProcess* mock_emulator_ptr = nullptr;

    EXPECT_CALL(*mock_signal_handlers, SetCallback(_))
            .WillOnce([&](::goldfish::async::SignalHandlers::Callback signal_cb) {
                launcher_signal_cb = std::move(signal_cb);
                callback_set.Notify();
            });
    EXPECT_CALL(*mock_signal_handlers, close()).Times(1);

    EXPECT_CALL(*mock_launcher, Launch(testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                                      emulator_paths.fishtank_binary),
                                       _))
            .WillOnce([&](const ::goldfish::async::LaunchConfig&,
                          ::goldfish::async::ProcessLauncher::ExitCallback) {
                auto process = std::make_unique<MockManagedProcess>();
                MockManagedProcess* process_ptr = process.get();
                EXPECT_CALL(*process_ptr, GetPid()).WillRepeatedly(Return(5678));
                EXPECT_CALL(*process_ptr, Kill(SIGTERM)).Times(1);
                fishtank_launched.Notify();
                return absl::StatusOr<std::unique_ptr<::goldfish::async::ManagedProcess>>(
                        std::move(process));
            });

    ExpectEmulatorLaunch(emulator_launched, emulator_exit_cb, &mock_emulator_ptr);

    std::thread trigger = TriggerEmulatorScript(
            emulator_launched,
            [&]() {
                callback_set.WaitForNotification();
                EXPECT_CALL(*mock_emulator_ptr, Kill(_)).Times(0);

                // Simulate QEMU exiting on its own without any prior signal, followed by signals
                // arriving while the launcher is shutting down.
                event_loop
                        ->Post([&]() {
                            emulator_exit_cb(0, 0);
                            launcher_signal_cb(SIGINT);
                            launcher_signal_cb(SIGTERM);
                        })
                        .IgnoreError();
            },
            &fishtank_launched);

    int status = RunLauncher(std::move(avd));
    trigger.join();
    EXPECT_EQ(status, 0);
}

TEST_F(LauncherTest, CancelsShutdownTimeoutWhenEmulatorExitsPromptly) {
    auto avd = CreateMockAvd();
    qemu_shutdown_timeout = absl::Milliseconds(50);

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    ::goldfish::async::SignalHandlers::Callback launcher_signal_cb;
    absl::Notification launched;
    absl::Notification callback_set;
    MockManagedProcess* mock_process_ptr = nullptr;

    ExpectEmulatorLaunch(launched, emulator_exit_cb, &mock_process_ptr);

    EXPECT_CALL(*mock_signal_handlers, SetCallback(_))
            .WillOnce([&](::goldfish::async::SignalHandlers::Callback signal_cb) {
                launcher_signal_cb = std::move(signal_cb);
                callback_set.Notify();
            });

    std::thread trigger = TriggerEmulatorScript(
            launched,
            [&]() {
                EXPECT_CALL(*mock_process_ptr, Kill(SIGINT)).Times(1);
                EXPECT_CALL(*mock_process_ptr, Kill(SIGKILL)).Times(0);

                event_loop
                        ->Post([&]() {
                            launcher_signal_cb(SIGINT);
                            emulator_exit_cb(0, 0);
                        })
                        .IgnoreError();
            },
            &callback_set);

    int status = RunLauncher(std::move(avd));
    trigger.join();
    EXPECT_EQ(status, 0);
}

TEST_F(LauncherTest, ReturnsErrorStatusWhenPortIsBusy) {
    auto avd = CreateMockAvd();
    opts.port = "5562";

    // -port tries both loopback families before giving up.
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5562), _, _))
            .Times(2)
            .WillRepeatedly(Return(nullptr));
    EXPECT_CALL(*mock_launcher, Launch(_, _)).Times(0);

    absl::StatusOr<int> status = RunLauncherStatus(std::move(avd));
    EXPECT_FALSE(status.ok());
}

TEST_F(LauncherTest, ReturnsErrorStatusWhenEmulatorLaunchFails) {
    auto avd = CreateMockAvd();

    EXPECT_CALL(*mock_launcher, Launch(testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                                      ExpectedQemuBinary()),
                                       _))
            .WillOnce(Return(absl::InternalError("failed to launch qemu")));

    absl::StatusOr<int> status = RunLauncherStatus(std::move(avd));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.status().code(), absl::StatusCode::kInternal);
}

TEST_F(LauncherTest, DefaultConfigUsesNetsimModem) {
    auto avd = CreateMockAvd();
    opts.no_netsim = false;
    opts.no_netsim_modem = false;
    EmulatorPorts ports{.serial_number = 5554, .adb_port = 5555};
    ChardevEndpoints chardev_endpoints{.netsim = "localhost:1234"};
    LaunchQemu emulator{EmulatorConfig{
        ports, chardev_endpoints, {}, user_paths, emulator_paths, *avd, opts, feature_flags}};

    auto launch_config = emulator.launch_config();
    ASSERT_TRUE(launch_config.ok()) << launch_config.status();
    EXPECT_THAT(launch_config->args, testing::Contains("netsim-cellular,id=modem"));
    EXPECT_THAT(launch_config->args, testing::Contains("netsim-netdev,id=eth0,mode=cellular"));
    EXPECT_THAT(launch_config->args,
                testing::Not(testing::Contains(testing::HasSubstr("socket,id=modem"))));
}

TEST_F(LauncherTest, FallbackConfigUsesModemSimulatorWhenNoNetsimModem) {
    auto avd = CreateMockAvd();
    opts.no_netsim = false;
    opts.no_netsim_modem = true;
    EmulatorPorts ports{.serial_number = 5554, .adb_port = 5555};
    ChardevEndpoints chardev_endpoints{.netsim = "localhost:1234",
                                       .modem_simulator = "localhost:5678"};
    LaunchQemu emulator{EmulatorConfig{
        ports, chardev_endpoints, {}, user_paths, emulator_paths, *avd, opts, feature_flags}};

    auto launch_config = emulator.launch_config();
    ASSERT_TRUE(launch_config.ok()) << launch_config.status();
    EXPECT_THAT(
            launch_config->args,
            testing::Contains(testing::HasSubstr("socket,id=modem,nodelay=on,reconnect-ms=100,")));
    EXPECT_THAT(launch_config->args, testing::Contains(testing::HasSubstr("user,id=eth0")));
    EXPECT_THAT(launch_config->args, testing::Not(testing::Contains("netsim-cellular,id=modem")));
}

TEST_F(LauncherTest, EmptyNetsimEndpointLogsWarningAndSkipsNetsimDevices) {
    auto avd = CreateMockAvd();
    opts.no_netsim = false;
    opts.no_netsim_modem = false;
    EmulatorPorts ports{.serial_number = 5554, .adb_port = 5555};
    ChardevEndpoints chardev_endpoints{.modem_simulator = "localhost:5678"};
    LaunchQemu emulator{EmulatorConfig{
        ports, chardev_endpoints, {}, user_paths, emulator_paths, *avd, opts, feature_flags}};

    absl::ScopedMockLog mock_log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(
            mock_log,
            Log(absl::LogSeverity::kWarning, _,
                testing::AllOf(testing::HasSubstr("netsim is requested, but the endpoint is empty"),
                               testing::HasSubstr("-no-netsim"))))
            .Times(1);
    mock_log.StartCapturingLogs();

    auto launch_config = emulator.launch_config();
    mock_log.StopCapturingLogs();

    ASSERT_TRUE(launch_config.ok()) << launch_config.status();
    EXPECT_THAT(launch_config->args,
                testing::Not(testing::Contains(testing::HasSubstr("netsim-connection"))));
    EXPECT_THAT(launch_config->args, testing::Not(testing::Contains("netsim-cellular,id=modem")));
    EXPECT_THAT(launch_config->args, testing::Contains(testing::HasSubstr("user,id=eth0")));
    EXPECT_THAT(
            launch_config->args,
            testing::Contains(testing::HasSubstr("socket,id=modem,nodelay=on,reconnect-ms=100,")));
}

TEST_F(LauncherTest, EndToEndNoNetsimFallsBackToModemSimulator) {
    auto avd = CreateMockAvd();
    opts.no_netsim = true;

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    auto config_matcher = testing::AllOf(
            testing::Field(&::goldfish::async::LaunchConfig::exe_path, ExpectedQemuBinary()),
            testing::Field(
                    &::goldfish::async::LaunchConfig::args,
                    testing::AllOf(testing::Contains(testing::HasSubstr(
                                           "socket,id=modem,nodelay=on,reconnect-ms=100,")),
                                   testing::Contains(testing::HasSubstr("user,id=eth0")),
                                   testing::Not(testing::Contains("netsim-cellular,id=modem")))));

    ExpectEmulatorLaunch(launched, emulator_exit_cb, nullptr, config_matcher);

    absl::ScopedMockLog mock_log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(mock_log,
                Log(absl::LogSeverity::kWarning, _, testing::HasSubstr("netsimd binary not found")))
            .Times(0);
    mock_log.StartCapturingLogs();

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    int status = RunLauncher(std::move(avd));
    trigger.join();
    mock_log.StopCapturingLogs();
    EXPECT_EQ(status, 0);
}

TEST_F(LauncherTest, EndToEndMissingNetsimBinaryFallsBackToModemSimulator) {
    auto avd = CreateMockAvd();
    opts.no_netsim = false;
    emulator_paths.netsim_binary.clear();

    absl::Notification launched;
    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;

    auto config_matcher = testing::AllOf(
            testing::Field(&::goldfish::async::LaunchConfig::exe_path, ExpectedQemuBinary()),
            testing::Field(
                    &::goldfish::async::LaunchConfig::args,
                    testing::AllOf(testing::Contains(testing::HasSubstr(
                                           "socket,id=modem,nodelay=on,reconnect-ms=100,")),
                                   testing::Contains(testing::HasSubstr("user,id=eth0")),
                                   testing::Not(testing::Contains(
                                           testing::HasSubstr("netsim-connection"))),
                                   testing::Not(testing::Contains("netsim-cellular,id=modem")))));

    ExpectEmulatorLaunch(launched, emulator_exit_cb, nullptr, config_matcher);

    absl::ScopedMockLog mock_log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(mock_log, Log(absl::LogSeverity::kWarning, _,
                              testing::HasSubstr("netsim is requested, but the endpoint is empty")))
            .Times(0);
    EXPECT_CALL(mock_log, Log(absl::LogSeverity::kWarning, _,
                              testing::AllOf(testing::HasSubstr("netsimd binary not found"),
                                             testing::HasSubstr("Wi-Fi, Bluetooth, UWB, and NFC"),
                                             testing::HasSubstr("SLIRP (eth0)"),
                                             testing::HasSubstr("-packet-streamer-endpoint"),
                                             testing::HasSubstr("-no-netsim"))))
            .Times(1);
    mock_log.StartCapturingLogs();

    std::thread trigger = TriggerEmulatorExit(launched, emulator_exit_cb);

    int status = RunLauncher(std::move(avd));
    trigger.join();
    mock_log.StopCapturingLogs();
    EXPECT_EQ(status, 0);
}

TEST_F(LauncherTest, ShutsDownMetricsAndFinalizesFilesBeforeClosingSignalHandlers) {
    auto avd = CreateMockAvd();
    opts.no_window = false;

    const fs::path unicode_dir = temp_dir / "测试_🚀_é";
    const fs::path spool_dir = unicode_dir / "spool";
    fs::create_directories(spool_dir);

    const auto session_uuid = ::goldfish::metrics::Uuid::Generate();
    const std::string session_id = session_uuid.ToString();
    metrics_writer_config.type = ::goldfish::metrics::MetricsWriterType::kStudio;
    metrics_writer_config.studio_spool_dir = spool_dir;

    // Simulate a QEMU child .open spool file left open when QEMU exits (pid = 1234).
    const fs::path qemu_open_file =
            spool_dir / absl::StrCat("emulator-metrics2-", session_id, "-1234-0.open");
    std::ofstream(qemu_open_file).close();
    fs::path qemu_trk_file = qemu_open_file;
    qemu_trk_file.replace_extension("trk");

    auto reporter = std::make_unique<::goldfish::metrics::MetricsReporter>(session_uuid);
    std::atomic<bool> writer_destroyed{false};
    auto mock_writer = std::make_unique<MockMetricsWriter>(&writer_destroyed);

    absl::Mutex events_mu;
    std::vector<android_studio::AndroidStudioEvent> events;
    EXPECT_CALL(*mock_writer, Write(_))
            .WillRepeatedly([&](::goldfish::metrics::MetricsEvent event) {
                absl::MutexLock lock(events_mu);
                events.push_back(std::move(event.as_event));
            });
    reporter->SetWriter(std::move(mock_writer));

    ::goldfish::async::AsyncSocketServer::ConnectCallback console_connect_cb;
    ::goldfish::async::AsyncSocketServer::LoopProvider console_loop_provider;
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5554), _, _))
            .Times(2)
            .WillRepeatedly([&](::goldfish::async::EventLoop*,
                                const ::goldfish::network::Endpoint& endpoint,
                                ::goldfish::async::AsyncSocketServer::ConnectCallback connect_cb,
                                ::goldfish::async::AsyncSocketServer::LoopProvider loop_provider) {
                if (!console_connect_cb) {
                    console_connect_cb = std::move(connect_cb);
                    console_loop_provider = std::move(loop_provider);
                }
                auto server = std::make_shared<MockAsyncSocketServer>();
                ON_CALL(*server, GetEndpoint()).WillByDefault(Return(endpoint));
                return server;
            });

    absl::Notification fishtank_launched;
    bool fishtank_killed_before_metrics_shutdown = false;
    EXPECT_CALL(*mock_launcher, Launch(testing::Field(&::goldfish::async::LaunchConfig::exe_path,
                                                      emulator_paths.fishtank_binary),
                                       _))
            .WillOnce([&](const ::goldfish::async::LaunchConfig&,
                          ::goldfish::async::ProcessLauncher::ExitCallback) {
                auto process = std::make_unique<MockManagedProcess>();
                MockManagedProcess* process_ptr = process.get();
                EXPECT_CALL(*process_ptr, GetPid()).WillRepeatedly(Return(5678));
                EXPECT_CALL(*process_ptr, Kill(SIGTERM)).WillOnce([&](int) {
                    fishtank_killed_before_metrics_shutdown =
                            !writer_destroyed.load(std::memory_order_acquire);
                });
                fishtank_launched.Notify();
                return absl::StatusOr<std::unique_ptr<::goldfish::async::ManagedProcess>>(
                        std::move(process));
            });

    bool writer_destroyed_before_signal_close = false;
    bool qemu_open_finalized_before_signal_close = false;
    EXPECT_CALL(*mock_signal_handlers, close()).WillOnce([&]() {
        writer_destroyed_before_signal_close = writer_destroyed.load(std::memory_order_acquire);
        qemu_open_finalized_before_signal_close = !android::base::file::exists(qemu_open_file) &&
                                                  android::base::file::exists(qemu_trk_file);
    });

    ::goldfish::async::ProcessLauncher::ExitCallback emulator_exit_cb;
    absl::Notification launched;
    ExpectEmulatorLaunch(launched, emulator_exit_cb);

    absl::Notification help_handled;
    auto console_socket =
            std::make_shared<testing::NiceMock<::goldfish::async::testing::FakeAsyncSocket>>();
    ON_CALL(*console_socket, Send(_, _, _))
            .WillByDefault([&](const char* buf, size_t len,
                               ::goldfish::async::AsyncSocket::OnSendCallback) {
                if (absl::StrContains(std::string_view(buf, len), "Try 'help-verbose'") &&
                    !help_handled.HasBeenNotified()) {
                    help_handled.Notify();
                }
                return absl::OkStatus();
            });
    ON_CALL(*console_socket, Close()).WillByDefault([&]() { console_socket->SimulateClose(); });

    std::thread trigger = TriggerEmulatorScript(
            launched,
            [&]() {
                event_loop
                        ->Post([&]() {
                            ASSERT_TRUE(console_connect_cb);
                            ASSERT_TRUE(console_loop_provider);
                            auto* conn_loop = console_loop_provider(::goldfish::network::ToEndpoint(
                                    ::goldfish::network::kIPv4LoopbackAddress, 12345));
                            console_socket->setEventLoop(conn_loop);
                            ASSERT_TRUE(console_connect_cb(console_socket));
                            console_socket->SimulateRead("help\r\n");
                        })
                        .IgnoreError();
                help_handled.WaitForNotification();
                event_loop->Post([&]() { emulator_exit_cb(0, 0); }).IgnoreError();
            },
            &fishtank_launched);

    int status = RunLauncher(std::move(avd), std::move(reporter));
    trigger.join();

    EXPECT_EQ(status, 0);
    EXPECT_TRUE(fishtank_killed_before_metrics_shutdown);
    EXPECT_TRUE(writer_destroyed_before_signal_close);
    EXPECT_TRUE(qemu_open_finalized_before_signal_close);

    bool saw_final_ping = false;
    bool saw_console_events = false;
    absl::MutexLock lock(events_mu);
    for (const auto& ev : events) {
        if (ev.emulator_details().has_crashes()) {
            saw_final_ping = true;
            EXPECT_EQ(ev.emulator_details().crashes(), kMetricsCrashesNone);
        }
        if (ev.kind() == android_studio::AndroidStudioEvent::EMULATOR_CONSOLE_EVENTS) {
            saw_console_events = true;
            ASSERT_EQ(ev.emulator_console_events_size(), 1);
            EXPECT_EQ(ev.emulator_console_events(0).command(), "help");
            EXPECT_EQ(ev.emulator_console_events(0).value(), 1);
        }
    }
    EXPECT_TRUE(saw_final_ping);
    EXPECT_TRUE(saw_console_events);
}

TEST_F(LauncherTest, StartupFailureShutsDownMetricsBeforeClosingSignalHandlers) {
    auto avd = CreateMockAvd();
    opts.port = "5562";

    // -port tries both loopback families before giving up.
    EXPECT_CALL(*mock_socket_factory, CreateServer(_, IsPort(5562), _, _))
            .Times(2)
            .WillRepeatedly(Return(nullptr));
    EXPECT_CALL(*mock_launcher, Launch(_, _)).Times(0);

    auto reporter = std::make_unique<::goldfish::metrics::MetricsReporter>();
    std::atomic<bool> writer_destroyed{false};
    reporter->SetWriter(std::make_unique<MockMetricsWriter>(&writer_destroyed));

    bool writer_destroyed_before_signal_close = false;
    EXPECT_CALL(*mock_signal_handlers, close()).WillOnce([&]() {
        writer_destroyed_before_signal_close = writer_destroyed.load(std::memory_order_acquire);
    });

    absl::StatusOr<int> status = RunLauncherStatus(std::move(avd), std::move(reporter));
    EXPECT_FALSE(status.ok());
    EXPECT_TRUE(writer_destroyed_before_signal_close);
}

}  // namespace
}  // namespace android::goldfish
