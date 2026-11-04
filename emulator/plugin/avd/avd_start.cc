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

#include "goldfish/avd/avd_start.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"

#include "android/goldfish/ini_file.h"
#include "avd_extended_universe.h"
#include "avd_start_internal.h"
#include "goldfish/archive/qemu_file_reader.h"
#include "goldfish/archive/qemu_file_writer.h"
#include "goldfish/avd/avd_info.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/file/file.h"
#include "host-common/constants.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
extern "C" {
#include "hw/core/qdev.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qom/object.h"
}
#undef listen
#undef shutdown
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::avd {

namespace {

namespace fs = std::filesystem;

struct AvdInfoDev {
    DeviceClass parent_class;
    // `mutable_props` is valid only between `instance_init` and `realize`.
    // It moves into `universe` in `realize` and stays there as immutable.
    AvdProperties* mutable_props;
    AvdExtendedUniverse* universe;  // deleted in `avd_info_instance_finalize`
};

#define TYPE_AVD "avdstart"
#define AVD_INFO_DEV(obj) OBJECT_CHECK(AvdInfoDev, (obj), TYPE_AVD)
#define AVD_INFO_DEVICE_GET_CLASS(obj) OBJECT_GET_CLASS(AvdInfoDev, obj, TYPE_AVD)

}  // namespace

void UniverseBuildComplete() {
    auto* u = static_cast<AvdExtendedUniverse*>(GetNullableAvd());
    DCHECK(u);
    u->connector_registry.Listen(5000);
    u->test_tools_connector_registry.Listen(5002);
    // `avdend` is realized after the display devices, so console 0 exists.
    u->InstallKeyboard();
}

namespace {
absl::Status ValidateAvdProps(AvdProperties& avd_props) {
    if (avd_props.serial_number <= 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "serial_number is unspecified (it must be > 0): %d", avd_props.serial_number));
    }

    if (avd_props.adb_port <= 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "adb_port is unspecified (it must be > 0): %d", avd_props.adb_port));
    }

    if (avd_props.metrics_session_id == ::goldfish::metrics::Uuid::Zero() &&
        avd_props.metrics_writer_config.type != goldfish::metrics::MetricsWriterType::kNone) {
        return absl::InvalidArgumentError(
                "metrics_session_id should be non-zero when metrics_writer is set");
    }

    if (avd_props.avd_content_path.empty()) {
        return absl::InvalidArgumentError("avd_content_path is unspecified");
    }

    if (avd_props.avd_api_str.empty()) {
        return absl::InvalidArgumentError("avd_api_str is unspecified");
    }

    fs::path hw_path = avd_props.avd_content_path / CORE_HARDWARE_INI;
    auto hw_ini = std::make_unique<android::goldfish::IniFile>(hw_path);
    if (!hw_ini->Read()) {
        return absl::NotFoundError(
                absl::StrFormat("Failed to parse hardware ini: %s", hw_path.string()));
    }

    avd_props.hw_config.Load(*hw_ini);
    return absl::OkStatus();
}

AvdProperties& toMutableAvdProperties(Object* obj) {
    DCHECK(obj);
    AvdInfoDev* avd_info = AVD_INFO_DEV(obj);
    DCHECK(avd_info);
    DCHECK(avd_info->mutable_props);
    return *avd_info->mutable_props;
}

AvdExtendedUniverse& toAvdExtendedUniverse(void* opaque) {
    DCHECK(opaque);
    AvdInfoDev* avd_info = AVD_INFO_DEV(opaque);
    DCHECK(avd_info);
    DCHECK(avd_info->universe);
    return *avd_info->universe;
}

