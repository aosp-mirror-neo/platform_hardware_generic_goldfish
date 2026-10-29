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
#include <cmath>
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
#include "absl/status/statusor.h"
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

// Frames mixed per pass, bounding each voice's float mix buffer (10 ms at 48 kHz).
constexpr size_t kMixChunkFrames = 480;

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

absl::Status ValidateWebRtcReservation(std::string_view action, std::string_view source_id,
                                       MicrophoneInputKind kind) {
    if (source_id != kWebRtcMicrophoneSourceId || kind == MicrophoneInputKind::kWebRtc) {
        return absl::OkStatus();
    }
    return absl::InvalidArgumentError(absl::StrFormat(
            "Failed to %s: source_id 'webrtc' is reserved for the microphone audio WebRTC "
            "clients send. Specify a distinct source_id or leave it empty.",
            action));
}

absl::Status ValidateInjector(std::string_view action, const AudioStreamConfig& config,
                              std::string_view source_id) {
    if (source_id == kHostMicrophoneSourceId) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to %s: source_id 'host' is reserved for the physical host microphone. "
                "Specify a distinct source_id or leave it empty.",
                action));
    }
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
    bool paced = false;  // Served by the pacer (injectors or silence) instead of the host mic.
    VirtualAudioPacer pacer;
    // One converter per registered injector, keyed by its owner.
    absl::flat_hash_map<const void*, std::unique_ptr<PcmPullPipeline>> pipelines;
    std::vector<float> mix;  // Interleaved sum of the injectors, kMixChunkFrames long.
};

struct AudioInputMux::ObserverEntry {
    StateObserverId id = 0;
    StateObserverCallback cb;
    size_t in_flight = 0;
    bool removed = false;
};

AudioInputMux::AudioInputMux(HostAudioInputInterface* host, bool vm_running)
        : host_(host)
        , vm_running_(vm_running)
        , host_input_{.source_id = std::string(kHostMicrophoneSourceId),
                      .kind = MicrophoneInputKind::kHost} {
    DCHECK(host != nullptr);
    absl::MutexLock lock(snapshot_mutex_);
    published_.microphone_inputs = {host_input_};
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
    RebuildPipelines(*voice);
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
    for (auto& [owner, pipeline] : found->pipelines) {
        pipeline->SetVolume(volume);
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
        const size_t bytes_read = host_->ReadHostVoice(found->host_voice, dst, byte_count);
        ScaleGuestPcmBuffer(cfg.format, cfg.big_endian, host_input_.gain, dst, bytes_read);
        return bytes_read;
    }

    const size_t bytes_per_frame = GuestBytesPerFrame(cfg);
    const size_t frames =
            std::min(byte_count, found->pacer.AvailableBytes(host_->NowVirtualTime())) /
            bytes_per_frame;
    if (frames == 0) {
        return 0;
    }
    found->pacer.Consume(frames * bytes_per_frame);
    if (injectors_.empty()) {
        FillGuestSilence(cfg.format, cfg.big_endian, dst, frames * bytes_per_frame);
    } else {
        MixPacedFrames(*found, frames, static_cast<uint8_t*>(dst));
    }
    return frames * bytes_per_frame;
}

void AudioInputMux::MixPacedFrames(MuxVoiceHandle& voice, size_t frames, uint8_t* dst) {
    const GuestAudioConfig& cfg = voice.config;
    const size_t bytes_per_frame = GuestBytesPerFrame(cfg);
    voice.mix.resize(kMixChunkFrames * cfg.channels);
    // ReadCallbacks must not register or unregister injectors, so injectors_ is stable here.
    for (size_t done = 0; done < frames;) {
        const size_t chunk = std::min(frames - done, kMixChunkFrames);
        const size_t samples = chunk * cfg.channels;
        std::fill_n(voice.mix.begin(), samples, 0.0F);
        for (const Injector& injector : injectors_) {
            voice.pipelines.at(injector.owner)
                    ->MixInto(chunk, voice.mix.data(), injector.read_cb,
                              microphone_muted_ || injector.input.muted, injector.input.gain);
        }
        EncodeGuestSamples(cfg, voice.mix.data(), samples, dst + done * bytes_per_frame);
        done += chunk;
    }
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
    injectors_.clear();
    absl::MutexLock lock(snapshot_mutex_);
    published_ = AudioInputStateSnapshot{.microphone_inputs = {host_input_}};
}

