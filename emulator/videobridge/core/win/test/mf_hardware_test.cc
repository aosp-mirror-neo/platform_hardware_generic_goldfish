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

#include <codecapi.h>
#include <gtest/gtest.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-reference-return"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/pps_parser.h"
#include "common_video/h264/sps_parser.h"
#include "libyuv/compare.h"
#include "libyuv/convert.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#pragma clang diagnostic pop

#include "android/base/scoped_com_initializer.h"
#include "core/win/mf_video_encoder_h264.h"
#include "goldfish/videobridge/codec_factories.h"

namespace goldfish::videobridge {
namespace {

struct NalUnitInfo {
    uint8_t type = 0;
    size_t offset = 0;
    size_t size = 0;
};

std::vector<NalUnitInfo> ParseAnnexBNalUnits(const uint8_t* data, size_t size) {
    std::vector<NalUnitInfo> nalus;
    if (!data || size < 4) return nalus;

    std::vector<size_t> start_offsets;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                start_offsets.push_back(i + 3);
                i += 2;
            } else if (i + 4 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                start_offsets.push_back(i + 4);
                i += 3;
            }
        }
    }

    for (size_t idx = 0; idx < start_offsets.size(); ++idx) {
        size_t payload_start = start_offsets[idx];
        if (payload_start >= size) continue;
        size_t next_end = size;
        if (idx + 1 < start_offsets.size()) {
            next_end = start_offsets[idx + 1] - 3;
            if (next_end > 0 && data[next_end - 1] == 0x00) {
                --next_end;
            }
        }
        uint8_t nal_type = data[payload_start] & 0x1F;
        nalus.push_back({nal_type, payload_start, next_end - payload_start});
    }
    return nalus;
}

bool HasNalUnitType(const std::vector<NalUnitInfo>& nalus, uint8_t target_type) {
    for (const auto& nalu : nalus) {
        if (nalu.type == target_type) return true;
    }
    return false;
}

class CapturingEncodedImageCallback : public webrtc::EncodedImageCallback {
  public:
    struct CapturedFrame {
        std::vector<uint8_t> bitstream;
        webrtc::VideoFrameType frame_type = webrtc::VideoFrameType::kEmptyFrame;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rtp_timestamp = 0;
        int64_t capture_time_ms = 0;
        webrtc::CodecSpecificInfo codec_info{};
    };

    Result OnEncodedImage(const webrtc::EncodedImage& encoded_image,
                          const webrtc::CodecSpecificInfo* codec_specific_info) override {
        CapturedFrame frame;
        if (encoded_image.data() && encoded_image.size() > 0) {
            frame.bitstream.assign(encoded_image.data(),
                                   encoded_image.data() + encoded_image.size());
        }
        frame.frame_type = encoded_image._frameType;
        frame.width = encoded_image._encodedWidth;
        frame.height = encoded_image._encodedHeight;
        frame.rtp_timestamp = encoded_image.RtpTimestamp();
        frame.capture_time_ms = encoded_image.capture_time_ms_;
        if (codec_specific_info) {
            frame.codec_info = *codec_specific_info;
        }
        frames_.push_back(std::move(frame));
        return Result(Result::OK);
    }

    void OnFrameDropped(uint32_t /*rtp_timestamp*/, int /*spatial_id*/,
                        bool /*is_end_of_temporal_unit*/) override {}

    const std::vector<CapturedFrame>& Frames() const { return frames_; }
    void Clear() { frames_.clear(); }

  private:
    std::vector<CapturedFrame> frames_;
};

std::vector<webrtc::scoped_refptr<webrtc::I420Buffer>> DecodeAnnexBFramesWithFfmpeg(
        const std::vector<CapturingEncodedImageCallback::CapturedFrame>& encoded_frames) {
    std::vector<webrtc::scoped_refptr<webrtc::I420Buffer>> decoded;
    const AVCodec* h264_decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!h264_decoder) return decoded;

    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> dec_ctx(
            avcodec_alloc_context3(h264_decoder),
            [](AVCodecContext* ctx) { avcodec_free_context(&ctx); });
    if (!dec_ctx || avcodec_open2(dec_ctx.get(), h264_decoder, nullptr) < 0) {
        return decoded;
    }

    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(),
                                                       [](AVPacket* p) { av_packet_free(&p); });
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(av_frame_alloc(),
                                                       [](AVFrame* f) { av_frame_free(&f); });

    auto drain_frames = [&]() {
        while (avcodec_receive_frame(dec_ctx.get(), frame.get()) == 0) {
            auto i420 = webrtc::I420Buffer::Copy(
                    frame->width, frame->height, frame->data[0], frame->linesize[0], frame->data[1],
                    frame->linesize[1], frame->data[2], frame->linesize[2]);
            decoded.push_back(std::move(i420));
        }
    };

    for (const auto& ef : encoded_frames) {
        pkt->data = const_cast<uint8_t*>(ef.bitstream.data());
        pkt->size = static_cast<int>(ef.bitstream.size());
        if (avcodec_send_packet(dec_ctx.get(), pkt.get()) >= 0) {
            drain_frames();
        }
    }
    avcodec_send_packet(dec_ctx.get(), nullptr);
    drain_frames();
    return decoded;
}

