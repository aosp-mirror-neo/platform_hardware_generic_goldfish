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

#include "goldfish/audio/pcm_pull_pipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace goldfish::audio {
namespace {

TEST(PcmPullPipelineTest, DirectPassthroughCopiesExactSamplesAndPadsUnderrun) {
    PcmPullPipeline pipeline(
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16});

    std::vector<int16_t> out(6, 9999);
    pipeline.Pull(out.size(), out.data(), [](void* dst, size_t max_frames) -> size_t {
        EXPECT_EQ(max_frames, 6U);
        const int16_t samples[4] = {1000, -2000, 3000, -4000};
        std::memcpy(dst, samples, sizeof(samples));
        return 4;  // Simulate partial underrun of 2 frames.
    });

    const std::vector<int16_t> expected = {1000, -2000, 3000, -4000, 0, 0};
    EXPECT_EQ(out, expected);
}

TEST(PcmPullPipelineTest, UnsignedUnderrunPadsWithMidpointSilence) {
    PcmPullPipeline u8_pipeline(
            AudioStreamConfig{.sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kU8},
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kU8});

    std::vector<uint8_t> u8_out(4, 0x00);
    u8_pipeline.Pull(4, u8_out.data(), [](void*, size_t) -> size_t { return 0; });
    EXPECT_EQ(u8_out, (std::vector<uint8_t>{0x80, 0x80, 0x80, 0x80}));

    PcmPullPipeline u16_be_pipeline(
            AudioStreamConfig{.sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
            GuestAudioConfig{.sample_rate_hz = 16000,
                             .channels = 1,
                             .format = GuestPcmFormat::kU16,
                             .big_endian = true});

    std::vector<uint8_t> u16_bytes(4, 0x00);
    u16_be_pipeline.Pull(2, u16_bytes.data(), [](void*, size_t) -> size_t { return 0; });
    // Big-endian 0x8000 -> [0x80, 0x00, 0x80, 0x00].
    EXPECT_EQ(u16_bytes, (std::vector<uint8_t>{0x80, 0x00, 0x80, 0x00}));
}

TEST(PcmPullPipelineTest, StereoFloatToMonoS16DownmixesCorrectly) {
    PcmPullPipeline pipeline(
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 2, .format = SampleFormat::kF32},
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16});

    std::vector<int16_t> out(3, 0);
    pipeline.Pull(3, out.data(), [](void* dst, size_t max_frames) -> size_t {
        EXPECT_EQ(max_frames, 3U);
        // Frame 0: (0.5, 0.5) -> 0.5 (~16384)
        // Frame 1: (1.0, -1.0) -> 0.0 (0)
        // Frame 2: (-0.5, -0.5) -> -0.5 (-16384)
        const float src[6] = {0.5F, 0.5F, 1.0F, -1.0F, -0.5F, -0.5F};
        std::memcpy(dst, src, sizeof(src));
        return 3;
    });

    EXPECT_NEAR(out[0], 16384, 2);
    EXPECT_EQ(out[1], 0);
    EXPECT_NEAR(out[2], -16384, 2);
}

TEST(PcmPullPipelineTest, MonoS16ToStereoF32UpmixesAndAppliesGuestVolume) {
    PcmPullPipeline pipeline(
            AudioStreamConfig{.sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 2, .format = GuestPcmFormat::kF32});

    GuestChannelVolume vol;
    vol.mute = false;
    vol.channels = 2;
    vol.gain[0] = 255;  // Full gain on left.
    vol.gain[1] = 128;  // ~Half gain on right.
    pipeline.SetVolume(vol);

    std::vector<float> out(4, 0.0F);
    pipeline.Pull(2, out.data(), [](void* dst, size_t max_frames) -> size_t {
        EXPECT_EQ(max_frames, 2U);
        const int16_t src[2] = {16384, -16384};  // +0.5, -0.5
        std::memcpy(dst, src, sizeof(src));
        return 2;
    });

    EXPECT_NEAR(out[0], 0.5F, 0.005F);
    EXPECT_NEAR(out[1], 0.5F * (128.0F / 255.0F), 0.005F);
    EXPECT_NEAR(out[2], -0.5F, 0.005F);
    EXPECT_NEAR(out[3], -0.5F * (128.0F / 255.0F), 0.005F);

    vol.mute = true;
    pipeline.SetVolume(vol);
    pipeline.Pull(2, out.data(), [](void* dst, size_t /*max_frames*/) -> size_t {
        const int16_t src[2] = {32767, 32767};
        std::memcpy(dst, src, sizeof(src));
        return 2;
    });
    EXPECT_EQ(out, (std::vector<float>{0.0F, 0.0F, 0.0F, 0.0F}));
}

