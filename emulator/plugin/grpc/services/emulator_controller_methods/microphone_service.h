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

#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "emulator_controller.pb.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/avd_universe/grpc/grpc_notification_channel.h"

namespace android::emulation::control {

using ::goldfish::avd_universe::grpc::GrpcNotificationEventSource;

/**
 * @brief Implements the v1 microphone RPCs on the guest microphone mixer.
 *
 * `MicrophoneState.realAudioEnabled` is the inverse of the mute flag of the mixer's `"host"`
 * input: the same control v2 `AudioService.UpdateAudioState` exposes. The v2 master mute and the
 * other inputs are left alone.
 *
 * The mixer may be created after this object, so it is looked up on the first call, or on
 * `Watch()`, and again if it shuts down: calls fail with `FAILED_PRECONDITION` until a new one is
 * registered. From then on, every change of the `"host"` mute, whether made through v1 or v2,
 * fires one `MicrophoneState` notification on the notification channel. The first lookup fires
 * the current state.
 *
 * @note **Threading:** Thread-safe. The mixer reports changes with the QEMU BQL held, on whichever
 * thread made them, so its observer only posts to @p loop, and notifications fire there. Each
 * reads the state when it runs: changes faster than the loop are coalesced, and the last
 * notification always matches the mixer. The first lookup fires on the caller's thread, which
 * must not hold the BQL.
 */
class MicrophoneServiceImpl {
  public:
    /**
     * @param notifications Receives `MicrophoneState` notifications. Must outlive the tasks this
     * object posts to @p loop.
     * @param loop Event loop, not a QEMU thread, that fires notifications. Must outlive this
     * object.
     * @param audio_backend Optional QEMU `AudioBackend`; defaults to `"mainaudiodev"`.
     */
    MicrophoneServiceImpl(GrpcNotificationEventSource& notifications,
                          ::goldfish::async::EventLoop& loop,
                          AudioBackend* audio_backend = nullptr);
    ~MicrophoneServiceImpl();

    MicrophoneServiceImpl(const MicrophoneServiceImpl&) = delete;
    MicrophoneServiceImpl& operator=(const MicrophoneServiceImpl&) = delete;

    /// Reports whether the host microphone feeds the guest. `FAILED_PRECONDITION` without a mixer.
    grpc::Status getMicrophoneState(MicrophoneState* reply) ABSL_LOCKS_EXCLUDED(bind_mutex_);

    /// Mutes or unmutes the host microphone input. `FAILED_PRECONDITION` without a mixer.
    grpc::Status setMicrophoneState(const MicrophoneState& request)
            ABSL_LOCKS_EXCLUDED(bind_mutex_);

    /// Looks up the mixer, if not done yet, so that changes start firing notifications.
    void Watch() ABSL_LOCKS_EXCLUDED(bind_mutex_);

  private:
    // Fires notifications. Shared with the tasks posted to the loop, which hold it weakly, so a
    // task that runs after this service is gone does nothing.
    class Publisher {
      public:
        Publisher(GrpcNotificationEventSource& notifications,
                  std::shared_ptr<::goldfish::audio::AudioInputMux> mux)
                : notifications_(notifications), mux_(std::move(mux)) {}

        // Fires a notification if the host microphone state differs from the last one fired.
        void Publish() ABSL_LOCKS_EXCLUDED(mutex_);

      private:
        GrpcNotificationEventSource& notifications_;
        const std::shared_ptr<::goldfish::audio::AudioInputMux> mux_;
        // Held while firing, so notifications go out in the order the state changed.
        absl::Mutex mutex_;
        std::optional<bool> last_enabled_ ABSL_GUARDED_BY(mutex_);
    };

    // Returns the mixer, looking it up again if there is none or it shut down; null if missing.
    std::shared_ptr<::goldfish::audio::AudioInputMux> BindMux() ABSL_LOCKS_EXCLUDED(bind_mutex_);
    // Stops observing the bound mixer, if any.
    void Unbind() ABSL_EXCLUSIVE_LOCKS_REQUIRED(bind_mutex_);

    GrpcNotificationEventSource& notifications_;
    ::goldfish::async::EventLoop& loop_;
    AudioBackend* const audio_backend_;

    absl::Mutex bind_mutex_;
    std::shared_ptr<::goldfish::audio::AudioInputMux> mux_ ABSL_GUARDED_BY(bind_mutex_);
    std::shared_ptr<Publisher> publisher_ ABSL_GUARDED_BY(bind_mutex_);
    ::goldfish::audio::AudioInputMux::StateObserverId observer_id_ ABSL_GUARDED_BY(bind_mutex_) = 0;
};

}  // namespace android::emulation::control
