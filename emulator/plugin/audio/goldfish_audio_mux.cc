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

#if defined(__clang__)
#pragma clang diagnostic ignored "-Waddress-of-temporary"
#endif

#include "goldfish/audio/goldfish_audio_mux.h"

#include <string>

#include "absl/log/log.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
extern "C" {
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "qemu/audio.h"
#include "qom/object.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::audio {
namespace {

struct GoldfishAudioBackendState {
    std::string audiodev_id;
    AudioBackend* host_be = nullptr;
    bool torn_down = false;
};

// NOLINTBEGIN(readability-identifier-naming)
struct GoldfishAudioBackend {
    AudioBackend parent_obj;
    GoldfishAudioBackendState* state;
};

#define GOLDFISH_AUDIO_BACKEND(obj) \
    OBJECT_CHECK(GoldfishAudioBackend, (obj), kGoldfishAudioBackendType)

ObjectUnparent* g_parent_backend_unparent = nullptr;

AudioBackend* WrappedHost(AudioBackend* be) {
    auto* self = GOLDFISH_AUDIO_BACKEND(be);
    return (self != nullptr && self->state != nullptr) ? self->state->host_be : nullptr;
}

void TearDownBackendState(GoldfishAudioBackend* self) {
    GoldfishAudioBackendState* state = self->state;
    if (!state || state->torn_down) {
        return;
    }
    state->torn_down = true;

    if (state->host_be) {
        object_unref(OBJECT(state->host_be));
        state->host_be = nullptr;
    }
}

void goldfish_audio_backend_instance_init(Object* obj) {
    auto* self = GOLDFISH_AUDIO_BACKEND(obj);
    self->state = new GoldfishAudioBackendState();
}

void goldfish_audio_backend_unparent(Object* obj) {
    auto* self = GOLDFISH_AUDIO_BACKEND(obj);
    TearDownBackendState(self);
    if (g_parent_backend_unparent) {
        g_parent_backend_unparent(obj);
    }
}

void goldfish_audio_backend_instance_finalize(Object* obj) {
    auto* self = GOLDFISH_AUDIO_BACKEND(obj);
    TearDownBackendState(self);
    delete self->state;
    self->state = nullptr;
}

bool goldfish_audio_backend_realize(AudioBackend* /*be*/, Audiodev* /*dev*/, Error** /*errp*/) {
    return true;
}

const char* goldfish_audio_backend_get_id(AudioBackend* be) {
    auto* self = GOLDFISH_AUDIO_BACKEND(be);
    return (self->state != nullptr) ? self->state->audiodev_id.c_str() : "";
}

SWVoiceOut* goldfish_audio_backend_open_out(AudioBackend* be, SWVoiceOut* sw, const char* name,
                                            void* callback_opaque, audio_callback_fn callback_fn,
                                            const struct audsettings* as) {
    AudioBackend* host = WrappedHost(be);
    if (!host) {
        return nullptr;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->open_out(host, sw, name, callback_opaque, callback_fn,
                                                   as);
}

void goldfish_audio_backend_close_out(AudioBackend* be, SWVoiceOut* sw) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->close_out(host, sw);
}

bool goldfish_audio_backend_is_active_out(AudioBackend* be, SWVoiceOut* sw) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return false;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->is_active_out(host, sw);
}

void goldfish_audio_backend_set_active_out(AudioBackend* be, SWVoiceOut* sw, bool on) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->set_active_out(host, sw, on);
}

void goldfish_audio_backend_set_volume_out(AudioBackend* be, SWVoiceOut* sw, Volume* vol) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw || !vol) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->set_volume_out(host, sw, vol);
}

size_t goldfish_audio_backend_write(AudioBackend* be, SWVoiceOut* sw, void* buf, size_t size) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return 0;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->write(host, sw, buf, size);
}

int goldfish_audio_backend_get_buffer_size_out(AudioBackend* be, SWVoiceOut* sw) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return 0;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->get_buffer_size_out(host, sw);
}

CaptureVoiceOut* goldfish_audio_backend_add_capture(AudioBackend* be, const struct audsettings* as,
                                                    const struct audio_capture_ops* ops,
                                                    void* cb_opaque) {
    AudioBackend* host = WrappedHost(be);
    if (!host) {
        return nullptr;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->add_capture(host, as, ops, cb_opaque);
}

void goldfish_audio_backend_del_capture(AudioBackend* be, CaptureVoiceOut* cap, void* cb_opaque) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !cap) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->del_capture(host, cap, cb_opaque);
}

