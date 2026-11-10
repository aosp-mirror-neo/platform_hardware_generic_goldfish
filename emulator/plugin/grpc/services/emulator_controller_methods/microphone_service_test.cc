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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"

#include "emulator/plugin/audio/test_bql.h"
#include "emulator/plugin/audio/test_qemu_audio_stub.h"
#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/qemu_audio_inject.h"

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
AudioBackend* audio_be_by_name(const char* name, void* errp);
}

namespace android::emulation::control {
namespace {

using ::goldfish::async::testing::TestEventLoop;
using ::goldfish::audio::AudioInputMux;
using ::goldfish::audio::AudioInputStateMutation;
using ::goldfish::audio::GuestChannelVolume;
using ::goldfish::audio::HostAudioInputInterface;
using ::goldfish::audio::MicrophoneInputChannelUpdate;
using ::goldfish::avd_universe::grpc::GrpcNotification;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class SilentHostInput final : public HostAudioInputInterface {
  public:
    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* /*name*/,
                                  const ::goldfish::audio::GuestAudioConfig& /*config*/,
                                  GuestDataCallback /*on_host_data*/) override {
        return existing ? existing : &voice_;
    }
    void CloseHostVoice(HostVoiceHandle /*voice*/) override {}
    void SetHostVoiceActive(HostVoiceHandle /*voice*/, bool /*active*/) override {}
    void SetHostVoiceVolume(HostVoiceHandle /*voice*/,
                            const GuestChannelVolume& /*volume*/) override {}
    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* /*dst*/, size_t byte_count) override {
        return byte_count;
    }
    void SetTimerArmed(bool /*armed*/) override {}
    absl::Duration NowVirtualTime() const override { return absl::ZeroDuration(); }

  private:
    int voice_ = 1;
};

// Like gRPC threads, the test body holds no VM lock; it takes it only to change the mixer
// directly, as QEMU device code and v2 UpdateAudioState do.
class MicrophoneServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_reset_audio_stubs();
        backend_ = audio_be_by_name("mainaudiodev", nullptr);
        callback_id_ = notifications_.AddCallback([this](const GrpcNotification& notification) {
            if (notification.has_microphonestate()) {
                absl::MutexLock lock(mutex_);
                events_.push_back(notification.microphonestate().realaudioenabled());
            }
        });
    }

    void TearDown() override {
        service_.reset();
        notifications_.RemoveCallback(callback_id_);
        if (mux_ != nullptr) {
            ::goldfish::audio::UnregisterAudioInputMux(backend_);
            ::android::goldfish::ScopedVmLock lock;
            mux_->Shutdown();
        }
        test_reset_audio_stubs();
    }

    // Registers a mixer whose "host" input starts muted, as goldfish-audio-mux does by default.
    void InstallMixer(bool host_muted = true) {
        mux_ = std::make_shared<AudioInputMux>(&host_);
        SetHostMuted(host_muted);
        ::goldfish::audio::RegisterAudioInputMux(backend_, mux_);
    }

    // Shuts the mixer down, as QEMU teardown does, and returns it.
    std::shared_ptr<AudioInputMux> ShutDownMixer() {
        ::goldfish::audio::UnregisterAudioInputMux(backend_);
        ::android::goldfish::ScopedVmLock lock;
        mux_->Shutdown();
        return std::exchange(mux_, nullptr);
    }

    void UpdateMixer(const AudioInputStateMutation& mutation) {
        ::android::goldfish::ScopedVmLock lock;
        ASSERT_TRUE(mux_->UpdateState(mutation).ok());
    }

    void SetHostMuted(bool muted) {
        UpdateMixer(AudioInputStateMutation{
            .input_updates = {MicrophoneInputChannelUpdate{.source_id = "host", .muted = muted}}});
    }

    bool HostMuted() const { return mux_->GetStateSnapshot().microphone_inputs.at(0).muted; }

    // Fires the notifications the mixer observer queued.
    std::vector<bool> Events() {
        loop_->RunAll();
        absl::MutexLock lock(mutex_);
        return events_;
    }

    ::goldfish::audio::TestBql bql_;
    SilentHostInput host_;
    AudioBackend* backend_ = nullptr;
    std::shared_ptr<AudioInputMux> mux_;
    GrpcNotificationEventSource notifications_;
    GrpcNotificationEventSource::CallbackId callback_id_{};
    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    std::unique_ptr<MicrophoneServiceImpl> service_ =
            std::make_unique<MicrophoneServiceImpl>(notifications_, *loop_, nullptr);

    absl::Mutex mutex_;
    std::vector<bool> events_ ABSL_GUARDED_BY(mutex_);
};

