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
#include <mfapi.h>
#include <mftransform.h>

#include "android/base/scoped_com_initializer.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "core/win/mf_video_encoder_h264.h"
#include "goldfish/videobridge/codec_factories.h"

namespace goldfish::videobridge {

namespace {

bool IsHardwareEncoderAvailable() {
    // Initialize Media Foundation temporarily to perform the check
    const android::base::ScopedComInitializer com_init(COINIT_APARTMENTTHREADED);
    if (!com_init) {
        return false;
    }

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        return false;
    }

    MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    // Enumerate hardware H.264 video encoders
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE,  // No software fallback please
                   &input_info, &output_info, &ppActivate, &count);

    if (SUCCEEDED(hr) && count > 0) {
        for (UINT32 i = 0; i < count; i++) {
            ppActivate[i]->Release();
        }
        CoTaskMemFree(ppActivate);
    }

    MFShutdown();

    return SUCCEEDED(hr) && count > 0;
}

// A simple H.264 encoder factory using Windows Media Foundation
class MFVideoEncoderFactoryH264 : public webrtc::VideoEncoderFactory {
  public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;

        if (!IsHardwareEncoderAvailable()) {
            return formats;
        }

        // Advertise H.264 Baseline Profile (42e01f) and High Profile (640c1f)
        // We elevate them to Level 5.2 (34 in hex) to support 1080x2400 high-res streams
        webrtc::SdpVideoFormat baseline("H264");
        baseline.parameters["level-asymmetry-allowed"] = "1";
        baseline.parameters["packetization-mode"] = "1";
        baseline.parameters["profile-level-id"] = "42e034";
        formats.push_back(baseline);

        webrtc::SdpVideoFormat high("H264");
        high.parameters["level-asymmetry-allowed"] = "1";
        high.parameters["packetization-mode"] = "1";
        high.parameters["profile-level-id"] = "640c34";
        formats.push_back(high);

        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if (format.name == "H264") {
            return std::make_unique<MFVideoEncoderH264>(format);
        }
        return nullptr;
    }
};

// A composite encoder factory that merges a hardware factory and a software factory.
class CompositeVideoEncoderFactory : public webrtc::VideoEncoderFactory {
  public:
    CompositeVideoEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> hardware_factory,
                                 std::unique_ptr<webrtc::VideoEncoderFactory> software_factory)
            : hardware_factory_(std::move(hardware_factory))
            , software_factory_(std::move(software_factory)) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        if (hardware_factory_) {
            auto hw_formats = hardware_factory_->GetSupportedFormats();
            formats.insert(formats.end(), hw_formats.begin(), hw_formats.end());
        }
        if (software_factory_) {
            auto sw_formats = software_factory_->GetSupportedFormats();
            formats.insert(formats.end(), sw_formats.begin(), sw_formats.end());
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if (hardware_factory_) {
            for (const auto& hw_format : hardware_factory_->GetSupportedFormats()) {
                if (hw_format.IsSameCodec(format) || format.name == "H264") {
                    auto encoder = hardware_factory_->Create(env, format);
                    if (encoder) return encoder;
                }
            }
        }
        if (software_factory_) {
            return software_factory_->Create(env, format);
        }
        return nullptr;
    }

  private:
    std::unique_ptr<webrtc::VideoEncoderFactory> hardware_factory_;
    std::unique_ptr<webrtc::VideoEncoderFactory> software_factory_;
};

}  // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreatePlatformVideoEncoderFactory() {
    auto hardware_factory = std::make_unique<MFVideoEncoderFactoryH264>();
    auto software_factory = webrtc::CreateBuiltinVideoEncoderFactory();
    return std::make_unique<CompositeVideoEncoderFactory>(std::move(hardware_factory),
                                                          std::move(software_factory));
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreatePlatformVideoDecoderFactory() {
    // Fall back to software decoders on Windows for now (D3D11 decoder can be added later)
    return webrtc::CreateBuiltinVideoDecoderFactory();
}

}  // namespace goldfish::videobridge
