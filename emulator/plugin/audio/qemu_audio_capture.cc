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

#pragma clang diagnostic ignored "-Waddress-of-temporary"

#include "goldfish/audio/qemu_audio_capture.h"

#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/goldfish_audio_mux.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/audio-capture.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::audio {
namespace {

AudioBackend* ResolveAudioBackend(AudioBackend* explicit_backend) {
    if (explicit_backend) {
        return explicit_backend;
    }
    if (AudioBackend* be = audio_get_default_audio_be(nullptr)) {
        return be;
    }
    return audio_be_by_name(kDefaultMainAudiodevId, nullptr);
}

void OnAudioNotify(void* /*opaque*/, audcnotification_e cmd) {
    VLOG(1) << "QEMU audio capture notification: cmd=" << cmd << " ("
            << (cmd == AUD_CNOTIFY_ENABLE    ? "ENABLE"
                : cmd == AUD_CNOTIFY_DISABLE ? "DISABLE"
                                             : "UNKNOWN")
            << ")";
}

void OnAudioDestroy(void* /*opaque*/) {
    VLOG(1) << "QEMU audio capture destroyed.";
}

}  // namespace

QemuAudioCapture::QemuAudioCapture(AudioCallback callback, uint32_t sample_rate_hz,
                                   uint32_t channels, AudioBackend* audio_backend)
        : callback_(std::move(callback))
        , audio_backend_(ResolveAudioBackend(audio_backend))
        , sample_rate_hz_(sample_rate_hz)
        , channels_(channels) {
    DCHECK_GT(sample_rate_hz_, 0U);
    DCHECK_GT(channels_, 0U);
}

QemuAudioCapture::~QemuAudioCapture() {
    Stop();
}

absl::Status QemuAudioCapture::Start() {
    if (!audio_backend_) {
        VLOG(1) << "No QEMU audio backend found for guest audio capture.";
        return absl::UnavailableError(
                "No active QEMU audio backend available. Guest audio may be disabled (-no-audio).");
    }

    audsettings as = {};
    as.freq = static_cast<int>(sample_rate_hz_);
    as.nchannels = static_cast<int>(channels_);
    as.fmt = AUDIO_FORMAT_S16;
    as.big_endian = false;

    struct audio_capture_ops ops = {};
    ops.notify = &OnAudioNotify;
    ops.capture = &QemuAudioCapture::OnCaptureStatic;
    ops.destroy = &OnAudioDestroy;

    // Only one capture voice can be active at a time. The VM lock serializes voice registration
    // and guarantees that at most one capture voice is registered with QEMU.
    android::goldfish::RecursiveScopedVmLock vmlock;
    if (capture_voice_.load(std::memory_order_relaxed) != nullptr) {
        return absl::OkStatus();
    }

    CaptureVoiceOut* voice = audio_be_add_capture(audio_backend_, &as, &ops, this);
    if (!voice) {
        VLOG(1) << "Failed to register audio capture voice with QEMU audio backend.";
        return absl::InternalError("Failed to add capture voice to QEMU AudioBackend.");
    }

    capture_voice_.store(voice, std::memory_order_release);

    VLOG(1) << "Started guest audio capture at " << sample_rate_hz_ << " Hz, " << channels_
            << " channels.";
    return absl::OkStatus();
}

void QemuAudioCapture::Stop() {
    // Atomically claim and clear the voice handle. Multiple threads may safely call Stop()
    // concurrently: exactly one thread will successfully exchange a non-null pointer and
    // unregister the voice with QEMU under the VM lock.
    CaptureVoiceOut* voice = capture_voice_.exchange(nullptr, std::memory_order_acq_rel);
    if (!voice) {
        // Safe no-op if Stop() is called concurrently or multiple times.
        return;
    }

    if (audio_backend_) {
        android::goldfish::RecursiveScopedVmLock vmlock;
        audio_be_del_capture(audio_backend_, voice, this);
    }

    VLOG(1) << "Stopped guest audio capture.";
}

void QemuAudioCapture::OnCapture(const void* buf, int size) {
    if (capture_voice_.load(std::memory_order_acquire) == nullptr || !callback_ || !buf ||
        size <= 0) {
        return;
    }

    const size_t bytes_per_frame = channels_ * sizeof(int16_t);
    DCHECK_GE(static_cast<size_t>(size), bytes_per_frame)
            << "Audio buffer smaller than single frame: " << size
            << " bytes (expected >= " << bytes_per_frame << ")";
    DCHECK_EQ(static_cast<size_t>(size) % bytes_per_frame, 0U)
            << "Unaligned audio buffer: " << size << " bytes for " << channels_ << " channels";

    const size_t num_frames = static_cast<size_t>(size) / bytes_per_frame;
    if (num_frames == 0) {
        return;
    }

    const size_t num_samples = num_frames * channels_;
    const auto* pcm_data = static_cast<const int16_t*>(buf);

    callback_(pcm_data, num_samples);
}

void QemuAudioCapture::OnCaptureStatic(void* opaque, const void* buf, int size) {
    static_cast<QemuAudioCapture*>(opaque)->OnCapture(buf, size);
}

}  // namespace goldfish::audio
