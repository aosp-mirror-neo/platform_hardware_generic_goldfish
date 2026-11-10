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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/time.h"

#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/pcm_format.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
extern "C" {
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/runstate.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::audio {
namespace {

constexpr int64_t kTimerPeriodNs = 10'000'000;  // 10 ms virtual-clock cadence.
constexpr char kDefaultInputVoiceName[] = "goldfish.in";

GuestPcmFormat ToGuestPcmFormat(AudioFormat fmt) {
    switch (fmt) {
    case AUDIO_FORMAT_S8:
        return GuestPcmFormat::kS8;
    case AUDIO_FORMAT_U8:
        return GuestPcmFormat::kU8;
    case AUDIO_FORMAT_S16:
        return GuestPcmFormat::kS16;
    case AUDIO_FORMAT_U16:
        return GuestPcmFormat::kU16;
    case AUDIO_FORMAT_S32:
        return GuestPcmFormat::kS32;
    case AUDIO_FORMAT_U32:
        return GuestPcmFormat::kU32;
    case AUDIO_FORMAT_F32:
        return GuestPcmFormat::kF32;
    case AUDIO_FORMAT__MAX:
        break;
    }
    return GuestPcmFormat::kS16;
}

AudioFormat ToQemuAudioFormat(GuestPcmFormat fmt) {
    switch (fmt) {
    case GuestPcmFormat::kS8:
        return AUDIO_FORMAT_S8;
    case GuestPcmFormat::kU8:
        return AUDIO_FORMAT_U8;
    case GuestPcmFormat::kS16:
        return AUDIO_FORMAT_S16;
    case GuestPcmFormat::kU16:
        return AUDIO_FORMAT_U16;
    case GuestPcmFormat::kS32:
        return AUDIO_FORMAT_S32;
    case GuestPcmFormat::kU32:
        return AUDIO_FORMAT_U32;
    case GuestPcmFormat::kF32:
        return AUDIO_FORMAT_F32;
    }
    return AUDIO_FORMAT_S16;
}

GuestAudioConfig ToGuestAudioConfig(const audsettings& as) {
    return GuestAudioConfig{
        .sample_rate_hz = static_cast<uint32_t>(std::max(1, as.freq)),
        .channels = static_cast<uint32_t>(
                std::clamp(as.nchannels, 1, static_cast<int>(kMaxAudioChannels))),
        .format = ToGuestPcmFormat(as.fmt),
        .big_endian = as.big_endian,
    };
}

audsettings ToQemuAudSettings(const GuestAudioConfig& cfg) {
    audsettings as = {};
    as.freq = static_cast<int>(cfg.sample_rate_hz);
    as.nchannels = static_cast<int>(cfg.channels);
    as.fmt = ToQemuAudioFormat(cfg.format);
    as.big_endian = cfg.big_endian;
    return as;
}

class QemuHostAudioAdapter final : public HostAudioInputInterface {
  public:
    struct HostVoiceContext {
        SWVoiceIn* host_sw = nullptr;
        GuestDataCallback on_host_data;

        static void OnHostCallbackStatic(void* opaque, int avail) {
            auto* ctx = static_cast<HostVoiceContext*>(opaque);
            if (ctx && ctx->on_host_data) {
                ctx->on_host_data(avail);
            }
        }
    };

    QemuHostAudioAdapter(AudioBackend* host_be, QEMUTimer* timer)
            : host_be_(host_be), timer_(timer) {}

    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* name,
                                  const GuestAudioConfig& config,
                                  GuestDataCallback on_host_data) override {
        if (!host_be_) {
            return nullptr;
        }
        AudioBackendClass* hk = AUDIO_BACKEND_GET_CLASS(host_be_);
        if (!hk || !hk->open_in) {
            return nullptr;
        }

        const char* voice_name =
                (name != nullptr && name[0] != '\0') ? name : kDefaultInputVoiceName;
        const audsettings as = ToQemuAudSettings(config);
        if (existing != nullptr) {
            auto* ctx = static_cast<HostVoiceContext*>(existing);
            ctx->on_host_data = std::move(on_host_data);
            SWVoiceIn* reopened = hk->open_in(host_be_, ctx->host_sw, voice_name, ctx,
                                              &HostVoiceContext::OnHostCallbackStatic, &as);
            if (!reopened) {
                // QEMU's audio_mixeng_backend_open_in closes the previous SWVoiceIn on every
                // failure path before returning nullptr; calling hk->close_in here would
                // double-free ctx->host_sw.
                delete ctx;
                return nullptr;
            }
            ctx->host_sw = reopened;
            return ctx;
        }

        auto ctx = std::make_unique<HostVoiceContext>();
        ctx->on_host_data = std::move(on_host_data);
        SWVoiceIn* created = hk->open_in(host_be_, nullptr, voice_name, ctx.get(),
                                         &HostVoiceContext::OnHostCallbackStatic, &as);
        if (!created) {
            return nullptr;
        }
        ctx->host_sw = created;
        return ctx.release();
    }

    void CloseHostVoice(HostVoiceHandle voice) override {
        auto* ctx = static_cast<HostVoiceContext*>(voice);
        if (!ctx) {
            return;
        }
        if (const auto resolved = ResolveActiveHostVoice(voice);
            resolved.has_value() && resolved->hk->close_in) {
            resolved->hk->close_in(host_be_, resolved->ctx->host_sw);
        }
        delete ctx;
    }

    void SetHostVoiceActive(HostVoiceHandle voice, bool active) override {
        const auto resolved = ResolveActiveHostVoice(voice);
        if (resolved.has_value() && resolved->hk->set_active_in) {
            resolved->hk->set_active_in(host_be_, resolved->ctx->host_sw, active);
        }
    }

    void SetHostVoiceVolume(HostVoiceHandle voice, const GuestChannelVolume& volume) override {
        const auto resolved = ResolveActiveHostVoice(voice);
        if (!resolved.has_value() || !resolved->hk->set_volume_in) {
            return;
        }
        Volume qvol = {};
        qvol.mute = volume.mute;
        qvol.channels = static_cast<int>(std::min(volume.channels, kMaxAudioChannels));
        for (int i = 0; i < qvol.channels; ++i) {
            qvol.vol[i] = volume.gain[static_cast<size_t>(i)];
        }
        resolved->hk->set_volume_in(host_be_, resolved->ctx->host_sw, &qvol);
    }

    size_t ReadHostVoice(HostVoiceHandle voice, void* dst, size_t byte_count) override {
        const auto resolved = ResolveActiveHostVoice(voice);
        if (!resolved.has_value() || !resolved->hk->read) {
            return 0;
        }
        return resolved->hk->read(host_be_, resolved->ctx->host_sw, dst, byte_count);
    }

    void SetTimerArmed(bool armed) override {
        timer_armed_ = armed;
        if (!timer_) {
            return;
        }
        if (armed) {
            RearmTimerIfArmed();
        } else {
            timer_del(timer_);
        }
    }

    absl::Duration NowVirtualTime() const override {
        return absl::Nanoseconds(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }

    void RearmTimerIfArmed() {
        if (timer_armed_ && timer_ != nullptr) {
            timer_mod_ns(timer_, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + kTimerPeriodNs);
        }
    }

    void DetachResources() {
        timer_armed_ = false;
        timer_ = nullptr;
        host_be_ = nullptr;
    }

  private:
    struct ActiveHostVoice {
        HostVoiceContext* ctx = nullptr;
        AudioBackendClass* hk = nullptr;
    };

    std::optional<ActiveHostVoice> ResolveActiveHostVoice(HostVoiceHandle voice) const {
        auto* ctx = static_cast<HostVoiceContext*>(voice);
        if (!ctx || !ctx->host_sw || !host_be_) {
            return std::nullopt;
        }
        AudioBackendClass* hk = AUDIO_BACKEND_GET_CLASS(host_be_);
        if (!hk) {
            return std::nullopt;
        }
        return ActiveHostVoice{.ctx = ctx, .hk = hk};
    }

    AudioBackend* host_be_ = nullptr;
    QEMUTimer* timer_ = nullptr;
    bool timer_armed_ = false;
};

struct GoldfishAudioBackendState {
    std::string audiodev_id;
    AudioBackend* host_be = nullptr;
    QEMUTimer* timer = nullptr;
    VMChangeStateEntry* vm_state_entry = nullptr;
    std::unique_ptr<QemuHostAudioAdapter> host_adapter;
    std::shared_ptr<AudioInputMux> mux;
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

AudioInputMux* BackendMux(AudioBackend* be) {
    auto* self = GOLDFISH_AUDIO_BACKEND(be);
    return (self != nullptr && self->state != nullptr) ? self->state->mux.get() : nullptr;
}

void goldfish_audio_backend_on_timer(void* opaque) {
    auto* state = static_cast<GoldfishAudioBackendState*>(opaque);
    if (!state || state->torn_down || !state->mux || !state->host_adapter) {
        return;
    }
    state->host_adapter->RearmTimerIfArmed();
    state->mux->OnTimerTick();
}

void goldfish_audio_backend_on_vm_state(void* opaque, bool running, RunState /*runstate*/) {
    auto* state = static_cast<GoldfishAudioBackendState*>(opaque);
    if (!state || state->torn_down || !state->mux) {
        return;
    }
    state->mux->OnVmRunningChanged(running);
}

void TearDownBackendState(GoldfishAudioBackend* self) {
    GoldfishAudioBackendState* state = self->state;
    if (!state || state->torn_down) {
        return;
    }
    state->torn_down = true;

    UnregisterAudioInputMux(AUDIO_BACKEND(self));
    if (state->mux) {
        state->mux->Shutdown();
    }
    if (state->host_adapter) {
        state->host_adapter->DetachResources();
    }
    if (state->vm_state_entry) {
        qemu_del_vm_change_state_handler(state->vm_state_entry);
        state->vm_state_entry = nullptr;
    }
    if (state->timer) {
        timer_free(state->timer);
        state->timer = nullptr;
    }
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
    AudioInputMux* mux = BackendMux(be);
    if (!mux || !as || !callback_fn) {
        return nullptr;
    }
    MuxVoiceHandle* handle = mux->OpenVoice(
            reinterpret_cast<MuxVoiceHandle*>(sw), name ? name : kDefaultInputVoiceName,
            ToGuestAudioConfig(*as),
            [callback_fn, callback_opaque](int avail) { callback_fn(callback_opaque, avail); });
    return reinterpret_cast<SWVoiceIn*>(handle);
}

void goldfish_audio_backend_close_in(AudioBackend* be, SWVoiceIn* sw) {
    AudioInputMux* mux = BackendMux(be);
    if (!mux || !sw) {
        return;
    }
    mux->CloseVoice(reinterpret_cast<MuxVoiceHandle*>(sw));
}

bool goldfish_audio_backend_is_active_in(AudioBackend* be, SWVoiceIn* sw) {
    AudioInputMux* mux = BackendMux(be);
    if (!mux || !sw) {
        return false;
    }
    return mux->IsVoiceActive(reinterpret_cast<const MuxVoiceHandle*>(sw));
}

void goldfish_audio_backend_set_active_in(AudioBackend* be, SWVoiceIn* sw, bool on) {
    AudioInputMux* mux = BackendMux(be);
    if (!mux || !sw) {
        return;
    }
    mux->SetVoiceActive(reinterpret_cast<MuxVoiceHandle*>(sw), on);
}

void goldfish_audio_backend_set_volume_in(AudioBackend* be, SWVoiceIn* sw, Volume* vol) {
    AudioInputMux* mux = BackendMux(be);
    if (!mux || !sw || !vol) {
        return;
    }
    GuestChannelVolume gvol;
    gvol.mute = vol->mute;
    gvol.channels = static_cast<uint32_t>(
            std::clamp(vol->channels, 1, static_cast<int>(kMaxAudioChannels)));
    for (uint32_t i = 0; i < gvol.channels; ++i) {
        gvol.gain[i] = vol->vol[i];
    }
    mux->SetVoiceVolume(reinterpret_cast<MuxVoiceHandle*>(sw), gvol);
}

size_t goldfish_audio_backend_read(AudioBackend* be, SWVoiceIn* sw, void* buf, size_t size) {
    AudioInputMux* mux = BackendMux(be);
    if (!mux || !sw || !buf || size == 0) {
        return 0;
    }
    return mux->ReadVoice(reinterpret_cast<MuxVoiceHandle*>(sw), buf, size);
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
    // The host microphone reaches the guest only when the user opts in (-allow-host-audio).
    bool allow_host_audio = false;
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

bool mux_get_allow_host_audio(Object* obj, Error** /*errp*/) {
    return GOLDFISH_AUDIO_MUX_DEV(obj)->config->allow_host_audio;
}

void mux_set_allow_host_audio(Object* obj, bool value, Error** /*errp*/) {
    GOLDFISH_AUDIO_MUX_DEV(obj)->config->allow_host_audio = value;
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
    state->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &goldfish_audio_backend_on_timer, state);
    state->host_adapter = std::make_unique<QemuHostAudioAdapter>(state->host_be, state->timer);
    state->mux = std::make_shared<AudioInputMux>(state->host_adapter.get(),
                                                 runstate_get() == RUN_STATE_RUNNING);
    state->vm_state_entry =
            qemu_add_vm_change_state_handler(&goldfish_audio_backend_on_vm_state, state);
    if (!dev->config->allow_host_audio) {
        // Start with the "host" input muted. Injection still works, and clients may unmute the
        // host microphone at runtime through the mixer state.
        (void)state->mux->UpdateState(AudioInputStateMutation{
            .input_updates = {MicrophoneInputChannelUpdate{
                .source_id = std::string(kHostMicrophoneSourceId), .muted = true}},
        });
    }

    RegisterAudioInputMux(AUDIO_BACKEND(mux_be), state->mux);

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

    object_class_property_add_bool(oc, "allow-host-audio", mux_get_allow_host_audio,
                                   mux_set_allow_host_audio);
    object_class_property_set_description(
            oc, "allow-host-audio",
            "Pass the host microphone to the guest. When off (default), the \"host\" mixer input "
            "starts muted.");

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
