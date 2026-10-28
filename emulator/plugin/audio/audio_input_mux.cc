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

#include "goldfish/audio/audio_input_mux.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/pcm_pull_pipeline.h"
#include "goldfish/audio/virtual_audio_pacer.h"

namespace goldfish::audio {
namespace {

// Upper bound (16 MiB) when advertising paced bytes through QEMU's signed `int avail` callback.
constexpr size_t kMaxAdvertisedAvailBytes = 1U << 24;

void AssertVmLockHeld() {
    DCHECK(android::goldfish::VmLock::Get()->IsLockedBySelf())
            << "AudioInputMux mutations must hold the VM lock (QEMU BQL). Wrap the call in "
               "android::goldfish::RecursiveScopedVmLock.";
}

absl::Status MuxShutdownError(std::string_view action) {
    return absl::FailedPreconditionError(absl::StrFormat(
            "Failed to %s: GoldfishAudioBackend has permanently shut down. Restart the emulator "
            "or invoke audio APIs only while the VM is running.",
            action));
}

absl::Status ValidateInjector(std::string_view action, const AudioStreamConfig& config) {
    if (config.sample_rate_hz == 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to %s: sample_rate_hz must be > 0 Hz (got 0). Specify a valid PCM sample "
                "rate such as 16000, 44100, or 48000 Hz.",
                action));
    }
    if (config.channels == 0 || config.channels > kMaxAudioChannels) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to %s: channels must be in [1, %u] (got %u). Specify 1 for mono or 2 for "
                "stereo.",
                action, kMaxAudioChannels, config.channels));
    }
    return absl::OkStatus();
}

size_t GuestBytesPerFrame(const GuestAudioConfig& cfg) {
    return static_cast<size_t>(cfg.channels) * BytesPerGuestSample(cfg.format);
}

struct MuxRegistry {
    absl::Mutex registry_mutex;
    absl::flat_hash_map<const AudioBackend*, std::weak_ptr<AudioInputMux>> map
            ABSL_GUARDED_BY(registry_mutex);
};

MuxRegistry& GlobalMuxRegistry() {
    static absl::NoDestructor<MuxRegistry> registry;
    return *registry;
}

}  // namespace

// One guest input voice. Its address is the opaque handle QEMU stores as `SWVoiceIn*`.
struct MuxVoiceHandle {
    GuestAudioConfig config;
    GuestChannelVolume volume;
    AudioInputMux::GuestDataCallback guest_cb;
    HostAudioInputInterface::HostVoiceHandle host_voice = nullptr;  // null: no host mic.
    bool guest_active = false;
    bool host_voice_active = false;
    bool paced = false;  // Served by the pacer (injector or silence) instead of the host mic.
    VirtualAudioPacer pacer;
    std::unique_ptr<PcmPullPipeline> pipeline;  // Non-null while an injector is registered.
};

AudioInputMux::AudioInputMux(HostAudioInputInterface* host, bool vm_running)
        : host_(host), vm_running_(vm_running) {
    DCHECK(host != nullptr);
}

AudioInputMux::~AudioInputMux() {
    // Destruction implies exclusive access, so the VM lock is not required here.
    ShutdownInternal();
}

MuxVoiceHandle* AudioInputMux::OpenVoice(MuxVoiceHandle* existing, std::string_view name,
                                         const GuestAudioConfig& guest_config,
                                         GuestDataCallback guest_cb) {
    AssertVmLockHeld();
    if (shutdown_) {
        return nullptr;
    }
    MuxVoiceHandle* voice = existing;
    if (existing == nullptr) {
        voice = voices_.emplace_back(std::make_unique<MuxVoiceHandle>()).get();
    } else if (FindVoice(existing) == nullptr) {
        return nullptr;
    }

    // On a reopen, the host consumes the previous host voice whether or not it succeeds.
    voice->host_voice = host_->OpenHostVoice(
            voice->host_voice, std::string(name).c_str(), guest_config,
            [this, voice](int avail_bytes) { OnHostDataAvailable(voice, avail_bytes); });
    voice->host_voice_active = false;
    voice->config = guest_config;
    voice->volume = GuestChannelVolume{.channels = guest_config.channels};
    voice->guest_cb = std::move(guest_cb);
    voice->pacer.Configure(guest_config.sample_rate_hz,
                           static_cast<uint32_t>(GuestBytesPerFrame(guest_config)),
                           host_->NowVirtualTime());
    RebuildPipeline(*voice);
    Commit(/*reset_paced_voices=*/existing != nullptr);
    return voice;
}

void AudioInputMux::CloseVoice(MuxVoiceHandle* voice) {
    AssertVmLockHeld();
    auto it = std::find_if(voices_.begin(), voices_.end(),
                           [voice](const auto& entry) { return entry.get() == voice; });
    if (it == voices_.end()) {
        return;
    }
    if ((*it)->host_voice != nullptr && host_ != nullptr) {
        host_->CloseHostVoice((*it)->host_voice);
    }
    voices_.erase(it);
    Commit(/*reset_paced_voices=*/false);
}

