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

#include "goldfish/audio/audio_input_mux.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "test_bql.h"

namespace goldfish::audio {
namespace {

class FakeHostAudioInput final : public HostAudioInputInterface {
  public:
    struct FakeHostVoice {
        std::string name;
        GuestAudioConfig config;
        GuestChannelVolume volume;
        GuestDataCallback on_host_data;
        bool active = false;
    };

    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* name,
                                  const GuestAudioConfig& config,
                                  GuestDataCallback on_host_data) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        HostVoiceHandle result = nullptr;
        {
            absl::MutexLock lock(fake_mutex_);
            if (fail_open_in_) {
                if (existing != nullptr) {
                    for (auto it = voices_.begin(); it != voices_.end(); ++it) {
                        if (it->get() == existing) {
                            voices_.erase(it);
                            ++closed_voices_;
                            break;
                        }
                    }
                }
            } else if (existing != nullptr) {
                auto* v = static_cast<FakeHostVoice*>(existing);
                v->name = name ? name : "";
                v->config = config;
                v->on_host_data = std::move(on_host_data);
                result = v;
            } else {
                auto v = std::make_unique<FakeHostVoice>();
                v->name = name ? name : "";
                v->config = config;
                v->on_host_data = std::move(on_host_data);
                FakeHostVoice* raw = v.get();
                voices_.push_back(std::move(v));
                result = raw;
            }
        }
        return result;
    }

    void CloseHostVoice(HostVoiceHandle voice) override ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        for (auto it = voices_.begin(); it != voices_.end(); ++it) {
            if (it->get() == voice) {
                voices_.erase(it);
                ++closed_voices_;
                return;
            }
        }
    }

    void SetHostVoiceActive(HostVoiceHandle voice, bool active) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        auto* v = static_cast<FakeHostVoice*>(voice);
        v->active = active;
    }

    void SetHostVoiceVolume(HostVoiceHandle voice, const GuestChannelVolume& volume) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        auto* v = static_cast<FakeHostVoice*>(voice);
        v->volume = volume;
    }

    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        const size_t n = std::min(byte_count, host_pcm_bytes_.size());
        if (n > 0) {
            std::memcpy(dst, host_pcm_bytes_.data(), n);
            host_pcm_bytes_.erase(host_pcm_bytes_.begin(),
                                  host_pcm_bytes_.begin() + static_cast<ptrdiff_t>(n));
        }
        return n;
    }

    void SetTimerArmed(bool armed) override ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        timer_armed_ = armed;
    }

    absl::Duration NowVirtualTime() const override ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return now_;
    }

    void AdvanceTime(absl::Duration delta) ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        now_ += delta;
    }

    void SetNow(absl::Duration now) ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        now_ = now;
    }

    void SetFailOpenIn(bool fail) ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        fail_open_in_ = fail;
    }

    void EnqueueHostS16Samples(const std::vector<int16_t>& samples)
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        GuestDataCallback cb;
        int avail = 0;
        {
            absl::MutexLock lock(fake_mutex_);
            const size_t old_size = host_pcm_bytes_.size();
            const size_t add_bytes = samples.size() * sizeof(int16_t);
            host_pcm_bytes_.resize(old_size + add_bytes);
            std::memcpy(host_pcm_bytes_.data() + old_size, samples.data(), add_bytes);
            if (!voices_.empty() && voices_[0]->active) {
                cb = voices_[0]->on_host_data;
                avail = static_cast<int>(add_bytes);
            }
        }
        if (cb && avail > 0) {
            cb(avail);
        }
    }

    bool IsTimerArmed() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return timer_armed_;
    }

    bool IsFirstHostVoiceActive() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return !voices_.empty() && voices_[0]->active;
    }

    size_t OpenVoiceCount() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return voices_.size();
    }

    size_t ClosedVoiceCount() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return closed_voices_;
    }

  private:
    mutable absl::Mutex fake_mutex_;
    bool fail_open_in_ ABSL_GUARDED_BY(fake_mutex_) = false;
    bool timer_armed_ ABSL_GUARDED_BY(fake_mutex_) = false;
    absl::Duration now_ ABSL_GUARDED_BY(fake_mutex_) = absl::Seconds(1);
    std::vector<std::unique_ptr<FakeHostVoice>> voices_ ABSL_GUARDED_BY(fake_mutex_);
    std::vector<uint8_t> host_pcm_bytes_ ABSL_GUARDED_BY(fake_mutex_);
    size_t closed_voices_ ABSL_GUARDED_BY(fake_mutex_) = 0;
};

using android::goldfish::ScopedVmLock;
using android::goldfish::ScopedVmUnlock;
using ::testing::AllOf;
using ::testing::Each;
using ::testing::Ge;
using ::testing::Le;

// Every test body runs with the VM lock held, as QEMU device callbacks do. Multithreaded tests
// release it with ScopedVmUnlock and take it per operation.
class AudioInputMuxTest : public ::testing::Test {
  protected:
    TestBql bql_;
    ScopedVmLock vm_lock_;
};

TEST_F(AudioInputMuxTest, DelegatesToHostMicrophoneWhenNoInjectorActive) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t read = mux.ReadVoice(voice, buf.data(), buf.size() * sizeof(int16_t));
                buf.resize(read / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    EXPECT_FALSE(mux.IsVoiceActive(voice));
    EXPECT_FALSE(host.IsFirstHostVoiceActive());

    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(mux.IsVoiceActive(voice));
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    const std::vector<int16_t> host_samples = {101, -202, 303, -404};
    host.EnqueueHostS16Samples(host_samples);
    EXPECT_EQ(guest_received, host_samples);

    mux.CloseVoice(voice);
    EXPECT_EQ(host.OpenVoiceCount(), 0U);
}

