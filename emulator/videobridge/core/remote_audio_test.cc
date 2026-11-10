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

// End-to-end coverage for audio a client sends, such as a browser microphone.
//
// A client PeerConnection negotiates with a real participant the way the web SDK does with
// `microphone: true`: it offers a sendrecv audio transceiver with no track, and turns its
// microphone on and off with replaceTrack() alone, without renegotiating. The tests check when
// GoldfishAudioDeviceModule reports playout starting and stopping, which is what tells the
// emulator to open the guest microphone input, and that a tone the client sends comes out of
// PullPlayout(), which is what feeds it.
//
// Both peers share the Switchboard's factory, and so its audio device module, and run on an
// in-memory network: no port is bound and no host interface is enumerated.

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-reference-return"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "p2p/client/basic_port_allocator.h"
#pragma clang diagnostic pop

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "goldfish/videobridge/goldfish_audio_device_module.h"
#include "goldfish/videobridge/in_process_audio_source.h"
#include "goldfish/videobridge/media_provider.h"
#include "goldfish/videobridge/switchboard.h"
#include "nlohmann/json.hpp"
#include "participant.h"
#include "virtual_network_fixture.h"

namespace goldfish::videobridge {
namespace {

using ::webrtc::RtpTransceiverDirection;

constexpr char kSessionId[] = "测试_🚀_é-session";
constexpr uint32_t kRateHz = GoldfishAudioDeviceModule::kPlayoutSampleRateHz;
constexpr size_t kBlock = kRateHz / 100;
constexpr absl::Duration kBudget = absl::Seconds(10);
constexpr double kToneHz = 1000;
constexpr double kOtherHz = 2500;

// Adds no tracks. The emulator would add a speaker track, but the client shares this factory's
// audio device module, so receiving it would start playout on the client's behalf.
class SilentMediaProvider : public MediaProvider {
  public:
    absl::Status AddTracks(::webrtc::PeerConnectionFactoryInterface*,
                           ::webrtc::PeerConnectionInterface*) override {
        return absl::OkStatus();
    }
};

class AudioSwitchboard : public Switchboard {
  public:
    explicit AudioSwitchboard(::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device)
            : Switchboard(std::make_shared<SilentMediaProvider>(), nullptr,
                          VirtualSwitchboard::MakeVirtualSubstrate(), std::move(audio_device)) {}
};

// Records what the audio device module reports about playout.
class PlayoutEvents {
  public:
    explicit PlayoutEvents(GoldfishAudioDeviceModule& audio_device) : audio_device_(audio_device) {
        audio_device_.SetPlayoutListener([this](bool playing) {
            absl::MutexLock lock(mutex_);
            events_.push_back(playing);
        });
    }

    ~PlayoutEvents() { audio_device_.SetPlayoutListener(nullptr); }

    // Waits until @p count events arrived, or kBudget passed; returns all of them.
    std::vector<bool> Await(size_t count) {
        absl::MutexLock lock(mutex_);
        auto arrived = [this, count]() ABSL_SHARED_LOCKS_REQUIRED(mutex_) {
            return events_.size() >= count;
        };
        mutex_.AwaitWithTimeout(absl::Condition(&arrived), kBudget);
        return events_;
    }

    // Returns the events so far after giving late ones @p settle to arrive.
    std::vector<bool> After(absl::Duration settle) {
        absl::SleepFor(settle);
        absl::MutexLock lock(mutex_);
        return events_;
    }

  private:
    GoldfishAudioDeviceModule& audio_device_;
    absl::Mutex mutex_;
    std::vector<bool> events_ ABSL_GUARDED_BY(mutex_);
};

// Completes a SetLocalDescription() or SetRemoteDescription() call.
class Completion : public ::webrtc::SetLocalDescriptionObserverInterface,
                   public ::webrtc::SetRemoteDescriptionObserverInterface {
  public:
    void OnSetLocalDescriptionComplete(::webrtc::RTCError error) override { Done(error); }
    void OnSetRemoteDescriptionComplete(::webrtc::RTCError error) override { Done(error); }

    bool Await() { return done_.WaitForNotificationWithTimeout(kBudget) && ok_; }

  private:
    void Done(const ::webrtc::RTCError& error) {
        ok_ = error.ok();
        EXPECT_TRUE(ok_) << error.message();
        done_.Notify();
    }

    absl::Notification done_;
    bool ok_ = false;
};

// The browser's side of the session: one audio transceiver in @p direction.
class Client : public EmptyConnectionObserver {
  public:
    Client(Switchboard& board, RtpTransceiverDirection direction) : board_(board) {
        board_.SignalingThread()->BlockingCall([this] {
            auto allocator = std::make_unique<::webrtc::BasicPortAllocator>(
                    ::webrtc::CreateEnvironment(), board_.GetNetworkManager(),
                    board_.GetSocketFactory());
            allocator->SetNetworkIgnoreMask(0);
            ::webrtc::PeerConnectionDependencies dependencies(this);
            dependencies.allocator = std::move(allocator);
            auto pc = board_.GetPeerConnectionFactory()->CreatePeerConnectionOrError(
                    ::webrtc::PeerConnectionInterface::RTCConfiguration(), std::move(dependencies));
            ASSERT_TRUE(pc.ok()) << pc.error().message();
            pc_ = pc.MoveValue();
        });
        ::webrtc::RtpTransceiverInit init;
        init.direction = direction;
        auto transceiver = pc_->AddTransceiver(::webrtc::MediaType::AUDIO, init);
        EXPECT_TRUE(transceiver.ok());
        audio_ = transceiver.MoveValue();
    }