absl::StatusOr<std::string> AudioInputMux::RegisterInjector(const void* owner,
                                                            const AudioStreamConfig& config,
                                                            ReadCallback read_cb,
                                                            ActiveCallback active_cb,
                                                            MicrophoneSourceMetadata metadata) {
    AssertVmLockHeld();
    if (absl::Status valid =
                ValidateInjector("start microphone injection", config, metadata.source_id);
        !valid.ok()) {
        return valid;
    }
    if (absl::Status reserved = ValidateWebRtcReservation("start microphone injection",
                                                          metadata.source_id, metadata.kind);
        !reserved.ok()) {
        return reserved;
    }
    if (shutdown_) {
        return MuxShutdownError("start microphone injection");
    }
    if (const Injector* existing = FindInjector(owner)) {
        return existing->input.source_id;
    }
    if (!metadata.source_id.empty() && FindInput(metadata.source_id) != nullptr) {
        return absl::AlreadyExistsError(absl::StrFormat(
                "Failed to start microphone injection: source_id '%s' is already in use by "
                "another microphone input. Use a distinct source_id, or stop that input first.",
                metadata.source_id));
    }
    if (injectors_.size() >= static_cast<size_t>(kMaxConcurrentMicrophoneStreams)) {
        return absl::ResourceExhaustedError(absl::StrFormat(
                "Failed to start microphone injection for source '%s': %d microphone inputs are "
                "already mixed, the most this device supports. Stop one of them first (see "
                "AudioState.microphone_inputs).",
                metadata.source_id.empty() ? "<auto>" : metadata.source_id,
                kMaxConcurrentMicrophoneStreams));
    }

    std::string source_id = std::move(metadata.source_id);
    while (source_id.empty() || FindInput(source_id) != nullptr) {
        source_id = absl::StrFormat("stream-%d", next_stream_id_++);
    }
    const Injector& injector = injectors_.emplace_back(Injector{
        .owner = owner,
        .config = config,
        .read_cb = std::move(read_cb),
        .active_cb = std::move(active_cb),
        .input = {.source_id = source_id, .kind = metadata.kind},
    });
    for (const auto& voice : voices_) {
        BuildPipeline(*voice, injector);
    }
    // The first injector takes over paced voices from silence; later ones join the running mix.
    Commit(/*reset_paced_voices=*/injectors_.size() == 1);
    return source_id;
}

absl::Status AudioInputMux::ReconfigureInjector(const void* owner, const AudioStreamConfig& config,
                                                std::optional<std::string_view> new_source_id) {
    AssertVmLockHeld();
    if (absl::Status valid = ValidateInjector("reconfigure microphone injection", config,
                                              new_source_id.value_or(""));
        !valid.ok()) {
        return valid;
    }
    if (shutdown_) {
        return MuxShutdownError("reconfigure microphone injection");
    }
    Injector* injector = FindInjector(owner);
    if (injector == nullptr) {
        return absl::FailedPreconditionError(
                "Failed to reconfigure microphone injection: the caller is not a registered "
                "microphone injector. Call Start() before Reconfigure().");
    }
    const bool renaming = new_source_id.has_value() && !new_source_id->empty() &&
                          *new_source_id != injector->input.source_id;
    if (renaming) {
        if (absl::Status reserved = ValidateWebRtcReservation("rename microphone injection",
                                                              *new_source_id, injector->input.kind);
            !reserved.ok()) {
            return reserved;
        }
    }
    if (renaming && FindInput(*new_source_id) != nullptr) {
        return absl::AlreadyExistsError(absl::StrFormat(
                "Failed to rename microphone input '%s' to '%s': another microphone input already "
                "uses that source_id. Choose a distinct source_id.",
                injector->input.source_id, *new_source_id));
    }

    if (renaming) {
        injector->input.source_id = std::string(*new_source_id);
    }
    if (injector->config != config) {
        injector->config = config;
        for (const auto& voice : voices_) {
            BuildPipeline(*voice, *injector);
        }
    }
    Commit(/*reset_paced_voices=*/false);
    return absl::OkStatus();
}

