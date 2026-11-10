// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "launch_qemu.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

// Use ABSL_LOG to avoid conflict with crashpadh logging
#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "grpc_device.h"

#include "adb_device.h"
#include "android/goldfish/avd.h"
#include "android/goldfish/constants.h"
#include "android/process/command.h"
#include "android/status/status_macros.h"
#include "audio_device.h"
#include "avd_info_device.h"
#include "configure_drives.h"
#include "cpu_device.h"
#include "display_device.h"
#include "env_passthrough_device.h"
#include "goldfish_address_space_device.h"
#include "gpu_device.h"
#include "hugepage_device.h"
#include "initrd_device.h"
#include "kernel_device.h"
#include "machine.h"
#include "memory_device.h"
#include "network_device.h"
#include "parameter_list.h"
#include "plugin_init.h"
#include "snapshot_device.h"
#include "wifi_device.h"

namespace android::goldfish {

absl::Status LaunchQemu::addDevices() {
    // Device are initialized in order of appearance
    // So if device B depends on device A, you should register them as:
    // -device A -device B ...
    const auto& o = config_.opts();
    const auto& a = config_.avd();
    // Checked before adding any device, since devices can change the AVD: e.g. the drives create a
    // new userdata qcow2 overlay when it is missing or invalid.
    const bool factory_boot = IsUserdataNew(a);

    addDevice<EnvPassthroughDevice>();
    addDevice<PluginInit>();

    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-nodefaults",
        // our iothread
        "-object",
        "iothread,id=disk-iothread",
    });
    if (o.strict_snapshot_load) {
        // A bad snapshot load might trigger a guest shutdown that is hidden by auto-rebooting.
        addDevice<ParameterList>(std::initializer_list<std::string>{
            "-no-reboot",
        });
    }

    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-name", absl::StrFormat("%s,debug-threads=on", Device::EscapeQemuOptionValue(a.Name()))});
    addDevice<Machine>();
    addDevice<CpuDevice>();
    addDevice<MemoryDevice>();
    addDevice<HugePageDevice>();
    addDevice<KernelDevice>();

    RETURN_IF_ERROR(addDrives(config_, *this));

    addDevice<AudioDevice>("09.0");

    // Hardware RNG device
    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-device",
        "virtio-rng-pci",
    });

    // This is needed for virtconsole (logcat, bt, uwb).
    // TODO old emulator also created a virtio-serial device, do we need to?
    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-device",
        "virtio-serial-pci,ioeventfd=off",
    });

    // virtio logcat consoles, note that order matters here!
    // This device is probably just to make sure that logcat is on device 1 and not 0.
    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-device",
        "virtconsole,chardev=forhvc0,name=logcat_null",
        "-chardev",
        "null,id=forhvc0",
    });
    if (o.logcat_output) {
        // virtio logcat consoles, note that order matters here!
        addDevice<ParameterList>(std::initializer_list<std::string>{
            // Actual logcat location.
            "-device", "virtconsole,chardev=forhvc1,name=logcat", "-chardev",
            absl::StrCat("file,id=forhvc1,path=", Device::EscapeQemuOptionValue(o.logcat_output))});
    } else {
        addDevice<ParameterList>(std::initializer_list<std::string>{
            // Actual logcat location.
            "-device", "virtconsole,chardev=forhvc1,name=logcat", "-chardev", "null,id=forhvc1"});
    }

    addDevice<ParameterList>(
            std::initializer_list<std::string>{"-serial", o.show_kernel ? "stdio" : "null"});

    addDevice<AvdInfoDevice>(factory_boot);

    bool netsim_cellular = false;
    if (!o.no_netsim && config_.chardev_endpoints().netsim.empty()) {
        ABSL_LOG(WARNING)
                << "netsim is requested, but the endpoint is empty; skipping netsim device "
                   "configuration and falling back to QEMU SLIRP (eth0) and non-netsim "
                   "telephony (pass '-no-netsim' if netsim is intentionally disabled).";
    } else if (!o.no_netsim) {
        addDevice<ParameterList>(std::initializer_list<std::string>{
            "-device",
            absl::StrCat("netsim-connection,id=netsim,grpc_endpoint=",
                         config_.chardev_endpoints().netsim),
        });

        if (!o.no_wifi) {
            addDevice<WifiDevice>("0b.0");
        }
        // The name of these vport devices should be used by
        // http://ac/device/generic/goldfish/qemu-props/vport_parser.cpp
        // It should lookup the actual port number and set the property
        // "vendor.qemu.vport.<name>" to "/dev/vport8p<N>"
        // e.g. /dev/vport8p3 for bt (4th port)
        std::string uwb_device = (a.VendorProperty("ro.vendor.uwb.dev") == "/dev/uwb0")
                                         ? "virtserialport,chardev=uwb,name=uwb"
                                         : "virtconsole,chardev=uwb,name=uwb";

        std::vector<std::string> radio_params = {
            "-chardev", "netsim-uwb,id=uwb",
            "-device",  uwb_device,
            "-chardev", "netsim-bt,id=bluetooth",
            "-device",  "virtserialport,chardev=bluetooth,name=bluetooth",
        };

        if (o.nfc) {
            radio_params.insert(radio_params.end(), {
                                                        "-chardev",
                                                        "netsim-nfc,id=nfc",
                                                        "-device",
                                                        "virtserialport,chardev=nfc,name=nfc",
                                                    });
        }

        addDevice<ParameterList>(std::move(radio_params));

        if (UseNetsimModem(o.no_netsim_modem, a.ApiLevel())) {
            netsim_cellular = true;
            addDevice<ParameterList>(std::initializer_list<std::string>{
                "-chardev",
                "netsim-cellular,id=modem",
                "-device",
                "virtserialport,chardev=modem,name=modem",
            });
        } else if (!o.no_netsim_modem) {
            ABSL_LOG(INFO) << "API " << a.ApiLevel() << " guest radio HAL predates netsim modem "
                           << "support (API " << kNetsimModemMinApiLevel
                           << "); using the in-process modem simulator";
        }
    }

    // Note that we assume Linux calls first (virtio-net*) ethernet device eth0 etc.
    addDevice<NetworkDevice>("eth0", "0c.0", /*cellular=*/true,
                             /*netsim_backend=*/netsim_cellular);
    // TODO TV ethernet addDevice<NetworkDevice>("eth1", "0d.0", /*cellular=*/false,
    // /*netsim_backend=*/false);

    if (!netsim_cellular && !config_.chardev_endpoints().modem_simulator.empty()) {
        addDevice<ParameterList>(std::initializer_list<std::string>{
            "-chardev",
            absl::StrCat("socket,id=modem,nodelay=on,reconnect-ms=100,",
                         config_.chardev_endpoints().modem_simulator),
            "-device",
            "virtserialport,chardev=modem,name=modem",
        });
    }

    const auto& snapshot_device = addDevice<SnapshotDevice>();

    std::string gpu_name = "gpu0";
    const auto& gpu_device = addDevice<GpuDevice>(gpu_name, snapshot_device);
    addDevice<InitrdDevice>(gpu_device, snapshot_device);

    // Also includes input devices for the display.
    addDevice<DisplayDevice>(gpu_name);

    if (a.ApiLevel() < kVirtioReadyApiLevel) {
        addDevice<GoldfishAddressSpaceDevice>("1e.0");
    }

    // Our virtio-vsock.
    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-device",
        "virtio-goldfish-vsock-pci,guest-cid=3",
    });

    // Must come after vsock.
    addDevice<AdbDevice>();

    // Make sure we have our other devices available before we setup the gRPC device, the gRPC
    // device depends on the virtio devices for input event delivery.
    if (!o.no_grpc) {
        // The webrtc device must be added before grpc, so it is realized *before* grpc
        // and unrealized *before* audio drivers.
        addDevice<ParameterList>(std::initializer_list<std::string>{"-device", "webrtc"});
        addDevice<GrpcDevice>();
    }

    // This should always be the last device, as it will finalize android emulator initialization.
    addDevice<ParameterList>(std::initializer_list<std::string>{"-device", "avdend"});

    addDevice<ParameterList>(std::initializer_list<std::string>{
        "-L", config_.emulator_paths().bios_directory.string()});

    if (o.qemu_telnet) {
        // Debug monitor
        addDevice<ParameterList>(std::initializer_list<std::string>{
            "-monitor",
            "telnet::15454,server,nowait",
        });
    }

    if (o.qemu) {
        addDevice<ParameterList>(absl::StrSplit(o.qemu, ' '));
    }

    return absl::OkStatus();
}