webrtc::VideoFrame CreateGradientI420Frame(int width, int height, int frame_index,
                                           uint32_t rtp_timestamp, int64_t render_time_ms = 0) {
    webrtc::scoped_refptr<webrtc::I420Buffer> buffer = webrtc::I420Buffer::Create(width, height);
    uint8_t* y_plane = buffer->MutableDataY();
    uint8_t* u_plane = buffer->MutableDataU();
    uint8_t* v_plane = buffer->MutableDataV();

    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            y_plane[r * buffer->StrideY() + c] =
                    static_cast<uint8_t>(16 + ((c + r + frame_index * 8) % 200));
        }
    }
    for (int r = 0; r < (height + 1) / 2; ++r) {
        for (int c = 0; c < (width + 1) / 2; ++c) {
            u_plane[r * buffer->StrideU() + c] =
                    static_cast<uint8_t>(64 + ((c + frame_index) % 128));
            v_plane[r * buffer->StrideV() + c] =
                    static_cast<uint8_t>(64 + ((r + frame_index) % 128));
        }
    }

    return webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_rtp_timestamp(rtp_timestamp)
            .set_timestamp_ms(render_time_ms)
            .set_rotation(webrtc::kVideoRotation_0)
            .build();
}

webrtc::VideoCodec MakeDefaultH264CodecSettings(uint16_t width = 1280, uint16_t height = 720,
                                                uint32_t start_bitrate_kbps = 2000,
                                                uint32_t max_framerate = 30) {
    webrtc::VideoCodec settings{};
    settings.codecType = webrtc::kVideoCodecH264;
    settings.width = width;
    settings.height = height;
    settings.startBitrate = start_bitrate_kbps;
    settings.maxBitrate = start_bitrate_kbps * 2;
    settings.minBitrate = 100;
    settings.maxFramerate = max_framerate;
    settings.numberOfSimulcastStreams = 0;
    return settings;
}

class MFHardwareTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        com_initializer_.emplace(COINIT_APARTMENTTHREADED);

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            skip_reason_ = "Failed to initialize Windows Media Foundation (MFStartup failed)";
            has_hardware_mft_ = false;
            return;
        }

        MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, &input_info,
                       &output_info, &activates, &count);

        if (FAILED(hr) || count == 0) {
            has_hardware_mft_ = false;
            skip_reason_ = "No hardware H.264 MFT video encoder detected on this machine";
        } else {
            has_hardware_mft_ = true;
            for (UINT32 i = 0; i < count; ++i) {
                WCHAR friendly_name[256] = {};
                UINT32 name_len = 0;
                if (SUCCEEDED(activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly_name,
                                                      ARRAYSIZE(friendly_name), &name_len))) {
                    char narrow_name[256] = {};
                    WideCharToMultiByte(CP_UTF8, 0, friendly_name, -1, narrow_name,
                                        sizeof(narrow_name), nullptr, nullptr);
                    hardware_encoder_names_.push_back(narrow_name);
                } else {
                    hardware_encoder_names_.push_back("Unknown Hardware MFT Encoder");
                }
                activates[i]->Release();
            }
            CoTaskMemFree(activates);
        }
    }

    static void TearDownTestSuite() {
        MFShutdown();
        com_initializer_.reset();
    }

    void SetUp() override {
        if (!has_hardware_mft_) {
            GTEST_SKIP() << "Skipping Windows Media Foundation hardware encoder test: "
                         << skip_reason_;
        }
    }

    static bool has_hardware_mft_;
    static std::optional<android::base::ScopedComInitializer> com_initializer_;
    static std::string skip_reason_;
    static std::vector<std::string> hardware_encoder_names_;
};