void AudioInputMux::UnregisterInjector(const void* owner) {
    AssertVmLockHeld();
    auto it = std::find_if(injectors_.begin(), injectors_.end(),
                           [owner](const Injector& injector) { return injector.owner == owner; });
    if (it == injectors_.end()) {
        return;
    }
    injectors_.erase(it);
    for (const auto& voice : voices_) {
        voice->pipelines.erase(owner);
    }
    // Only the last injector leaving hands paced voices back to the host or to silence.
    Commit(/*reset_paced_voices=*/injectors_.empty());
}

absl::StatusOr<AudioInputStateSnapshot> AudioInputMux::UpdateState(
        const AudioInputStateMutation& mutation) {
    AssertVmLockHeld();
    if (shutdown_) {
        return MuxShutdownError("update microphone audio state");
    }
    for (const auto& update : mutation.input_updates) {
        if (update.source_id.empty()) {
            return absl::InvalidArgumentError(
                    "Failed to update microphone input: source_id must not be empty. Use 'host' "
                    "or the source_id of an active microphone input.");
        }
        if (update.gain.has_value() &&
            !(std::isfinite(*update.gain) && *update.gain >= kMinMicrophoneInputGain &&
              *update.gain <= kMaxMicrophoneInputGain)) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to update microphone input '%s': gain must be finite and in "
                    "[%.1f, %.1f] (got %g). Use 1.0 for unity gain.",
                    update.source_id, kMinMicrophoneInputGain, kMaxMicrophoneInputGain,
                    *update.gain));
        }
        if (FindInput(update.source_id) == nullptr) {
            return absl::NotFoundError(absl::StrFormat(
                    "Failed to update microphone input '%s': no active microphone input has that "
                    "source_id. Read the current state to list active inputs.",
                    update.source_id));
        }
    }

    if (mutation.microphone_muted.has_value()) {
        microphone_muted_ = *mutation.microphone_muted;
    }
    for (const auto& update : mutation.input_updates) {
        MicrophoneInputInfo* input = FindInput(update.source_id);
        input->muted = update.muted.value_or(input->muted);
        input->gain = update.gain.value_or(input->gain);
    }
    Commit(/*reset_paced_voices=*/false);
    return GetStateSnapshot();
}

AudioInputStateSnapshot AudioInputMux::GetStateSnapshot() const {
    absl::MutexLock lock(snapshot_mutex_);
    return published_;
}

AudioInputMux::StateObserverId AudioInputMux::AddStateObserver(StateObserverCallback cb) {
    if (!cb) {
        return 0;
    }
    absl::MutexLock lock(observers_mutex_);
    const StateObserverId id = next_observer_id_++;
    observers_.push_back(
            std::make_shared<ObserverEntry>(ObserverEntry{.id = id, .cb = std::move(cb)}));
    return id;
}