#ifdef CONFIG_GIO
bool goldfish_audio_backend_set_dbus_server(AudioBackend* be, GDBusObjectManagerServer* manager,
                                            bool p2p, Error** errp) {
    const char* mux_id = goldfish_audio_backend_get_id(be);
    AudioBackend* host = WrappedHost(be);
    if (!host) {
        error_setg(errp,
                   "Failed to attach DBus server to GoldfishAudioBackend '%s': wrapped host "
                   "audiodev is not active.",
                   mux_id);
        return false;
    }
    AudioBackendClass* hk = AUDIO_BACKEND_GET_CLASS(host);
    if (!hk->set_dbus_server) {
        const char* host_id = audio_be_get_id(host);
        error_setg(errp,
                   "Failed to attach DBus server to GoldfishAudioBackend '%s': wrapped host "
                   "audiodev '%s' does not implement set_dbus_server.",
                   mux_id, host_id ? host_id : "<unknown>");
        return false;
    }
    return hk->set_dbus_server(host, manager, p2p, errp);
}
#endif

SWVoiceIn* goldfish_audio_backend_open_in(AudioBackend* be, SWVoiceIn* sw, const char* name,
                                          void* callback_opaque, audio_callback_fn callback_fn,
                                          const struct audsettings* as) {
    AudioBackend* host = WrappedHost(be);
    if (!host) {
        return nullptr;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->open_in(host, sw, name, callback_opaque, callback_fn, as);
}

void goldfish_audio_backend_close_in(AudioBackend* be, SWVoiceIn* sw) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->close_in(host, sw);
}

bool goldfish_audio_backend_is_active_in(AudioBackend* be, SWVoiceIn* sw) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return false;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->is_active_in(host, sw);
}

void goldfish_audio_backend_set_active_in(AudioBackend* be, SWVoiceIn* sw, bool on) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->set_active_in(host, sw, on);
}

void goldfish_audio_backend_set_volume_in(AudioBackend* be, SWVoiceIn* sw, Volume* vol) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw || !vol) {
        return;
    }
    AUDIO_BACKEND_GET_CLASS(host)->set_volume_in(host, sw, vol);
}

size_t goldfish_audio_backend_read(AudioBackend* be, SWVoiceIn* sw, void* buf, size_t size) {
    AudioBackend* host = WrappedHost(be);
    if (!host || !sw || !buf || size == 0) {
        return 0;
    }
    return AUDIO_BACKEND_GET_CLASS(host)->read(host, sw, buf, size);
}

void goldfish_audio_backend_class_init(ObjectClass* oc, const void* /*data*/) {
    g_parent_backend_unparent = oc->unparent;
    oc->unparent = goldfish_audio_backend_unparent;

    AudioBackendClass* abc = AUDIO_BACKEND_CLASS(oc);
    abc->realize = goldfish_audio_backend_realize;
    abc->get_id = goldfish_audio_backend_get_id;
    abc->open_out = goldfish_audio_backend_open_out;
    abc->close_out = goldfish_audio_backend_close_out;
    abc->is_active_out = goldfish_audio_backend_is_active_out;
    abc->set_active_out = goldfish_audio_backend_set_active_out;
    abc->set_volume_out = goldfish_audio_backend_set_volume_out;
    abc->write = goldfish_audio_backend_write;
    abc->get_buffer_size_out = goldfish_audio_backend_get_buffer_size_out;
    abc->add_capture = goldfish_audio_backend_add_capture;
    abc->del_capture = goldfish_audio_backend_del_capture;
#ifdef CONFIG_GIO
    abc->set_dbus_server = goldfish_audio_backend_set_dbus_server;
#endif
    abc->open_in = goldfish_audio_backend_open_in;
    abc->close_in = goldfish_audio_backend_close_in;
    abc->is_active_in = goldfish_audio_backend_is_active_in;
    abc->set_active_in = goldfish_audio_backend_set_active_in;
    abc->set_volume_in = goldfish_audio_backend_set_volume_in;
    abc->read = goldfish_audio_backend_read;
}

const TypeInfo goldfish_audio_backend_type_info = {
    .name = kGoldfishAudioBackendType,
    .parent = TYPE_AUDIO_BACKEND,
    .instance_size = sizeof(GoldfishAudioBackend),
    .instance_init = goldfish_audio_backend_instance_init,
    .instance_finalize = goldfish_audio_backend_instance_finalize,
    .class_init = goldfish_audio_backend_class_init,
};

struct GoldfishAudioMuxConfig {
    std::string host_audiodev = kDefaultHostAudiodevId;
    std::string audiodev_id = kDefaultMainAudiodevId;
};

struct GoldfishAudioMuxDev {
    DeviceState parent_obj;
    GoldfishAudioMuxConfig* config;
};

#define GOLDFISH_AUDIO_MUX_DEV(obj) \
    OBJECT_CHECK(GoldfishAudioMuxDev, (obj), kGoldfishAudioMuxDeviceType)

char* mux_get_host_audiodev(Object* obj, Error** /*errp*/) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(obj);
    return g_strdup(dev->config->host_audiodev.c_str());
}