void avd_info_realize(DeviceState* dev, Error** errp) {
    VLOG(1) << "avd_info_realize: " << object_get_canonical_path(OBJECT(dev));

    AvdInfoDev* avd_info = AVD_INFO_DEV(dev);
    DCHECK(avd_info);

    VLOG(1) << "Device configuration, AVD name: '" << avd_info->mutable_props->avd_name << "'";

    if (auto s = ValidateAvdProps(*avd_info->mutable_props); !s.ok()) {
        error_setg(errp, "%s", s.ToString().c_str());
        return;
    }

    auto universe = AvdExtendedUniverse::Create(
            std::unique_ptr<AvdProperties>(std::exchange(avd_info->mutable_props, nullptr)));
    if (!universe.ok()) {
        error_setg(errp, "%s", universe.status().ToString().c_str());
        return;
    }
    avd_info->universe = universe->release();

    // Make avd universe visible to other modules.
    SetGlobalAvdUniverse(avd_info->universe);
}

void avd_info_set_serial_number(Object* obj, Visitor* v, const char* name, void* opaque,
                                Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    toMutableAvdProperties(obj).serial_number = value;
}

void avd_info_set_adb_port(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    toMutableAvdProperties(obj).adb_port = value;
}

void avd_info_set_avd_name(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).avd_name = value;
}

void avd_info_set_avd_id(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).avd_id = value;
}

void avd_info_set_avd_abi(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).avd_abi = value;
}

void avd_info_set_avd_api(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    toMutableAvdProperties(obj).avd_api = value;
}

void avd_info_set_avd_api_str(Object* obj, const char* value, Error** errp) {
    if (value) {
        toMutableAvdProperties(obj).avd_api_str = value;
    } else {
        toMutableAvdProperties(obj).avd_api_str.clear();
    }
}

void avd_info_set_avd_type(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    toMutableAvdProperties(obj).avd_type = static_cast<android::goldfish::DeviceType>(value);
}

void avd_info_set_avd_dir(Object* obj, const char* value, Error** errp) {
    fs::path dir(value);
    if (!android::base::file::is_dir(dir)) {
        error_setg(errp, "avd_dir specified is not a valid directory: %s", value);
        return;
    }

    toMutableAvdProperties(obj).avd_content_path = dir;
}

void avd_info_set_build_sdk(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).build_sdk = value;
}

void avd_info_set_build_id(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).build_id = value;
}

void avd_info_set_build_flavor(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).build_flavor = value;
}

void avd_info_set_build_fingerprint(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).build_fingerprint = value;
}

void avd_info_set_snapshot_name(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).snapshot_name = value;
}

void avd_info_set_snapshot_update_time(Object* obj, bool value, Error** errp) {
    toMutableAvdProperties(obj).snapshot_update_time = value;
}

void avd_info_set_snapshots_enabled(Object* obj, bool value, Error** errp) {
    toMutableAvdProperties(obj).snapshots_enabled = value;
}

void avd_info_set_snapshot_version(Object* obj, Visitor* v, const char* name, void* opaque,
                                   Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    toMutableAvdProperties(obj).snapshot_version = value;
}

void avd_info_set_quit_after_boot_timeout(Object* obj, Visitor* v, const char* name, void* opaque,
                                          Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    toMutableAvdProperties(obj).quit_after_boot_timeout_seconds = value;
}

void avd_info_set_metrics_session(Object* obj, const char* value, Error** errp) {
    if (auto s = ::goldfish::metrics::Uuid::FromString(value); !s.ok()) {
        error_setg(errp, "metrics_session - failed to parse UUID: %s - %s",
                   s.status().ToString().c_str(), value);
        return;
    } else {
        toMutableAvdProperties(obj).metrics_session_id = *std::move(s);
    }
}

void avd_info_set_metrics_writer(Object* obj, Visitor* v, const char* name, void* opaque,
                                 Error** errp) {
    int32_t value;
    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    toMutableAvdProperties(obj).metrics_writer_config.type =
            static_cast<goldfish::metrics::MetricsWriterType>(value);
}

void avd_info_set_metrics_file_path(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).metrics_writer_config.file_path = value;
}