TEST(PcmPullPipelineTest, Resamples48KhzStereoTo16KhzMonoPreserving1KhzSineTone) {
    // WebRTC inbound audio (48 kHz stereo S16) -> Android voice recognition (16 kHz mono S16).
    PcmPullPipeline pipeline(
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 2, .format = SampleFormat::kS16},
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16});

    size_t source_cursor = 0;
    constexpr double kToneHz = 1000.0;
    constexpr double kAmplitude = 20000.0;

    auto pull_sine = [&source_cursor](void* dst, size_t max_frames) -> size_t {
        auto* out = static_cast<int16_t*>(dst);
        for (size_t i = 0; i < max_frames; ++i) {
            const double t = static_cast<double>(source_cursor + i) / 48000.0;
            const auto sample = static_cast<int16_t>(
                    std::lround(kAmplitude * std::sin(2.0 * std::numbers::pi * kToneHz * t)));
            out[2 * i] = sample;
            out[2 * i + 1] = sample;
        }
        source_cursor += max_frames;
        return max_frames;
    };

    // Pull in odd-sized guest chunks (137 frames) to exercise resampler boundary continuity.
    std::vector<int16_t> guest_pcm;
    guest_pcm.reserve(1600);
    for (int iter = 0; iter < 12; ++iter) {
        std::vector<int16_t> chunk(137, 0);
        pipeline.Pull(chunk.size(), chunk.data(), pull_sine);
        guest_pcm.insert(guest_pcm.end(), chunk.begin(), chunk.end());
    }

    // Skip initial 64 frames of sinc filter warm-up delay and measure zero-crossings & RMS
    // over 1000 steady-state samples at 16 kHz (1 kHz = 16 samples/period -> ~125 zero-crossings).
    constexpr size_t kStart = 64;
    constexpr size_t kWindow = 1000;
    double sum_sq = 0.0;
    size_t zero_crossings = 0;
    for (size_t i = kStart; i < kStart + kWindow; ++i) {
        const double s = static_cast<double>(guest_pcm[i]);
        sum_sq += s * s;
        if ((guest_pcm[i - 1] < 0 && guest_pcm[i] >= 0) ||
            (guest_pcm[i - 1] >= 0 && guest_pcm[i] < 0)) {
            ++zero_crossings;
        }
    }

    const double rms = std::sqrt(sum_sq / static_cast<double>(kWindow));
    const double expected_rms = kAmplitude / std::numbers::sqrt2;
    EXPECT_NEAR(rms, expected_rms, expected_rms * 0.05);
    EXPECT_NEAR(static_cast<double>(zero_crossings), 125.0, 3.0);
}

TEST(PcmPullPipelineTest, ResamplesStereo44100ToStereo48000KeepingChannelsIndependent) {
    // Left channel = +0.5 DC, Right channel = -0.5 DC across stereo -> stereo sinc resampling.
    PcmPullPipeline pipeline(
            AudioStreamConfig{.sample_rate_hz = 44100, .channels = 2, .format = SampleFormat::kF32},
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kF32});

    std::vector<float> out(size_t{480} * 2, 0.0F);
    pipeline.Pull(480, out.data(), [](void* dst, size_t max_frames) -> size_t {
        auto* f = static_cast<float*>(dst);
        for (size_t i = 0; i < max_frames; ++i) {
            f[2 * i] = 0.5F;
            f[2 * i + 1] = -0.5F;
        }
        return max_frames;
    });

    // After filter group delay (~32 frames), left must converge to +0.5F and right to -0.5F.
    for (size_t frame = 64; frame < 480; ++frame) {
        EXPECT_NEAR(out[2 * frame], 0.5F, 0.02F);
        EXPECT_NEAR(out[2 * frame + 1], -0.5F, 0.02F);
    }
}

