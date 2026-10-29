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

#include "goldfish/audio/qemu_audio_inject.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/audio_input_mux.h"
#include "test_bql.h"
#include "test_qemu_audio_stub.h"

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
AudioBackend* audio_be_by_name(const char* name, void* errp);
}

namespace goldfish::audio {
namespace {

class StubHostInput final : public HostAudioInputInterface {
  public:
    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* /*name*/,
                                  const GuestAudioConfig& /*config*/,
                                  GuestDataCallback /*on_host_data*/) override {
        return existing ? existing : &dummy_voice_;
    }
    void CloseHostVoice(HostVoiceHandle /*voice*/) override {}
    void SetHostVoiceActive(HostVoiceHandle /*voice*/, bool active) override {
        host_active_.store(active, std::memory_order_relaxed);
    }
    void SetHostVoiceVolume(HostVoiceHandle /*voice*/,
                            const GuestChannelVolume& /*volume*/) override {}
    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override {
        std::memset(dst, 0, byte_count);
        return byte_count;
    }
    void SetTimerArmed(bool armed) override {
        timer_armed_.store(armed, std::memory_order_relaxed);
    }
    absl::Duration NowVirtualTime() const override {
        return absl::Nanoseconds(now_ns_.load(std::memory_order_relaxed));
    }

    void AdvanceMs(int64_t ms) { now_ns_.fetch_add(ms * 1'000'000, std::memory_order_relaxed); }

    bool HostActive() const { return host_active_.load(std::memory_order_relaxed); }

  private:
    int dummy_voice_ = 1;
    std::atomic<bool> host_active_{false};
    std::atomic<bool> timer_armed_{false};
    std::atomic<int64_t> now_ns_{1'000'000'000};
};

using android::goldfish::ScopedVmLock;
using android::goldfish::ScopedVmUnlock;

// Test bodies hold the VM lock, like QEMU device code. Multithreaded tests release it with
// ScopedVmUnlock; QemuAudioInject takes it itself.
class QemuAudioInjectTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_reset_audio_stubs();
        default_be_ = audio_be_by_name("mainaudiodev", nullptr);
        mux_ = std::make_shared<AudioInputMux>(&host_);
        RegisterAudioInputMux(default_be_, mux_);
    }

    void TearDown() override {
        UnregisterAudioInputMux(default_be_);
        if (mux_) {
            mux_->Shutdown();
        }
        test_reset_audio_stubs();
    }

    TestBql bql_;
    ScopedVmLock vm_lock_;
    StubHostInput host_;
    AudioBackend* default_be_ = nullptr;
    std::shared_ptr<AudioInputMux> mux_;
};