void LaunchQemu::clear() {
    for (auto& device : mDevices) {
        ABSL_LOG(INFO) << "Reset: " << device->id();
        device->clear();
    }
}

absl::Status LaunchQemu::initialize() {
    RETURN_IF_ERROR(addDevices());

    for (auto& device : mDevices) {
        VLOG(1) << "Preparing Qemu device args: " << device->id();
        auto status = device->initialize(config_);
        if (!status.ok()) {
            return status;
        }
    }
    return absl::OkStatus();
}

std::string LaunchQemu::qemu_exe_path() const {
    const auto& p = config_.emulator_paths();
    std::string base;
    switch (config_.avd().Arch()) {
    case Avd::CpuArchitecture::kX86:
        return p.qemu_system_x86_binary.string();
    case Avd::CpuArchitecture::kArm:
        return p.qemu_system_arm_binary.string();
    case Avd::CpuArchitecture::kRiscV:
        return p.qemu_system_riscv_binary.string();
    default:
        return "unknown";
    }

    return base;
}

std::vector<std::string> LaunchQemu::getCmdline() const {
    std::vector<std::string> params;

    for (const auto& device : mDevices) {
        auto component = device->getQemuParameters(config_);
        params.insert(params.end(), component.begin(), component.end());
    }

    return params;
}

