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

#include "goldfish/audio/webrtc_microphone.h"

#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"

namespace goldfish::audio {

std::shared_ptr<WebRtcMicrophone> WebRtcMicrophone::Create(async::EventLoop& loop, PullFn pull,
                                                           AudioBackend* audio_backend) {
    return std::shared_ptr<WebRtcMicrophone>(
            new WebRtcMicrophone(loop, std::move(pull), audio_backend));
}

WebRtcMicrophone::WebRtcMicrophone(async::EventLoop& loop, PullFn pull, AudioBackend* audio_backend)
        : loop_(loop)
        , audio_backend_(audio_backend)
        , inject_(
                  [pull = std::move(pull)](void* dst, size_t max_frames) {
                      // Mono, so a frame is one sample.
                      return pull(std::span(static_cast<int16_t*>(dst), max_frames));
                  },
                  kConfig, /*active_cb=*/nullptr, audio_backend,
                  MicrophoneSourceMetadata{.source_id = std::string(kSourceId),
                                           .kind = MicrophoneInputKind::kWebRtc}) {}

WebRtcMicrophone::~WebRtcMicrophone() {
    if (watched_mux_) {
        watched_mux_->RemoveStateObserver(observer_id_);
    }
}

void WebRtcMicrophone::SetPlaying(bool playing) {
    if (playing_.exchange(playing) != playing) {
        PostUpdate(loop_, weak_from_this());
    }
}

void WebRtcMicrophone::PostUpdate(async::EventLoop& loop, std::weak_ptr<WebRtcMicrophone> weak) {
    const absl::Status posted = loop.Post([weak = std::move(weak)] {
        if (auto self = weak.lock()) {
            self->Update();
        }
    });
    if (!posted.ok()) {
        LOG(WARNING) << "Could not schedule an update of the \"" << kSourceId
                     << "\" microphone input: " << posted
                     << ". The event loop is likely shutting down.";
    }
}

void WebRtcMicrophone::Update() {
    if (!playing_.load()) {
        inject_.Stop();
        failure_logged_ = false;
        return;
    }
    if (inject_.IsRunning()) {
        return;
    }
    const absl::Status status = inject_.Start();
    if (status.ok()) {
        failure_logged_ = false;
        return;
    }
    // Only these clear up as other inputs stop; the rest last as long as the device or mixer.
    const bool retryable = absl::IsResourceExhausted(status) || absl::IsAlreadyExists(status);
    if (!failure_logged_) {
        if (retryable) {
            LOG(WARNING) << "Could not add the \"" << kSourceId
                         << "\" input for WebRTC audio to the guest microphone mixer: " << status
                         << ". It is added once the mixer has room; to make room, stop another "
                            "microphone input (see AudioService.GetAudioState).";
        } else {
            LOG(WARNING) << "Dropping WebRTC microphone audio: could not add the \"" << kSourceId
                         << "\" input to the guest microphone mixer: " << status;
        }
        failure_logged_ = true;
    }
    if (retryable) {
        WatchMixer();
    }
}

void WebRtcMicrophone::WatchMixer() {
    if (watched_mux_) {
        return;
    }
    watched_mux_ = ResolveTargetAudioInputMux(audio_backend_);
    if (!watched_mux_) {
        return;
    }
    // Runs with the BQL held on whichever thread changed the mixer, so it only posts. It must
    // not keep this object alive: the destructor removes the observer and waits for it.
    observer_id_ = watched_mux_->AddStateObserver(
            [loop = &loop_, weak = weak_from_this()](const AudioInputStateSnapshot&) {
                PostUpdate(*loop, weak);
            });
}

}  // namespace goldfish::audio
