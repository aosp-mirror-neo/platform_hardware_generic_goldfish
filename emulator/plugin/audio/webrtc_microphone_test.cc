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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/qemu_audio_inject.h"
#include "test_bql.h"
#include "test_qemu_audio_stub.h"

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
AudioBackend* audio_be_by_name(const char* name, void* errp);
}

namespace goldfish::audio {
namespace {

using ::android::goldfish::ScopedVmLock;
using ::android::goldfish::ScopedVmUnlock;
using ::goldfish::async::testing::TestEventLoop;
using ::testing::_;
using ::testing::AllOf;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr char kAgent[] = "测试_🚀_é-agent";
constexpr int16_t kWebRtcSample = 1234;

class StubHostInput final : public HostAudioInputInterface {
  public:
    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* /*name*/,
                                  const GuestAudioConfig& /*config*/,
                                  GuestDataCallback /*on_host_data*/) override {
        return existing ? existing : &dummy_voice_;
    }
    void CloseHostVoice(HostVoiceHandle /*voice*/) override {}
    void SetHostVoiceActive(HostVoiceHandle /*voice*/, bool /*active*/) override {}
    void SetHostVoiceVolume(HostVoiceHandle /*voice*/,
                            const GuestChannelVolume& /*volume*/) override {}
    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override {
        std::memset(dst, 0, byte_count);
        return byte_count;
    }
    void SetTimerArmed(bool /*armed*/) override {}
    absl::Duration NowVirtualTime() const override { return absl::Milliseconds(now_ms_); }

    void AdvanceMs(int64_t ms) { now_ms_ += ms; }

  private:
    int dummy_voice_ = 1;
    int64_t now_ms_ = 1000;
};

std::vector<std::string> InputIds(const AudioInputStateSnapshot& snapshot) {
    std::vector<std::string> ids;
    for (const MicrophoneInputInfo& input : snapshot.microphone_inputs) {
        ids.push_back(input.source_id);
    }
    return ids;
}

// A mono S16 source that fills every request with @p value.
QemuAudioInject::ReadCallback ConstantFrames(int16_t value) {
    return [value](void* dst, size_t frames) {
        std::fill_n(static_cast<int16_t*>(dst), frames, value);
        return frames;
    };
}

std::vector<std::string> WebRtcInputs(const AudioInputStateSnapshot& snapshot) {
    std::vector<std::string> ids;
    for (const MicrophoneInputInfo& input : snapshot.microphone_inputs) {
        if (input.kind == MicrophoneInputKind::kWebRtc) {
            ids.push_back(input.source_id);
        }
    }
    return ids;
}

// The test body holds the VM lock, like QEMU device code.
class WebRtcMicrophoneTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_reset_audio_stubs();
        backend_ = audio_be_by_name("mainaudiodev", nullptr);
        mux_ = std::make_shared<AudioInputMux>(&host_);
        RegisterAudioInputMux(backend_, mux_);
        microphone_ = WebRtcMicrophone::Create(
                *loop_,
                [this](std::span<int16_t> dst) {
                    ++pulls_;
                    std::fill(dst.begin(), dst.end(), kWebRtcSample);
                    return dst.size();
                },
                backend_);
    }

    void TearDown() override {
        microphone_.reset();
        UnregisterAudioInputMux(backend_);
        mux_->Shutdown();
        test_reset_audio_stubs();
    }

    // Runs the queued updates on the loop's thread, which takes the VM lock itself.
    void RunLoop() {
        ScopedVmUnlock unlock;
        loop_->RunAll();
    }

    // Opens and starts a 48 kHz mono guest capture stream, then advances it by 10 ms.
    std::vector<int16_t> CaptureTenMilliseconds() {
        std::vector<int16_t> captured;
        MuxVoiceHandle* voice = nullptr;
        voice = mux_->OpenVoice(
                nullptr, "virtio-sound.in",
                GuestAudioConfig{
                    .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
                [this, &voice, &captured](int avail_bytes) {
                    std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                    const size_t read = mux_->ReadVoice(voice, buf.data(), buf.size() * 2);
                    captured.insert(captured.end(), buf.begin(), buf.begin() + read / 2);
                });
        mux_->SetVoiceActive(voice, true);
        host_.AdvanceMs(10);
        mux_->OnTimerTick();
        mux_->CloseVoice(voice);
        return captured;
    }

    // Starts kMaxConcurrentMicrophoneStreams silent injectors.
    std::vector<std::unique_ptr<QemuAudioInject>> FillMixer() {
        std::vector<std::unique_ptr<QemuAudioInject>> agents;
        for (int i = 0; i < kMaxConcurrentMicrophoneStreams; ++i) {
            agents.push_back(std::make_unique<QemuAudioInject>(
                    ConstantFrames(0), AudioStreamConfig{}, nullptr, backend_));
            EXPECT_TRUE(agents.back()->Start().ok());
        }
        return agents;
    }

    TestBql bql_;
    ScopedVmLock vm_lock_;
    StubHostInput host_;
    AudioBackend* backend_ = nullptr;
    std::shared_ptr<AudioInputMux> mux_;
    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    std::atomic<int> pulls_{0};
    std::shared_ptr<WebRtcMicrophone> microphone_;
};