    ~Client() override { pc_->Close(); }

    void OnDataChannel(::webrtc::scoped_refptr<::webrtc::DataChannelInterface>) override {}

    void OnIceCandidate(const ::webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);
        absl::MutexLock lock(mutex_);
        candidates_.push_back({{"candidate",
                                {{"sdpMid", candidate->sdp_mid()},
                                 {"sdpMLineIndex", candidate->sdp_mline_index()},
                                 {"candidate", sdp}}}});
    }

    void OnConnectionChange(::webrtc::PeerConnectionInterface::PeerConnectionState state) override {
        absl::MutexLock lock(mutex_);
        connected_ = state == ::webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
    }

    // Offers, applies the participant's answer, and waits for the transport to connect.
    bool Connect() { return Negotiate() && AwaitConnected(); }

    // Attaches or detaches the microphone track, like RtcController's replaceTrack().
    void SetMicrophone(bool on) {
        EXPECT_TRUE(audio_->sender()->SetTrack(on ? microphone_track_.get() : nullptr));
    }

    // Sends `samples` as the microphone input.
    void Speak(const std::vector<int16_t>& samples) {
        microphone_->OnAudioData(samples.data(), samples.size());
    }

  private:
    bool Negotiate() {
        auto set_local = ::webrtc::make_ref_counted<Completion>();
        pc_->SetLocalDescription(set_local);
        if (!set_local->Await()) {
            return false;
        }
        std::string offer;
        pc_->local_description()->ToString(&offer);
        if (!board_.AcceptJsepMessage(kSessionId,
                                      nlohmann::json{{"type", "offer"}, {"sdp", offer}}.dump())
                     .ok()) {
            return false;
        }
        const absl::Time deadline = absl::Now() + kBudget;
        while (absl::Now() < deadline) {
            auto answer = Pump();
            if (!answer.empty()) {
                ::webrtc::SdpParseError error;
                auto completion = ::webrtc::make_ref_counted<Completion>();
                pc_->SetRemoteDescription(::webrtc::CreateSessionDescription(
                                                  ::webrtc::SdpType::kAnswer, answer, &error),
                                          completion);
                return completion->Await();
            }
        }
        return false;
    }

    bool AwaitConnected() {
        const absl::Time deadline = absl::Now() + kBudget;
        while (absl::Now() < deadline) {
            Pump();
            absl::MutexLock lock(mutex_);
            if (connected_) {
                return true;
            }
        }
        return false;
    }

    // Forwards queued candidates both ways; returns an answer's SDP if one arrived.
    std::string Pump() {
        std::vector<nlohmann::json> outgoing;
        {
            absl::MutexLock lock(mutex_);
            outgoing.swap(candidates_);
        }
        for (const auto& candidate : outgoing) {
            EXPECT_TRUE(board_.AcceptJsepMessage(kSessionId, candidate.dump()).ok());
        }
        auto next = board_.NextMessage(kSessionId, absl::Milliseconds(20));
        if (!next.ok()) {
            return "";
        }
        auto msg = nlohmann::json::parse(*next, nullptr, false);
        if (msg.contains("sdp")) {
            return msg["sdp"].get<std::string>();
        }
        if (msg.contains("candidate")) {
            const auto& c = msg["candidate"];
            ::webrtc::SdpParseError error;
            std::unique_ptr<::webrtc::IceCandidateInterface> candidate(::webrtc::CreateIceCandidate(
                    c["sdpMid"].get<std::string>(), c["sdpMLineIndex"].get<int>(),
                    c["candidate"].get<std::string>(), &error));
            if (candidate) {
                pc_->AddIceCandidate(std::move(candidate), [](::webrtc::RTCError) {});
            }
        }
        return "";
    }

    Switchboard& board_;
    ::webrtc::scoped_refptr<::webrtc::PeerConnectionInterface> pc_;
    ::webrtc::scoped_refptr<::webrtc::RtpTransceiverInterface> audio_;
    ::webrtc::scoped_refptr<InProcessAudioSource> microphone_ =
            ::webrtc::make_ref_counted<InProcessAudioSource>(kRateHz, 1);
    ::webrtc::scoped_refptr<::webrtc::AudioTrackInterface> microphone_track_ =
            board_.GetPeerConnectionFactory()->CreateAudioTrack("microphone", microphone_.get());

