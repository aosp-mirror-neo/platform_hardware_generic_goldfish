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

#include "goldfish/videobridge/goldfish_audio_device_module.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

namespace goldfish::videobridge {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

constexpr size_t kBlock = GoldfishAudioDeviceModule::kPlayoutSampleRateHz / 100;

// Plays a ramp 0, 1, 2, ... so tests can tell exactly which samples came out, and records how
// it was asked.
class RampTransport : public ::webrtc::AudioTransport {
  public:
    int32_t RecordedDataIsAvailable(const void*, size_t, size_t, size_t, uint32_t, uint32_t,
                                    int32_t, uint32_t, bool, uint32_t&) override {
        return 0;
    }

    int32_t NeedMorePlayData(size_t samples, size_t bytes_per_sample, size_t channels,
                             uint32_t rate_hz, void* audio, size_t& samples_out, int64_t* elapsed,
                             int64_t* ntp) override {
        ++calls;
        last_samples = samples;
        last_bytes_per_sample = bytes_per_sample;
        last_channels = channels;
        last_rate_hz = rate_hz;
        auto* out = static_cast<int16_t*>(audio);
        samples_out = short_block ? samples / 2 : samples * channels;
        for (size_t i = 0; i < samples_out; ++i) {
            out[i] = next++;
        }
        *elapsed = 0;
        *ntp = 0;
        return 0;
    }

    void PullRenderData(int, int, size_t, size_t, void*, int64_t*, int64_t*) override {}