bool MFHardwareTest::has_hardware_mft_ = false;
std::optional<android::base::ScopedComInitializer> MFHardwareTest::com_initializer_;
std::string MFHardwareTest::skip_reason_;
std::vector<std::string> MFHardwareTest::hardware_encoder_names_;

// 1. Hardware Discovery & MFT Activation
TEST_F(MFHardwareTest, HardwareMFTProbingAndActivation) {
    std::cout << "[MF Hardware Test] Version: 1.4-20260923" << std::endl;
    ASSERT_FALSE(hardware_encoder_names_.empty());
    std::cout << "[MF Hardware Test] Discovered " << hardware_encoder_names_.size()
              << " hardware H.264 encoder(s):" << std::endl;
    for (const auto& name : hardware_encoder_names_) {
        std::cout << "  * " << name << std::endl;
    }

    MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, &input_info,
                           &output_info, &activates, &count);
    ASSERT_HRESULT_SUCCEEDED(hr);
    ASSERT_GT(count, 0U);

    ComPtr<IMFTransform> encoder_mft;
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_mft));
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    ASSERT_HRESULT_SUCCEEDED(hr);
    ASSERT_TRUE(encoder_mft);

    DWORD input_count = 0;
    DWORD output_count = 0;
    hr = encoder_mft->GetStreamCount(&input_count, &output_count);
    ASSERT_HRESULT_SUCCEEDED(hr);
    EXPECT_GE(input_count, 1U);
    EXPECT_GE(output_count, 1U);
}

// 2. Format Negotiation & Profile Probing
TEST_F(MFHardwareTest, FactoryProbesSupportedH264FormatsOnHardware) {
    auto factory = CreatePlatformVideoEncoderFactory();
    ASSERT_NE(factory, nullptr);

    std::vector<webrtc::SdpVideoFormat> formats = factory->GetSupportedFormats();
    EXPECT_FALSE(formats.empty());

    bool has_baseline = false;
    bool has_high = false;
    for (const auto& fmt : formats) {
        if (fmt.name == "H264") {
            auto it = fmt.parameters.find("profile-level-id");
            if (it != fmt.parameters.end()) {
                if (it->second.find("42e0") != std::string::npos) has_baseline = true;
                if (it->second.find("640c") != std::string::npos) has_high = true;
            }
        }
    }
    EXPECT_TRUE(has_baseline) << "Baseline profile format should be supported on hardware";
    EXPECT_TRUE(has_high) << "High profile format should be supported on hardware";
}

// 3. Basic I420 Frame Encoding and Annex-B NAL Unit Validation
TEST_F(MFHardwareTest, EncodesI420FramesAndProducesValidAnnexBBitstream) {
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr int kNumFrames = 10;

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 3000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    for (int i = 0; i < kNumFrames; ++i) {
        webrtc::VideoFrame frame =
                CreateGradientI420Frame(kWidth, kHeight, i, static_cast<uint32_t>(i * 3000));
        ASSERT_EQ(encoder->Encode(frame, nullptr), WEBRTC_VIDEO_CODEC_OK);
    }

    const auto& captured = callback.Frames();
    ASSERT_GE(captured.size(), static_cast<size_t>(kNumFrames));

    // First frame must be a keyframe with SPS, PPS, and IDR slice
    const auto& first_frame = captured[0];
    EXPECT_EQ(first_frame.frame_type, webrtc::VideoFrameType::kVideoFrameKey);
    EXPECT_EQ(first_frame.width, static_cast<uint32_t>(kWidth));
    EXPECT_EQ(first_frame.height, static_cast<uint32_t>(kHeight));

    auto first_nalus =
            ParseAnnexBNalUnits(first_frame.bitstream.data(), first_frame.bitstream.size());
    EXPECT_TRUE(HasNalUnitType(first_nalus, 7)) << "First frame must include SPS";
    EXPECT_TRUE(HasNalUnitType(first_nalus, 8)) << "First frame must include PPS";
    EXPECT_TRUE(HasNalUnitType(first_nalus, 5)) << "First frame must include IDR slice";

    // Subsequent frames must contain non-IDR slice (type 1)
    for (size_t i = 1; i < captured.size(); ++i) {
        auto nalus =
                ParseAnnexBNalUnits(captured[i].bitstream.data(), captured[i].bitstream.size());
        EXPECT_TRUE(HasNalUnitType(nalus, 1) || HasNalUnitType(nalus, 5));
    }
}