TEST_F(AudioInputMuxTest, HotSwapsMidStreamBetweenHostMicAndPacedInjector) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                // Read in two split chunks (simulating virtio-snd ring-buffer wrap-around).
                const size_t total = static_cast<size_t>(avail_bytes);
                const size_t first = total / 3;
                const size_t aligned_first = first - (first % sizeof(int16_t));
                const size_t second = total - aligned_first;

                std::vector<uint8_t> raw(total, 0);
                const size_t r1 = mux.ReadVoice(voice, raw.data(), aligned_first);
                const size_t r2 = mux.ReadVoice(voice, raw.data() + r1, second);
                ASSERT_EQ(r1 + r2, total);
                const size_t count = total / sizeof(int16_t);
                const auto* s16 = reinterpret_cast<const int16_t*>(raw.data());
                guest_received.insert(guest_received.end(), s16, s16 + count);
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());

    int owner_tag = 1;
    std::vector<bool> active_events;
    absl::StatusOr<std::string> source_id = mux.RegisterInjector(
            &owner_tag,
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            [](void* dst, size_t max_frames) -> size_t {
                auto* out = static_cast<int16_t*>(dst);
                std::fill(out, out + max_frames, static_cast<int16_t>(7777));
                return max_frames;
            },
            [&active_events](bool active) { active_events.push_back(active); });
    ASSERT_TRUE(source_id.ok()) << source_id.status();
    EXPECT_EQ(*source_id, "stream-1");
    // The guest was already recording, so registration reports it immediately.
    EXPECT_EQ(active_events, std::vector<bool>{true});
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // Advance virtual clock by 10 ms -> 480 frames at 48 kHz.
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 480U);
    EXPECT_EQ(guest_received.front(), 7777);
    EXPECT_EQ(guest_received.back(), 7777);

    // Unregistering mid-stream restores the host microphone and disarms the virtual timer.
    mux.UnregisterInjector(&owner_tag);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    guest_received.clear();
    host.EnqueueHostS16Samples({-1234, 5678});
    EXPECT_EQ(guest_received, (std::vector<int16_t>{-1234, 5678}));

    mux.SetVoiceActive(voice, false);
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, PacesSilenceWhenHostHasNoPhysicalMicAndUpgradesToInjector) {
    FakeHostAudioInput host;
    host.SetFailOpenIn(true);
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 99);
                const size_t r = mux.ReadVoice(voice, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);

    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsTimerArmed());

    // 10 ms at 16 kHz -> 160 frames of paced silence even without a host mic or injector.
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 160U);
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    // Attach injector mid-stream -> delivers injected samples.
    guest_received.clear();
    int owner_tag = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner_tag,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(4242));
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 160U);
    EXPECT_EQ(guest_received.front(), 4242);

    // Detach injector -> stays armed and returns to paced silence because host_voice is null.
    guest_received.clear();
    mux.UnregisterInjector(&owner_tag);
    EXPECT_TRUE(host.IsTimerArmed());
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    mux.CloseVoice(voice);
}

constexpr AudioStreamConfig kMono48kS16{
    .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16};

// A source that fills every request with @p value and counts the frames it handed out.
AudioInputMux::ReadCallback ConstantSource(int16_t value, size_t* pulled = nullptr) {
    return [value, pulled](void* dst, size_t max_frames) -> size_t {
        auto* out = static_cast<int16_t*>(dst);
        std::fill(out, out + max_frames, value);
        if (pulled != nullptr) {
            *pulled += max_frames;
        }
        return max_frames;
    };
}

std::vector<std::string> InputIds(const AudioInputStateSnapshot& snapshot) {
    std::vector<std::string> ids;
    for (const MicrophoneInputInfo& input : snapshot.microphone_inputs) {
        ids.push_back(input.source_id);
    }
    return ids;
}

// A recording 48 kHz mono S16 guest voice; Tick() advances 10 ms and returns what was read.
class RecordingVoice {
  public:
    RecordingVoice(AudioInputMux& mux, FakeHostAudioInput& host) : mux_(mux), host_(host) {
        Reopen(GuestAudioConfig{
            .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16});
        mux_.SetVoiceActive(voice_, true);
    }

    // Opens the voice, or reopens it in place with a new format, as the guest does.
    void Reopen(const GuestAudioConfig& config) {
        voice_ = mux_.OpenVoice(voice_, "virtio-sound.in", config, [this](int avail_bytes) {
            std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / 2);
            const size_t read = mux_.ReadVoice(voice_, buf.data(), buf.size() * 2);
            buf.resize(read / 2);
            received_.insert(received_.end(), buf.begin(), buf.end());
        });
    }
    ~RecordingVoice() { mux_.CloseVoice(voice_); }

    std::vector<int16_t> Tick() {
        received_.clear();
        host_.AdvanceTime(absl::Milliseconds(10));
        mux_.OnTimerTick();
        return received_;
    }

    MuxVoiceHandle* Handle() const { return voice_; }

  private:
    AudioInputMux& mux_;
    FakeHostAudioInput& host_;
    MuxVoiceHandle* voice_ = nullptr;
    std::vector<int16_t> received_;
};

TEST_F(AudioInputMuxTest, MixesConcurrentInjectorsAndTellsEachAboutGuestActivity) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    std::vector<bool> events_a;
    std::vector<bool> events_b;
    int owner_a = 1;
    int owner_b = 2;
    ASSERT_TRUE(
            mux.RegisterInjector(&owner_a, kMono48kS16, ConstantSource(7000),
                                 [&events_a](bool active) { events_a.push_back(active); },
                                 {.source_id = "测试_🚀_é", .kind = MicrophoneInputKind::kWebRtc})
                    .ok());
    ASSERT_TRUE(
            mux.RegisterInjector(&owner_b, kMono48kS16, ConstantSource(3000),
                                 [&events_b](bool active) { events_b.push_back(active); },
                                 {.source_id = "agent", .kind = MicrophoneInputKind::kGrpcStream})
                    .ok());
    EXPECT_EQ(InputIds(mux.GetStateSnapshot()),
              (std::vector<std::string>{"host", "测试_🚀_é", "agent"}));

    {
        RecordingVoice voice(mux, host);
        const std::vector<int16_t> received = voice.Tick();
        ASSERT_EQ(received.size(), 480U);
        EXPECT_THAT(received, Each(10000));
        mux.SetVoiceActive(voice.Handle(), false);
    }

    EXPECT_EQ(events_a, (std::vector<bool>{true, false}));
    EXPECT_EQ(events_b, (std::vector<bool>{true, false}));
    mux.UnregisterInjector(&owner_a);
    mux.UnregisterInjector(&owner_b);
}

TEST_F(AudioInputMuxTest, MixedSumsAreClampedWhenEncoded) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int owner_a = 1;
    int owner_b = 2;
    ASSERT_TRUE(mux.RegisterInjector(&owner_a, kMono48kS16, ConstantSource(30000), nullptr).ok());
    ASSERT_TRUE(mux.RegisterInjector(&owner_b, kMono48kS16, ConstantSource(30000), nullptr).ok());
    RecordingVoice voice(mux, host);

    EXPECT_THAT(voice.Tick(), Each(32767));
}