void avd_info_set_metrics_spool_dir(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).metrics_writer_config.studio_spool_dir = value;
}

void avd_info_set_metrics_playstore_url(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).metrics_writer_config.playstore_url = value;
}

void avd_info_set_metrics_user_id(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).metrics_writer_config.user_id = value;
}

void avd_info_set_dump_perf_stat_path(Object* obj, const char* value, Error** errp) {
    fs::path path(value);
    if (!android::base::file::is_dir(path.parent_path())) {
        error_setg(errp, "dump_perf_stat_path parent is not a valid directory: %s", value);
        return;
    }

    toMutableAvdProperties(obj).dump_perf_stat_path = path;
}

void avd_info_set_icc_profile(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).icc_profile = std::filesystem::path(value);
}

void avd_info_set_timezone(Object* obj, const char* value, Error** errp) {
    toMutableAvdProperties(obj).timezone = value;
}

void avd_info_unrealize(DeviceState* dev) {
    VLOG(1) << "avd_info_unrealize";
    SetGlobalAvdUniverse(nullptr);
}

void avd_info_reset(Object* obj, ResetType) {
    VLOG(1) << "avd_info_reset";
    toAvdExtendedUniverse(obj).Reset();
}

int avd_info_pre_load(void* opaque) {
    toAvdExtendedUniverse(opaque).OnPreLoad();
    return 0;
}

int avd_info_vmstate_impl_get(QEMUFile* f, void* pv, size_t size, const VMStateField* field) {
    goldfish::archive::QEMUFileReader reader(f);
    const absl::Status s = toAvdExtendedUniverse(pv).OnLoad(reader);
    if (s.ok()) {
        return 0;
    } else {
        LOG(ERROR) << "OnLoad failed: " << s;
        return -1;
    }
}

int avd_info_post_load(void* opaque, int version_id) {
    const absl::Status s = toAvdExtendedUniverse(opaque).OnPostLoad();
    if (s.ok()) {
        return 0;
    } else {
        LOG(ERROR) << "OnPostLoad failed: " << s;
        return -1;
    }
}

int avd_info_pre_save(void* opaque) {
    toAvdExtendedUniverse(opaque).OnPreSave();
    return 0;
}

int avd_info_vmstate_impl_put(QEMUFile* f, void* pv, size_t size, const VMStateField* field,
                              JSONWriter* vmdes) {
    goldfish::archive::QEMUFileWriter writer(f);
    const absl::Status s = toAvdExtendedUniverse(pv).OnSave(writer);
    if (s.ok()) {
        return 0;
    } else {
        LOG(ERROR) << "OnSave failed: " << s;
        return -1;
    }
}

int avd_info_post_save(void* opaque) {
    toAvdExtendedUniverse(opaque).OnPostSave();
    return 0;
}

const VMStateInfo avd_info_vmstate_impl = {
    .name = "virtio_snd_device_remaining",
    .get = avd_info_vmstate_impl_get,
    .put = avd_info_vmstate_impl_put,
};

const VMStateDescription avd_info_vmsd = {
    .name = "avd_info",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = &avd_info_pre_load,
    .post_load = &avd_info_post_load,
    .pre_save = &avd_info_pre_save,
    .post_save = &avd_info_post_save,
    .fields = (const VMStateField[]){{
                                         .name = "impl",
                                         .info = &avd_info_vmstate_impl,
                                         .flags = VMS_SINGLE,
                                     },
                                     VMSTATE_END_OF_LIST()}};

