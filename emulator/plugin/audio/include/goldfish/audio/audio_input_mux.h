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
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/audio/pcm_format.h"

extern "C" {
typedef struct AudioBackend AudioBackend;
}

namespace goldfish::audio {

/**
 * Opaque guest input voice handle returned by `AudioInputMux::OpenVoice()`. `GoldfishAudioBackend`
 * hands it to QEMU as the `SWVoiceIn*`.
 */
struct MuxVoiceHandle;

/**
 * One input channel of the virtual microphone mixer.
 */
struct MicrophoneInputInfo {
    std::string source_id;
    MicrophoneInputKind kind = MicrophoneInputKind::kUnspecified;
    bool muted = false;
    float gain = 1.0F;

    bool operator==(const MicrophoneInputInfo&) const = default;
};

/**
 * Snapshot of the virtual microphone mixer.
 */
struct AudioInputStateSnapshot {
    bool microphone_muted = false;
    bool microphone_in_use_by_guest = false;
    /// `"host"` first, followed by the active injector (if any).
    std::vector<MicrophoneInputInfo> microphone_inputs;

    bool operator==(const AudioInputStateSnapshot&) const = default;
};

/**
 * Partial per-channel mute/gain mutation keyed by `source_id`.
 */
struct MicrophoneInputChannelUpdate {
    std::string source_id;
    std::optional<bool> muted;
    std::optional<float> gain;
};

/**
 * Partial mutation applied atomically by `AudioInputMux::UpdateState()`.
 */
struct AudioInputStateMutation {
    std::optional<bool> microphone_muted;
    std::vector<MicrophoneInputChannelUpdate> input_updates;
};

/**
 * The mux's view of QEMU: the wrapped host `AudioBackend` and the virtual clock. Implemented by
 * `GoldfishAudioBackend` in QEMU and by fakes in unit tests. Called only with the VM lock held.
 */
class HostAudioInputInterface {
  public:
    using HostVoiceHandle = void*;
    using GuestDataCallback = std::function<void(int avail_bytes)>;

    virtual ~HostAudioInputInterface() = default;

    /**
     * Opens or reopens an input voice on the wrapped host `AudioBackend`. Returns `nullptr` if the
     * host has no capture device. If reopening @p existing fails, the implementation has already
     * closed @p existing (QEMU's `audio_mixeng_backend_open_in` contract).
     */
    virtual HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* name,
                                          const GuestAudioConfig& config,
                                          GuestDataCallback on_host_data) = 0;

    virtual void CloseHostVoice(HostVoiceHandle voice) = 0;
    virtual void SetHostVoiceActive(HostVoiceHandle voice, bool active) = 0;
    virtual void SetHostVoiceVolume(HostVoiceHandle voice, const GuestChannelVolume& volume) = 0;
    virtual size_t ReadHostVoice(HostVoiceHandle voice, void* dst, size_t byte_count) = 0;

    /**
     * Arms or disarms the periodic (10 ms) virtual-clock timer that drives `OnTimerTick()`.
     */
    virtual void SetTimerArmed(bool armed) = 0;

    /**
     * Returns the current virtual clock (`QEMU_CLOCK_VIRTUAL`).
     */
    virtual absl::Duration NowVirtualTime() const = 0;
};

/**
 * Routes the guest microphone (`SWVoiceIn`) to either the host microphone or a single external
 * pull injector (`QemuAudioInject`), and implements the virtual microphone mixer (master mute,
 * per-input mute and gain).
 *
 * Threading: every method except `GetStateSnapshot()`, `IsShutdown()`, `AddStateObserver()` and
 * `RemoveStateObserver()` must be called with the VM lock held (`android::goldfish::VmLock`, the
 * BQL in QEMU). Debug builds check this. The VM lock is the only lock protecting routing state:
 * QEMU already holds it for every `AudioBackendClass` call, timer, and VM state change, and
 * `QemuAudioInject` / `UpdateAudioInputMuxState()` take it. Host calls, guest callbacks,
 * `ReadCallback`, `ActiveCallback`, and state observers therefore run inline on the calling thread
 * with the VM lock held, and may re-enter the mux.
 *
 * Any-thread readers see a copy of the public state that is republished at the end of every
 * mutation; observers are notified whenever that copy changes.
 */
class AudioInputMux {
  public:
    using ReadCallback = std::function<size_t(void* dst_buffer, size_t max_frames)>;
    using ActiveCallback = std::function<void(bool guest_recording_active)>;
    using GuestDataCallback = HostAudioInputInterface::GuestDataCallback;
    using StateObserverId = uint64_t;
    using StateObserverCallback = std::function<void(const AudioInputStateSnapshot&)>;

    /**
     * Constructs a mux bound to @p host, which must outlive the mux or `Shutdown()`.
     */
    explicit AudioInputMux(HostAudioInputInterface* host, bool vm_running = true);
    ~AudioInputMux();

    AudioInputMux(const AudioInputMux&) = delete;
    AudioInputMux& operator=(const AudioInputMux&) = delete;

    // ---- QEMU `AudioBackendClass` input slots, timer, and VM state (VM lock required). ----

    /**
     * Opens a guest voice, or reconfigures @p existing in place when it is non-null.
     */
    MuxVoiceHandle* OpenVoice(MuxVoiceHandle* existing, std::string_view name,
                              const GuestAudioConfig& guest_config, GuestDataCallback guest_cb);
    void CloseVoice(MuxVoiceHandle* voice);
    bool IsVoiceActive(const MuxVoiceHandle* voice) const;
    void SetVoiceActive(MuxVoiceHandle* voice, bool active);
    void SetVoiceVolume(MuxVoiceHandle* voice, const GuestChannelVolume& volume);
    size_t ReadVoice(MuxVoiceHandle* voice, void* dst, size_t byte_count);
    void OnTimerTick();
    void OnVmRunningChanged(bool running);