// 4. Mid-stream Keyframe Insertion on Demand
TEST_F(MFHardwareTest, ForcesMidStreamKeyFrameOnDemand) {
    constexpr int kWidth = 640;
    constexpr int kHeight = 480;
    constexpr int kNumFrames = 10;
    constexpr int kKeyframeIndex = 5;

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 2000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    for (int i = 0; i < kNumFrames; ++i) {
        webrtc::VideoFrame frame =
                CreateGradientI420Frame(kWidth, kHeight, i, static_cast<uint32_t>(i * 3000));
        std::vector<webrtc::VideoFrameType> frame_types;
        if (i == kKeyframeIndex) {
            frame_types.push_back(webrtc::VideoFrameType::kVideoFrameKey);
        } else {
            frame_types.push_back(webrtc::VideoFrameType::kVideoFrameDelta);
        }
        ASSERT_EQ(encoder->Encode(frame, &frame_types), WEBRTC_VIDEO_CODEC_OK);
    }

    const auto& captured = callback.Frames();
    ASSERT_GE(captured.size(), static_cast<size_t>(kNumFrames));

    EXPECT_EQ(captured[kKeyframeIndex].frame_type, webrtc::VideoFrameType::kVideoFrameKey);
    auto nalus = ParseAnnexBNalUnits(captured[kKeyframeIndex].bitstream.data(),
                                     captured[kKeyframeIndex].bitstream.size());
    EXPECT_TRUE(HasNalUnitType(nalus, 5)) << "Forced keyframe must contain an IDR slice (type 5)";

    // Verify delta frame on subsequent frame
    if (captured.size() > static_cast<size_t>(kKeyframeIndex + 1)) {
        EXPECT_EQ(captured[kKeyframeIndex + 1].frame_type,
                  webrtc::VideoFrameType::kVideoFrameDelta);
    }
}

// 5. Dynamic Bitrate and Framerate Adjustments
TEST_F(MFHardwareTest, HandlesDynamicBitrateAndFramerateAdjustments) {
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 2000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    // Initial 5 frames at 2000 kbps
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(kWidth, kHeight, i, i * 3000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
    }

    // Step down to 500 kbps
    webrtc::VideoEncoder::RateControlParameters rates_low;
    rates_low.bitrate.SetBitrate(0, 0, 500000);
    rates_low.framerate_fps = 30.0;
    encoder->SetRates(rates_low);

    for (int i = 5; i < 10; ++i) {
        ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(kWidth, kHeight, i, i * 3000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
    }

    // Step up to 4000 kbps and 60 fps
    webrtc::VideoEncoder::RateControlParameters rates_high;
    rates_high.bitrate.SetBitrate(0, 0, 4000000);
    rates_high.framerate_fps = 60.0;
    encoder->SetRates(rates_high);

    for (int i = 10; i < 15; ++i) {
        ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(kWidth, kHeight, i, i * 1500), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
    }

    EXPECT_GE(callback.Frames().size(), 15U);
}

// 6. Dynamic Resolution Change & Buffer Reallocation
TEST_F(MFHardwareTest, HandlesDynamicResolutionChangeAndBufferReallocation) {
    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    // Phase 1: 640x480
    webrtc::VideoCodec codec1 = MakeDefaultH264CodecSettings(640, 480, 1000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec1, settings), WEBRTC_VIDEO_CODEC_OK);

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(640, 480, i, i * 3000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
    }

    size_t count_phase1 = callback.Frames().size();
    ASSERT_GE(count_phase1, 5U);
    EXPECT_EQ(callback.Frames()[0].width, 640U);
    EXPECT_EQ(callback.Frames()[0].height, 480U);

    // Phase 2: Dynamic upscale to 1280x720
    webrtc::VideoCodec codec2 = MakeDefaultH264CodecSettings(1280, 720, 3000, 30);
    ASSERT_EQ(encoder->InitEncode(&codec2, settings), WEBRTC_VIDEO_CODEC_OK);

    for (int i = 5; i < 10; ++i) {
        ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(1280, 720, i, i * 3000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
    }

    ASSERT_GE(callback.Frames().size(), 10U);
    EXPECT_EQ(callback.Frames()[count_phase1].width, 1280U);
    EXPECT_EQ(callback.Frames()[count_phase1].height, 720U);
}

// 7. Non-16-Aligned Display Resolutions (e.g. 854x480 16:9, 1082x722, 638x358)
TEST_F(MFHardwareTest, HandlesNon16AlignedResolutionWithCorrectStride) {
    const std::vector<std::pair<int, int>> non_16_resolutions = {
        {854, 480},   // Standard 480p 16:9
        {1082, 722},  // Non-multiple of 16 or 8
        {638, 358}    // Odd dimension
    };

    for (const auto& [w, h] : non_16_resolutions) {
        auto encoder = std::make_unique<MFVideoEncoderH264>();
        CapturingEncodedImageCallback callback;
        ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

        webrtc::VideoCodec codec = MakeDefaultH264CodecSettings(w, h, 2000, 30);
        webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1,
                                                1000000);
        ASSERT_EQ(encoder->InitEncode(&codec, settings), WEBRTC_VIDEO_CODEC_OK);

        for (int i = 0; i < 5; ++i) {
            ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(w, h, i, i * 3000), nullptr),
                      WEBRTC_VIDEO_CODEC_OK);
        }

        ASSERT_GE(callback.Frames().size(), 5U);
        EXPECT_EQ(callback.Frames()[0].width, static_cast<uint32_t>(w));
        EXPECT_EQ(callback.Frames()[0].height, static_cast<uint32_t>(h));

        auto nalus = ParseAnnexBNalUnits(callback.Frames()[0].bitstream.data(),
                                         callback.Frames()[0].bitstream.size());
        EXPECT_TRUE(HasNalUnitType(nalus, 7)) << "Resolution " << w << "x" << h << " missing SPS";
        EXPECT_TRUE(HasNalUnitType(nalus, 5)) << "Resolution " << w << "x" << h << " missing IDR";
    }
}