bool AudioInputMux::IsVoiceActive(const MuxVoiceHandle* voice) const {
    AssertVmLockHeld();
    const MuxVoiceHandle* found = FindVoice(voice);
    return found != nullptr && found->guest_active;
}

void AudioInputMux::SetVoiceActive(MuxVoiceHandle* voice, bool active) {
    AssertVmLockHeld();
    MuxVoiceHandle* found = FindVoice(voice);
    if (shutdown_ || found == nullptr || found->guest_active == active) {
        return;
    }
    found->guest_active = active;
    Commit(/*reset_paced_voices=*/false);
}

void AudioInputMux::SetVoiceVolume(MuxVoiceHandle* voice, const GuestChannelVolume& volume) {
    AssertVmLockHeld();
    MuxVoiceHandle* found = FindVoice(voice);
    if (shutdown_ || found == nullptr) {
        return;
    }
    found->volume = volume;
    if (found->pipeline) {
        found->pipeline->SetVolume(volume);
    }
    if (found->host_voice != nullptr) {
        host_->SetHostVoiceVolume(found->host_voice, volume);
    }
}

size_t AudioInputMux::ReadVoice(MuxVoiceHandle* voice, void* dst, size_t byte_count) {
    AssertVmLockHeld();
    MuxVoiceHandle* found = FindVoice(voice);
    if (shutdown_ || found == nullptr || dst == nullptr || !found->guest_active) {
        return 0;
    }
    const GuestAudioConfig& cfg = found->config;

    if (!found->paced) {
        if (found->host_voice == nullptr) {
            return 0;
        }
        return host_->ReadHostVoice(found->host_voice, dst, byte_count);
    }

    const size_t bytes_per_frame = GuestBytesPerFrame(cfg);
    const size_t frames =
            std::min(byte_count, found->pacer.AvailableBytes(host_->NowVirtualTime())) /
            bytes_per_frame;
    if (frames == 0) {
        return 0;
    }
    found->pacer.Consume(frames * bytes_per_frame);
    if (found->pipeline) {
        found->pipeline->Pull(frames, dst, active_injector_->read_cb);
    } else {
        FillGuestSilence(cfg.format, cfg.big_endian, dst, frames * bytes_per_frame);
    }
    return frames * bytes_per_frame;
}

void AudioInputMux::OnTimerTick() {
    AssertVmLockHeld();
    if (shutdown_ || !vm_running_) {
        return;
    }
    // Collect first: guest callbacks read from the mux and may change voices_.
    struct Ready {
        GuestDataCallback cb;
        int avail_bytes = 0;
    };
    std::vector<Ready> ready;
    const absl::Duration now = host_->NowVirtualTime();
    for (const auto& voice : voices_) {
        const size_t avail = voice->paced ? voice->pacer.AvailableBytes(now) : 0;
        if (avail > 0 && voice->guest_cb) {
            ready.push_back(
                    {voice->guest_cb, static_cast<int>(std::min(avail, kMaxAdvertisedAvailBytes))});
        }
    }
    for (const Ready& item : ready) {
        item.cb(item.avail_bytes);
    }
}

void AudioInputMux::OnHostDataAvailable(MuxVoiceHandle* voice, int avail_bytes) {
    AssertVmLockHeld();
    MuxVoiceHandle* found = FindVoice(voice);
    if (avail_bytes <= 0 || shutdown_ || found == nullptr || !found->guest_active || found->paced) {
        return;
    }
    const GuestDataCallback cb = found->guest_cb;  // Copy: the callback may close the voice.
    if (cb) {
        cb(avail_bytes);
    }
}

void AudioInputMux::OnVmRunningChanged(bool running) {
    AssertVmLockHeld();
    if (shutdown_ || vm_running_ == running) {
        return;
    }
    vm_running_ = running;
    Commit(/*reset_paced_voices=*/running);
}

void AudioInputMux::Shutdown() {
    AssertVmLockHeld();
    ShutdownInternal();
}

void AudioInputMux::ShutdownInternal() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    for (const auto& voice : voices_) {
        if (voice->host_voice != nullptr) {
            host_->CloseHostVoice(voice->host_voice);
        }
    }
    if (timer_armed_) {
        host_->SetTimerArmed(false);
        timer_armed_ = false;
    }
    host_ = nullptr;
    voices_.clear();
    active_injector_.reset();
}