void avd_info_class_init(ObjectClass* oc, const void* data) {
    object_class_property_add(oc, "serial_number", "int", nullptr, avd_info_set_serial_number,
                              nullptr, nullptr);
    object_class_property_add(oc, "adb_port", "int", nullptr, avd_info_set_adb_port, nullptr,
                              nullptr);

    object_class_property_add_str(oc, "avd_name", nullptr, avd_info_set_avd_name);
    object_class_property_add_str(oc, "avd_id", nullptr, avd_info_set_avd_id);
    object_class_property_add_str(oc, "avd_abi", nullptr, avd_info_set_avd_abi);
    object_class_property_add(oc, "avd_api", "int", nullptr, avd_info_set_avd_api, nullptr,
                              nullptr);
    object_class_property_add_str(oc, "avd_api_str", nullptr, avd_info_set_avd_api_str);
    object_class_property_add(oc, "avd_type", "int", nullptr, avd_info_set_avd_type, nullptr,
                              nullptr);
    object_class_property_add_str(oc, "avd_dir", nullptr, avd_info_set_avd_dir);

    object_class_property_add_str(oc, "build_sdk", nullptr, avd_info_set_build_sdk);
    object_class_property_add_str(oc, "build_id", nullptr, avd_info_set_build_id);
    object_class_property_add_str(oc, "build_flavor", nullptr, avd_info_set_build_flavor);
    object_class_property_add_str(oc, "build_fingerprint", nullptr, avd_info_set_build_fingerprint);
    object_class_property_add_str(oc, "snapshot_name", nullptr, avd_info_set_snapshot_name);
    object_class_property_add_bool(oc, "snapshot_update_time", nullptr,
                                   avd_info_set_snapshot_update_time);
    object_class_property_add_bool(oc, "snapshots_enabled", nullptr,
                                   avd_info_set_snapshots_enabled);
    object_class_property_add(oc, "snapshot_version", "int", nullptr,
                              avd_info_set_snapshot_version, nullptr, nullptr);

    object_class_property_add(oc, "quit_after_boot_timeout", "int", nullptr,
                              avd_info_set_quit_after_boot_timeout, nullptr, nullptr);

    object_class_property_add_str(oc, "metrics_session", nullptr, avd_info_set_metrics_session);
    object_class_property_add(oc, "metrics_writer", "int", nullptr, avd_info_set_metrics_writer,
                              nullptr, nullptr);
    object_class_property_add_str(oc, "metrics_file_path", nullptr, avd_info_set_metrics_file_path);
    object_class_property_add_str(oc, "metrics_spool_dir", nullptr, avd_info_set_metrics_spool_dir);
    object_class_property_add_str(oc, "metrics_playstore_url", nullptr,
                                  avd_info_set_metrics_playstore_url);
    object_class_property_add_str(oc, "metrics_user_id", nullptr, avd_info_set_metrics_user_id);
    object_class_property_add_str(oc, "dump_perf_stat_path", nullptr,
                                  avd_info_set_dump_perf_stat_path);
    object_class_property_add_str(oc, "icc_profile", nullptr, avd_info_set_icc_profile);
    object_class_property_add_str(oc, "timezone", nullptr, avd_info_set_timezone);

    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = avd_info_realize;
    dc->unrealize = avd_info_unrealize;
    dc->vmsd = &avd_info_vmsd;

    ResettableClass* rc = RESETTABLE_CLASS(oc);
    rc->phases.hold = avd_info_reset;
}

void avd_info_instance_init(Object* obj) {
    AVD_INFO_DEV(obj)->mutable_props = new AvdProperties();
    add_deletable_object(obj);
}

void avd_info_instance_finalize(Object* obj) {
    VLOG(1) << "avd_info_instance_finalize";
    AvdInfoDev* avd_info = AVD_INFO_DEV(obj);

    DCHECK(!GetNullableAvd());

    delete avd_info->universe;
    delete avd_info->mutable_props;
}

const TypeInfo avd_info_type_info = {
    .name = TYPE_AVD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(AvdInfoDev),
    .instance_init = avd_info_instance_init,
    .instance_finalize = avd_info_instance_finalize,
    .class_init = avd_info_class_init,
};

}  // namespace

void avd_info_register_types(void) {
    type_register_static(&avd_info_type_info);
}

}  // namespace goldfish::avd
