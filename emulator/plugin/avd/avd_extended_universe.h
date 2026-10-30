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

#include <memory>

#include "absl/status/statusor.h"

#include "android/emulation/control/keyboard/qemu_keyboard_sink.h"
#include "emulator/plugin/grpc/input/input_dispatcher.h"
#include "emulator/plugin/grpc/input/multi_display_touch_devices.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/devices/battery/battery.h"
#include "goldfish/devices/boot/boot_properties_device.h"
#include "goldfish/devices/camera/register_device.h"
#include "goldfish/devices/clipboard/clipboard_device.h"
#include "goldfish/devices/connector_registry_impl.h"
#include "goldfish/devices/fingerprint/fingerprint_device.h"
#include "goldfish/devices/gps/gps_device.h"
#include "goldfish/devices/guest_status/guest_status_device.h"
#include "goldfish/devices/multidisplay/multidisplay_device.h"
#include "goldfish/devices/sensor/sensor_device.h"
#include "goldfish/devices/unix_pipe/unix_pipe.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/perf_stat_reporter.h"
#include "goldfish/metrics/snapshot_metrics.h"
#include "vcpu_event_loop.h"

extern "C" {
typedef struct VMChangeStateEntry VMChangeStateEntry;
}

namespace goldfish::avd {

struct AvdExtendedUniverse : public AvdUniverse {
  private:
    struct Private {};

  public:
    static absl::StatusOr<std::unique_ptr<AvdExtendedUniverse>> Create(
            std::unique_ptr<AvdProperties> props);
    AvdExtendedUniverse(Private, std::unique_ptr<AvdProperties> props);

    ~AvdExtendedUniverse() override;

    void WaitUntilEventLoopsIdle();
    void ShutdownQemuLoop();
    void OnPreGrpcShutdown() override;
    async::EventLoop& GetQemuEventLoop() override;
    metrics::MetricsReporter& GetMetricsReporter() override;
    android::goldfish::VmOperations& GetVmOperations() override;
    display::IMultiDisplay& GetMultiDisplay() const override;
    std::shared_ptr<::goldfish::grpc::v2::IInputDispatcher> GetInputDispatcher() override;

    /**
     * @brief Gives the input dispatcher the guest keyboard.
     *
     * Call once QEMU has created console 0, which holds the keyboard: QEMU
     * creates the display devices, and their consoles, after this universe.
     */
    void InstallKeyboard();

    void OnPreSave() override;
    absl::Status OnSave(archive::IWriter&) const override;
    void OnPreLoad() override;
    absl::Status OnLoad(archive::IReader&) override;
    absl::Status OnPostLoad() override;

    void Reset();

    std::unique_ptr<async::EventLoop> qemu_event_loop;
    std::vector<VCpuEventLoop> qemu_cpu_loops;
    std::unique_ptr<android::goldfish::VmOperations> vm_operations;

    std::unique_ptr<display::IMultiDisplay> multi_display;
    display::IMultiDisplay::ScopedRotationSubscription display_rotation_subscription;

    devices::ConnectorRegistry connector_registry;
    devices::ConnectorRegistry test_tools_connector_registry;
    avd_universe::battery::ObservableBattery::ScopedCallbackHandle battery_subscription;
    std::unique_ptr<metrics::MetricsReporter> metrics_reporter;
    std::shared_ptr<async::EventLoop::Timer> metrics_ping_timer;

    std::unique_ptr<metrics::PerfStatReporter> perf_stat_reporter;
    std::shared_ptr<async::EventLoop::Timer> perf_stat_reporter_task;

  private:
    using DisplayEventSubscription =
            std::unique_ptr<android::base::eventing::ScopedEventCallback<display::IMultiDisplay,
                                                                         display::DisplayEvent>>;

    absl::Status Initialize();

    void OnSaveProps(archive::IWriter&) const;
    void OnSavePhysicalState(archive::IWriter&) const;
    absl::Status OnSaveDisplayState(archive::IWriter&) const;

    absl::Status OnLoadProps(archive::IReader&);
    absl::Status OnLoadPhysicalState(archive::IReader&);
    absl::Status OnLoadDisplayState(archive::IReader&);

    void SyncGuestTimeToHost();

    template <typename State>
    static void OnVmStateChanged(void* opaque, bool running, State state);

    metrics::QuickbootMetricsState quickboot_metrics_;

    // Declared after the QEMU loop and the multi-display they use, and in
    // this order, so the dispatcher is released first. The universe is
    // destroyed in qemu_cleanup(), after the main loop, and its destructor
    // stops the QEMU loop and then the dispatcher before any of them is
    // destroyed. Input clients may still hold the stopped dispatcher.
    std::unique_ptr<::goldfish::grpc::v2::MultiDisplayTouchDevices> touch_devices_;
    std::unique_ptr<::android::emulation::control::keyboard::QemuKeyboardSink> keyboard_sink_;
    std::shared_ptr<::goldfish::grpc::v2::InputDispatcher> input_dispatcher_;
    DisplayEventSubscription input_display_subscription_;
    VMChangeStateEntry* vm_state_entry_ = nullptr;
    bool snapshot_loaded_ = false;
};

}  // namespace goldfish::avd