TEST_F(AudioInputMuxTest, PerInputMuteGainAndMasterMuteApplyWithinTheMix) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int owner_a = 1;
    int owner_b = 2;
    size_t pulled_a = 0;
    size_t pulled_b = 0;
    ASSERT_TRUE(mux.RegisterInjector(&owner_a, kMono48kS16, ConstantSource(8000, &pulled_a),
                                     nullptr, {.source_id = "person"})
                        .ok());
    ASSERT_TRUE(mux.RegisterInjector(&owner_b, kMono48kS16, ConstantSource(4000, &pulled_b),
                                     nullptr, {.source_id = "agent"})
                        .ok());
    RecordingVoice voice(mux, host);

    ASSERT_TRUE(mux.UpdateState({.input_updates = {{.source_id = "agent", .gain = 0.5F}}}).ok());
    EXPECT_THAT(voice.Tick(), Each(10000));

    ASSERT_TRUE(mux.UpdateState({.input_updates = {{.source_id = "person", .muted = true}}}).ok());
    EXPECT_THAT(voice.Tick(), Each(2000));

    // Master mute silences everything, but every source keeps being drained in real time.
    const size_t pulled_before = pulled_a + pulled_b;
    ASSERT_TRUE(mux.UpdateState({.microphone_muted = true}).ok());
    EXPECT_THAT(voice.Tick(), Each(0));
    EXPECT_EQ(pulled_a + pulled_b - pulled_before, 2 * 480U);
}

TEST_F(AudioInputMuxTest, UnregisteringOneInjectorKeepsTheOthersFlowing) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int owner_a = 1;
    int owner_b = 2;
    ASSERT_TRUE(mux.RegisterInjector(&owner_a, kMono48kS16, ConstantSource(1000), nullptr).ok());
    RecordingVoice voice(mux, host);
    EXPECT_THAT(voice.Tick(), Each(1000));

    ASSERT_TRUE(mux.RegisterInjector(&owner_b, kMono48kS16, ConstantSource(2000), nullptr).ok());
    EXPECT_THAT(voice.Tick(), Each(3000));

    mux.UnregisterInjector(&owner_a);
    EXPECT_THAT(voice.Tick(), Each(2000));
    EXPECT_TRUE(host.IsTimerArmed());

    mux.UnregisterInjector(&owner_b);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());
}

TEST_F(AudioInputMuxTest, ReconfiguringOneInjectorLeavesTheOthersConverterAlone) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int person = 1;
    int agent = 2;
    bool person_sends_float = false;
    ASSERT_TRUE(mux.RegisterInjector(
                           &person, kMono48kS16,
                           [&person_sends_float](void* dst, size_t frames) {
                               if (person_sends_float) {
                                   std::fill_n(static_cast<float*>(dst), frames, 1000.0F / 32768);
                               } else {
                                   std::fill_n(static_cast<int16_t*>(dst), frames, int16_t{1000});
                               }
                               return frames;
                           },
                           nullptr)
                        .ok());
    // The agent's 16 kHz stream runs through a resampler. Rebuilding its converter would restart
    // the resampler's delay line, and the mix would dip while it refills.
    ASSERT_TRUE(mux.RegisterInjector(&agent,
                                     AudioStreamConfig{.sample_rate_hz = 16000,
                                                       .channels = 1,
                                                       .format = SampleFormat::kS16},
                                     ConstantSource(2000), nullptr)
                        .ok());
    RecordingVoice voice(mux, host);
    for (int i = 0; i < 5; ++i) {
        voice.Tick();
    }
    ASSERT_THAT(voice.Tick(), Each(AllOf(Ge(2995), Le(3005))));

    person_sends_float = true;
    ASSERT_TRUE(mux.ReconfigureInjector(&person, AudioStreamConfig{.sample_rate_hz = 48000,
                                                                   .channels = 1,
                                                                   .format = SampleFormat::kF32})
                        .ok());

    EXPECT_THAT(voice.Tick(), Each(AllOf(Ge(2995), Le(3005))));
    mux.UnregisterInjector(&person);
    mux.UnregisterInjector(&agent);
}

TEST_F(AudioInputMuxTest, MixSurvivesVmPauseResumeAndAGuestReopen) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int person = 1;
    int agent = 2;
    ASSERT_TRUE(mux.RegisterInjector(&person, kMono48kS16, ConstantSource(1000), nullptr).ok());
    ASSERT_TRUE(mux.RegisterInjector(&agent, kMono48kS16, ConstantSource(2000), nullptr).ok());
    RecordingVoice voice(mux, host);
    ASSERT_THAT(voice.Tick(), Each(3000));

    // A long pause (or a snapshot load) must not turn into a burst on resume.
    mux.OnVmRunningChanged(false);
    EXPECT_FALSE(host.IsTimerArmed());
    host.AdvanceTime(absl::Seconds(5));
    mux.OnVmRunningChanged(true);
    const std::vector<int16_t> resumed = voice.Tick();
    EXPECT_EQ(resumed.size(), 480U);
    EXPECT_THAT(resumed, Each(3000));

    // The guest reopens its stream at 16 kHz: both converters are rebuilt for the new format.
    voice.Reopen(GuestAudioConfig{
        .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16});
    voice.Tick();  // The fresh resamplers fill their delay lines.
    const std::vector<int16_t> reopened = voice.Tick();
    EXPECT_EQ(reopened.size(), 160U);
    EXPECT_THAT(reopened, Each(AllOf(Ge(2995), Le(3005))));

    mux.UnregisterInjector(&person);
    mux.UnregisterInjector(&agent);
}