// 8. Software Decode Round-Trip and PSNR Visual Fidelity Verification
TEST_F(MFHardwareTest, DecodesHardwareBitstreamAndVerifiesVisualFidelity) {
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr int kNumFrames = 10;

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 4000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    std::vector<webrtc::VideoFrame> src_frames;
    for (int i = 0; i < kNumFrames; ++i) {
        auto frame = CreateGradientI420Frame(kWidth, kHeight, i, static_cast<uint32_t>(i * 3000));
        src_frames.push_back(frame);
        ASSERT_EQ(encoder->Encode(frame, nullptr), WEBRTC_VIDEO_CODEC_OK);
    }

    ASSERT_GE(callback.Frames().size(), static_cast<size_t>(kNumFrames));

    auto decoded_frames = DecodeAnnexBFramesWithFfmpeg(callback.Frames());
    ASSERT_GE(decoded_frames.size(), 1U) << "FFmpeg must successfully decode encoded bitstream";

    double total_psnr = 0.0;
    size_t compared = std::min(decoded_frames.size(), src_frames.size());
    for (size_t i = 0; i < compared; ++i) {
        auto src_i420 = src_frames[i].video_frame_buffer()->ToI420();
        auto dec_i420 = decoded_frames[i];

        double psnr = libyuv::I420Psnr(src_i420->DataY(), src_i420->StrideY(), src_i420->DataU(),
                                       src_i420->StrideU(), src_i420->DataV(), src_i420->StrideV(),
                                       dec_i420->DataY(), dec_i420->StrideY(), dec_i420->DataU(),
                                       dec_i420->StrideU(), dec_i420->DataV(), dec_i420->StrideV(),
                                       kWidth, kHeight);
        total_psnr += psnr;
    }

    double avg_psnr = total_psnr / static_cast<double>(compared);
    std::cout << "[MF Hardware Test] Average decode PSNR over " << compared
              << " frames: " << avg_psnr << " dB" << std::endl;
    EXPECT_GT(avg_psnr, 30.0) << "Decoded hardware stream visual fidelity should exceed 30 dB";
}