absl::flat_hash_map<std::string, std::string> LaunchQemu::getEnv() const {
    absl::flat_hash_map<std::string, std::string> env;

    for (const auto& device : mDevices) {
        for (auto& [k, v] : device->getQemuEnv(config_)) {
            if (v.empty()) {
                env.erase(k);
            } else {
                env.insert_or_assign(k, std::move(v));
            }
        }
    }

    return env;
}

absl::StatusOr<::goldfish::async::LaunchConfig> LaunchQemu::launch_config() {
    auto status = initialize();
    if (!status.ok()) {
        VLOG(1) << "Failed to prepare emulator: " << status.message();
        return status;
    }

    fs::path exe_path = qemu_exe_path();
    if (exe_path.empty()) {
        return absl::FailedPreconditionError(absl::StrCat(
                "This emulator does not include a qemu-system binary for the AVD's "
                "ABI (",
                config_.avd().Abi(), "). Only the host-native guest architecture is supported."));
    }
    std::vector<std::string> args = getCmdline();
    {
        std::vector<std::string> printableArgs;
        printableArgs.reserve(args.size() + 1);
        printableArgs.push_back(exe_path.string());
        std::transform(args.begin(), args.end(), std::back_inserter(printableArgs),
                       [](const std::string& str) -> std::string {
                           if (std::any_of(str.begin(), str.end(),
                                           [](const char c) { return std::isspace(c); })) {
                               using namespace std::literals::string_literals;
                               return "\""s + str + "\""s;
                           } else {
                               return str;
                           }
                       });

        ABSL_LOG(INFO) << "Emulator launch command: " << absl::StrJoin(printableArgs, " ");
    }

    return ::goldfish::async::LaunchConfig{
        .exe_path = std::move(exe_path),
        .args = std::move(args),
        .environment = getEnv(),
        .new_process_group = true,
        .stdio_mode = ::goldfish::async::LaunchConfig::StdioMode::kInherit,
    };
}

}  // namespace android::goldfish