absl::Status AudioInputMux::RegisterInjector(const void* owner, const AudioStreamConfig& config,
                                             ReadCallback read_cb, ActiveCallback active_cb) {
    AssertVmLockHeld();
    if (absl::Status valid = ValidateInjector("start microphone injection", config); !valid.ok()) {
        return valid;
    }
    if (shutdown_) {
        return MuxShutdownError("start microphone injection");
    }
    if (active_injector_.has_value()) {
        if (active_injector_->owner == owner) {
            return absl::OkStatus();
        }
        return absl::AlreadyExistsError(
                "Failed to start microphone injection: another injector already owns the guest "
                "microphone. Stop the active injector before starting a new one.");
    }

    active_injector_ = Injector{
        .owner = owner,
        .config = config,
        .read_cb = std::move(read_cb),
        .active_cb = std::move(active_cb),
    };
    for (const auto& voice : voices_) {
        RebuildPipeline(*voice);
    }
    Commit(/*reset_paced_voices=*/true);
    return absl::OkStatus();
}

void AudioInputMux::UnregisterInjector(const void* owner) {
    AssertVmLockHeld();
    if (!active_injector_.has_value() || active_injector_->owner != owner) {
        return;
    }
    active_injector_.reset();
    for (const auto& voice : voices_) {
        voice->pipeline.reset();
    }
    Commit(/*reset_paced_voices=*/true);
}

MuxVoiceHandle* AudioInputMux::FindVoice(const MuxVoiceHandle* handle) const {
    for (const auto& voice : voices_) {
        if (voice.get() == handle) {
            return voice.get();
        }
    }
    return nullptr;
}

bool AudioInputMux::AnyGuestVoiceActive() const {
    return std::any_of(voices_.begin(), voices_.end(),
                       [](const auto& voice) { return voice->guest_active; });
}

void AudioInputMux::RebuildPipeline(MuxVoiceHandle& voice) const {
    voice.pipeline.reset();
    if (active_injector_.has_value()) {
        voice.pipeline = std::make_unique<PcmPullPipeline>(active_injector_->config, voice.config);
        voice.pipeline->SetVolume(voice.volume);
    }
}

void AudioInputMux::Commit(bool reset_paced_voices) {
    if (!shutdown_) {
        ApplyRouting(reset_paced_voices);
    }
    ReportGuestActivity();
}

void AudioInputMux::ApplyRouting(bool reset_paced_voices) {
    const bool use_host_mic = !active_injector_;
    const absl::Duration now = host_->NowVirtualTime();

    // Collect host changes first: the host may call back into the mux and change voices_.
    std::vector<std::pair<HostAudioInputInterface::HostVoiceHandle, bool>> host_changes;
    bool any_paced = false;
    for (const auto& voice : voices_) {
        const bool want_host = voice->guest_active && use_host_mic && voice->host_voice != nullptr;
        const bool want_paced = voice->guest_active && !want_host;
        if (voice->host_voice != nullptr && voice->host_voice_active != want_host) {
            voice->host_voice_active = want_host;
            host_changes.emplace_back(voice->host_voice, want_host);
        }
        // Entering paced mode starts from a clean budget and an empty resampler delay line.
        if (want_paced && (!voice->paced || reset_paced_voices)) {
            voice->pacer.Reset(now);
            if (voice->pipeline) {
                voice->pipeline->Reset();
            }
        }
        voice->paced = want_paced;
        any_paced = any_paced || want_paced;
    }
    const bool want_timer = vm_running_ && any_paced;
    const bool timer_changed = timer_armed_ != want_timer;
    timer_armed_ = want_timer;

    for (const auto& [host_voice, active] : host_changes) {
        host_->SetHostVoiceActive(host_voice, active);
    }
    if (timer_changed) {
        host_->SetTimerArmed(want_timer);
    }
}

void AudioInputMux::ReportGuestActivity() {
    if (!active_injector_.has_value() || !active_injector_->active_cb) {
        return;
    }
    const bool guest_active = AnyGuestVoiceActive();
    if (active_injector_->reported_guest_active == guest_active) {
        return;
    }
    active_injector_->reported_guest_active = guest_active;
    const ActiveCallback cb = active_injector_->active_cb;  // Copy: cb may unregister.
    cb(guest_active);
}

void RegisterAudioInputMux(const AudioBackend* backend, std::shared_ptr<AudioInputMux> mux) {
    if (!backend || !mux) {
        return;
    }
    MuxRegistry& registry = GlobalMuxRegistry();
    absl::MutexLock lock(registry.registry_mutex);
    registry.map[backend] = std::move(mux);
}

void UnregisterAudioInputMux(const AudioBackend* backend) {
    if (!backend) {
        return;
    }
    MuxRegistry& registry = GlobalMuxRegistry();
    absl::MutexLock lock(registry.registry_mutex);
    registry.map.erase(backend);
}

std::shared_ptr<AudioInputMux> FindAudioInputMux(const AudioBackend* backend) {
    if (!backend) {
        return nullptr;
    }
    MuxRegistry& registry = GlobalMuxRegistry();
    absl::MutexLock lock(registry.registry_mutex);
    const auto it = registry.map.find(backend);
    if (it == registry.map.end()) {
        return nullptr;
    }
    return it->second.lock();
}

}  // namespace goldfish::audio