// 9. Spatial Quadrant Color and Luma Gradient Preservation (Verifies UV ordering and orientation)
TEST_F(MFHardwareTest, PreservesSpatialQuadrantColorsAndLumaGradient) {
    constexpr int kWidth = 640;
    constexpr int kHeight = 480;
    webrtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(kWidth, kHeight);

    // Top-Left: Red (Y=82, U=90, V=240), Top-Right: Green (Y=145, U=54, V=34)
    // Bottom-Left: Blue (Y=41, U=240, V=110), Bottom-Right: Horizontal Luma Ramp (U=128, V=128)
    for (int r = 0; r < kHeight; ++r) {
        for (int c = 0; c < kWidth; ++c) {
            uint8_t y_val = 0;
            if (r < kHeight / 2 && c < kWidth / 2) {
                y_val = 82;
            } else if (r < kHeight / 2 && c >= kWidth / 2) {
                y_val = 145;
            } else if (r >= kHeight / 2 && c < kWidth / 2) {
                y_val = 41;
            } else {
                y_val = static_cast<uint8_t>(32 + ((c - kWidth / 2) * 180) /
                                                          std::max(1, (kWidth / 2 - 1)));
            }
            i420->MutableDataY()[r * i420->StrideY() + c] = y_val;
        }
    }
    for (int r = 0; r < kHeight / 2; ++r) {
        for (int c = 0; c < kWidth / 2; ++c) {
            uint8_t u_val = 128;
            uint8_t v_val = 128;
            if (r < kHeight / 4 && c < kWidth / 4) {
                u_val = 90;
                v_val = 240;
            } else if (r < kHeight / 4 && c >= kWidth / 4) {
                u_val = 54;
                v_val = 34;
            } else if (r >= kHeight / 4 && c < kWidth / 4) {
                u_val = 240;
                v_val = 110;
            }
            i420->MutableDataU()[r * i420->StrideU() + c] = u_val;
            i420->MutableDataV()[r * i420->StrideV() + c] = v_val;
        }
    }

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 4000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoFrame src_frame = webrtc::VideoFrame::Builder()
                                           .set_video_frame_buffer(i420)
                                           .set_rtp_timestamp(1000)
                                           .build();
    ASSERT_EQ(encoder->Encode(src_frame, nullptr), WEBRTC_VIDEO_CODEC_OK);
    ASSERT_GE(callback.Frames().size(), 1U);

    auto decoded_frames = DecodeAnnexBFramesWithFfmpeg(callback.Frames());
    ASSERT_GE(decoded_frames.size(), 1U);
    auto dec_i420 = decoded_frames[0];

    auto sample_yuv = [&](int x, int y, int& out_y, int& out_u, int& out_v) {
        out_y = dec_i420->DataY()[y * dec_i420->StrideY() + x];
        out_u = dec_i420->DataU()[(y / 2) * dec_i420->StrideU() + (x / 2)];
        out_v = dec_i420->DataV()[(y / 2) * dec_i420->StrideV() + (x / 2)];
    };

    int y = 0, u = 0, v = 0;
    // Top-Left (Red)
    sample_yuv(kWidth / 4, kHeight / 4, y, u, v);
    EXPECT_NEAR(y, 82, 20);
    EXPECT_NEAR(u, 90, 25);
    EXPECT_NEAR(v, 240, 25);

    // Top-Right (Green)
    sample_yuv((kWidth * 3) / 4, kHeight / 4, y, u, v);
    EXPECT_NEAR(y, 145, 20);
    EXPECT_NEAR(u, 54, 25);
    EXPECT_NEAR(v, 34, 25);

    // Bottom-Left (Blue)
    sample_yuv(kWidth / 4, (kHeight * 3) / 4, y, u, v);
    EXPECT_NEAR(y, 41, 20);
    EXPECT_NEAR(u, 240, 25);
    EXPECT_NEAR(v, 110, 25);

    // Bottom-Right (Luma ramp monotonic increase)
    int y_left = 0, y_mid = 0, y_right = 0;
    sample_yuv(kWidth / 2 + 20, (kHeight * 3) / 4, y_left, u, v);
    sample_yuv((kWidth * 3) / 4, (kHeight * 3) / 4, y_mid, u, v);
    sample_yuv(kWidth - 20, (kHeight * 3) / 4, y_right, u, v);
    EXPECT_LT(y_left, y_mid);
    EXPECT_LT(y_mid, y_right);
}

// 10. Negotiated H.264 Profile IDC Enforcement in Bitstream SPS
TEST_F(MFHardwareTest, EnforcesRequestedH264ProfileIdcInBitstreamSps) {
    struct ProfileTestCase {
        const char* profile_level_id;
        uint8_t expected_profile_idc;
    };
    const ProfileTestCase kCases[] = {
        {"42e034", 66},   // Baseline profile
        {"640c34", 100},  // High profile
    };

    for (const auto& tc : kCases) {
        webrtc::SdpVideoFormat format("H264", {{"profile-level-id", tc.profile_level_id}});
        auto encoder = std::make_unique<MFVideoEncoderH264>(format);

        CapturingEncodedImageCallback callback;
        ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

        webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(640, 360, 1500, 30);
        webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1,
                                                1000000);
        ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

        ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(640, 360, 0, 1000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
        ASSERT_GE(callback.Frames().size(), 1U);

        const auto& bs = callback.Frames()[0].bitstream;
        auto nalus = ParseAnnexBNalUnits(bs.data(), bs.size());
        bool found_sps = false;
        for (const auto& nalu : nalus) {
            if (nalu.type == 7 && nalu.size > 1) {
                // SPS payload: 1st byte is NAL header, 2nd byte is profile_idc
                uint8_t profile_idc = bs[nalu.offset + 1];
                std::cout << "[MF Hardware Test] Negotiated " << tc.profile_level_id
                          << " yielded SPS profile_idc: " << static_cast<int>(profile_idc)
                          << std::endl;
                EXPECT_EQ(profile_idc, tc.expected_profile_idc);
                found_sps = true;
                break;
            }
        }
        EXPECT_TRUE(found_sps) << "Could not locate SPS NAL unit for profile "
                               << tc.profile_level_id;
    }
}