TEST_F(WebRtcMicrophoneTest, PlayoutFeedsTheGuestMicrophone) {
    microphone_->SetPlaying(true);
    RunLoop();

    EXPECT_THAT(
            mux_->GetStateSnapshot().microphone_inputs,
            ElementsAre(Field(&MicrophoneInputInfo::source_id, "host"),
                        AllOf(Field(&MicrophoneInputInfo::source_id, "webrtc"),
                              Field(&MicrophoneInputInfo::kind, MicrophoneInputKind::kWebRtc))));
    const std::vector<int16_t> captured = CaptureTenMilliseconds();
    ASSERT_THAT(captured, Not(IsEmpty()));
    EXPECT_THAT(captured, Each(kWebRtcSample));
}

TEST_F(WebRtcMicrophoneTest, AddingTheInputWaitsForTheEventLoop) {
    microphone_->SetPlaying(true);

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());
    RunLoop();
    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), ElementsAre("webrtc"));
}

TEST_F(WebRtcMicrophoneTest, StoppingPlayoutRemovesTheInput) {
    microphone_->SetPlaying(true);
    RunLoop();

    microphone_->SetPlaying(false);
    RunLoop();

    EXPECT_THAT(InputIds(mux_->GetStateSnapshot()), ElementsAre("host"));
}

TEST_F(WebRtcMicrophoneTest, RepeatedReportsChangeNothing) {
    microphone_->SetPlaying(true);
    microphone_->SetPlaying(true);
    EXPECT_EQ(loop_->TaskCount(), 1u);
    RunLoop();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), ElementsAre("webrtc"));
}

TEST_F(WebRtcMicrophoneTest, StartAndStopBeforeTheLoopRunsLeavesNoInput) {
    microphone_->SetPlaying(true);
    microphone_->SetPlaying(false);
    RunLoop();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());
}

TEST_F(WebRtcMicrophoneTest, NothingIsDecodedWhileNoGuestAppRecords) {
    microphone_->SetPlaying(true);
    RunLoop();
    for (int i = 0; i < 10; ++i) {
        host_.AdvanceMs(10);
        mux_->OnTimerTick();
    }
    EXPECT_EQ(pulls_.load(), 0);

    ASSERT_THAT(CaptureTenMilliseconds(), Not(IsEmpty()));
    EXPECT_GT(pulls_.load(), 0);
}

TEST_F(WebRtcMicrophoneTest, MixesWithAnAgentsGrpcStream) {
    QemuAudioInject agent(ConstantFrames(500), AudioStreamConfig{}, nullptr, backend_,
                          MicrophoneSourceMetadata{.source_id = kAgent,
                                                   .kind = MicrophoneInputKind::kGrpcStream});
    ASSERT_TRUE(agent.Start().ok());

    microphone_->SetPlaying(true);
    RunLoop();

    EXPECT_THAT(InputIds(mux_->GetStateSnapshot()), ElementsAre("host", kAgent, "webrtc"));
    EXPECT_THAT(CaptureTenMilliseconds(), Each(kWebRtcSample + 500));
}