TEST_F(MicrophoneServiceTest, ReportsTheHostInputMuteAsRealAudio) {
    InstallMixer();
    MicrophoneState state;

    ASSERT_TRUE(service_->getMicrophoneState(&state).ok());
    EXPECT_FALSE(state.realaudioenabled());

    SetHostMuted(false);
    ASSERT_TRUE(service_->getMicrophoneState(&state).ok());
    EXPECT_TRUE(state.realaudioenabled());
}

TEST_F(MicrophoneServiceTest, SetUnmutesAndMutesTheHostInput) {
    InstallMixer();
    MicrophoneState enable;
    enable.set_realaudioenabled(true);
    MicrophoneState disable;
    disable.set_realaudioenabled(false);

    ASSERT_TRUE(service_->setMicrophoneState(enable).ok());
    EXPECT_FALSE(HostMuted());

    ASSERT_TRUE(service_->setMicrophoneState(disable).ok());
    EXPECT_TRUE(HostMuted());
}

TEST_F(MicrophoneServiceTest, LeavesTheMasterMuteAndInjectorsAlone) {
    InstallMixer();
    UpdateMixer(AudioInputStateMutation{.microphone_muted = true});
    ::goldfish::audio::QemuAudioInject agent([](void*, size_t frames) { return frames; },
                                             ::goldfish::audio::AudioStreamConfig{}, nullptr,
                                             backend_, {.source_id = "测试_🚀_é-agent"});
    ASSERT_TRUE(agent.Start().ok());
    MicrophoneState enable;
    enable.set_realaudioenabled(true);

    ASSERT_TRUE(service_->setMicrophoneState(enable).ok());

    const auto snapshot = mux_->GetStateSnapshot();
    EXPECT_TRUE(snapshot.microphone_muted);
    ASSERT_EQ(snapshot.microphone_inputs.size(), 2U);
    EXPECT_FALSE(snapshot.microphone_inputs[0].muted);
    EXPECT_EQ(snapshot.microphone_inputs[1].source_id, "测试_🚀_é-agent");
    EXPECT_FALSE(snapshot.microphone_inputs[1].muted);
    agent.Stop();
}