// 11. Portrait and Foldable Aspect Ratio Displays
TEST_F(MFHardwareTest, EncodesPortraitAndFoldableAspectRatios) {
    const std::vector<std::pair<int, int>> aspect_ratios = {
        {720, 1280},   // Standard phone portrait (9:16)
        {1080, 2400},  // Tall aspect ratio (20:9)
        {1840, 2208}   // Pixel Fold inner display (6:5)
    };

    for (const auto& [w, h] : aspect_ratios) {
        auto encoder = std::make_unique<MFVideoEncoderH264>();
        CapturingEncodedImageCallback callback;
        ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

        webrtc::VideoCodec codec = MakeDefaultH264CodecSettings(w, h, 3000, 30);
        webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1,
                                                1000000);
        ASSERT_EQ(encoder->InitEncode(&codec, settings), WEBRTC_VIDEO_CODEC_OK);

        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(w, h, i, i * 3000), nullptr),
                      WEBRTC_VIDEO_CODEC_OK);
        }

        ASSERT_GE(callback.Frames().size(), 3U);
        EXPECT_EQ(callback.Frames()[0].width, static_cast<uint32_t>(w));
        EXPECT_EQ(callback.Frames()[0].height, static_cast<uint32_t>(h));

        auto decoded = DecodeAnnexBFramesWithFfmpeg(callback.Frames());
        ASSERT_GE(decoded.size(), 1U);
        EXPECT_EQ(decoded[0]->width(), w);
        EXPECT_EQ(decoded[0]->height(), h);
    }
}

// 12. Cross-Thread Encoding (Encoder invoked from worker thread)
TEST_F(MFHardwareTest, SupportsEncodeOnWorkerThreadSeparateFromInitThread) {
    constexpr int kWidth = 640;
    constexpr int kHeight = 480;
    constexpr int kNumFrames = 10;

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 2000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    // Dispatch encode calls from worker thread
    std::thread worker([&]() {
        const android::base::ScopedComInitializer com_init(COINIT_MULTITHREADED);
        for (int i = 0; i < kNumFrames; ++i) {
            webrtc::VideoFrame frame =
                    CreateGradientI420Frame(kWidth, kHeight, i, static_cast<uint32_t>(i * 3000));
            EXPECT_EQ(encoder->Encode(frame, nullptr), WEBRTC_VIDEO_CODEC_OK);
        }
    });
    worker.join();

    EXPECT_GE(callback.Frames().size(), static_cast<size_t>(kNumFrames));
}

// 13. Zero B-Frames Ultra-Low Latency Verification
TEST_F(MFHardwareTest, EmitsZeroBFramesForUltraLowLatency) {
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr int kNumFrames = 15;

    auto encoder = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoCodec codec_settings = MakeDefaultH264CodecSettings(kWidth, kHeight, 3000, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&codec_settings, settings), WEBRTC_VIDEO_CODEC_OK);

    for (int i = 0; i < kNumFrames; ++i) {
        uint32_t rtp_ts = static_cast<uint32_t>((i + 1) * 3000);
        int64_t capture_time = (i + 1) * 33;
        webrtc::VideoFrame frame =
                CreateGradientI420Frame(kWidth, kHeight, i, rtp_ts, capture_time);
        ASSERT_EQ(encoder->Encode(frame, nullptr), WEBRTC_VIDEO_CODEC_OK);

        // Immediate frame emission without buffering latency
        ASSERT_EQ(callback.Frames().size(), static_cast<size_t>(i + 1))
                << "Hardware encoder buffered frame " << i
                << " (violating zero-latency requirement)";
        EXPECT_EQ(callback.Frames()[i].rtp_timestamp, rtp_ts);
        EXPECT_EQ(callback.Frames()[i].capture_time_ms, capture_time);
    }
}

