// Copyright 2026 The Android Open Source Project
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

#include "pcm_converter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace goldfish::wasapi {
namespace {

int16_t ReadS16(const uint8_t* p) {
    int16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteS16(uint8_t* p, int16_t v) {
    std::memcpy(p, &v, sizeof(v));
}

TEST(PcmConverterTest, ParseWaveFormatStandardAndExtensible) {
    PcmFormat out{};

    // Standard 16-bit stereo PCM @ 48 kHz.
    RawWaveFormatFields pcm16{
        .format_tag = kWaveFormatTagPcm,
        .channels = 2U,
        .samples_per_sec = 48000U,
        .block_align = 4U,
        .bits_per_sample = 16U,
    };
    ASSERT_TRUE(ParseWaveFormat(pcm16, &out));
    EXPECT_EQ(out.format, PcmSampleFormat::kS16);
    EXPECT_EQ(out.sample_rate, 48000U);
    EXPECT_EQ(out.channels, 2U);
    EXPECT_EQ(out.BytesPerFrame(), 4U);

    // WAVEFORMATEXTENSIBLE 32-bit IEEE float 7.1 surround @ 48 kHz.
    RawWaveFormatFields ext_float{
        .format_tag = kWaveFormatTagExtensible,
        .channels = 8U,
        .samples_per_sec = 48000U,
        .block_align = 32U,
        .bits_per_sample = 32U,
        .cb_size = 22U,
        .valid_bits_per_sample = 32U,
        .subformat_is_ieee_float = true,
    };
    ASSERT_TRUE(ParseWaveFormat(ext_float, &out));
    EXPECT_EQ(out.format, PcmSampleFormat::kF32);
    EXPECT_EQ(out.channels, 8U);
    EXPECT_EQ(out.BytesPerFrame(), 32U);

    // WAVEFORMATEXTENSIBLE 24-bit valid PCM in 32-bit container.
    RawWaveFormatFields ext_s24_in_32{
        .format_tag = kWaveFormatTagExtensible,
        .channels = 2U,
        .samples_per_sec = 96000U,
        .block_align = 8U,
        .bits_per_sample = 32U,
        .cb_size = 22U,
        .valid_bits_per_sample = 24U,
        .subformat_is_pcm = true,
    };
    ASSERT_TRUE(ParseWaveFormat(ext_s24_in_32, &out));
    EXPECT_EQ(out.format, PcmSampleFormat::kS24In32);
    EXPECT_EQ(out.sample_rate, 96000U);

    // Packed 24-bit PCM.
    RawWaveFormatFields pcm24{
        .format_tag = kWaveFormatTagPcm,
        .channels = 2U,
        .samples_per_sec = 44100U,
        .block_align = 6U,
        .bits_per_sample = 24U,
    };
    ASSERT_TRUE(ParseWaveFormat(pcm24, &out));
    EXPECT_EQ(out.format, PcmSampleFormat::kS24Packed);
    EXPECT_EQ(out.BytesPerFrame(), 6U);
}

TEST(PcmConverterTest, ParseWaveFormatRejectsMalformedInputs) {
    PcmFormat out{};
    EXPECT_FALSE(ParseWaveFormat(RawWaveFormatFields{}, &out));

    // Mismatched block_align.
    RawWaveFormatFields bad_align{
        .format_tag = kWaveFormatTagPcm,
        .channels = 2U,
        .samples_per_sec = 48000U,
        .block_align = 2U,
        .bits_per_sample = 16U,
    };
    EXPECT_FALSE(ParseWaveFormat(bad_align, &out));

    // Extensible with cbSize < 22.
    RawWaveFormatFields short_ext{
        .format_tag = kWaveFormatTagExtensible,
        .channels = 2U,
        .samples_per_sec = 48000U,
        .block_align = 8U,
        .bits_per_sample = 32U,
        .cb_size = 10U,
        .valid_bits_per_sample = 32U,
        .subformat_is_ieee_float = true,
    };
    EXPECT_FALSE(ParseWaveFormat(short_ext, &out));

    // Sample rate below 8 kHz.
    RawWaveFormatFields low_rate{
        .format_tag = kWaveFormatTagPcm,
        .channels = 2U,
        .samples_per_sec = 4000U,
        .block_align = 4U,
        .bits_per_sample = 16U,
    };
    EXPECT_FALSE(ParseWaveFormat(low_rate, &out));
}

TEST(PcmConverterTest, ReadRingToHardwareChannelOnlyStereoToSurroundWithWrapAndUnderrun) {
    const PcmFormat sw_fmt{PcmSampleFormat::kS16, 48000U, 2U};
    // Ring holds 4 stereo frames = 16 bytes. Start at byte 12 so reading 3 frames wraps.
    std::vector<uint8_t> ring(16U, 0U);
    // Frame 0 @ offset 12: L=1000, R=-1000
    WriteS16(&ring[12], 1000);
    WriteS16(&ring[14], -1000);
    // Frame 1 @ offset 0: L=2000, R=-2000
    WriteS16(&ring[0], 2000);
    WriteS16(&ring[2], -2000);
    // Frame 2 @ offset 4: L=3000, R=-3000
    WriteS16(&ring[4], 3000);
    WriteS16(&ring[6], -3000);

    size_t pos_emul = 8U;       // 3 frames written starting at offset 12 -> (12 + 12) % 16 = 8
    size_t pending_emul = 12U;  // 3 frames * 4 bytes

    // Drain into 5 frames of 6-channel S16 hardware buffer (12 bytes per hw frame).
    std::vector<uint8_t> hw_dst(size_t{5U} * 12U, 0x7FU);
    const size_t copied = ReadRingToHardwareChannelOnly(
            ring.data(), ring.size(), pos_emul, &pending_emul, hw_dst.data(), 5U, sw_fmt, 6U);

    EXPECT_EQ(copied, 3U);
    EXPECT_EQ(pending_emul, 0U);

    for (size_t f = 0U; f < 3U; ++f) {
        const auto expected = static_cast<int16_t>((f + 1U) * 1000U);
        const uint8_t* frame_ptr = hw_dst.data() + f * 12U;
        EXPECT_EQ(ReadS16(frame_ptr + 0U), expected);
        EXPECT_EQ(ReadS16(frame_ptr + 2U), -expected);
        for (size_t ch = 2U; ch < 6U; ++ch) {
            EXPECT_EQ(ReadS16(frame_ptr + ch * 2U), 0);
        }
    }
    // Frames 3..4 must be zeroed (underrun silence).
    for (size_t f = 3U; f < 5U; ++f) {
        const uint8_t* frame_ptr = hw_dst.data() + f * 12U;
        for (size_t ch = 0U; ch < 6U; ++ch) {
            EXPECT_EQ(ReadS16(frame_ptr + ch * 2U), 0);
        }
    }
}

TEST(PcmConverterTest, ReadRingToHardwareUnsigned8UsesMidpointSilence) {
    const PcmFormat sw_fmt{PcmSampleFormat::kU8, 48000U, 1U};
    std::vector<uint8_t> ring = {200U, 50U, 0U, 0U};
    size_t pending_emul = 2U;
    std::vector<uint8_t> hw_dst(size_t{4U} * 2U, 0U);

    const size_t copied =
            ReadRingToHardwareChannelOnly(ring.data(), ring.size(), /*pos_emul=*/2U, &pending_emul,
                                          hw_dst.data(), 4U, sw_fmt, 2U);
    EXPECT_EQ(copied, 2U);
    EXPECT_EQ(pending_emul, 0U);
    // Frame 0: ch0=200, ch1=0x80
    EXPECT_EQ(hw_dst[0], 200U);
    EXPECT_EQ(hw_dst[1], 0x80U);
    // Frame 1: ch0=50, ch1=0x80
    EXPECT_EQ(hw_dst[2], 50U);
    EXPECT_EQ(hw_dst[3], 0x80U);
    // Underrun frames 2..3: all 0x80
    for (size_t i = 4U; i < 8U; ++i) {
        EXPECT_EQ(hw_dst[i], 0x80U);
    }
}

TEST(PcmConverterTest, WriteHardwareToRingChannelOnlyMonoToStereoAndSilentFlag) {
    const PcmFormat sw_fmt{PcmSampleFormat::kS16, 48000U, 2U};
    // Ring holds 4 stereo S16 frames = 16 bytes. Start at offset 12 to test wrap.
    std::vector<uint8_t> ring(16U, 0xAAU);
    size_t pos_emul = 12U;
    size_t pending_emul = 0U;

    // 2 mono S16 frames: 1234, -5678.
    std::vector<uint8_t> hw_mono(4U);
    WriteS16(&hw_mono[0], 1234);
    WriteS16(&hw_mono[2], -5678);

    size_t written = WriteHardwareToRingChannelOnly(hw_mono.data(), 2U, /*hw_is_silent=*/false,
                                                    ring.data(), ring.size(), &pos_emul,
                                                    &pending_emul, sw_fmt, /*hw_channels=*/1U);
    EXPECT_EQ(written, 2U);
    EXPECT_EQ(pos_emul, 4U);
    EXPECT_EQ(pending_emul, 8U);
    EXPECT_EQ(ReadS16(&ring[12]), 1234);
    EXPECT_EQ(ReadS16(&ring[14]), 0);
    EXPECT_EQ(ReadS16(&ring[0]), -5678);
    EXPECT_EQ(ReadS16(&ring[2]), 0);

    // Next 2 frames arrive with AUDCLNT_BUFFERFLAGS_SILENT = true and garbage payload.
    std::vector<uint8_t> garbage(4U, 0xFFU);
    written = WriteHardwareToRingChannelOnly(garbage.data(), 2U, /*hw_is_silent=*/true, ring.data(),
                                             ring.size(), &pos_emul, &pending_emul, sw_fmt,
                                             /*hw_channels=*/1U);
    EXPECT_EQ(written, 2U);
    EXPECT_EQ(pos_emul, 12U);
    EXPECT_EQ(pending_emul, 16U);
    EXPECT_EQ(ReadS16(&ring[4]), 0);
    EXPECT_EQ(ReadS16(&ring[6]), 0);
    EXPECT_EQ(ReadS16(&ring[8]), 0);
    EXPECT_EQ(ReadS16(&ring[10]), 0);
}

}  // namespace
}  // namespace goldfish::wasapi