TEST_F(AudioInputMuxTest, InjectorChurnBesideALongLivedInjectorIsRaceFree) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    std::atomic<size_t> person_frames{0};
    absl::Notification person_pulled;
    int person = 0;
    ASSERT_TRUE(mux.RegisterInjector(
                           &person, kMono48kS16,
                           [&person_frames, &person_pulled](void* dst, size_t frames) {
                               std::fill_n(static_cast<int16_t*>(dst), frames, 0);
                               if (person_frames.fetch_add(frames) == 0 && frames > 0) {
                                   person_pulled.Notify();
                               }
                               return frames;
                           },
                           nullptr)
                        .ok());
    RecordingVoice voice(mux, host);

    ScopedVmUnlock unlock;
    std::atomic<bool> stop{false};
    std::thread reader([&mux, &host, &stop] {
        while (!stop.load()) {
            {
                ScopedVmLock lock;
                host.AdvanceTime(absl::Milliseconds(10));
                mux.OnTimerTick();
            }
            std::this_thread::yield();
        }
    });

    // Agents come and go while the person keeps talking; a removed agent is never pulled again.
    for (int round = 0; round < 50; ++round) {
        std::atomic<bool> removed{false};
        std::atomic<int> pulls_after_removal{0};
        std::vector<int> agents(3);
        ScopedVmLock lock;
        for (int& agent : agents) {
            ASSERT_TRUE(mux.RegisterInjector(
                                   &agent, kMono48kS16,
                                   [&removed, &pulls_after_removal](void* dst, size_t frames) {
                                       if (removed.load()) {
                                           pulls_after_removal.fetch_add(1);
                                       }
                                       std::fill_n(static_cast<int16_t*>(dst), frames, 0);
                                       return frames;
                                   },
                                   nullptr)
                                .ok());
        }
        {
            ScopedVmUnlock let_reader_run;
            std::this_thread::yield();
        }
        for (int& agent : agents) {
            mux.UnregisterInjector(&agent);
        }
        removed.store(true);
        EXPECT_EQ(pulls_after_removal.load(), 0);
    }

    EXPECT_TRUE(person_pulled.WaitForNotificationWithTimeout(absl::Seconds(5)));
    stop.store(true);
    reader.join();
    ScopedVmLock lock;
    EXPECT_GT(person_frames.load(), 0U);
    EXPECT_EQ(InputIds(mux.GetStateSnapshot()), (std::vector<std::string>{"host", "stream-1"}));
    mux.UnregisterInjector(&person);
}

TEST_F(AudioInputMuxTest, RegistrationRejectsDuplicateSourceIdsAndInjectorsBeyondTheLimit) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    std::vector<int> owners(kMaxConcurrentMicrophoneStreams + 1);
    for (int i = 0; i < kMaxConcurrentMicrophoneStreams; ++i) {
        absl::StatusOr<std::string> id =
                mux.RegisterInjector(&owners[i], kMono48kS16, ConstantSource(0), nullptr);
        ASSERT_TRUE(id.ok()) << id.status();
        EXPECT_EQ(*id, absl::StrCat("stream-", i + 1));
    }

    absl::StatusOr<std::string> again =
            mux.RegisterInjector(&owners[0], kMono48kS16, ConstantSource(0), nullptr);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(*again, "stream-1");
    EXPECT_EQ(mux.RegisterInjector(&owners.back(), kMono48kS16, ConstantSource(0), nullptr)
                      .status()
                      .code(),
              absl::StatusCode::kResourceExhausted);

    mux.UnregisterInjector(&owners[0]);
    EXPECT_EQ(mux.RegisterInjector(&owners.back(), kMono48kS16, ConstantSource(0), nullptr,
                                   {.source_id = "stream-2"})
                      .status()
                      .code(),
              absl::StatusCode::kAlreadyExists);
    // Generated ids skip ones a caller picked.
    absl::StatusOr<std::string> generated =
            mux.RegisterInjector(&owners.back(), kMono48kS16, ConstantSource(0), nullptr);
    ASSERT_TRUE(generated.ok());
    EXPECT_EQ(*generated, absl::StrCat("stream-", kMaxConcurrentMicrophoneStreams + 1));

    for (int& owner : owners) {
        mux.UnregisterInjector(&owner);
    }
}

TEST_F(AudioInputMuxTest, RenamingAnInjectorToAnotherInputsSourceIdFails) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int owner_a = 1;
    int owner_b = 2;
    ASSERT_TRUE(mux.RegisterInjector(&owner_a, kMono48kS16, ConstantSource(0), nullptr,
                                     {.source_id = "a"})
                        .ok());
    ASSERT_TRUE(mux.RegisterInjector(&owner_b, kMono48kS16, ConstantSource(0), nullptr,
                                     {.source_id = "b"})
                        .ok());

    EXPECT_EQ(mux.ReconfigureInjector(&owner_b, kMono48kS16, "a").code(),
              absl::StatusCode::kAlreadyExists);
    EXPECT_EQ(mux.ReconfigureInjector(&owner_b, kMono48kS16, "host").code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(mux.ReconfigureInjector(&owner_b, kMono48kS16, "webrtc").code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(mux.ReconfigureInjector(&owner_b, kMono48kS16, "c").ok());
    EXPECT_EQ(InputIds(mux.GetStateSnapshot()), (std::vector<std::string>{"host", "a", "c"}));

    mux.UnregisterInjector(&owner_a);
    mux.UnregisterInjector(&owner_b);
}

TEST_F(AudioInputMuxTest, OnlyAWebRtcInjectorMayUseTheWebRtcSourceId) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int agent = 1;
    int unlabeled = 2;
    int webrtc = 3;

    EXPECT_EQ(
            mux.RegisterInjector(&agent, kMono48kS16, ConstantSource(0), nullptr,
                                 {.source_id = "webrtc", .kind = MicrophoneInputKind::kGrpcStream})
                    .status()
                    .code(),
            absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(mux.RegisterInjector(&unlabeled, kMono48kS16, ConstantSource(0), nullptr,
                                   {.source_id = "webrtc"})
                      .status()
                      .code(),
              absl::StatusCode::kInvalidArgument);
    ASSERT_TRUE(mux.RegisterInjector(&webrtc, kMono48kS16, ConstantSource(0), nullptr,
                                     {.source_id = "webrtc", .kind = MicrophoneInputKind::kWebRtc})
                        .ok());

    EXPECT_EQ(InputIds(mux.GetStateSnapshot()), (std::vector<std::string>{"host", "webrtc"}));
    mux.UnregisterInjector(&webrtc);
}

TEST_F(AudioInputMuxTest, AnActiveCallbackThatUnregistersAnotherInjectorSilencesIt) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    int owner_a = 1;
    int owner_b = 2;
    std::vector<bool> events_b;
    ASSERT_TRUE(
            mux.RegisterInjector(&owner_a, kMono48kS16, ConstantSource(0), [&mux, &owner_b](bool) {
                   mux.UnregisterInjector(&owner_b);
               }).ok());
    ASSERT_TRUE(mux.RegisterInjector(&owner_b, kMono48kS16, ConstantSource(0),
                                     [&events_b](bool active) { events_b.push_back(active); })
                        .ok());

    RecordingVoice voice(mux, host);

    EXPECT_TRUE(events_b.empty());
    EXPECT_EQ(InputIds(mux.GetStateSnapshot()), (std::vector<std::string>{"host", "stream-1"}));
    mux.UnregisterInjector(&owner_a);
}