// 14. Concurrent Multi-Display Encoder Sessions
TEST_F(MFHardwareTest, SupportsConcurrentEncoderSessionsForMultiDisplay) {
    constexpr int kNumFrames = 8;

    auto encoder1 = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback1;
    ASSERT_EQ(encoder1->RegisterEncodeCompleteCallback(&callback1), WEBRTC_VIDEO_CODEC_OK);
    webrtc::VideoCodec codec1 = MakeDefaultH264CodecSettings(1280, 720, 2500, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder1->InitEncode(&codec1, settings), WEBRTC_VIDEO_CODEC_OK);

    auto encoder2 = std::make_unique<MFVideoEncoderH264>();
    CapturingEncodedImageCallback callback2;
    ASSERT_EQ(encoder2->RegisterEncodeCompleteCallback(&callback2), WEBRTC_VIDEO_CODEC_OK);
    webrtc::VideoCodec codec2 = MakeDefaultH264CodecSettings(720, 1280, 2500, 30);
    ASSERT_EQ(encoder2->InitEncode(&codec2, settings), WEBRTC_VIDEO_CODEC_OK);

    // Interleave encoding frames between displays
    for (int i = 0; i < kNumFrames; ++i) {
        ASSERT_EQ(encoder1->Encode(CreateGradientI420Frame(1280, 720, i, i * 3000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
        ASSERT_EQ(encoder2->Encode(CreateGradientI420Frame(720, 1280, i, i * 3000), nullptr),
                  WEBRTC_VIDEO_CODEC_OK);
    }

    ASSERT_GE(callback1.Frames().size(), static_cast<size_t>(kNumFrames));
    ASSERT_GE(callback2.Frames().size(), static_cast<size_t>(kNumFrames));
    EXPECT_EQ(callback1.Frames()[0].width, 1280U);
    EXPECT_EQ(callback1.Frames()[0].height, 720U);
    EXPECT_EQ(callback2.Frames()[0].width, 720U);
    EXPECT_EQ(callback2.Frames()[0].height, 1280U);
}

// 15. Graceful Failure on Invalid Parameters and Clean Recovery
TEST_F(MFHardwareTest, FailsGracefullyOnInvalidDimensionsAndRecovers) {
    auto encoder = std::make_unique<MFVideoEncoderH264>();

    // Null settings
    EXPECT_EQ(encoder->InitEncode(nullptr, 1, 1200), WEBRTC_VIDEO_CODEC_ERR_PARAMETER);

    // Unsupported codec type
    webrtc::VideoCodec bad_codec = MakeDefaultH264CodecSettings(640, 480);
    bad_codec.codecType = webrtc::kVideoCodecVP8;
    EXPECT_EQ(encoder->InitEncode(&bad_codec, 1, 1200), WEBRTC_VIDEO_CODEC_ERR_PARAMETER);

    // Zero dimensions
    webrtc::VideoCodec zero_codec = MakeDefaultH264CodecSettings(0, 0);
    EXPECT_NE(encoder->InitEncode(&zero_codec, 1, 1200), WEBRTC_VIDEO_CODEC_OK);

    // Clean recovery with valid settings
    CapturingEncodedImageCallback callback;
    ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);
    webrtc::VideoCodec good_codec = MakeDefaultH264CodecSettings(640, 480, 1500, 30);
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1000000);
    ASSERT_EQ(encoder->InitEncode(&good_codec, settings), WEBRTC_VIDEO_CODEC_OK);

    ASSERT_EQ(encoder->Encode(CreateGradientI420Frame(640, 480, 0, 1000), nullptr),
              WEBRTC_VIDEO_CODEC_OK);
    EXPECT_GE(callback.Frames().size(), 1U);
}

// 16. Platform Factory Integration and Metadata Verification
TEST_F(MFHardwareTest, PlatformCodecFactorySelectsHardwareMFOnSupportedHost) {
    auto factory = CreatePlatformVideoEncoderFactory();
    ASSERT_NE(factory, nullptr);

    auto formats = factory->GetSupportedFormats();
    ASSERT_FALSE(formats.empty());

    webrtc::EnvironmentFactory env_factory;
    webrtc::Environment env = env_factory.Create();
    auto encoder = factory->Create(env, formats[0]);
    ASSERT_NE(encoder, nullptr);

    webrtc::VideoEncoder::EncoderInfo info = encoder->GetEncoderInfo();
    EXPECT_EQ(info.implementation_name, "WindowsMediaFoundationH264");
    EXPECT_TRUE(info.is_hardware_accelerated);
    EXPECT_FALSE(info.supports_native_handle);
}

}  // namespace
}  // namespace goldfish::videobridge