void AudioInputMux::RemoveStateObserver(StateObserverId id) {
    StateObserverCallback retired_cb;  // Destroyed after the lock is released.
    absl::MutexLock lock(observers_mutex_);
    auto it = std::find_if(observers_.begin(), observers_.end(),
                           [id](const auto& entry) { return entry->id == id; });
    if (it == observers_.end()) {
        return;
    }
    std::shared_ptr<ObserverEntry> entry = *it;
    observers_.erase(it);
    entry->removed = true;
    observers_mutex_.Await(
            absl::Condition(+[](ObserverEntry* e) { return e->in_flight == 0; }, entry.get()));
    retired_cb = std::move(entry->cb);
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

AudioInputMux::Injector* AudioInputMux::FindInjector(const void* owner) {
    for (Injector& injector : injectors_) {
        if (injector.owner == owner) {
            return &injector;
        }
    }
    return nullptr;
}

void AudioInputMux::BuildPipeline(MuxVoiceHandle& voice, const Injector& injector) const {
    auto pipeline = std::make_unique<PcmPullPipeline>(injector.config, voice.config);
    pipeline->SetVolume(voice.volume);
    voice.pipelines[injector.owner] = std::move(pipeline);
}

void AudioInputMux::RebuildPipelines(MuxVoiceHandle& voice) const {
    voice.pipelines.clear();
    for (const Injector& injector : injectors_) {
        BuildPipeline(voice, injector);
    }
}

MicrophoneInputInfo* AudioInputMux::FindInput(std::string_view source_id) {
    if (source_id == host_input_.source_id) {
        return &host_input_;
    }
    for (Injector& injector : injectors_) {
        if (source_id == injector.input.source_id) {
            return &injector.input;
        }
    }
    return nullptr;
}

void AudioInputMux::Commit(bool reset_paced_voices) {
    if (!shutdown_) {
        ApplyRouting(reset_paced_voices);
    }
    ReportGuestActivity();
    PublishState();
}

void AudioInputMux::ApplyRouting(bool reset_paced_voices) {
    const bool use_host_mic = !microphone_muted_ && !host_input_.muted && injectors_.empty();
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
            for (auto& [owner, pipeline] : voice->pipelines) {
                pipeline->Reset();
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
    const bool guest_active = AnyGuestVoiceActive();
    // Collect owners first: a callback may unregister injectors, including ones not yet told.
    std::vector<const void*> pending;
    for (const Injector& injector : injectors_) {
        if (injector.active_cb && injector.reported_guest_active != guest_active) {
            pending.push_back(injector.owner);
        }
    }
    for (const void* owner : pending) {
        Injector* injector = FindInjector(owner);
        if (injector == nullptr || injector->reported_guest_active == guest_active) {
            continue;
        }
        injector->reported_guest_active = guest_active;
        const ActiveCallback cb = injector->active_cb;  // Copy: cb may unregister.
        cb(guest_active);
    }
}

void AudioInputMux::PublishState() {
    AudioInputStateSnapshot snapshot{
        .microphone_muted = microphone_muted_,
        .microphone_in_use_by_guest = AnyGuestVoiceActive(),
        .microphone_inputs = {host_input_},
    };
    for (const Injector& injector : injectors_) {
        snapshot.microphone_inputs.push_back(injector.input);
    }
    {
        absl::MutexLock lock(snapshot_mutex_);
        if (snapshot == published_) {
            return;
        }
        published_ = snapshot;
    }
    NotifyStateObservers(snapshot);
}

void AudioInputMux::NotifyStateObservers(const AudioInputStateSnapshot& snapshot) {
    std::vector<std::shared_ptr<ObserverEntry>> observers;
    {
        absl::MutexLock lock(observers_mutex_);
        observers = observers_;
    }
    for (const auto& entry : observers) {
        {
            // An observer may mutate the mux re-entrantly. The nested publish already delivered
            // the newer snapshot to every observer, so handing out this one now would leave the
            // remaining observers on stale state.
            absl::MutexLock lock(snapshot_mutex_);
            if (snapshot != published_) {
                return;
            }
        }
        {
            absl::MutexLock lock(observers_mutex_);
            if (entry->removed) {
                continue;
            }
            ++entry->in_flight;
        }
        entry->cb(snapshot);
        absl::MutexLock lock(observers_mutex_);
        --entry->in_flight;
    }
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