    absl::Mutex mutex_;
    std::vector<nlohmann::json> candidates_ ABSL_GUARDED_BY(mutex_);
    bool connected_ ABSL_GUARDED_BY(mutex_) = false;
};

// Power of @p samples at @p frequency_hz (Goertzel).
double PowerAt(const std::vector<int16_t>& samples, double frequency_hz) {
    const double coefficient = 2 * std::cos(2 * std::numbers::pi * frequency_hz / kRateHz);
    double previous = 0;
    double before_previous = 0;
    for (int16_t sample : samples) {
        const double current = sample + coefficient * previous - before_previous;
        before_previous = previous;
        previous = current;
    }
    return previous * previous + before_previous * before_previous -
           coefficient * previous * before_previous;
}

// PowerAt() of a tone of the amplitude Talk() sends, over @p count samples: (A * N / 2)^2.
double FullTonePower(size_t count) {
    const double full = 8000.0 * static_cast<double>(count) / 2;
    return full * full;
}

class RemoteAudioTest : public ::testing::Test {
  protected:
    void SetUp() override { ASSERT_TRUE(board_.Connect(kSessionId)); }

    void TearDown() override { board_.Disconnect(kSessionId); }

    // Speaks a tone (or silence) and pulls in real time for @p blocks 10 ms periods, as a guest
    // recording 10 ms periods would; returns the second half, by when NetEq has settled.
    std::vector<int16_t> Talk(Client& client, bool tone_on, int blocks) {
        std::vector<int16_t> tone(kBlock);
        std::vector<int16_t> block(kBlock);
        std::vector<int16_t> played;
        absl::Time next = absl::Now();
        for (int i = 0; i < blocks; ++i) {
            for (int16_t& sample : tone) {
                sample = static_cast<int16_t>(
                        8000 * std::sin(2 * std::numbers::pi * kToneHz * phase_++ / kRateHz));
            }
            if (tone_on) {
                client.Speak(tone);
            }
            EXPECT_EQ(audio_device_->PullPlayout(block), kBlock);
            if (i >= blocks / 2) {
                played.insert(played.end(), block.begin(), block.end());
            }
            next += absl::Milliseconds(10);
            absl::SleepFor(next - absl::Now());
        }
        return played;
    }

    ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device_ =
            GoldfishAudioDeviceModule::Create();
    PlayoutEvents playout_{*audio_device_};
    AudioSwitchboard board_{audio_device_};
    size_t phase_ = 0;
};

TEST_F(RemoteAudioTest, PlayoutStartsWhenAClientOffersMicrophoneAudio) {
    Client client(board_, RtpTransceiverDirection::kSendRecv);

    ASSERT_TRUE(client.Connect());

    // No track is attached yet: offering to send is enough.
    EXPECT_EQ(playout_.Await(1), std::vector<bool>({true}));
    EXPECT_TRUE(audio_device_->Playing());
}

TEST_F(RemoteAudioTest, RecvonlyClientNeverStartsPlayout) {
    Client client(board_, RtpTransceiverDirection::kRecvOnly);

    ASSERT_TRUE(client.Connect());

    EXPECT_TRUE(playout_.After(absl::Milliseconds(300)).empty());
    EXPECT_FALSE(audio_device_->Playing());
}

TEST_F(RemoteAudioTest, MicrophoneToneComesOutOfPlayoutAfterReplaceTrack) {
    Client client(board_, RtpTransceiverDirection::kSendRecv);
    ASSERT_TRUE(client.Connect());
    ASSERT_EQ(playout_.Await(1), std::vector<bool>({true}));

    client.SetMicrophone(true);
    const std::vector<int16_t> played = Talk(client, /*tone_on=*/true, 150);

    const double tone_power = PowerAt(played, kToneHz);
    EXPECT_GT(tone_power, 100 * PowerAt(played, kOtherHz));
    // Allow 20 dB of codec and NetEq loss.
    EXPECT_GT(tone_power, FullTonePower(played.size()) / 100);
}

TEST_F(RemoteAudioTest, RemovingTheTrackKeepsPlayoutUntilTheSessionCloses) {
    {
        Client client(board_, RtpTransceiverDirection::kSendRecv);
        ASSERT_TRUE(client.Connect());
        client.SetMicrophone(true);
        Talk(client, /*tone_on=*/true, 20);

        client.SetMicrophone(false);
        const std::vector<int16_t> played = Talk(client, /*tone_on=*/false, 50);

        EXPECT_EQ(playout_.After(absl::Milliseconds(100)), std::vector<bool>({true}));
        EXPECT_LT(PowerAt(played, kToneHz), FullTonePower(played.size()) / 1000);
    }
    board_.Disconnect(kSessionId);

    EXPECT_EQ(playout_.Await(2), std::vector<bool>({true, false}));
    EXPECT_FALSE(audio_device_->Playing());
}

}  // namespace
}  // namespace goldfish::videobridge
