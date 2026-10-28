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

#include <cstddef>
#include <functional>
#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/audio/pcm_format.h"

extern "C" {
typedef struct AudioBackend AudioBackend;
}

namespace goldfish::audio {

class AudioInputMux;

/**
 * Injects external PCM microphone audio into QEMU's `AudioBackend` via pull callbacks.
 *
 * @note **Threading Model:** `Start()` and `Stop()` take the QEMU BQL, so they are serialized with
 * each other and with guest audio reads. `ReadCallback` is invoked synchronously on QEMU's
 * main-loop / vCPU thread while holding the BQL. Callbacks must not perform blocking I/O, heavy CPU
 * work, wait on anything that needs the BQL, or call `Start()` / `Stop()` on their owning
 * `QemuAudioInject` instance.
 */
class QemuAudioInject {
  public:
    /**
     * Pulls up to @p max_frames interleaved PCM frames into @p dst_buffer.
     *
     * @param dst_buffer Destination buffer with capacity
     *        `max_frames * channels * BytesPerSample(format)` bytes.
     * @param max_frames Maximum number of frames requested for this read.
     * @return Number of frames actually written (`0 <= written <= max_frames`). Any remaining
     *         frames are silence-padded by the mux to preserve steady guest period timing.
     */
    using ReadCallback = std::function<size_t(void* dst_buffer, size_t max_frames)>;

    /**
     * Optional callback invoked when the guest activates (`true`) or deactivates (`false`) its
     * microphone capture stream while this injector is running (including during `Start()` if
     * guest recording is already active).
     */
    using ActiveCallback = std::function<void(bool guest_recording_active)>;

    /**
     * Constructs a microphone audio injector handle.
     *
     * @param read_cb Pull callback invoked on each paced audio read.
     * @param config Caller's PCM stream configuration (resampled and remixed to the guest capture
     * format as needed).
     * @param active_cb Optional notification callback for guest capture stream state changes.
     * @param audio_backend Optional explicit QEMU `AudioBackend`; defaults to `"mainaudiodev"`.
     */
    QemuAudioInject(ReadCallback read_cb, AudioStreamConfig config,
                    ActiveCallback active_cb = nullptr, AudioBackend* audio_backend = nullptr);
    ~QemuAudioInject();

    QemuAudioInject(const QemuAudioInject&) = delete;
    QemuAudioInject& operator=(const QemuAudioInject&) = delete;

    /**
     * Registers this injector as the exclusive active microphone source.
     *
     * @return
     * - `absl::OkStatus()` when registered (or idempotent no-op if `this` is already running).
     * - `absl::InvalidArgumentError` if `config()` has invalid parameters (`sample_rate_hz == 0`,
     *   `channels == 0` or `> 16`), or `read_cb` is empty.
     * - `absl::UnavailableError` if no `goldfish-audio-mux` is configured (e.g., RISC-V AVDs).
     * - `absl::FailedPreconditionError` if the target `GoldfishAudioBackend` has already shut down.
     * - `absl::AlreadyExistsError` if another `QemuAudioInject` instance currently owns the mic.
     */
    absl::Status Start() ABSL_LOCKS_EXCLUDED(mutex_);

    /**
     * Unregisters this injector and restores the host microphone (if guest capture is active).
     * Thread-safe and idempotent. No `ReadCallback` executes after `Stop()` returns.
     */
    void Stop() ABSL_LOCKS_EXCLUDED(mutex_);

    /**
     * Returns true if this injector is currently registered as the active microphone source.
     */
    bool IsRunning() const ABSL_LOCKS_EXCLUDED(mutex_);

    /**
     * Returns the configured input stream format.
     */
    const AudioStreamConfig& config() const { return config_; }

  private:
    const ReadCallback read_cb_;
    const AudioStreamConfig config_;
    const ActiveCallback active_cb_;
    AudioBackend* const explicit_backend_;

    // Lets IsRunning() run on any thread. mux_ only changes under the BQL, and mutex_ is never held
    // while calling into the mux, so mux callbacks may call IsRunning() or Stop().
    mutable absl::Mutex mutex_;
    std::shared_ptr<AudioInputMux> mux_ ABSL_GUARDED_BY(mutex_);  // Non-null while running.
};

}  // namespace goldfish::audio
