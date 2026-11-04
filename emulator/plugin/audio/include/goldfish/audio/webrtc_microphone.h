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
#include <span>
#include <string_view>

#include "goldfish/async/event_loop.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/qemu_audio_inject.h"

namespace goldfish::audio {

/**
 * Feeds the audio WebRTC peers send, such as a browser microphone, into the guest microphone.
 *
 * While WebRTC plays out received audio (`SetPlaying(true)`), this keeps one
 * `MicrophoneInputKind::kWebRtc` input named `kSourceId` in the guest microphone mixer. WebRTC
 * mixes all peers into the audio pulled from @p pull; the input is silent while no peer has a
 * track attached. The guest mixer mixes it with any other injectors, such as an agent's
 * `AudioService` gRPC stream.
 *
 * If the mixer is full (`kMaxConcurrentMicrophoneStreams`), this logs a warning and adds the
 * input as soon as the mixer state changes and it fits. Other failures, such as a device without
 * a microphone mixer or one shutting down, drop the WebRTC audio with a warning.
 *
 * @note **Threading:** `SetPlaying()` may be called on any thread and never blocks. Adding and
 * removing the mixer input needs the QEMU BQL, so it runs on the event loop given to `Create()`.
 * @p pull runs on the guest audio path with the BQL held, so WebRTC decodes there; the measured
 * cost and the review that accepted it are in `emulator/videobridge/docs/DESIGN.md` §4.4.
 */
class WebRtcMicrophone : public std::enable_shared_from_this<WebRtcMicrophone> {
  public:
    /// The mixer `source_id` of the input all WebRTC peers share.
    static constexpr std::string_view kSourceId = "webrtc";

    /// Format of the audio `PullFn` delivers.
    static constexpr AudioStreamConfig kConfig{
        .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16};

    /// Fills @p dst with received audio in `kConfig` format; returns the samples written.
    using PullFn = std::function<size_t(std::span<int16_t> dst)>;

    /**
     * @param loop Event loop that adds and removes the mixer input. Must outlive the returned
     * object.
     * @param pull Source of the received audio.
     * @param audio_backend Optional QEMU `AudioBackend`; defaults to `"mainaudiodev"`.
     */
    static std::shared_ptr<WebRtcMicrophone> Create(async::EventLoop& loop, PullFn pull,
                                                    AudioBackend* audio_backend = nullptr);

    ~WebRtcMicrophone();

    WebRtcMicrophone(const WebRtcMicrophone&) = delete;
    WebRtcMicrophone& operator=(const WebRtcMicrophone&) = delete;

    /// Adds (`true`) or removes (`false`) the mixer input. Repeating the current state is a no-op.
    void SetPlaying(bool playing);

  private:
    WebRtcMicrophone(async::EventLoop& loop, PullFn pull, AudioBackend* audio_backend);

    // Runs Update() on @p loop unless the object is gone by then.
    static void PostUpdate(async::EventLoop& loop, std::weak_ptr<WebRtcMicrophone> weak);
    // Brings the mixer input in line with playing_. Runs on loop_.
    void Update();
    // Retries Update() whenever the mixer state changes. Runs on loop_.
    void WatchMixer();

    async::EventLoop& loop_;
    AudioBackend* const audio_backend_;
    QemuAudioInject inject_;
    std::atomic<bool> playing_{false};

    // Only used on loop_, and by the destructor.
    // Whether a failure to add the input was logged since playout last started or succeeded.
    bool failure_logged_ = false;
    std::shared_ptr<AudioInputMux> watched_mux_;
    AudioInputMux::StateObserverId observer_id_ = 0;
};

}  // namespace goldfish::audio