    /**
     * Closes all host voices, disarms the timer, drops the injector, and detaches from the host.
     * Terminal: later calls are no-ops or fail with `FailedPreconditionError`.
     */
    void Shutdown();

    // ---- Injector (VM lock required). ----

    /**
     * Registers @p owner as the exclusive microphone source.
     *
     * @param metadata Mixer channel identity. An empty `source_id` is replaced by `"stream-<n>"`.
     * @return The mixer `source_id` of the registered injector, or:
     * - `InvalidArgumentError` for an invalid @p config or `source_id == "host"`.
     * - `FailedPreconditionError` after `Shutdown()`.
     * - `AlreadyExistsError` if a different owner is registered.
     *
     * If a guest voice is already recording, @p active_cb is invoked with `true` before returning.
     */
    absl::StatusOr<std::string> RegisterInjector(const void* owner, const AudioStreamConfig& config,
                                                 ReadCallback read_cb, ActiveCallback active_cb,
                                                 MicrophoneSourceMetadata metadata = {});

    /**
     * Changes the active injector's format and optionally its `source_id` (empty keeps it). Mute
     * and gain are preserved.
     */
    absl::Status ReconfigureInjector(const void* owner, const AudioStreamConfig& config,
                                     std::optional<std::string_view> new_source_id = std::nullopt);

    /**
     * Unregisters @p owner if it is the active injector. Its `ReadCallback` is never called again.
     */
    void UnregisterInjector(const void* owner);

    /**
     * Validates and applies @p mutation as a whole.
     *
     * @return The updated snapshot, or:
     * - `InvalidArgumentError` if a `source_id` is empty or a `gain` is outside `[0.0, 4.0]`.
     * - `NotFoundError` if a `source_id` is neither `"host"` nor the active injector.
     * - `FailedPreconditionError` after `Shutdown()`.
     */
    absl::StatusOr<AudioInputStateSnapshot> UpdateState(const AudioInputStateMutation& mutation);

    // ---- Any thread. ----

    bool IsShutdown() const { return shutdown_.load(std::memory_order_acquire); }

    AudioInputStateSnapshot GetStateSnapshot() const ABSL_LOCKS_EXCLUDED(snapshot_mutex_);

    /**
     * Subscribes @p cb to state changes. @p cb runs on the mutating thread with the VM lock held.
     * It may remove other observers but must not remove itself.
     */
    StateObserverId AddStateObserver(StateObserverCallback cb)
            ABSL_LOCKS_EXCLUDED(observers_mutex_);

    /**
     * Unsubscribes @p id and blocks until any in-flight call of that observer has returned.
     */
    void RemoveStateObserver(StateObserverId id) ABSL_LOCKS_EXCLUDED(observers_mutex_);

  private:
    struct Injector {
        const void* owner = nullptr;
        AudioStreamConfig config;
        ReadCallback read_cb;
        ActiveCallback active_cb;
        MicrophoneInputInfo input;
        bool reported_guest_active = false;
    };
    struct ObserverEntry;

    MuxVoiceHandle* FindVoice(const MuxVoiceHandle* handle) const;
    bool AnyGuestVoiceActive() const;
    void RebuildPipeline(MuxVoiceHandle& voice) const;
    void OnHostDataAvailable(MuxVoiceHandle* voice, int avail_bytes);
    MicrophoneInputInfo* FindInput(std::string_view source_id);
    void ShutdownInternal();

    // Ends every mutation: re-applies routing, reports guest activity to the injector, publishes
    // the snapshot, and notifies observers. Must be the last step because callbacks may re-enter.
    void Commit(bool reset_paced_voices);
    void ApplyRouting(bool reset_paced_voices);
    void ReportGuestActivity();
    void PublishState() ABSL_LOCKS_EXCLUDED(snapshot_mutex_, observers_mutex_);
    void NotifyStateObservers(const AudioInputStateSnapshot& snapshot)
            ABSL_LOCKS_EXCLUDED(snapshot_mutex_, observers_mutex_);

    // Routing state: guarded by the VM lock.
    HostAudioInputInterface* host_ = nullptr;
    std::atomic<bool> shutdown_{false};
    bool vm_running_ = true;
    bool timer_armed_ = false;
    bool microphone_muted_ = false;
    MicrophoneInputInfo host_input_;
    std::optional<Injector> active_injector_;
    uint64_t next_stream_id_ = 1;
    std::vector<std::unique_ptr<MuxVoiceHandle>> voices_;

    mutable absl::Mutex snapshot_mutex_;
    AudioInputStateSnapshot published_ ABSL_GUARDED_BY(snapshot_mutex_);

    mutable absl::Mutex observers_mutex_;
    StateObserverId next_observer_id_ ABSL_GUARDED_BY(observers_mutex_) = 1;
    std::vector<std::shared_ptr<ObserverEntry>> observers_ ABSL_GUARDED_BY(observers_mutex_);
};

/**
 * Associates a realized `GoldfishAudioBackend` with its `AudioInputMux`.
 */
void RegisterAudioInputMux(const AudioBackend* backend, std::shared_ptr<AudioInputMux> mux);

/**
 * Removes @p backend from the registry on teardown.
 */
void UnregisterAudioInputMux(const AudioBackend* backend);

/**
 * Returns the `AudioInputMux` registered for @p backend, or `nullptr`.
 */
std::shared_ptr<AudioInputMux> FindAudioInputMux(const AudioBackend* backend);

}  // namespace goldfish::audio
