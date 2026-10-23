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

#include "avd_extended_universe.h"

#include <chrono>
#include <string>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/base/system.h"
#include "android/crashreport/crash_reporter.h"
#include "android/emulation/control/keyboard/key_code_resolver.h"
#include "android/goldfish/qemu_vm_operations.h"
#include "android/goldfish/vm_interface.h"
#include "goldfish/adb/adb_host_server.h"
#include "goldfish/archive/collections/string.h"
#include "goldfish/archive/reader.h"
#include "goldfish/archive/writer.h"
#include "goldfish/async/qemu_event_loop.h"
#include "goldfish/avd/global_event_loop.h"
#include "goldfish/avd/gralloc_impl.h"
#include "goldfish/devices/vehicle/vehicle_device.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/snapshot/snapshot_store.h"
#include "goldfish/tools/aemu_version.h"
#include "goldfish/vsock/clear.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
extern "C" {
#include "hw/core/qdev.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "qemu/main-loop.h"
#include "ui/console.h"
}
#undef listen
#undef shutdown
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::avd {

using devices::boot::EventLoop;
using namespace goldfish::archive;

namespace {

void BqlSafeUnregisterEmulatorReset(QEMUResetHandler* func, void* opaque) {
    if (bql_locked()) {
        qemu_unregister_reset(func, opaque);
    } else {
        LOG(QFATAL) << "Attempted to call qemu_unregister_reset without BQL held.";
    }
}

std::vector<VCpuEventLoop> createVCpuEventLoops() {
    int cpus_count = VCpuEventLoop::cpus_count();
    std::vector<VCpuEventLoop> loops;
    loops.reserve(cpus_count);
    for (int i = 0; i < cpus_count; ++i) {
        loops.emplace_back(i);
    }
    return loops;
}

std::string GetCurrentVkIcd() {
    std::string vk_icd = android::base::System::Get()->GetEnvironmentVariable("ANDROID_EMU_VK_ICD");
    return vk_icd;
}

// note: when the avd is moved to different location,
// the path (such as disk_systemPartition_initPath, or disk_systemPartition_path)
// will change, and that is typical use case
// and should not invalidate snapshot; firstboot properties
// are deprecated and not a factor to consider either.
// in addition, the avd home or sdk root are not valid reason
// to invalidet snapshot. we will likely add more factors to
// ignore in the future. after the exclude, most of the properties
// are important such as all the ones start with hw_, or size related
bool IsExcludedProp(std::string_view name) {
    return name.find("path") != std::string_view::npos ||
           name.find("Path") != std::string_view::npos || name.find("firstboot") == 0 ||
           name == "android_sdk_root" || name == "android_sdk_home" || name == "android_avd_home";
}

}  // namespace

absl::StatusOr<std::unique_ptr<AvdExtendedUniverse>> AvdExtendedUniverse::Create(
        std::unique_ptr<AvdProperties> props) {
    auto universe = std::make_unique<AvdExtendedUniverse>(Private(), std::move(props));
    if (auto s = universe->Initialize(); !s.ok()) {
        return s;
    }
    return universe;
}

AvdExtendedUniverse::AvdExtendedUniverse(Private, std::unique_ptr<AvdProperties> props)
        : AvdUniverse(std::move(props)) {}

template <>
void AvdExtendedUniverse::OnVmStateChanged(void* opaque, bool running, RunState /*state*/) {
    auto* self = static_cast<AvdExtendedUniverse*>(opaque);
    if (running && self->snapshot_loaded_) {
        self->snapshot_loaded_ = false;
        self->input_dispatcher_->OnSnapshotLoaded();
    }
}

absl::Status AvdExtendedUniverse::Initialize() {
    auto& avd_universe = *this;
    const AvdProperties& avd_props = avd_universe.Props();

    LOG(INFO) << "Loaded avd directory: " << avd_props.avd_content_path;
    auto* client_loop = globalEventLoop();

    avd_universe.metrics_reporter =
            std::make_unique<::goldfish::metrics::MetricsReporter>(avd_props.metrics_session_id);
    ::goldfish::metrics::ConfigureMetricsWriter(*avd_universe.metrics_reporter,
                                                avd_props.metrics_writer_config, *client_loop);
    // PING every 5 minutes.
    using namespace std::chrono_literals;
    avd_universe.metrics_ping_timer = client_loop->ScheduleRepeating(
            [metrics_reporter = avd_universe.metrics_reporter.get()] {
                metrics_reporter->Report([](android_studio::AndroidStudioEvent& event) {});
                return true;
            },
            absl::ZeroDuration(), absl::Seconds(300));
    avd_universe.GetGuestStatus().SetMetricsReporter(avd_universe.metrics_reporter.get());

    avd_universe.qemu_event_loop = goldfish::async::QemuEventLoop::Create();
    auto& qemu_loop = avd_universe.qemu_event_loop;

    avd_universe.vm_operations = android::goldfish::CreateQemuVmOperations(
            /*event_loop=*/client_loop, /*qemu_loop=*/qemu_loop.get(),
            android::goldfish::QemuSnapshotConfig{
                .store = std::make_unique<::goldfish::snapshot::FileSnapshotStore>(
                        avd_props.avd_content_path, avd_props.snapshot_version),
                .enabled = avd_props.snapshots_enabled,
            },
            /*hang_detector=*/&android::crashreport::CrashReporter::GetCrashingHangDetector());
    auto is_active = [vm = avd_universe.vm_operations.get()]() { return vm->IsRunning(); };

    android::crashreport::CrashReporter::GetCrashingHangDetector().AddWatchedLooper(
            "QemuEventLoop", *qemu_loop, absl::Seconds(15), is_active);

    avd_universe.qemu_cpu_loops = createVCpuEventLoops();
    std::vector<goldfish::async::EventLoop*> vcpu_loop_ptrs;
    vcpu_loop_ptrs.reserve(avd_universe.qemu_cpu_loops.size());
    for (auto& loop : avd_universe.qemu_cpu_loops) {
        android::crashreport::CrashReporter::GetCrashingHangDetector().AddWatchedLooper(
                absl::StrCat("QemuCpuLoop:", loop.getCpuIndex()), loop, absl::Seconds(15),
                is_active);
        vcpu_loop_ptrs.push_back(&loop);
    }
    avd_universe.perf_stat_reporter = std::make_unique<goldfish::metrics::PerfStatReporter>(
            client_loop, vcpu_loop_ptrs, avd_props.hw_config, avd_props.dump_perf_stat_path);
    // One-shot task to send perf metrics report after 60s.
    avd_universe.perf_stat_reporter_task = client_loop->ScheduleDelayed(
            [&avd_universe] {
                avd_universe.metrics_reporter->Report(
                        [&avd_universe](android_studio::AndroidStudioEvent& event) {
                            VLOG(1) << "Reporting perf stat metric";
                            event.set_kind(
                                    android_studio::AndroidStudioEvent::EMULATOR_PERFORMANCE_STATS);
                            avd_universe.perf_stat_reporter->FillEvent(event);
                        });
            },
            /*initial_delay=*/absl::Seconds(60));

    auto* registry = &avd_universe.connector_registry;

    namespace DEVS = ::goldfish::devices;

    avd_universe.GetGuestStatus().SetGrpcNotificationChannel(
            &avd_universe.GetGrpcNotificationChannel());

    DEVS::sensor::ISensorDevice::RegisterDevice(&avd_universe.GetSensorsPhysicalModel(), registry,
                                                avd_props.avd_type, avd_props.avd_api,
                                                avd_props.hw_config, client_loop, qemu_loop.get());
    DEVS::clipboard::IClipboardDevice::RegisterDevice(&avd_universe.GetClipboardChannel(), registry,
                                                      client_loop, qemu_loop.get());
    DEVS::vehicle::IVehicleDevice::RegisterDevice(&avd_universe.GetVehicleChannel(), registry,
                                                  client_loop, qemu_loop.get());
    DEVS::guest_status::IGuestStatusDevice::RegisterDevice(
            &avd_universe.GetGuestStatus(), registry,
            {qemu_register_reset, BqlSafeUnregisterEmulatorReset},
            [vm = avd_universe.vm_operations.get()] {
                vm->SystemShutdownRequest(android::goldfish::QemuShutdownCause::GuestShutdown);
            },
            client_loop, qemu_loop.get(), avd_props.quit_after_boot_timeout_seconds);
    DEVS::fingerprint::IFingerprintDevice::RegisterDevice(&avd_universe.GetFingerprintSensor(),
                                                          registry, client_loop, qemu_loop.get());
    DEVS::gps::IGpsDevice::RegisterDevice(&avd_universe.GetLocation(), registry, qemu_loop.get());

    auto multi_display_device =
            std::make_shared<DEVS::multidisplay::MultiDisplayDevice>(client_loop, qemu_loop.get());
    avd_universe.SetActiveMultiDisplayDevice(multi_display_device);
    DEVS::multidisplay::MultiDisplayDevice::RegisterDevice(multi_display_device, registry,
                                                           client_loop, qemu_loop.get());

    std::string emulatedCameraProp;
    DEVS::camera::RegisterDevice(&avd_universe.GetSensorsPhysicalModel(), registry,
                                 &emulatedCameraProp, avd_props.hw_config,
                                 avd_props.avd_content_path, []() { return GetGrallocImpl(); });

    using namespace std::string_literals;
    absl::flat_hash_map<std::string, std::string> boot_properties = {
        {"qemu.sf.fake_camera"s, emulatedCameraProp},
        {"qemu.sf.lcd_density"s, absl::StrCat(avd_props.hw_config.hw_lcd_density)},
        // This is the same value that is passed to the virtio-wifi module.
        {"net.wifi_mac_prefix"s, absl::StrCat(avd_props.serial_number)},
    };
    if (!avd_props.timezone.empty()) {
        // The guest copies it into persist.sys.timezone.
        boot_properties.emplace("qemu.timezone"s, avd_props.timezone);
    }
    auto props = devices::boot::IBootPropertiesDevice::MakeProperties(boot_properties);
    if (!props.ok()) {
        return props.status();
    }
    DEVS::boot::IBootPropertiesDevice::RegisterDevice(registry, *std::move(props), client_loop,
                                                      qemu_loop.get());

    DEVS::unix_pipe::IUnixPipe::RegisterDevice(&avd_universe.test_tools_connector_registry,
                                               client_loop, qemu_loop.get());

    avd_universe.multi_display = display::IMultiDisplay::Create(
            client_loop, qemu_loop.get(),
            avd_universe.GetSensorsPhysicalModel().GetFoldableConfig());
    avd_universe.display_rotation_subscription = avd_universe.multi_display->RegisterPhysicalModel(
            avd_universe.GetSensorsPhysicalModel());

    // Without a keyboard until InstallKeyboard(): QEMU has no consoles yet.
    avd_universe.touch_devices_ = std::make_unique<::goldfish::grpc::v2::MultiDisplayTouchDevices>(
            *avd_universe.multi_display, avd_props.hw_config.hw_sensor_hinge);
    avd_universe.input_dispatcher_ = std::make_shared<::goldfish::grpc::v2::InputDispatcher>(
            *qemu_loop, *avd_universe.touch_devices_, /*keyboard=*/nullptr,
            android::emulation::control::keyboard::ResolveEvdevKeyCode);
    avd_universe.input_display_subscription_ = android::base::eventing::MakeScopedCallback(
            *avd_universe.multi_display,
            [dispatcher = avd_universe.input_dispatcher_](const display::DisplayEvent& event) {
                if (event.IsAddedEvent()) {
                    if (auto display = event.Display().lock()) {
                        dispatcher->OnDisplayAdded(display->Id());
                    }
                } else if (event.IsDeletedEvent()) {
                    dispatcher->OnDisplayRemoved(event.DisplayId());
                }
            });
    avd_universe.vm_state_entry_ = qemu_add_vm_change_state_handler(OnVmStateChanged, this);

    // Initialize the battery to a default state and register it.
    avd_universe.battery_subscription = DEVS::battery::RegisterBattery(
            &avd_universe.GetBattery(), avd_props.hw_config.hw_battery, qemu_loop.get());

    return absl::OkStatus();
}

AvdExtendedUniverse::~AvdExtendedUniverse() {
    if (metrics_ping_timer) {
        metrics_ping_timer->Cancel();
    }
    if (perf_stat_reporter_task) {
        perf_stat_reporter_task->Cancel();
    }

    goldfish::vsock::clear();

    {
        auto& hd = android::crashreport::CrashReporter::GetCrashingHangDetector();

        for (auto& loop : qemu_cpu_loops) {
            hd.RemoveWatchedLooper(loop);
        }

        if (qemu_event_loop) {
            hd.RemoveWatchedLooper(*qemu_event_loop);
        }
    }

    if (vm_state_entry_ != nullptr) {
        qemu_del_vm_change_state_handler(vm_state_entry_);
    }
    input_display_subscription_.reset();

    if (qemu_event_loop) {
        WaitUntilEventLoopsIdle();
        ShutdownQemuLoop();
    }

    // Input clients that outlive the universe may still hold the dispatcher.
    if (input_dispatcher_) {
        input_dispatcher_->Shutdown();
    }
}

void AvdExtendedUniverse::WaitUntilEventLoopsIdle() {
    const auto wait_event_loops_idle = [](size_t n, size_t* c, EventLoop** l) -> bool {
        bool updated = false;
        for (; n > 0; --n, ++c, ++l) {
            const size_t nc = (*l)->WaitUntilIdle();
            if (nc != *c) {
                *c = nc;
                updated = true;
            }
        }
        return updated;
    };

    EventLoop* loops[] = {globalEventLoop(), qemu_event_loop.get()};
    size_t counters[std::size(loops)];
    std::transform(std::begin(loops), std::end(loops), std::begin(counters), [](EventLoop* l) {
        l->ShutdownTimers();
        return l->WaitUntilIdle();
    });

    while (wait_event_loops_idle(std::size(loops), counters, loops)) {
    }
}

void AvdExtendedUniverse::ShutdownQemuLoop() {
    auto f = qemu_event_loop->Shutdown();
    // In the current Qemu implementation, we are already running on the Qemu main thread and so
    // shutdown will have run serially.
    if (f.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        LOG(QFATAL) << "Qemu loop shutdown failed to complete within 15s";
    }
    auto s = f.get();
    LOG_IF(QFATAL, !s.ok()) << "Qemu loop shutdown failed: " << s;
}

async::EventLoop& AvdExtendedUniverse::GetQemuEventLoop() {
    return *qemu_event_loop;
}

goldfish::metrics::MetricsReporter& AvdExtendedUniverse::GetMetricsReporter() {
    return *metrics_reporter;
}

android::goldfish::VmOperations& AvdExtendedUniverse::GetVmOperations() {
    return *vm_operations;
}

display::IMultiDisplay& AvdExtendedUniverse::GetMultiDisplay() const {
    return *multi_display;
}

std::shared_ptr<::goldfish::grpc::v2::IInputDispatcher> AvdExtendedUniverse::GetInputDispatcher() {
    return input_dispatcher_;
}

void AvdExtendedUniverse::InstallKeyboard() {
    DCHECK(keyboard_sink_ == nullptr) << "Keyboard is already installed";
    keyboard_sink_ = std::make_unique<android::emulation::control::keyboard::QemuKeyboardSink>(
            qemu_console_lookup_by_index(0), *qemu_event_loop);
    input_dispatcher_->InstallKeyboardSink(*keyboard_sink_);
}

void AvdExtendedUniverse::OnPreSave() {
    quickboot_metrics_.op_start_time = absl::Now();
}

absl::Status AvdExtendedUniverse::OnSave(archive::IWriter& writer) const {
    auto do_save = [&]() -> absl::Status {
        OnSaveProps(writer);
        OnSavePhysicalState(writer);

        if (auto s = GetMultiDisplay().Save(writer); !s.ok()) {
            LOG(WARNING) << "Failed to save multidisplay state: " << s;
            return absl::UnknownError("-1");
        }
        return absl::OkStatus();
    };
    absl::Status s = do_save();
    if (!IsGenericSnapshotInProgress()) {
        metrics::ReportQuickbootSave(*metrics_reporter, props_->snapshot_name,
                                     /*success=*/s.ok(), quickboot_metrics_);
    }
    return s;
}

void AvdExtendedUniverse::OnSaveProps(archive::IWriter& writer) const {
    const auto& p = Props();
    const std::string_view platform = ::goldfish::version::GetPlatformString();
    const std::string_view full_version = ::goldfish::version::GetEmulatorFullVersion();
    const std::string_view version = ::goldfish::version::GetEmulatorVersion();
    const std::string_view build_id = ::goldfish::version::GetEmulatorBuildId();
    std::string vk_icd = GetCurrentVkIcd();
    LOG(INFO) << "Saving AvdProperties: "
              << "avd_abi=" << p.avd_abi << ", "
              << "avd_api=" << p.avd_api << ", "
              << "build_sdk=" << p.build_sdk << ", "
              << "build_id=" << p.build_id << ", "
              << "build_flavor=" << p.build_flavor << ", "
              << "emulator_full_version=" << full_version << ", "
              << "emulator_version=" << version << ", "
              << "emulator_build_id=" << build_id << ", "
              << "emulator_platform=" << platform << ", "
              << "emulator_vk_icd=" << vk_icd;

    writer << p.avd_abi;
    writer << p.avd_api;
    writer << p.build_sdk;
    writer << p.build_id;
    writer << p.build_flavor;
    writer << full_version;
    writer << version;
    writer << build_id;
    writer << platform;
    writer << vk_icd;

    class HwCfgWriterVisitor {
      public:
        HwCfgWriterVisitor(archive::IWriter& writer) : mWriter(writer) {}

        void operator()(const char* name, bool val) {
            if (!IsExcludedProp(name)) {
                mWriter << val;
                LOG(INFO) << "Saving HWCFG: " << name << "=" << val;
            } else {
                LOG(INFO) << "Not saving HWCFG: " << name << " (excluded)";
            }
        }
        void operator()(const char* name, int32_t val) {
            if (!IsExcludedProp(name)) {
                mWriter << val;
                LOG(INFO) << "Saving HWCFG: " << name << "=" << val;
            } else {
                LOG(INFO) << "Not saving HWCFG: " << name << " (excluded)";
            }
        }
        void operator()(const char* name, const std::string& val) {
            if (!IsExcludedProp(name)) {
                mWriter << val;
                LOG(INFO) << "Saving HWCFG: " << name << "=" << val;
            } else {
                LOG(INFO) << "Not saving HWCFG: " << name << " (excluded)";
            }
        }
        void operator()(const char* name, double val) {
            if (!IsExcludedProp(name)) {
                mWriter << val;
                LOG(INFO) << "Saving HWCFG: " << name << "=" << val;
            } else {
                LOG(INFO) << "Not saving HWCFG: " << name << " (excluded)";
            }
        }
        void operator()(const char* name, const android::goldfish::StorageCapacity& val) {
            if (!IsExcludedProp(name)) {
                mWriter << static_cast<uint64_t>(val.Bytes());
                LOG(INFO) << "Saving HWCFG: " << name << "=" << val.Bytes();
            } else {
                LOG(INFO) << "Not saving HWCFG: " << name << " (excluded)";
            }
        }

      private:
        archive::IWriter& mWriter;
    };

    HwCfgWriterVisitor visitor(writer);
    p.hw_config.Accept(visitor);
}

void AvdExtendedUniverse::OnSavePhysicalState(archive::IWriter& writer) const {
    writer << battery_ << guest_status_ << location_ << sensors_physical_model_;
}

void AvdExtendedUniverse::OnPreLoad() {
    snapshot_loaded_ = false;
    quickboot_metrics_.op_start_time = absl::Now();
}

absl::Status AvdExtendedUniverse::OnLoad(archive::IReader& reader) {
    // Note: Hooking into AvdExtendedUniverse's VMState callbacks only measures the duration of the
    // avd_info device's VMState section rather than the full snapshot load/save duration (RAM,
    // block devices, and all QEMU devices), and does not observe failures that occur before QEMU
    // reaches the avd_info section.
    // TODO: Move to a more comprehensive solution that orchestrates or hooks full quickboot
    // snapshot load/save operations.
    bool config_mismatch = false;
    auto do_load = [&]() -> absl::Status {
        if (absl::Status s = OnLoadProps(reader); !s.ok()) {
            config_mismatch = absl::IsFailedPrecondition(s);
            return s;
        }
        if (absl::Status s = OnLoadPhysicalState(reader); !s.ok()) {
            return s;
        }
        if (absl::Status s = GetMultiDisplay().Load(reader); !s.ok()) {
            LOG(WARNING) << "Failed to load multidisplay state: " << s;
            return s;
        }
        return absl::OkStatus();
    };
    absl::Status s = do_load();
    if (!s.ok() && !IsGenericSnapshotInProgress()) {
        if (config_mismatch) {
            metrics::ReportQuickbootLoad(
                    *metrics_reporter, props_->snapshot_name, /*success=*/false, quickboot_metrics_,
                    android_studio::EMULATOR_SNAPSHOT_FAILURE_REASON_CONFIG_MISMATCH_AVD);
        } else {
            metrics::ReportQuickbootLoad(*metrics_reporter, props_->snapshot_name,
                                         /*success=*/false, quickboot_metrics_);
        }
    }
    return s;
}

absl::Status AvdExtendedUniverse::OnLoadProps(archive::IReader& reader) {
    const auto& p = Props();
    bool ok = true;
    bool stream_ok = true;

    auto check_int32 = [&](const char* name, int32_t val) {
        if (!stream_ok) return;
        int32_t loaded = 0;
        if (const absl::Status s = ReadValue(reader, loaded); s.ok()) {
            if (loaded != val) {
                LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                             << ", expected: " << val << ")";
                ok = false;
            }
        } else {
            LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
            ok = false;
            stream_ok = false;
        }
    };
    auto check_str = [&](const char* name, std::string_view val) {
        if (!stream_ok) return;
        std::string loaded;
        if (const absl::Status s = ReadValue(reader, loaded); s.ok()) {
            if (loaded != val) {
                LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                             << ", expected: " << val << ")";
                ok = false;
            }
        } else {
            LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
            ok = false;
            stream_ok = false;
        }
    };

    const std::string_view platform = ::goldfish::version::GetPlatformString();
    check_str("avd_abi", p.avd_abi);
    check_int32("avd_api", p.avd_api);
    check_str("build_sdk", p.build_sdk);
    check_str("build_id", p.build_id);
    check_str("build_flavor", p.build_flavor);
    check_str("emulator_full_version", ::goldfish::version::GetEmulatorFullVersion());
    check_str("emulator_version", ::goldfish::version::GetEmulatorVersion());
    check_str("emulator_build_id", ::goldfish::version::GetEmulatorBuildId());
    check_str("emulator_platform", platform);

    if (stream_ok) {
        std::string current_vk_icd = GetCurrentVkIcd();
        std::string loaded_vk_icd;
        if (const absl::Status s = ReadValue(reader, loaded_vk_icd); s.ok()) {
            if (loaded_vk_icd != current_vk_icd) {
                LOG(WARNING) << "Property mismatch: emulator_vk_icd (loaded: " << loaded_vk_icd
                             << ", expected: " << current_vk_icd << ")";
                ok = false;
            }
        } else {
            LOG(WARNING) << "Could not load the 'emulator_vk_icd' property: " << s;
            ok = false;
            stream_ok = false;
        }
    }

    class HwCfgReaderVisitor {
      public:
        HwCfgReaderVisitor(archive::IReader& reader, bool& ok, bool& stream_ok)
                : mReader(reader), mOk(ok), mStreamOk(stream_ok) {}

        void operator()(const char* name, bool val) {
            if (!mStreamOk || IsExcludedProp(name)) return;
            bool loaded = false;
            if (const absl::Status s = ReadValue(mReader, loaded); s.ok()) {
                if (loaded != val) {
                    LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                                 << ", expected: " << val << ")";
                    mOk = false;
                }
            } else {
                LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
                mOk = false;
                mStreamOk = false;
            }
        }
        void operator()(const char* name, int32_t val) {
            if (!mStreamOk || IsExcludedProp(name)) return;
            int32_t loaded = 0;
            if (const absl::Status s = ReadValue(mReader, loaded); s.ok()) {
                if (loaded != val) {
                    LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                                 << ", expected: " << val << ")";
                    mOk = false;
                }
            } else {
                LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
                mOk = false;
                mStreamOk = false;
            }
        }
        void operator()(const char* name, const std::string& val) {
            if (!mStreamOk || IsExcludedProp(name)) return;
            std::string loaded;
            if (const absl::Status s = ReadValue(mReader, loaded); s.ok()) {
                if (loaded != val) {
                    LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                                 << ", expected: " << val << ")";
                    mOk = false;
                }
            } else {
                LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
                mOk = false;
                mStreamOk = false;
            }
        }
        void operator()(const char* name, double val) {
            if (!mStreamOk || IsExcludedProp(name)) return;
            double loaded = 0;
            if (const absl::Status s = ReadValue(mReader, loaded); s.ok()) {
                if (loaded != val) {
                    LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                                 << ", expected: " << val << ")";
                    mOk = false;
                }
            } else {
                LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
                mOk = false;
                mStreamOk = false;
            }
        }
        void operator()(const char* name, const android::goldfish::StorageCapacity& val) {
            if (!mStreamOk || IsExcludedProp(name)) return;
            uint64_t loaded = 0;
            if (const absl::Status s = ReadValue(mReader, loaded); s.ok()) {
                if (loaded != val.Bytes()) {
                    LOG(WARNING) << "Property mismatch: " << name << " (loaded: " << loaded
                                 << ", expected: " << val.Bytes() << ")";
                    mOk = false;
                }
            } else {
                LOG(WARNING) << "Could not load the '" << name << "' property: " << s;
                mOk = false;
                mStreamOk = false;
            }
        }

      private:
        archive::IReader& mReader;
        bool& mOk;
        bool& mStreamOk;
    };

    HwCfgReaderVisitor visitor(reader, ok, stream_ok);
    p.hw_config.Accept(visitor);

    if (!stream_ok) {
        return absl::DataLossError("Failed to read AVD properties from snapshot stream");
    }
    if (!ok) {
        return absl::FailedPreconditionError("AVD configuration mismatch in snapshot");
    }
    return absl::OkStatus();
}

absl::Status AvdExtendedUniverse::OnLoadPhysicalState(archive::IReader& reader) {
    return ReadValue(reader, battery_, guest_status_, location_, sensors_physical_model_);
}

void AvdExtendedUniverse::SyncGuestTimeToHost() {
    std::thread([serialNumber = props_->serial_number]() {
        int clientPort = goldfish::adb::AdbHostServer::getClientPort();
        auto last_status = absl::OkStatus();
        for (int retry = 0; retry < 20; ++retry) {
            // We use 'cmd alarm set-time <epoch_ms>' because:
            // 1. Android's shell user (UID 2000) holds android.permission.SET_TIME,
            //    allowing it to work on production user/Play Store images without root
            //    (unlike 'date' or 'hwclock' which fail with EPERM).
            // 2. It immediately updates CLOCK_REALTIME without disabling automatic
            //    time detection (autoDetectionEnabled), so subsequent network (NTP)
            //    or telephony (NITZ) updates continue to work seamlessly.
            // 3. It only requires target epoch milliseconds, without needing to coordinate
            //    with guest elapsedRealtime.
            //
            // Why 'cmd time_detector' commands do not work for us:
            // - 'cmd time_detector suggest_manual_time' is ignored when auto-detection is
            //   enabled unless 'cmd time_detector set_auto_detection_enabled false' is run
            //   first, which permanently disables automatic time detection.
            // - 'suggest_manual_time' and 'suggest_external_time' both require coordinating
            //   guest elapsedRealtime with epoch time over ADB.
            // - External time suggestions ('suggest_external_time') are often lower priority
            //   than network/telephony or disabled by default in device configuration.
            int64_t now_ms = absl::ToUnixMillis(absl::Now());
            std::string cmd = absl::StrCat("cmd alarm set-time ", now_ms);
            auto s = goldfish::adb::AdbHostServer::runShellCommand(cmd, serialNumber, clientPort);
            if (s.ok()) {
                LOG(INFO) << "Updated guest system time to " << now_ms
                          << " via ADB after snapshot restore.";
                return;
            }
            if (absl::IsUnauthenticated(s)) {
                LOG(WARNING) << "Aborting guest time sync for emulator-" << serialNumber
                             << ": ADB authentication mismatch (device is unauthorized).";
                return;
            }
            last_status = s;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        LOG(WARNING) << "Failed to update guest system time via ADB after snapshot restore: "
                     << last_status;
    }).detach();
}

absl::Status AvdExtendedUniverse::OnPostLoad() {
    snapshot_loaded_ = true;
    guest_status_.OnPostLoad();

    if (props_->snapshot_update_time) {
        SyncGuestTimeToHost();
    }

    if (!IsGenericSnapshotInProgress()) {
        metrics::ReportQuickbootLoad(*metrics_reporter, props_->snapshot_name, /*success=*/true,
                                     quickboot_metrics_);
    }

    return absl::OkStatus();
}

void AvdExtendedUniverse::Reset() {
    GetMultiDisplay().Reset();
}

}  // namespace goldfish::avd
