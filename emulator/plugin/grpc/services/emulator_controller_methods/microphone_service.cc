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

#include "microphone_service.h"

#include <string>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"

#include "android/emulation/control/absl_status_translate.h"
#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/qemu_audio_inject.h"

namespace android::emulation::control {
namespace {

using ::goldfish::audio::AudioInputMux;
using ::goldfish::audio::AudioInputStateMutation;
using ::goldfish::audio::AudioInputStateSnapshot;
using ::goldfish::audio::kHostMicrophoneSourceId;
using ::goldfish::audio::MicrophoneInputChannelUpdate;
using ::goldfish::audio::MicrophoneInputInfo;

grpc::Status MissingMixerStatus(std::string_view operation) {
    return {grpc::StatusCode::FAILED_PRECONDITION,
            absl::StrFormat("Failed to execute %s: this device has no virtual microphone mixer "
                            "(goldfish-audio-mux), or it has shut down. Start the emulator with "
                            "audio input enabled (hw.audioInput=yes, without -no-audio).",
                            operation)};
}

// True only if the current thread holds the BQL. Without an installed VmLock there is no BQL;
// the default VmLock::IsLockedBySelf() would claim otherwise.
bool HoldsBql() {
    return ::android::goldfish::VmLock::HasInstance() &&
           ::android::goldfish::VmLock::Get()->IsLockedBySelf();
}

bool HostMicrophoneEnabled(const AudioInputStateSnapshot& snapshot) {
    for (const MicrophoneInputInfo& input : snapshot.microphone_inputs) {
        if (input.source_id == kHostMicrophoneSourceId) {
            return !input.muted;
        }
    }
    return false;
}

}  // namespace

MicrophoneServiceImpl::MicrophoneServiceImpl(GrpcNotificationEventSource& notifications,
                                             ::goldfish::async::EventLoop& loop,
                                             AudioBackend* audio_backend)
        : notifications_(notifications), loop_(loop), audio_backend_(audio_backend) {}

MicrophoneServiceImpl::~MicrophoneServiceImpl() {
    absl::MutexLock lock(bind_mutex_);
    Unbind();
}

void MicrophoneServiceImpl::Unbind() {
    if (mux_ == nullptr) {
        return;
    }
    // Waits for an observer call in progress; it only posts, and never takes bind_mutex_. A task
    // already queued keeps the old publisher, and fires at most the old mixer's last state.
    mux_->RemoveStateObserver(observer_id_);
    mux_.reset();
    publisher_.reset();
    observer_id_ = 0;
}

grpc::Status MicrophoneServiceImpl::getMicrophoneState(MicrophoneState* reply) {
    std::shared_ptr<AudioInputMux> mux = BindMux();
    if (mux == nullptr) {
        return MissingMixerStatus("getMicrophoneState");
    }
    reply->set_realaudioenabled(HostMicrophoneEnabled(mux->GetStateSnapshot()));
    return grpc::Status::OK;
}

grpc::Status MicrophoneServiceImpl::setMicrophoneState(const MicrophoneState& request) {
    std::shared_ptr<AudioInputMux> mux = BindMux();
    if (mux == nullptr) {
        return MissingMixerStatus("setMicrophoneState");
    }
    const AudioInputStateMutation mutation{
        .input_updates = {MicrophoneInputChannelUpdate{
            .source_id = std::string(kHostMicrophoneSourceId),
            .muted = !request.realaudioenabled()}},
    };
    auto updated = ::goldfish::audio::UpdateAudioInputMuxState(*mux, mutation);
    if (absl::IsUnavailable(updated.status()) || absl::IsFailedPrecondition(updated.status())) {
        return MissingMixerStatus("setMicrophoneState");
    }
    return AbslStatusToGrpcStatus(updated.status());
}

void MicrophoneServiceImpl::Watch() {
    (void)BindMux();
}

std::shared_ptr<AudioInputMux> MicrophoneServiceImpl::BindMux() {
    absl::MutexLock lock(bind_mutex_);
    if (mux_ != nullptr && !mux_->IsShutdown()) {
        return mux_;
    }
    // A mixer that shut down reports stale state and rejects changes; look again, in case a new
    // one replaced it.
    Unbind();
    std::shared_ptr<AudioInputMux> mux =
            ::goldfish::audio::ResolveTargetAudioInputMux(audio_backend_);
    if (mux == nullptr || mux->IsShutdown()) {
        return nullptr;
    }
    publisher_ = std::make_shared<Publisher>(notifications_, mux);
    // Runs with the BQL held on the thread that changed the mixer, so it only posts. Holding the
    // publisher weakly keeps the last reference from being dropped here.
    observer_id_ =
            mux->AddStateObserver([loop = &loop_, weak = std::weak_ptr<Publisher>(publisher_)](
                                          const AudioInputStateSnapshot&) {
                const absl::Status posted = loop->Post([weak] {
                    if (auto publisher = weak.lock()) {
                        publisher->Publish();
                    }
                });
                if (!posted.ok()) {
                    VLOG(1) << "Dropped a MicrophoneState notification: " << posted;
                }
            });
    mux_ = std::move(mux);
    // Fires the current state right away, so a stream subscribing next finds it in the
    // notification store. Callers are gRPC threads, which hold no BQL (checked in Publish()).
    publisher_->Publish();
    return mux_;
}

void MicrophoneServiceImpl::Publisher::Publish() {
    // Notification listeners take gRPC stream locks. They must never run under the BQL, so the
    // mixer observer posts here instead of calling this.
    DCHECK(!HoldsBql()) << "MicrophoneState notifications must not fire with the BQL held.";
    absl::MutexLock lock(mutex_);
    // Read under the lock: a snapshot taken earlier could be older than one already fired.
    const bool enabled = HostMicrophoneEnabled(mux_->GetStateSnapshot());
    if (last_enabled_ == enabled) {
        return;
    }
    last_enabled_ = enabled;
    Notification notification;
    notification.mutable_microphonestate()->set_realaudioenabled(enabled);
    notifications_.FireEvent(notification);
}

}  // namespace android::emulation::control