TEST_F(QemuAudioInjectTest, RejectsInvalidArguments) {
    QemuAudioInject empty_cb(nullptr, AudioStreamConfig{});
    EXPECT_EQ(empty_cb.Start().code(), absl::StatusCode::kInvalidArgument);

    QemuAudioInject zero_rate([](void*, size_t) -> size_t { return 0; },
                              AudioStreamConfig{.sample_rate_hz = 0, .channels = 1});
    EXPECT_EQ(zero_rate.Start().code(), absl::StatusCode::kInvalidArgument);

    QemuAudioInject zero_channels([](void*, size_t) -> size_t { return 0; },
                                  AudioStreamConfig{.sample_rate_hz = 48000, .channels = 0});
    EXPECT_EQ(zero_channels.Start().code(), absl::StatusCode::kInvalidArgument);

    QemuAudioInject too_many_channels([](void*, size_t) -> size_t { return 0; },
                                      AudioStreamConfig{.sample_rate_hz = 48000, .channels = 17});
    EXPECT_EQ(too_many_channels.Start().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(QemuAudioInjectTest, ReturnsUnavailableWhenMuxIsNotRegistered) {
    UnregisterAudioInputMux(default_be_);

    QemuAudioInject inject([](void*, size_t) -> size_t { return 0; }, AudioStreamConfig{});
    absl::Status status = inject.Start();
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_FALSE(inject.IsRunning());
}

TEST_F(QemuAudioInjectTest, PullsInjectedSamplesAndNotifiesActiveState) {
    std::vector<int16_t> guest_captured;
    MuxVoiceHandle* voice = nullptr;
    voice = mux_->OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [this, &voice, &guest_captured](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t r = mux_->ReadVoice(voice, buf.data(), buf.size() * sizeof(int16_t));
                buf.resize(r / sizeof(int16_t));
                guest_captured.insert(guest_captured.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    mux_->SetVoiceActive(voice, true);
    EXPECT_TRUE(host_.HostActive());

    std::vector<bool> active_notifications;
    QemuAudioInject inject(
            [](void* dst, size_t max_frames) -> size_t {
                auto* out = static_cast<int16_t*>(dst);
                for (size_t i = 0; i < max_frames; ++i) {
                    out[i] = static_cast<int16_t>(1234);
                }
                return max_frames;
            },
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            [&active_notifications](bool active) { active_notifications.push_back(active); });

    ASSERT_TRUE(inject.Start().ok());
    EXPECT_TRUE(inject.IsRunning());
    // Idempotent second Start().
    EXPECT_TRUE(inject.Start().ok());
    EXPECT_EQ(active_notifications, std::vector<bool>{true});
    EXPECT_FALSE(host_.HostActive());

    host_.AdvanceMs(10);
    mux_->OnTimerTick();
    ASSERT_EQ(guest_captured.size(), 480U);
    EXPECT_EQ(guest_captured[0], 1234);

    inject.Stop();
    EXPECT_FALSE(inject.IsRunning());
    // Idempotent second Stop().
    inject.Stop();
    EXPECT_TRUE(host_.HostActive());

    mux_->CloseVoice(voice);
}

TEST_F(QemuAudioInjectTest, SecondInjectorStartReturnsAlreadyExistsUntilFirstStops) {
    QemuAudioInject first([](void*, size_t n) -> size_t { return n; }, AudioStreamConfig{});
    QemuAudioInject second([](void*, size_t n) -> size_t { return n; }, AudioStreamConfig{});

    ASSERT_TRUE(first.Start().ok());
    EXPECT_EQ(second.Start().code(), absl::StatusCode::kAlreadyExists);
    EXPECT_FALSE(second.IsRunning());

    first.Stop();
    EXPECT_TRUE(second.Start().ok());
    EXPECT_TRUE(second.IsRunning());
}

TEST_F(QemuAudioInjectTest, SurvivesAudioCleanupBeforeInjectorDestruction) {
    auto inject = std::make_unique<QemuAudioInject>([](void*, size_t n) -> size_t { return n; },
                                                    AudioStreamConfig{});
    ASSERT_TRUE(inject->Start().ok());

    // Starting against a shut-down mux that is still registered returns FailedPrecondition.
    mux_->Shutdown();
    QemuAudioInject late_start([](void*, size_t n) -> size_t { return n; }, AudioStreamConfig{});
    EXPECT_EQ(late_start.Start().code(), absl::StatusCode::kFailedPrecondition);

    // Simulate QEMU's audio_cleanup() running before deletable_cleanup().
    UnregisterAudioInputMux(default_be_);
    mux_.reset();

    inject->Stop();
    EXPECT_FALSE(inject->IsRunning());
    inject.reset();
}

TEST_F(QemuAudioInjectTest, ConcurrentStartAndStopIsThreadSafe) {
    QemuAudioInject inject(
            [](void* dst, size_t n) -> size_t {
                std::memset(dst, 0, n * sizeof(int16_t));
                return n;
            },
            AudioStreamConfig{});
    QemuAudioInject probe([](void*, size_t n) -> size_t { return n; }, AudioStreamConfig{});
    ScopedVmUnlock unlock;

    // Verify that concurrent Stop() and Start() never leave IsRunning() out of sync with mux
    // ownership.
    for (int round = 0; round < 100; ++round) {
        ASSERT_TRUE(inject.Start().ok());
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::thread stopper([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
            }
            inject.Stop();
        });
        std::thread starter([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
            }
            EXPECT_TRUE(inject.Start().ok());
        });
        while (ready.load(std::memory_order_acquire) < 2) {
        }
        go.store(true, std::memory_order_release);
        stopper.join();
        starter.join();

        if (inject.IsRunning()) {
            ASSERT_EQ(probe.Start().code(), absl::StatusCode::kAlreadyExists)
                    << "IsRunning() was true while mux had already unregistered inject on round "
                    << round;
        } else {
            ASSERT_TRUE(probe.Start().ok());
            probe.Stop();
        }
        inject.Stop();
    }

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&inject, t]() {
            for (int i = 0; i < 25; ++i) {
                if ((i + t) % 2 == 0) {
                    (void)inject.Start();
                } else {
                    inject.Stop();
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (inject.IsRunning()) {
        EXPECT_EQ(probe.Start().code(), absl::StatusCode::kAlreadyExists);
    } else {
        EXPECT_TRUE(probe.Start().ok());
        probe.Stop();
    }
    inject.Stop();
    EXPECT_FALSE(inject.IsRunning());
}

TEST_F(QemuAudioInjectTest, ReconfiguresFormatAndSourceIdInPlaceWhilePreservingMixerGain) {
    ASSERT_EQ(ResolveTargetAudioInputMux(), mux_);

    QemuAudioInject reserved_host([](void*, size_t n) -> size_t { return n; }, AudioStreamConfig{},
                                  nullptr, nullptr, MicrophoneSourceMetadata{.source_id = "host"});
    EXPECT_EQ(reserved_host.Start().code(), absl::StatusCode::kInvalidArgument);

    std::vector<int16_t> guest_captured;
    MuxVoiceHandle* voice = nullptr;
    voice = mux_->OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [this, &voice, &guest_captured](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t r = mux_->ReadVoice(voice, buf.data(), buf.size() * sizeof(int16_t));
                buf.resize(r / sizeof(int16_t));
                guest_captured.insert(guest_captured.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    mux_->SetVoiceActive(voice, true);

    QemuAudioInject inject(
            [](void* dst, size_t max_frames) -> size_t {
                auto* out = static_cast<int16_t*>(dst);
                std::fill(out, out + max_frames, static_cast<int16_t>(8000));
                return max_frames;
            },
            AudioStreamConfig{.sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
            nullptr, nullptr, MicrophoneSourceMetadata{.kind = MicrophoneInputKind::kGrpcStream});
    ASSERT_TRUE(inject.Start().ok());

    auto initial_state = mux_->GetStateSnapshot();
    ASSERT_EQ(initial_state.microphone_inputs.size(), 2U);
    EXPECT_EQ(initial_state.microphone_inputs[1].source_id, "stream-1");
    EXPECT_EQ(initial_state.microphone_inputs[1].kind, MicrophoneInputKind::kGrpcStream);

    // Apply 2.0x gain on "stream-1" via BQL-serialized UpdateAudioInputMuxState, then reconfigure
    // to 48 kHz and rename to "agent_测试_🚀_é".
    ASSERT_TRUE(UpdateAudioInputMuxState(*mux_,
                                         AudioInputStateMutation{
                                             .input_updates = {MicrophoneInputChannelUpdate{
                                                 .source_id = "stream-1", .gain = 2.0F}},
                                         })
                        .ok());
    ASSERT_TRUE(inject.Reconfigure(AudioStreamConfig{.sample_rate_hz = 48000,
                                                     .channels = 1,
                                                     .format = SampleFormat::kS16},
                                   "agent_测试_🚀_é")
                        .ok());
    EXPECT_EQ(inject.config().sample_rate_hz, 48000U);
    EXPECT_EQ(inject.metadata().source_id, "agent_测试_🚀_é");

    auto after_reconfig = mux_->GetStateSnapshot();
    ASSERT_EQ(after_reconfig.microphone_inputs.size(), 2U);
    EXPECT_EQ(after_reconfig.microphone_inputs[1].source_id, "agent_测试_🚀_é");
    EXPECT_FLOAT_EQ(after_reconfig.microphone_inputs[1].gain, 2.0F);

    host_.AdvanceMs(10);
    mux_->OnTimerTick();
    ASSERT_EQ(guest_captured.size(), 480U);
    EXPECT_NEAR(guest_captured[0], 16000, 2);

    inject.Stop();
    mux_->CloseVoice(voice);
}

TEST_F(QemuAudioInjectTest, StateObserverCanInspectAndStopInjectorReentrantlyWithoutDeadlock) {
    QemuAudioInject inject(
            [](void* dst, size_t max_frames) -> size_t {
                std::memset(dst, 0, max_frames * sizeof(int16_t));
                return max_frames;
            },
            AudioStreamConfig{.sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
            nullptr, nullptr,
            MicrophoneSourceMetadata{
                .source_id = "initial_测试_🚀_é",
                .kind = MicrophoneInputKind::kGrpcStream,
            });

    std::atomic<int> observer_events{0};
    const auto obs_id = mux_->AddStateObserver([&](const AudioInputStateSnapshot& state) {
        const int idx = observer_events.fetch_add(1, std::memory_order_relaxed);
        const AudioStreamConfig cfg = inject.config();
        const MicrophoneSourceMetadata meta = inject.metadata();
        if (idx == 0) {
            EXPECT_TRUE(inject.IsRunning());
            EXPECT_EQ(cfg.sample_rate_hz, 16000U);
            EXPECT_EQ(meta.source_id, "initial_测试_🚀_é");
        } else if (idx == 1) {
            EXPECT_TRUE(inject.IsRunning());
            EXPECT_EQ(cfg.sample_rate_hz, 48000U);
            EXPECT_EQ(meta.source_id, "reconfigured_测试_🚀_é");
            // Re-entrantly stop the injector from inside the state observer callback!
            inject.Stop();
        } else if (idx == 2) {
            EXPECT_FALSE(inject.IsRunning());
            EXPECT_EQ(state.microphone_inputs.size(), 1U);
        }
    });

    ScopedVmUnlock unlock;
    absl::Notification done;
    std::thread runner([&]() {
        ASSERT_TRUE(inject.Start().ok());
        ASSERT_TRUE(inject.Reconfigure(AudioStreamConfig{.sample_rate_hz = 48000,
                                                         .channels = 1,
                                                         .format = SampleFormat::kS16},
                                       "reconfigured_测试_🚀_é")
                            .ok());
        EXPECT_FALSE(inject.IsRunning());
        done.Notify();
    });

    if (!done.WaitForNotificationWithTimeout(absl::Seconds(1))) {
        ADD_FAILURE() << "Deadlocked on QemuAudioInject::mutex_ inside state observer";
        std::_Exit(1);
    }
    runner.join();
    EXPECT_EQ(observer_events.load(std::memory_order_relaxed), 3);
    mux_->RemoveStateObserver(obs_id);
}

}  // namespace
}  // namespace goldfish::audio