TEST_F(AudioInputMuxTest, VmRunStateAndShutdownHandleSnapshotJumpAndLateInjectorTeardown) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
                       [](void*, size_t n) -> size_t { return n; }, nullptr)
                    .ok());
    EXPECT_TRUE(host.IsTimerArmed());

    // Pause VM -> disarms timer; snapshot load jumps clock backwards -> resume re-arms cleanly.
    mux.OnVmRunningChanged(false);
    EXPECT_FALSE(host.IsTimerArmed());
    host.SetNow(absl::Milliseconds(200));
    mux.OnVmRunningChanged(true);
    EXPECT_TRUE(host.IsTimerArmed());

    // Shutdown (simulating audio_cleanup before ~QemuAudioInject) disarms timer & closes host
    // voice.
    mux.Shutdown();
    EXPECT_TRUE(mux.IsShutdown());
    EXPECT_FALSE(host.IsTimerArmed());
    EXPECT_EQ(host.ClosedVoiceCount(), 1U);

    // Late UnregisterInjector after Shutdown is a safe no-op, while RegisterInjector fails with
    // FAILED_PRECONDITION because Shutdown() is terminal.
    mux.UnregisterInjector(&owner);
    absl::StatusOr<std::string> after_shutdown = mux.RegisterInjector(
            &owner,
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            [](void*, size_t) -> size_t { return 0; }, nullptr);
    EXPECT_EQ(after_shutdown.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(AudioInputMuxTest, ConcurrentInjectorChurnAndReadsNeverInvokeCallbackAfterUnregister) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
            [&mux, &voice](int avail_bytes) {
                std::vector<uint8_t> buf(static_cast<size_t>(avail_bytes));
                mux.ReadVoice(voice, buf.data(), buf.size());
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    ScopedVmUnlock unlock;
    std::atomic<bool> stop_threads{false};
    std::thread reader_thread([&mux, &host, &stop_threads]() {
        while (!stop_threads.load(std::memory_order_relaxed)) {
            {
                ScopedVmLock lock;
                host.AdvanceTime(absl::Milliseconds(10));
                mux.OnTimerTick();
            }
            std::this_thread::yield();
        }
    });

    for (int iter = 0; iter < 50; ++iter) {
        std::atomic<bool> unregistered{false};
        std::atomic<int> post_unregister_calls{0};
        int owner = iter;
        ScopedVmLock lock;
        ASSERT_TRUE(mux.RegisterInjector(
                               &owner,
                               AudioStreamConfig{.sample_rate_hz = 16000,
                                                 .channels = 1,
                                                 .format = SampleFormat::kS16},
                               [&unregistered, &post_unregister_calls](
                                       void* dst, size_t max_frames) -> size_t {
                                   if (unregistered.load(std::memory_order_acquire)) {
                                       post_unregister_calls.fetch_add(1,
                                                                       std::memory_order_relaxed);
                                   }
                                   std::memset(dst, 0, max_frames * sizeof(int16_t));
                                   return max_frames;
                               },
                               nullptr)
                            .ok());
        {
            ScopedVmUnlock let_reader_run;
            std::this_thread::yield();
        }
        mux.UnregisterInjector(&owner);
        unregistered.store(true, std::memory_order_release);
        EXPECT_EQ(post_unregister_calls.load(std::memory_order_relaxed), 0);
    }

    stop_threads.store(true, std::memory_order_relaxed);
    reader_thread.join();
    ScopedVmLock lock;
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, MultipleConcurrentGuestVoicesTrackAggregateActiveStateAndHostFallback) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> voice_a_samples;
    MuxVoiceHandle* voice_a = nullptr;
    voice_a = mux.OpenVoice(
            nullptr, "virtio-sound.in.0",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice_a, &voice_a_samples](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 77);
                const size_t r =
                        mux.ReadVoice(voice_a, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                voice_a_samples.insert(voice_a_samples.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice_a, nullptr);

    MuxVoiceHandle* voice_b = mux.OpenVoice(
            nullptr, "virtio-sound.in.1",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice_b, nullptr);
    EXPECT_EQ(host.OpenVoiceCount(), 2U);

    std::vector<bool> active_events;
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(1234));
                           return max_frames;
                       },
                       [&active_events](bool active) { active_events.push_back(active); })
                    .ok());

    // Activating the first voice fires ActiveCallback(true); activating the second voice does not
    // fire a duplicate true event.
    mux.SetVoiceActive(voice_a, true);
    EXPECT_EQ(active_events, (std::vector<bool>{true}));
    mux.SetVoiceActive(voice_b, true);
    EXPECT_EQ(active_events, (std::vector<bool>{true}));

    // Deactivating voice_a while voice_b remains active must NOT fire ActiveCallback(false).
    mux.SetVoiceActive(voice_a, false);
    EXPECT_EQ(active_events, (std::vector<bool>{true}));
    EXPECT_TRUE(host.IsTimerArmed());

    // Closing the last active voice (voice_b) transitions aggregate active state to false.
    mux.CloseVoice(voice_b);
    EXPECT_EQ(active_events, (std::vector<bool>{true, false}));
    EXPECT_FALSE(host.IsTimerArmed());

    mux.UnregisterInjector(&owner);

    // Simulate host reopen failure on existing voice_a: OpenHostVoice consumes the old host handle
    // and returns nullptr, so AudioInputMux must fall back to virtual-clock-paced silence.
    host.SetFailOpenIn(true);
    MuxVoiceHandle* reopened_a = mux.OpenVoice(
            voice_a, "virtio-sound.in.0",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice_a, &voice_a_samples](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 77);
                const size_t r =
                        mux.ReadVoice(voice_a, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                voice_a_samples.insert(voice_a_samples.end(), buf.begin(), buf.end());
            });
    EXPECT_EQ(reopened_a, voice_a);
    EXPECT_EQ(host.OpenVoiceCount(), 0U);

    voice_a_samples.clear();
    mux.SetVoiceActive(voice_a, true);
    EXPECT_TRUE(host.IsTimerArmed());
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(voice_a_samples.size(), 160U);
    EXPECT_EQ(voice_a_samples, std::vector<int16_t>(160, 0));

    mux.CloseVoice(voice_a);
}

