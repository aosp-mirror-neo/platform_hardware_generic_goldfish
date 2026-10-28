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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"

#include "goldfish/audio/goldfish_audio_mux.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/qemu_audio_inject.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include <gmodule.h>
extern "C" {
#include "google/system/aemu_func_defs.h"
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "qom/object.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::audio {
namespace {

constexpr char kTestCtrlDeviceType[] = "goldfish-audio-inject-test-ctrl";
constexpr uint32_t kTestInputSampleRateHz = 16000;
constexpr uint32_t kTestInputChannels = 1;
constexpr int16_t kTestSample = 12000;

struct TestCtrlState {
    std::atomic<bool> guest_active{false};
    std::atomic<uint64_t> frames_pulled{0};
    bool inject_enabled = false;
    std::unique_ptr<QemuAudioInject> injector;
};

// Tracked so GF_SHUTDOWN_FUNC can exercise post-audio_cleanup() destruction of
// QemuAudioInject when QEMU exits while an injector is still active.
TestCtrlState* g_active_ctrl_state = nullptr;

// NOLINTBEGIN(readability-identifier-naming)
struct AudioInjectTestCtrlDev {
    DeviceState parent_obj;
    TestCtrlState* state;
};

#define AUDIO_INJECT_TEST_CTRL_DEV(obj) \
    OBJECT_CHECK(AudioInjectTestCtrlDev, (obj), kTestCtrlDeviceType)

bool ctrl_get_inject_enabled(Object* obj, Error** /*errp*/) {
    auto* dev = AUDIO_INJECT_TEST_CTRL_DEV(obj);
    return (dev->state != nullptr) && dev->state->inject_enabled;
}

void ctrl_set_inject_enabled(Object* obj, bool enabled, Error** errp) {
    auto* dev = AUDIO_INJECT_TEST_CTRL_DEV(obj);
    TestCtrlState* state = dev->state;
    if (!state) {
        return;
    }
    if (!enabled) {
        if (state->injector) {
            state->injector->Stop();
            state->injector.reset();
        }
        state->inject_enabled = false;
        return;
    }
    if (state->inject_enabled && state->injector) {
        return;
    }

    AudioStreamConfig cfg{
        .sample_rate_hz = kTestInputSampleRateHz,
        .channels = kTestInputChannels,
        .format = SampleFormat::kS16,
    };
    auto injector = std::make_unique<QemuAudioInject>(
            [state](void* dst, size_t max_frames) -> size_t {
                auto* out = static_cast<int16_t*>(dst);
                for (size_t i = 0; i < max_frames; ++i) {
                    out[i] = kTestSample;
                }
                state->frames_pulled.fetch_add(max_frames, std::memory_order_release);
                return max_frames;
            },
            cfg,
            [state](bool active) { state->guest_active.store(active, std::memory_order_release); });

    absl::Status status = injector->Start();
    if (!status.ok()) {
        const std::string msg(status.message());
        error_setg(errp, "%s", msg.c_str());
        return;
    }
    state->injector = std::move(injector);
    state->inject_enabled = true;
}

bool ctrl_get_guest_active(Object* obj, Error** /*errp*/) {
    auto* dev = AUDIO_INJECT_TEST_CTRL_DEV(obj);
    return (dev->state != nullptr) && dev->state->guest_active.load(std::memory_order_acquire);
}

bool ctrl_get_has_pulled_frames(Object* obj, Error** /*errp*/) {
    auto* dev = AUDIO_INJECT_TEST_CTRL_DEV(obj);
    return (dev->state != nullptr) &&
           (dev->state->frames_pulled.load(std::memory_order_acquire) > 0);
}

bool ctrl_get_conflict_rejected(Object* /*obj*/, Error** /*errp*/) {
    AudioStreamConfig cfg{
        .sample_rate_hz = kTestInputSampleRateHz,
        .channels = kTestInputChannels,
        .format = SampleFormat::kS16,
    };
    QemuAudioInject second([](void* /*dst*/, size_t /*max_frames*/) -> size_t { return 0; }, cfg);
    absl::Status status = second.Start();
    const bool rejected = absl::IsAlreadyExists(status);
    second.Stop();
    return rejected;
}

void test_ctrl_instance_init(Object* obj) {
    auto* dev = AUDIO_INJECT_TEST_CTRL_DEV(obj);
    dev->state = new TestCtrlState();
    g_active_ctrl_state = dev->state;
}

void test_ctrl_instance_finalize(Object* obj) {
    auto* dev = AUDIO_INJECT_TEST_CTRL_DEV(obj);
    if (g_active_ctrl_state == dev->state) {
        g_active_ctrl_state = nullptr;
    }
    delete dev->state;
    dev->state = nullptr;
}

void test_ctrl_class_init(ObjectClass* oc, const void* /*data*/) {
    object_class_property_add_bool(oc, "inject-enabled", ctrl_get_inject_enabled,
                                   ctrl_set_inject_enabled);
    object_class_property_add_bool(oc, "guest-active", ctrl_get_guest_active, nullptr);
    object_class_property_add_bool(oc, "has-pulled-frames", ctrl_get_has_pulled_frames, nullptr);
    object_class_property_add_bool(oc, "conflict-rejected", ctrl_get_conflict_rejected, nullptr);
}

const TypeInfo test_ctrl_type_info = {
    .name = kTestCtrlDeviceType,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(AudioInjectTestCtrlDev),
    .instance_init = test_ctrl_instance_init,
    .instance_finalize = test_ctrl_instance_finalize,
    .class_init = test_ctrl_class_init,
};
// NOLINTEND(readability-identifier-naming)

}  // namespace
}  // namespace goldfish::audio

extern "C" {

G_MODULE_EXPORT void GF_REGISTER_TYPES_FUNC(void) {
    goldfish::audio::audio_mux_register_types();
    type_register_static(&goldfish::audio::test_ctrl_type_info);
}

G_MODULE_EXPORT void GF_STARTUP_FUNC(int /*argc*/, char** /*argv*/) {}

G_MODULE_EXPORT void GF_SHUTDOWN_FUNC(void) {
    if (goldfish::audio::g_active_ctrl_state != nullptr) {
        goldfish::audio::g_active_ctrl_state->injector.reset();
        goldfish::audio::g_active_ctrl_state->inject_enabled = false;
    }
}

}  // extern "C"
