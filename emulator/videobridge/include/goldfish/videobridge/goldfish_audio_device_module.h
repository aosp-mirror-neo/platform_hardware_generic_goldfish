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

// Disable compiler warnings for external third-party headers. We wrap these in localized
// pragma blocks rather than using target 'copts' so that thread-safety analysis remains
// active on our own local source files.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-reference-return"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "api/scoped_refptr.h"
#include "modules/audio_device/include/fake_audio_device.h"
#include "rtc_base/ref_counted_object.h"
#pragma clang diagnostic pop

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

namespace goldfish::videobridge {

/**
 * @class GoldfishAudioDeviceModule
 * @brief Audio device module with no hardware behind it, subclassing WebRTC's
 * FakeAudioDeviceModule.
 *
 * It tells the WebRTC audio engine that a stereo recording device is present, so audio pipelines
 * initialize on hosts without one.
 *
 * WebRTC only decodes the audio it receives when the device module asks for playout data, and no
 * thread here does that on its own. A consumer drives it with PullPlayout(), at the rate it wants
 * the audio: the emulator's virtual microphone pulls at the guest's capture rate, so NetEq stays
 * the only jitter buffer. Received audio from all peers arrives mixed.
 */
class GoldfishAudioDeviceModule : public ::webrtc::FakeAudioDeviceModule {
  public:
    /// Format of the audio PullPlayout() returns: 48 kHz, mono, signed 16-bit.
    static constexpr uint32_t kPlayoutSampleRateHz = 48000;
    static constexpr size_t kPlayoutChannels = 1;

    /**
     * @brief Factory method creating a properly reference-counted GoldfishAudioDeviceModule.
     *
     * WebRTC AudioDeviceModule instances must be wrapped in a reference counter (such as
     * webrtc::RefCountedObject) to ensure safe reference counting and deallocation via
     * scoped_refptr. Direct construction via new or stack allocation is prevented by
     * protected constructors to avoid memory leaks and lifetime errors.
     *
     * @return A scoped_refptr owning a reference-counted GoldfishAudioDeviceModule instance.
     */
    static ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> Create() {
        return ::webrtc::scoped_refptr<GoldfishAudioDeviceModule>(
                new ::webrtc::RefCountedObject<GoldfishAudioDeviceModule>());
    }

    /**
     * @brief Reports the number of recording devices.
     * @return Always returns 1 to indicate a single recording channel is available.
     */
    int16_t RecordingDevices() override { return 1; }

    /**
     * @brief Declares if stereo recording is available.
     *
     * @param available Output pointer populated with true.
     * @return Always returns 0 (success status).
     */
    int32_t StereoRecordingIsAvailable(bool* available) const override {
        *available = true;
        return 0;
    }

    /**
     * @brief Declares if stereo recording is enabled.
     *
     * @param enabled Output pointer populated with true.
     * @return Always returns 0 (success status).
     */
    int32_t StereoRecording(bool* enabled) const override {
        *enabled = true;
        return 0;
    }

    /**
     * @brief Stores the WebRTC engine's transport, the source of PullPlayout() audio.
     *
     * WebRTC registers it when the voice engine starts and passes null when it stops.
     */
    int32_t RegisterAudioCallback(::webrtc::AudioTransport* transport) override
            ABSL_LOCKS_EXCLUDED(mutex_);

    /**
     * @brief Fills @p dst with received audio (see kPlayoutSampleRateHz and kPlayoutChannels).
     *
     * WebRTC hands out audio in 10 ms blocks, so a request that ends inside a block keeps the
     * rest for the next call. Decodes on the calling thread, about 55 µs per 10 ms block
     * (`docs/DESIGN.md` §4.4). Thread-safe.
     *
     * @return The number of samples written. Either all of @p dst, or 0 while no WebRTC
     * engine is registered.
     */
    size_t PullPlayout(std::span<int16_t> dst) ABSL_LOCKS_EXCLUDED(mutex_);

    /// Told true when WebRTC starts playout and false when it stops it.
    using PlayoutListener = std::function<void(bool playing)>;

    /**
     * @brief Sets who hears about playout starting and stopping; null clears it.
     *
     * WebRTC starts playout when a peer offers to send audio, even before a track is attached
     * to its sender, and stops it when the last such stream goes away. Only changes are
     * reported. A listener set while playout runs is told true right away.
     *
     * The listener runs on WebRTC's worker thread and must not block or call back into this
     * module. Once this returns, the previous listener is no longer running and is never
     * called again. Thread-safe.
     */
    void SetPlayoutListener(PlayoutListener listener) ABSL_LOCKS_EXCLUDED(playout_mutex_);

    int32_t StartPlayout() override ABSL_LOCKS_EXCLUDED(playout_mutex_);
    int32_t StopPlayout() override ABSL_LOCKS_EXCLUDED(playout_mutex_);
    bool Playing() const override ABSL_LOCKS_EXCLUDED(playout_mutex_);

  protected:
    GoldfishAudioDeviceModule() = default;
    ~GoldfishAudioDeviceModule() override = default;

  private:
    friend class ::webrtc::RefCountedObject<GoldfishAudioDeviceModule>;

    static constexpr size_t kBlockSamples = kPlayoutSampleRateHz / 100 * kPlayoutChannels;

    // Held while calling into transport_, so RegisterAudioCallback(nullptr) waits for a pull
    // in progress and the transport is never used after WebRTC withdraws it.
    absl::Mutex mutex_;
    ::webrtc::AudioTransport* transport_ ABSL_GUARDED_BY(mutex_) = nullptr;
    // The current 10 ms block, of which block_offset_ samples have been handed out.
    std::array<int16_t, kBlockSamples> block_ ABSL_GUARDED_BY(mutex_) = {};
    size_t block_offset_ ABSL_GUARDED_BY(mutex_) = kBlockSamples;

    void SetPlaying(bool playing) ABSL_LOCKS_EXCLUDED(playout_mutex_);

    // Held while calling the listener, so clearing it waits for a call in progress.
    mutable absl::Mutex playout_mutex_;
    bool playing_ ABSL_GUARDED_BY(playout_mutex_) = false;
    PlayoutListener listener_ ABSL_GUARDED_BY(playout_mutex_);
};

}  // namespace goldfish::videobridge