TEST_F(WebRtcMicrophoneTest, FullMixerAddsTheInputOnceASlotFrees) {
    std::vector<std::unique_ptr<QemuAudioInject>> agents = FillMixer();
    microphone_->SetPlaying(true);
    RunLoop();
    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());

    agents.back()->Stop();
    RunLoop();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), ElementsAre("webrtc"));
}

TEST_F(WebRtcMicrophoneTest, AgentCannotTakeTheNameFirst) {
    QemuAudioInject impostor(ConstantFrames(0), AudioStreamConfig{}, nullptr, backend_,
                             MicrophoneSourceMetadata{.source_id = "webrtc",
                                                      .kind = MicrophoneInputKind::kGrpcStream});
    EXPECT_EQ(impostor.Start().code(), absl::StatusCode::kInvalidArgument);

    microphone_->SetPlaying(true);
    RunLoop();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), ElementsAre("webrtc"));
}

TEST_F(WebRtcMicrophoneTest, StoppingPlayoutCancelsTheRetry) {
    std::vector<std::unique_ptr<QemuAudioInject>> agents = FillMixer();
    microphone_->SetPlaying(true);
    RunLoop();
    microphone_->SetPlaying(false);
    RunLoop();

    agents.back()->Stop();
    RunLoop();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());
}

TEST_F(WebRtcMicrophoneTest, FullMixerWarnsAgainEachTimePlayoutStarts) {
    std::vector<std::unique_ptr<QemuAudioInject>> agents = FillMixer();
    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _, HasSubstr("once the mixer has room")))
            .Times(2);
    log.StartCapturingLogs();

    microphone_->SetPlaying(true);
    RunLoop();
    microphone_->SetPlaying(false);
    RunLoop();
    microphone_->SetPlaying(true);
    RunLoop();
}

TEST_F(WebRtcMicrophoneTest, MissingMixerDropsTheAudio) {
    UnregisterAudioInputMux(backend_);
    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _, HasSubstr("once the mixer has room")))
            .Times(0);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         AllOf(HasSubstr("Dropping WebRTC microphone audio"),
                               HasSubstr("UNAVAILABLE"))));
    log.StartCapturingLogs();

    microphone_->SetPlaying(true);
    RunLoop();
}

TEST_F(WebRtcMicrophoneTest, ShutDownMixerDropsTheAudio) {
    mux_->Shutdown();
    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         AllOf(HasSubstr("Dropping WebRTC microphone audio"),
                               HasSubstr("FAILED_PRECONDITION"))));
    log.StartCapturingLogs();

    microphone_->SetPlaying(true);
    RunLoop();
}

TEST_F(WebRtcMicrophoneTest, DestroyingReleasesTheInput) {
    microphone_->SetPlaying(true);
    RunLoop();
    ASSERT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), ElementsAre("webrtc"));

    microphone_.reset();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());
}

TEST_F(WebRtcMicrophoneTest, DestroyingWhileWaitingForTheMixerStopsWatchingIt) {
    std::vector<std::unique_ptr<QemuAudioInject>> agents = FillMixer();
    microphone_->SetPlaying(true);
    RunLoop();

    microphone_.reset();
    agents.back()->Stop();

    EXPECT_EQ(loop_->TaskCount(), 0u);
    RunLoop();
    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());
}

TEST_F(WebRtcMicrophoneTest, UpdateQueuedBeforeDestructionIsDropped) {
    microphone_->SetPlaying(true);
    microphone_.reset();

    RunLoop();

    EXPECT_THAT(WebRtcInputs(mux_->GetStateSnapshot()), IsEmpty());
}

}  // namespace
}  // namespace goldfish::audio