TEST(PcmPullPipelineTest, RemixesMultiChannelFourToTwoAndTwoToFourAndFourToMono) {
    // 1. 4-channel -> 2-channel modular folding:
    //    dst_ch0 = (src_ch0 + src_ch2) / 2, dst_ch1 = (src_ch1 + src_ch3) / 2.
    {
        PcmPullPipeline downmix_4_to_2(
                AudioStreamConfig{
                    .sample_rate_hz = 48000, .channels = 4, .format = SampleFormat::kF32},
                GuestAudioConfig{
                    .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kF32});
        std::array<float, 4> out{};
        downmix_4_to_2.Pull(2, out.data(), [](void* dst, size_t /*max_frames*/) -> size_t {
            const float src[8] = {
                0.2F, 0.4F, 0.6F, 0.8F, -0.2F, -0.4F, -0.6F, -0.8F,
            };
            std::memcpy(dst, src, sizeof(src));
            return 2;
        });
        EXPECT_NEAR(out[0], 0.4F, 1e-5F);
        EXPECT_NEAR(out[1], 0.6F, 1e-5F);
        EXPECT_NEAR(out[2], -0.4F, 1e-5F);
        EXPECT_NEAR(out[3], -0.6F, 1e-5F);
    }

    // 2. 2-channel -> 4-channel modular upmix:
    //    dst_ch0 = src_ch0, dst_ch1 = src_ch1, dst_ch2 = src_ch0, dst_ch3 = src_ch1.
    {
        PcmPullPipeline upmix_2_to_4(
                AudioStreamConfig{
                    .sample_rate_hz = 48000, .channels = 2, .format = SampleFormat::kF32},
                GuestAudioConfig{
                    .sample_rate_hz = 48000, .channels = 4, .format = GuestPcmFormat::kF32});
        std::array<float, 4> out{};
        upmix_2_to_4.Pull(1, out.data(), [](void* dst, size_t /*max_frames*/) -> size_t {
            const float src[2] = {0.25F, -0.75F};
            std::memcpy(dst, src, sizeof(src));
            return 1;
        });
        EXPECT_FLOAT_EQ(out[0], 0.25F);
        EXPECT_FLOAT_EQ(out[1], -0.75F);
        EXPECT_FLOAT_EQ(out[2], 0.25F);
        EXPECT_FLOAT_EQ(out[3], -0.75F);
    }

    // 3. 4-channel -> 1-channel mono average.
    {
        PcmPullPipeline downmix_4_to_1(
                AudioStreamConfig{
                    .sample_rate_hz = 48000, .channels = 4, .format = SampleFormat::kF32},
                GuestAudioConfig{
                    .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kF32});
        float out = 0.0F;
        downmix_4_to_1.Pull(1, &out, [](void* dst, size_t /*max_frames*/) -> size_t {
            const float src[4] = {0.1F, 0.3F, 0.5F, 0.7F};
            std::memcpy(dst, src, sizeof(src));
            return 1;
        });
        EXPECT_NEAR(out, 0.4F, 1e-5F);
    }
}

TEST(PcmPullPipelineTest, EncodesBigEndianGuestFormatsAndSanitizesNonFiniteFloats) {
    // Big-endian S16 with NaN sanitization and saturation clamping.
    {
        PcmPullPipeline s16_be(
                AudioStreamConfig{
                    .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kF32},
                GuestAudioConfig{.sample_rate_hz = 48000,
                                 .channels = 1,
                                 .format = GuestPcmFormat::kS16,
                                 .big_endian = true});
        std::array<uint8_t, 8> raw{};
        s16_be.Pull(4, raw.data(), [](void* dst, size_t /*max_frames*/) -> size_t {
            const float src[4] = {
                0.5F,
                std::numeric_limits<float>::quiet_NaN(),
                2.5F,
                -2.5F,
            };
            std::memcpy(dst, src, sizeof(src));
            return 4;
        });
        // Frame 0: +0.5F -> 0x4000 big-endian.
        EXPECT_EQ(raw[0], 0x40);
        EXPECT_EQ(raw[1], 0x00);
        // Frame 1: NaN -> 0.0F -> 0x0000.
        EXPECT_EQ(raw[2], 0x00);
        EXPECT_EQ(raw[3], 0x00);
        // Frame 2: +2.5F -> clamped +1.0F -> 0x7FFF big-endian.
        EXPECT_EQ(raw[4], 0x7F);
        EXPECT_EQ(raw[5], 0xFF);
        // Frame 3: -2.5F -> clamped -1.0F -> 0x8000 big-endian.
        EXPECT_EQ(raw[6], 0x80);
        EXPECT_EQ(raw[7], 0x00);
    }

    // Big-endian U16 and U32 short-read silence padding must byte-swap the unsigned midpoint
    // (0x8000 -> {0x80, 0x00} and 0x80000000 -> {0x80, 0x00, 0x00, 0x00}).
    {
        std::array<uint8_t, 4> u16_be_silence = {0xFF, 0xFF, 0xFF, 0xFF};
        FillGuestSilence(GuestPcmFormat::kU16, /*big_endian=*/true, u16_be_silence.data(),
                         u16_be_silence.size());
        EXPECT_EQ(u16_be_silence, (std::array<uint8_t, 4>{0x80, 0x00, 0x80, 0x00}));

        std::array<uint8_t, 4> u32_be_silence = {0xFF, 0xFF, 0xFF, 0xFF};
        FillGuestSilence(GuestPcmFormat::kU32, /*big_endian=*/true, u32_be_silence.data(),
                         u32_be_silence.size());
        EXPECT_EQ(u32_be_silence, (std::array<uint8_t, 4>{0x80, 0x00, 0x00, 0x00}));
    }
}

}  // namespace
}  // namespace goldfish::audio