TEST_F(MicrophoneServiceTest, WithoutAMixerBothCallsFailWithAPrecondition) {
    MicrophoneState state;

    const grpc::Status get = service_->getMicrophoneState(&state);
    const grpc::Status set = service_->setMicrophoneState(state);

    EXPECT_EQ(get.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_THAT(get.error_message(), ::testing::HasSubstr("hw.audioInput"));
    EXPECT_EQ(set.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_THAT(Events(), IsEmpty());
}

TEST_F(MicrophoneServiceTest, FindsAMixerCreatedAfterTheService) {
    MicrophoneState state;
    ASSERT_FALSE(service_->getMicrophoneState(&state).ok());

    InstallMixer();

    EXPECT_TRUE(service_->getMicrophoneState(&state).ok());
}

TEST_F(MicrophoneServiceTest, NothingIsNotifiedBeforeTheMixerIsLookedUp) {
    InstallMixer();

    SetHostMuted(false);

    EXPECT_THAT(Events(), IsEmpty());
}

TEST_F(MicrophoneServiceTest, WatchNotifiesTheCurrentStateOnce) {
    InstallMixer();

    service_->Watch();
    service_->Watch();

    EXPECT_THAT(Events(), ElementsAre(false));
}

TEST_F(MicrophoneServiceTest, NotifiesEveryHostMuteChangeFromV1AndV2) {
    InstallMixer();
    service_->Watch();
    MicrophoneState enable;
    enable.set_realaudioenabled(true);

    ASSERT_TRUE(service_->setMicrophoneState(enable).ok());  // v1
    EXPECT_THAT(Events(), ElementsAre(false, true));
    SetHostMuted(true);  // v2 UpdateAudioState
    EXPECT_THAT(Events(), ElementsAre(false, true, false));
}

TEST_F(MicrophoneServiceTest, ChangesFasterThanTheLoopReportTheLatestState) {
    InstallMixer();
    service_->Watch();

    SetHostMuted(false);
    SetHostMuted(true);
    SetHostMuted(false);

    // Each queued notification reads the state when it runs, so none is stale.
    EXPECT_THAT(Events(), ElementsAre(false, true));
}

TEST_F(MicrophoneServiceTest, OtherMixerChangesAreNotNotified) {
    InstallMixer();
    service_->Watch();

    UpdateMixer(AudioInputStateMutation{.microphone_muted = true});
    UpdateMixer(AudioInputStateMutation{
        .input_updates = {MicrophoneInputChannelUpdate{.source_id = "host", .gain = 0.5F}}});
    SetHostMuted(true);

    EXPECT_THAT(Events(), ElementsAre(false));
}

TEST_F(MicrophoneServiceTest, MixerChangesOnlyQueueNotificationsOnTheLoop) {
    InstallMixer();
    service_->Watch();

    SetHostMuted(false);

    EXPECT_EQ(loop_->TaskCount(), 1U);
    {
        absl::MutexLock lock(mutex_);
        EXPECT_THAT(events_, ElementsAre(false));
    }
    EXPECT_THAT(Events(), ElementsAre(false, true));
}

TEST_F(MicrophoneServiceTest, NotificationQueuedBeforeDestructionIsDropped) {
    InstallMixer();
    service_->Watch();
    SetHostMuted(false);

    service_.reset();

    EXPECT_THAT(Events(), ElementsAre(false));
}

TEST_F(MicrophoneServiceTest, ShutDownMixerFailsWithAPrecondition) {
    InstallMixer();
    MicrophoneState state;
    ASSERT_TRUE(service_->getMicrophoneState(&state).ok());

    ShutDownMixer();

    EXPECT_EQ(service_->getMicrophoneState(&state).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(service_->setMicrophoneState(state).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(MicrophoneServiceTest, FollowsAMixerThatReplacesAShutDownOne) {
    InstallMixer();
    service_->Watch();
    std::shared_ptr<AudioInputMux> old_mux = ShutDownMixer();

    InstallMixer(/*host_muted=*/false);
    MicrophoneState state;
    ASSERT_TRUE(service_->getMicrophoneState(&state).ok());
    EXPECT_TRUE(state.realaudioenabled());
    SetHostMuted(true);

    // The new mixer's state on rebinding, then its change.
    EXPECT_THAT(Events(), ElementsAre(false, true, false));
}

TEST_F(MicrophoneServiceTest, NotificationsNeverFireWithTheBqlHeld) {
    InstallMixer();
    ::android::goldfish::ScopedVmLock lock;

    EXPECT_DEBUG_DEATH(service_->Watch(), "must not fire with the BQL held");
}

TEST_F(MicrophoneServiceTest, DestroyingTheServiceStopsNotifications) {
    InstallMixer();
    service_->Watch();

    service_.reset();
    SetHostMuted(false);

    EXPECT_THAT(Events(), ElementsAre(false));
}

}  // namespace
}  // namespace android::emulation::control