TEST_F(AudioInputMuxTest, HandlesBoundaryReads) {
    // Boundary ReadVoice calls (nullptr handle, nullptr buffer, 0 bytes, sub-frame bytes).
    {
        FakeHostAudioInput host;
        AudioInputMux mux(&host);

        MuxVoiceHandle* stereo_voice = mux.OpenVoice(
                nullptr, "virtio-sound.in",
                GuestAudioConfig{
                    .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
                [](int) {});
        ASSERT_NE(stereo_voice, nullptr);
        mux.SetVoiceActive(stereo_voice, true);

        uint8_t scratch[16] = {};
        EXPECT_EQ(mux.ReadVoice(nullptr, scratch, sizeof(scratch)), 0U);
        EXPECT_EQ(mux.ReadVoice(stereo_voice, nullptr, sizeof(scratch)), 0U);
        EXPECT_EQ(mux.ReadVoice(stereo_voice, scratch, 0), 0U);

        // Switch to paced mode and verify sub-frame ReadVoice (< 4 bytes for stereo S16) returns 0
        // without invoking the injector callback.
        size_t injector_calls = 0;
        int owner = 1;
        ASSERT_TRUE(mux.RegisterInjector(
                               &owner,
                               AudioStreamConfig{.sample_rate_hz = 48000,
                                                 .channels = 2,
                                                 .format = SampleFormat::kS16},
                               [&injector_calls](void* /*dst*/, size_t max_frames) -> size_t {
                                   ++injector_calls;
                                   return max_frames;
                               },
                               nullptr)
                            .ok());
        host.AdvanceTime(absl::Milliseconds(10));
        mux.OnTimerTick();
        EXPECT_EQ(mux.ReadVoice(stereo_voice, scratch, 3), 0U);
        EXPECT_EQ(injector_calls, 0U);

        mux.UnregisterInjector(&owner);
        mux.CloseVoice(stereo_voice);
    }
}

TEST_F(AudioInputMuxTest, MutationsWithoutTheVmLockFailTheLockCheck) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);

    ScopedVmUnlock unlock;
    EXPECT_DEBUG_DEATH(mux.SetVoiceActive(voice, true), "must hold the VM lock");
    EXPECT_DEBUG_DEATH(mux.OnTimerTick(), "must hold the VM lock");
    // IsShutdown() stays usable from any thread.
    EXPECT_FALSE(mux.IsShutdown());
}

TEST_F(AudioInputMuxTest, ReopeningVoiceAndUpdatingVolumeReconfiguresInjectedPipeline) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* s = static_cast<int16_t*>(dst);
                           std::fill(s, s + max_frames, static_cast<int16_t>(16384));
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    // Reopen existing voice as stereo F32 at 16 kHz and mute right channel via SetVoiceVolume.
    std::vector<float> stereo_out;
    MuxVoiceHandle* reopened = nullptr;
    reopened = mux.OpenVoice(
            voice, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 2, .format = GuestPcmFormat::kF32},
            [&mux, &reopened, &stereo_out](int avail_bytes) {
                std::vector<float> buf(static_cast<size_t>(avail_bytes) / sizeof(float));
                const size_t r = mux.ReadVoice(reopened, buf.data(), buf.size() * sizeof(float));
                buf.resize(r / sizeof(float));
                stereo_out.insert(stereo_out.end(), buf.begin(), buf.end());
            });
    EXPECT_EQ(reopened, voice);

    GuestChannelVolume vol;
    vol.channels = 2;
    vol.gain[0] = 255;
    vol.gain[1] = 0;
    mux.SetVoiceVolume(reopened, vol);

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    // 10 ms at 16 kHz stereo -> 160 frames * 2 channels = 320 floats.
    ASSERT_EQ(stereo_out.size(), 320U);
    EXPECT_NEAR(stereo_out[0], 0.5F, 0.01F);
    EXPECT_FLOAT_EQ(stereo_out[1], 0.0F);

    mux.UnregisterInjector(&owner);
    mux.CloseVoice(reopened);
}