void mux_set_host_audiodev(Object* obj, const char* value, Error** /*errp*/) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(obj);
    if (value) {
        dev->config->host_audiodev = value;
    }
}

char* mux_get_audiodev_id(Object* obj, Error** /*errp*/) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(obj);
    return g_strdup(dev->config->audiodev_id.c_str());
}

void mux_set_audiodev_id(Object* obj, const char* value, Error** /*errp*/) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(obj);
    if (value) {
        dev->config->audiodev_id = value;
    }
}

void goldfish_audio_mux_dev_instance_init(Object* obj) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(obj);
    dev->config = new GoldfishAudioMuxConfig();
}

void goldfish_audio_mux_dev_instance_finalize(Object* obj) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(obj);
    delete dev->config;
    dev->config = nullptr;
}

void goldfish_audio_mux_dev_realize(DeviceState* qdev, Error** errp) {
    auto* dev = GOLDFISH_AUDIO_MUX_DEV(qdev);
    if (dev->config->host_audiodev.empty() || dev->config->audiodev_id.empty()) {
        error_setg(errp,
                   "Failed to realize goldfish-audio-mux: 'host-audiodev' ('%s') and "
                   "'audiodev-id' ('%s') must both be non-empty (e.g. '-device "
                   "goldfish-audio-mux,host-audiodev=%s,audiodev-id=%s').",
                   dev->config->host_audiodev.c_str(), dev->config->audiodev_id.c_str(),
                   kDefaultHostAudiodevId, kDefaultMainAudiodevId);
        return;
    }
    if (dev->config->host_audiodev == dev->config->audiodev_id) {
        error_setg(errp,
                   "Failed to realize goldfish-audio-mux: 'host-audiodev' and 'audiodev-id' must "
                   "have distinct QOM IDs (both were '%s'). Specify e.g. 'host-audiodev=%s' and "
                   "'audiodev-id=%s'.",
                   dev->config->audiodev_id.c_str(), kDefaultHostAudiodevId,
                   kDefaultMainAudiodevId);
        return;
    }

    AudioBackend* host_be = audio_be_by_name(dev->config->host_audiodev.c_str(), errp);
    if (!host_be) {
        return;
    }

    Object* audiodevs_root = object_resolve_path_component(object_get_root(), "audiodevs");
    if (!audiodevs_root) {
        error_setg(errp,
                   "Failed to realize goldfish-audio-mux for '%s' -> '%s': QOM container "
                   "'/audiodevs' does not exist. Ensure '-audiodev' is initialized before "
                   "'-device goldfish-audio-mux'.",
                   dev->config->host_audiodev.c_str(), dev->config->audiodev_id.c_str());
        return;
    }

    auto* mux_be = GOLDFISH_AUDIO_BACKEND(object_new(kGoldfishAudioBackendType));
    GoldfishAudioBackendState* state = mux_be->state;
    state->audiodev_id = dev->config->audiodev_id;
    object_ref(OBJECT(host_be));
    state->host_be = host_be;

    if (!object_property_try_add_child(audiodevs_root, state->audiodev_id.c_str(), OBJECT(mux_be),
                                       errp)) {
        TearDownBackendState(mux_be);
        object_unref(OBJECT(mux_be));
        return;
    }
    object_unref(OBJECT(mux_be));

    VLOG(1) << "Registered GoldfishAudioBackend '/audiodevs/" << state->audiodev_id
            << "' wrapping '/audiodevs/" << dev->config->host_audiodev << "'.";
}

void goldfish_audio_mux_dev_class_init(ObjectClass* oc, const void* /*data*/) {
    object_class_property_add_str(oc, "host-audiodev", mux_get_host_audiodev,
                                  mux_set_host_audiodev);
    object_class_property_set_description(
            oc, "host-audiodev", "ID of the underlying host AudioBackend under /audiodevs.");

    object_class_property_add_str(oc, "audiodev-id", mux_get_audiodev_id, mux_set_audiodev_id);
    object_class_property_set_description(oc, "audiodev-id",
                                          "ID under /audiodevs exposed to guest audio devices.");

    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = goldfish_audio_mux_dev_realize;
}

const TypeInfo goldfish_audio_mux_dev_type_info = {
    .name = kGoldfishAudioMuxDeviceType,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GoldfishAudioMuxDev),
    .instance_init = goldfish_audio_mux_dev_instance_init,
    .instance_finalize = goldfish_audio_mux_dev_instance_finalize,
    .class_init = goldfish_audio_mux_dev_class_init,
};
// NOLINTEND(readability-identifier-naming)

}  // namespace

void audio_mux_register_types() {
    type_register_static(&goldfish_audio_backend_type_info);
    type_register_static(&goldfish_audio_mux_dev_type_info);
}

}  // namespace goldfish::audio
