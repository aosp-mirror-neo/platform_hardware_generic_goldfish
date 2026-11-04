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

#include "goldfish/audio/qemu_audio_inject.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/goldfish_audio_mux.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "qemu/audio.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::audio {
namespace {

AudioBackend* ResolveTargetAudioBackend(AudioBackend* explicit_backend) {
    if (explicit_backend != nullptr) {
        return explicit_backend;
    }
    if (AudioBackend* named = audio_be_by_name(kDefaultMainAudiodevId, nullptr)) {
        return named;
    }
    return audio_get_default_audio_be(nullptr);
}

}  // namespace

std::shared_ptr<AudioInputMux> ResolveTargetAudioInputMux(AudioBackend* explicit_backend) {
    if (explicit_backend != nullptr) {
        return FindAudioInputMux(explicit_backend);
    }
    android::goldfish::RecursiveScopedVmLock vmlock;
    return FindAudioInputMux(ResolveTargetAudioBackend(nullptr));
}

absl::StatusOr<AudioInputStateSnapshot> UpdateAudioInputMuxState(
        AudioInputMux& mux, const AudioInputStateMutation& mutation) {
    android::goldfish::RecursiveScopedVmLock vmlock;
    return mux.UpdateState(mutation);
}

QemuAudioInject::QemuAudioInject(ReadCallback read_cb, AudioStreamConfig config,
                                 ActiveCallback active_cb, AudioBackend* audio_backend,
                                 MicrophoneSourceMetadata metadata)
        : read_cb_(std::move(read_cb))
        , active_cb_(std::move(active_cb))
        , explicit_backend_(audio_backend)
        , config_(config)
        , metadata_(std::move(metadata)) {}

QemuAudioInject::~QemuAudioInject() {
    Stop();
}

absl::Status QemuAudioInject::Start() {
    if (!read_cb_) {
        return absl::InvalidArgumentError(
                "Failed to start QemuAudioInject: ReadCallback must be non-empty. Pass a valid "
                "PCM pull callback to QemuAudioInject.");
    }

    android::goldfish::RecursiveScopedVmLock vmlock;
    if (IsRunning()) {
        return absl::OkStatus();
    }
    std::shared_ptr<AudioInputMux> mux =
            FindAudioInputMux(ResolveTargetAudioBackend(explicit_backend_));
    if (!mux) {
        return absl::UnavailableError(absl::StrFormat(
                "Failed to start QemuAudioInject: %s is not wrapped by GoldfishAudioBackend. "
                "Ensure guest audio is enabled and '-device goldfish-audio-mux' is configured.",
                explicit_backend_ != nullptr
                        ? std::string("the explicit AudioBackend")
                        : absl::StrFormat("default audiodev '%s'", kDefaultMainAudiodevId)));
    }

    AudioStreamConfig config;
    MicrophoneSourceMetadata metadata;
    {
        // Mark as running first: mux callbacks during registration may observe or Stop() us.
        absl::MutexLock lock(mutex_);
        config = config_;
        metadata = metadata_;
        mux_ = mux;
    }
    absl::StatusOr<std::string> source_id =
            mux->RegisterInjector(this, config, read_cb_, active_cb_, metadata);
    absl::MutexLock lock(mutex_);
    if (!source_id.ok()) {
        mux_ = nullptr;
        return source_id.status();
    }
    metadata_.source_id = *source_id;
    VLOG(1) << "Started QemuAudioInject (source_id='" << *source_id << "', "
            << config.sample_rate_hz << " Hz, " << config.channels << " channel(s)).";
    return absl::OkStatus();
}

absl::Status QemuAudioInject::Reconfigure(AudioStreamConfig new_config,
                                          std::optional<std::string_view> new_source_id) {
    android::goldfish::RecursiveScopedVmLock vmlock;
    std::shared_ptr<AudioInputMux> mux;
    AudioStreamConfig old_config;
    MicrophoneSourceMetadata old_metadata;
    {
        // Update first so mux observers see the new values; roll back on failure.
        absl::MutexLock lock(mutex_);
        mux = mux_;
        old_config = std::exchange(config_, new_config);
        old_metadata = metadata_;
        if (new_source_id.has_value() && !new_source_id->empty()) {
            metadata_.source_id = std::string(*new_source_id);
        }
    }
    if (!mux) {
        return absl::OkStatus();
    }
    absl::Status status = mux->ReconfigureInjector(this, new_config, new_source_id);
    if (!status.ok()) {
        absl::MutexLock lock(mutex_);
        config_ = old_config;
        metadata_ = old_metadata;
    }
    return status;
}

void QemuAudioInject::Stop() {
    if (!IsRunning()) {
        return;
    }
    android::goldfish::RecursiveScopedVmLock vmlock;
    std::shared_ptr<AudioInputMux> mux;
    {
        absl::MutexLock lock(mutex_);
        mux = std::move(mux_);
    }
    if (mux) {
        mux->UnregisterInjector(this);
        VLOG(1) << "Stopped QemuAudioInject.";
    }
}

bool QemuAudioInject::IsRunning() const {
    absl::MutexLock lock(mutex_);
    return mux_ != nullptr;
}

AudioStreamConfig QemuAudioInject::config() const {
    absl::MutexLock lock(mutex_);
    return config_;
}

MicrophoneSourceMetadata QemuAudioInject::metadata() const {
    absl::MutexLock lock(mutex_);
    return metadata_;
}

}  // namespace goldfish::audio