TEST_F(AudioInputMuxTest, ResetsResamplerDelayLineWhenGuestVoiceReactivates) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host, /*vm_running=*/true);

    float injected_value = 1.0F;
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 44100, .channels = 1, .format = SampleFormat::kF32},
                       [&injected_value](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<float*>(dst);
                           std::fill(out, out + max_frames, injected_value);
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    std::vector<float> captured;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kF32},
            [&mux, &voice, &captured](int avail_bytes) {
                std::vector<float> buf(static_cast<size_t>(avail_bytes) / sizeof(float));
                const size_t r = mux.ReadVoice(voice, buf.data(), buf.size() * sizeof(float));
                buf.resize(r / sizeof(float));
                captured.insert(captured.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);

    // Session 1: saturate the sinc resampler delay line with +1.0F DC samples.
    mux.SetVoiceActive(voice, true);
    host.AdvanceTime(absl::Milliseconds(20));
    mux.OnTimerTick();
    ASSERT_EQ(captured.size(), 960U);
    EXPECT_NEAR(captured.back(), 1.0F, 0.02F);

    // Pause guest capture, switch source to silence, and reactivate guest capture.
    mux.SetVoiceActive(voice, false);
    injected_value = 0.0F;
    captured.clear();

    mux.SetVoiceActive(voice, true);
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(captured.size(), 480U);
    for (size_t i = 0; i < captured.size(); ++i) {
        EXPECT_FLOAT_EQ(captured[i], 0.0F) << "Stale resampler bleed at frame " << i;
    }

    mux.UnregisterInjector(&owner);
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, MasterMicrophoneMuteDetachesHostAndAdvancesInjectorWhileFeedingSilence) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 99);
                const size_t r = mux.ReadVoice(voice, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());

    // Mute master microphone -> detaches host voice and arms virtual silence pacer.
    auto muted_state = mux.UpdateState(AudioInputStateMutation{.microphone_muted = true});
    ASSERT_TRUE(muted_state.ok()) << muted_state.status();
    EXPECT_TRUE(muted_state->microphone_muted);
    EXPECT_TRUE(muted_state->microphone_in_use_by_guest);
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // Ambient host samples while muted must be ignored; virtual timer feeds silence at 1.0x.
    host.EnqueueHostS16Samples({5555, -5555});
    EXPECT_TRUE(guest_received.empty());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    // Attach active injector while master-muted -> injector still drains at 1.0x, guest gets 0s!
    guest_received.clear();
    size_t injector_pulled_frames = 0;
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [&injector_pulled_frames](void* dst, size_t max_frames) -> size_t {
                           injector_pulled_frames += max_frames;
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(9999));
                           return max_frames;
                       },
                       nullptr,
                       MicrophoneSourceMetadata{
                           .source_id = "agent-clip",
                           .kind = MicrophoneInputKind::kGrpcClip,
                       })
                    .ok());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    EXPECT_EQ(injector_pulled_frames, 160U);
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    // Unmute master -> injected audio immediately flows through.
    guest_received.clear();
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = false}).ok());
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 160U);
    EXPECT_EQ(guest_received.front(), 9999);

    mux.UnregisterInjector(&owner);
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, HostMicrophoneDisabledFeedsPacedSilenceWhenIdleWhileAllowingInjection) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 77);
                const size_t r = mux.ReadVoice(voice, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());

    // Disable host physical microphone -> detaches host mic and feeds paced silence when idle.
    auto disabled = mux.UpdateState(AudioInputStateMutation{
        .input_updates = {MicrophoneInputChannelUpdate{.source_id = "host", .muted = true}},
    });
    ASSERT_TRUE(disabled.ok()) << disabled.status();
    ASSERT_EQ(disabled->microphone_inputs.size(), 1U);
    EXPECT_EQ(disabled->microphone_inputs[0].source_id, "host");
    EXPECT_TRUE(disabled->microphone_inputs[0].muted);
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    // Injecting audio via QemuAudioInject succeeds without ALREADY_EXISTS and delivers audio!
    guest_received.clear();
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(6000));
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 160U);
    EXPECT_EQ(guest_received.front(), 6000);

    // Stopping injector returns to paced silence without reactivating host mic!
    guest_received.clear();
    mux.UnregisterInjector(&owner);
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    // Re-enabling host channel via microphone_inputs["host"].muted = false restores host mic!
    auto reenabled = mux.UpdateState(AudioInputStateMutation{
        .input_updates = {MicrophoneInputChannelUpdate{
            .source_id = "host", .muted = false, .gain = 0.5F}},
    });
    ASSERT_TRUE(reenabled.ok()) << reenabled.status();
    EXPECT_FALSE(reenabled->microphone_inputs[0].muted);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    guest_received.clear();
    host.EnqueueHostS16Samples({4000, -8000});
    ASSERT_EQ(guest_received.size(), 2U);
    EXPECT_NEAR(guest_received[0], 2000, 2);
    EXPECT_NEAR(guest_received[1], -4000, 2);

    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, StackedMasterMuteHostDisableAndInjectorLifecycleNeverLeakHostActivation) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    // 1. Both mute the host input AND enable master mute.
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{
                                    .microphone_muted = true,
                                    .input_updates = {MicrophoneInputChannelUpdate{
                                        .source_id = "host", .muted = true}},
                                })
                        .ok());
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // 2. Unmuting master while the host input is still muted must NOT reactivate the
    //    physical host microphone.
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = false}).ok());
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // 3. Unmuting the host input while master microphone_muted is true must also keep the
    //    physical host microphone detached.
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{
                                    .microphone_muted = true,
                                    .input_updates = {MicrophoneInputChannelUpdate{
                                        .source_id = "host", .muted = false}},
                                })
                        .ok());
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // 4. Registering and unregistering an injector while master mute is active must NOT reactivate
    //    the physical host microphone on unregister.
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void*, size_t n) -> size_t { return n; }, nullptr)
                    .ok());
    mux.UnregisterInjector(&owner);
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // 5. Clearing master mute when the host input is unmuted and no injector is active
    //    finally restores physical host microphone passthrough.
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = false}).ok());
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, PerChannelControlsReconfigureValidationAndUnicodeSourceId) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<AudioInputStateSnapshot> observed;
    const auto obs_id = mux.AddStateObserver(
            [&observed](const AudioInputStateSnapshot& s) { observed.push_back(s); });

    const std::string unicode_id = "agent_测试_🚀_é";
    EXPECT_EQ(mux.RegisterInjector(
                         &obs_id,
                         AudioStreamConfig{
                             .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                         [](void*, size_t n) -> size_t { return n; }, nullptr,
                         MicrophoneSourceMetadata{.source_id = "host"})
                      .status()
                      .code(),
              absl::StatusCode::kInvalidArgument);

    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(10000));
                           return max_frames;
                       },
                       nullptr,
                       MicrophoneSourceMetadata{
                           .source_id = unicode_id,
                           .kind = MicrophoneInputKind::kGrpcStream,
                       })
                    .ok());
    ASSERT_EQ(observed.size(), 1U);
    ASSERT_EQ(observed.back().microphone_inputs.size(), 2U);
    EXPECT_EQ(observed.back().microphone_inputs[1].source_id, unicode_id);
    EXPECT_EQ(observed.back().microphone_inputs[1].kind, MicrophoneInputKind::kGrpcStream);

    // Validation errors leave state untouched.
    EXPECT_EQ(mux.UpdateState(AudioInputStateMutation{
                                  .input_updates = {MicrophoneInputChannelUpdate{.source_id = ""}},
                              })
                      .status()
                      .code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(mux.UpdateState(AudioInputStateMutation{
                                  .input_updates = {MicrophoneInputChannelUpdate{
                                      .source_id = "host", .gain = -0.1F}},
                              })
                      .status()
                      .code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(mux.UpdateState(AudioInputStateMutation{
                                  .input_updates = {MicrophoneInputChannelUpdate{
                                      .source_id = "host", .gain = 4.1F}},
                              })
                      .status()
                      .code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(
            mux
                    .UpdateState(AudioInputStateMutation{
                        .input_updates = {MicrophoneInputChannelUpdate{
                            .source_id = "host", .gain = std::numeric_limits<float>::quiet_NaN()}},
                    })
                    .status()
                    .code(),
            absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(mux.UpdateState(AudioInputStateMutation{
                                  .input_updates = {MicrophoneInputChannelUpdate{
                                      .source_id = "nonexistent-stream", .muted = true}},
                              })
                      .status()
                      .code(),
              absl::StatusCode::kNotFound);

    // Update active channel gain to 2.0x and verify ReconfigureInjector preserves it!
    auto updated = mux.UpdateState(AudioInputStateMutation{
        .input_updates = {MicrophoneInputChannelUpdate{.source_id = unicode_id, .gain = 2.0F}},
    });
    ASSERT_TRUE(updated.ok()) << updated.status();
    EXPECT_FLOAT_EQ(updated->microphone_inputs[1].gain, 2.0F);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t r = mux.ReadVoice(voice, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    ASSERT_TRUE(mux.ReconfigureInjector(&owner, AudioStreamConfig{.sample_rate_hz = 48000,
                                                                  .channels = 1,
                                                                  .format = SampleFormat::kS16})
                        .ok());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 480U);
    // 10000 * 2.0 gain = 20000!
    EXPECT_NEAR(guest_received[0], 20000, 2);

    // Renaming with the SAME AudioStreamConfig keeps the channel's gain and notifies observers.
    guest_received.clear();
    const size_t observed_before_rename = observed.size();
    ASSERT_TRUE(mux.ReconfigureInjector(&owner,
                                        AudioStreamConfig{.sample_rate_hz = 48000,
                                                          .channels = 1,
                                                          .format = SampleFormat::kS16},
                                        "renamed_same_cfg_测试_🚀_é")
                        .ok());
    ASSERT_EQ(observed.size(), observed_before_rename + 1);
    EXPECT_EQ(observed.back().microphone_inputs[1].source_id, "renamed_same_cfg_测试_🚀_é");
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 480U);
    EXPECT_NEAR(guest_received[0], 20000, 2);

    mux.RemoveStateObserver(obs_id);
    mux.UnregisterInjector(&owner);
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, ObserverCallbackCanRemoveSiblingObserverWithoutDeadlockOrStaleInvoke) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::atomic<int> obs2_calls{0};
    AudioInputMux::StateObserverId obs2_id = 0;

    const auto obs1_id = mux.AddStateObserver(
            [&](const AudioInputStateSnapshot& /*state*/) { mux.RemoveStateObserver(obs2_id); });
    obs2_id = mux.AddStateObserver([&](const AudioInputStateSnapshot& /*state*/) {
        obs2_calls.fetch_add(1, std::memory_order_relaxed);
    });

    ScopedVmUnlock unlock;
    absl::Notification update_done;
    std::thread updater([&]() {
        ScopedVmLock lock;
        ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = true}).ok());
        update_done.Notify();
    });

    if (!update_done.WaitForNotificationWithTimeout(absl::Seconds(1))) {
        ADD_FAILURE() << "Deadlocked when observer callback removed sibling observer";
        std::_Exit(1);
    }
    updater.join();
    EXPECT_EQ(obs2_calls.load(std::memory_order_relaxed), 0);
    mux.RemoveStateObserver(obs1_id);
}