    int calls = 0;
    size_t last_samples = 0;
    size_t last_bytes_per_sample = 0;
    size_t last_channels = 0;
    uint32_t last_rate_hz = 0;
    bool short_block = false;
    int16_t next = 0;
};

std::vector<int16_t> Ramp(int16_t first, size_t count) {
    std::vector<int16_t> ramp(count);
    std::iota(ramp.begin(), ramp.end(), first);
    return ramp;
}

TEST(GoldfishAudioDeviceModuleTest, CreateReturnsValidReferenceCountedModule) {
    auto module = GoldfishAudioDeviceModule::Create();
    ASSERT_NE(module, nullptr);

    EXPECT_EQ(module->RecordingDevices(), 1);

    bool stereo_available = false;
    EXPECT_EQ(module->StereoRecordingIsAvailable(&stereo_available), 0);
    EXPECT_TRUE(stereo_available);

    bool stereo_enabled = false;
    EXPECT_EQ(module->StereoRecording(&stereo_enabled), 0);
    EXPECT_TRUE(stereo_enabled);
}

TEST(GoldfishAudioDeviceModuleTest, PullPlayoutReturnsNothingWithoutAnEngine) {
    auto module = GoldfishAudioDeviceModule::Create();
    std::vector<int16_t> dst(kBlock, 7);

    EXPECT_EQ(module->PullPlayout(dst), 0u);
    EXPECT_THAT(dst, Each(7));
}

TEST(GoldfishAudioDeviceModuleTest, PullPlayoutAsksForTenMillisecondMonoBlocks) {
    auto module = GoldfishAudioDeviceModule::Create();
    RampTransport transport;
    module->RegisterAudioCallback(&transport);
    std::vector<int16_t> dst(kBlock);

    ASSERT_EQ(module->PullPlayout(dst), kBlock);

    EXPECT_EQ(transport.calls, 1);
    EXPECT_EQ(transport.last_samples, kBlock);
    EXPECT_EQ(transport.last_bytes_per_sample, sizeof(int16_t));
    EXPECT_EQ(transport.last_channels, 1u);
    EXPECT_EQ(transport.last_rate_hz, 48000u);
    EXPECT_THAT(dst, ElementsAreArray(Ramp(0, kBlock)));
    module->RegisterAudioCallback(nullptr);
}

TEST(GoldfishAudioDeviceModuleTest, PullPlayoutKeepsTheRestOfAPartlyReadBlock) {
    auto module = GoldfishAudioDeviceModule::Create();
    RampTransport transport;
    module->RegisterAudioCallback(&transport);
    // 100 + 900 samples straddle three blocks; nothing may be dropped or repeated.
    std::vector<int16_t> first(100);
    std::vector<int16_t> second(900);

    ASSERT_EQ(module->PullPlayout(first), first.size());
    ASSERT_EQ(module->PullPlayout(second), second.size());

    EXPECT_THAT(first, ElementsAreArray(Ramp(0, 100)));
    EXPECT_THAT(second, ElementsAreArray(Ramp(100, 900)));
    EXPECT_EQ(transport.calls, 3);
    module->RegisterAudioCallback(nullptr);
}

TEST(GoldfishAudioDeviceModuleTest, PullPlayoutPadsAShortBlockWithSilence) {
    auto module = GoldfishAudioDeviceModule::Create();
    RampTransport transport;
    transport.short_block = true;
    transport.next = 1;
    module->RegisterAudioCallback(&transport);
    std::vector<int16_t> dst(kBlock);

    ASSERT_EQ(module->PullPlayout(dst), kBlock);

    EXPECT_THAT(std::vector<int16_t>(dst.begin(), dst.begin() + kBlock / 2),
                ElementsAreArray(Ramp(1, kBlock / 2)));
    EXPECT_THAT(std::vector<int16_t>(dst.begin() + kBlock / 2, dst.end()), Each(0));
    module->RegisterAudioCallback(nullptr);
}

TEST(GoldfishAudioDeviceModuleTest, UnregisteringDropsLeftoversAndStopsPulling) {
    auto module = GoldfishAudioDeviceModule::Create();
    RampTransport old_engine;
    module->RegisterAudioCallback(&old_engine);
    std::vector<int16_t> partial(10);
    ASSERT_EQ(module->PullPlayout(partial), partial.size());

    module->RegisterAudioCallback(nullptr);
    std::vector<int16_t> dst(kBlock);
    EXPECT_EQ(module->PullPlayout(dst), 0u);

    RampTransport new_engine;
    new_engine.next = 1000;
    module->RegisterAudioCallback(&new_engine);
    ASSERT_EQ(module->PullPlayout(dst), kBlock);
    EXPECT_THAT(dst, ElementsAreArray(Ramp(1000, kBlock)));
    EXPECT_EQ(old_engine.calls, 1);
    module->RegisterAudioCallback(nullptr);
}

TEST(GoldfishAudioDeviceModuleTest, PlayoutListenerHearsEachChangeOnce) {
    auto module = GoldfishAudioDeviceModule::Create();
    std::vector<bool> events;
    module->SetPlayoutListener([&events](bool playing) { events.push_back(playing); });

    EXPECT_FALSE(module->Playing());
    module->StartPlayout();
    EXPECT_TRUE(module->Playing());
    module->StartPlayout();
    // WebRTC stops playout twice when a session closes.
    module->StopPlayout();
    module->StopPlayout();
    EXPECT_FALSE(module->Playing());

    EXPECT_THAT(events, ElementsAre(true, false));
    module->SetPlayoutListener(nullptr);
}

TEST(GoldfishAudioDeviceModuleTest, PlayoutListenerSetLateHearsThatPlayoutRuns) {
    auto module = GoldfishAudioDeviceModule::Create();
    module->StartPlayout();
    std::vector<bool> events;

    module->SetPlayoutListener([&events](bool playing) { events.push_back(playing); });

    EXPECT_THAT(events, ElementsAre(true));
    module->SetPlayoutListener(nullptr);
}

TEST(GoldfishAudioDeviceModuleTest, ClearedPlayoutListenerIsNotCalled) {
    auto module = GoldfishAudioDeviceModule::Create();
    int calls = 0;
    module->SetPlayoutListener([&calls](bool) { ++calls; });
    module->SetPlayoutListener(nullptr);

    module->StartPlayout();
    module->StopPlayout();

    EXPECT_EQ(calls, 0);
}

TEST(GoldfishAudioDeviceModuleTest, PlayoutWorksWithoutAListener) {
    auto module = GoldfishAudioDeviceModule::Create();

    EXPECT_EQ(module->StartPlayout(), 0);
    EXPECT_TRUE(module->Playing());
    EXPECT_EQ(module->StopPlayout(), 0);
    EXPECT_FALSE(module->Playing());
}

}  // namespace
}  // namespace goldfish::videobridge