TEST_F(AudioInputMuxTest, ReentrantMutationNeverLeavesObserversOnAStaleSnapshot) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    // The first observer mutes the microphone as soon as it sees it unmuted; the second observer
    // only records what it is told.
    const auto muter = mux.AddStateObserver([&mux](const AudioInputStateSnapshot& state) {
        if (!state.microphone_muted) {
            ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = true}).ok());
        }
    });
    std::vector<bool> recorded_muted;
    const auto recorder = mux.AddStateObserver([&recorded_muted](const AudioInputStateSnapshot& s) {
        recorded_muted.push_back(s.microphone_muted);
    });

    // Start muted, then unmute: the muter immediately mutes again from inside its callback.
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = true}).ok());
    recorded_muted.clear();
    ASSERT_TRUE(mux.UpdateState(AudioInputStateMutation{.microphone_muted = false}).ok());

    EXPECT_TRUE(mux.GetStateSnapshot().microphone_muted);
    ASSERT_FALSE(recorded_muted.empty());
    EXPECT_TRUE(recorded_muted.back()) << "the last snapshot delivered must be the current one";

    mux.RemoveStateObserver(recorder);
    mux.RemoveStateObserver(muter);
}

TEST_F(AudioInputMuxTest, RemoveStateObserverBlocksUntilInFlightCallbackCompletes) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    auto heap_canary = std::make_unique<int>(0x12345678);
    absl::Notification callback_entered;
    absl::Notification allow_callback_exit;
    std::atomic<int> observed_canary{0};

    const auto obs_id = mux.AddStateObserver([&](const AudioInputStateSnapshot& /*state*/) {
        callback_entered.Notify();
        allow_callback_exit.WaitForNotification();
        observed_canary.store(*heap_canary, std::memory_order_relaxed);
    });

    ScopedVmUnlock unlock;
    std::thread updater([&mux]() {
        ScopedVmLock lock;
        (void)mux.UpdateState(AudioInputStateMutation{.microphone_muted = true});
    });

    ASSERT_TRUE(callback_entered.WaitForNotificationWithTimeout(absl::Seconds(5)));

    absl::Notification remover_finished;
    std::thread remover([&]() {
        mux.RemoveStateObserver(obs_id);
        // Destroy heap_canary immediately after RemoveStateObserver returns: under ASAN this would
        // trigger heap-use-after-free if RemoveStateObserver returned before the callback exited.
        heap_canary.reset();
        remover_finished.Notify();
    });

    EXPECT_FALSE(remover_finished.WaitForNotificationWithTimeout(absl::Milliseconds(25)));
    allow_callback_exit.Notify();

    remover.join();
    updater.join();
    EXPECT_EQ(observed_canary.load(std::memory_order_relaxed), 0x12345678);
}

TEST_F(AudioInputMuxTest, ConcurrentStateUpdatesObserverChurnAndReadsAreRaceFree) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice](int avail_bytes) {
                std::vector<uint8_t> buf(static_cast<size_t>(avail_bytes));
                mux.ReadVoice(voice, buf.data(), buf.size());
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           std::memset(dst, 0, max_frames * sizeof(int16_t));
                           return max_frames;
                       },
                       nullptr,
                       MicrophoneSourceMetadata{
                           .source_id = "stress-stream",
                           .kind = MicrophoneInputKind::kGrpcStream,
                       })
                    .ok());

    ScopedVmUnlock unlock;
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    threads.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 60; ++i) {
            ScopedVmLock lock;
            host.AdvanceTime(absl::Milliseconds(10));
            mux.OnTimerTick();
        }
    });

    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 40; ++i) {
                const bool flag = ((i + t) % 2 == 0);
                {
                    ScopedVmLock lock;
                    (void)mux.UpdateState(AudioInputStateMutation{
                        .microphone_muted = flag,
                        .input_updates = {MicrophoneInputChannelUpdate{.source_id = "host",
                                                                       .muted = flag},
                                          MicrophoneInputChannelUpdate{
                                              .source_id = "stress-stream",
                                              .muted = !flag,
                                              .gain = flag ? 0.75F : 1.25F,
                                          }},
                    });
                }
                (void)mux.GetStateSnapshot();
            }
        });
    }

    threads.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 40; ++i) {
            const auto id = mux.AddStateObserver(
                    [&mux](const AudioInputStateSnapshot&) { (void)mux.GetStateSnapshot(); });
            std::this_thread::yield();
            mux.RemoveStateObserver(id);
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    ScopedVmLock lock;
    mux.UnregisterInjector(&owner);
    mux.CloseVoice(voice);
}

}  // namespace
}  // namespace goldfish::audio
