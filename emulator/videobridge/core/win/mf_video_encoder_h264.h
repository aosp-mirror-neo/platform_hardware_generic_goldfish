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
#pragma once

#include <memory>
#include <vector>

// Disable compiler warnings for external third-party headers.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-reference-return"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "absl/synchronization/mutex.h"

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#pragma clang diagnostic pop

#include <mfapi.h>
#include <mftransform.h>
#include <windows.h>

#include "android/base/scoped_com_initializer.h"

namespace goldfish::videobridge {

struct H264BitstreamInfo {
    bool has_sps = false;
    bool has_vcl_nal = false;
    bool is_keyframe = false;
};

H264BitstreamInfo InspectH264Bitstream(const uint8_t* data, DWORD length);
uint32_t CalculateNV12BufferSize(int width, int height);

template <typename T>
class ComPtr {
  public:
    ComPtr() : ptr_(nullptr) {}
    ComPtr(T* p) : ptr_(p) {
        if (ptr_) ptr_->AddRef();
    }
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }
    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            if (ptr_) ptr_->AddRef();
        }
        return *this;
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const { return ptr_; }
    T** GetAddressOf() {
        Reset();
        return &ptr_;
    }
    T** operator&() {
        Reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

  private:
    T* ptr_ = nullptr;
};

/**
 * @class MFVideoEncoderH264
 * @brief A Windows Media Foundation (MSDK/MF) H.264 hardware video encoder.
 *
 * @details
 * This class implements the @c webrtc::VideoEncoder interface using Windows Media Foundation
 * APIs. It negotiates and instantiates the hardware-accelerated H.264 Encoder MFT (Media Foundation
 * Transform), converts incoming WebRTC video frames (I420) to NV12 format, and feeds them to the
 * GPU for compression. The resulting H.264 bitstream is wrapped in @c webrtc::EncodedImage
 * and passed back via the registered callback.
 *
 * The Windows Media Foundation MFT operates synchronously. Calling @c ProcessInput() and
 * @c ProcessOutput() blocks WebRTC's dedicated encoder thread until compression is complete.
 * This synchronous design guarantees that no GPU callbacks are in flight when the encoder
 * is released, significantly simplifying the class's lifecycle management.
 */
class MFVideoEncoderH264 : public webrtc::VideoEncoder {
  public:
    static constexpr uint32_t kDefaultTargetBitrateBps = 3000000;  // 3 Mbps
    static constexpr uint32_t kDefaultTargetFps = 60;
    static constexpr size_t kMaxPendingHeaderBytes = 64 * 1024;

    explicit MFVideoEncoderH264(webrtc::SdpVideoFormat format = webrtc::SdpVideoFormat("H264"));
    ~MFVideoEncoderH264() override;

    int32_t InitEncode(const webrtc::VideoCodec* codec_settings, int32_t number_of_cores,
                       size_t max_payload_size) override;
    int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                       const webrtc::VideoEncoder::Settings& settings) override;

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;

    int32_t Release() override;

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* frame_types) override;

    void SetRates(const RateControlParameters& parameters) override;

    void OnPacketLossRateUpdate(float packet_loss_rate) override;

    void OnRttUpdate(int64_t rtt_ms) override;

    void OnLossNotification(const LossNotification& loss_notification) override;

    EncoderInfo GetEncoderInfo() const override;

  private:
    friend class MFVideoEncoderH264Test;

    int32_t ReleaseUnlocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT InitializeMFT();
    HRESULT ConfigureMFT() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT ConfigureInputOutputTypes(int width, int height, int framerate)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT SetBitrateAndRateControl(uint32_t bitrate_bps)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT AllocateInputBuffers(int width, int height)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT AllocateOutputBuffers() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    void RequestKeyFrame() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT PollNextAsyncEvent(int max_wait_attempts, MediaEventType* out_event_type)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    bool WaitForAsyncOutputEvent(bool drained_any) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT HandleOutputStreamChange(bool* mft_provides_samples)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    void CachePendingHeaderData(const uint8_t* data, DWORD length)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    webrtc::scoped_refptr<webrtc::EncodedImageBuffer> BuildEncodedBuffer(const uint8_t* data,
                                                                         DWORD length,
                                                                         bool is_keyframe,
                                                                         bool has_sps)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT ProcessInputFrame(const webrtc::VideoFrame& frame, bool force_keyframe)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
    HRESULT DrainEncodedFrames(uint32_t rtp_timestamp, int64_t capture_time_ms,
                               webrtc::EncodedImageCallback* callback)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

    webrtc::SdpVideoFormat format_;

    absl::Mutex mutex_;
    bool initialized_ ABSL_GUARDED_BY(mutex_);
    webrtc::EncodedImageCallback* callback_ ABSL_GUARDED_BY(mutex_);
    webrtc::VideoCodec codec_settings_ ABSL_GUARDED_BY(mutex_);

    // Media Foundation COM Objects
    const android::base::ScopedComInitializer com_initializer_;
    ComPtr<IMFTransform> encoder_mft_;
    ComPtr<IMFMediaEventGenerator> event_generator_;
    ComPtr<IMFMediaType> input_type_;
    ComPtr<IMFMediaType> output_type_;

    // MFT Stream IDs and info
    DWORD input_stream_id_ = 0;
    DWORD output_stream_id_ = 0;
    MFT_INPUT_STREAM_INFO input_stream_info_ = {};
    MFT_OUTPUT_STREAM_INFO output_stream_info_ = {};

    // Allocator for input buffers
    ComPtr<IMFMediaBuffer> input_buffer_;
    ComPtr<IMFSample> input_sample_;

    // Pre-allocated output buffers
    ComPtr<IMFMediaBuffer> output_buffer_;
    ComPtr<IMFSample> output_sample_;

    bool is_async_mft_ = false;
    uint32_t pending_input_count_ ABSL_GUARDED_BY(mutex_) = 0;
    uint32_t pending_output_count_ ABSL_GUARDED_BY(mutex_) = 0;
    LONGLONG last_sample_time_hns_ ABSL_GUARDED_BY(mutex_) = -1;
    std::vector<uint8_t> pending_header_data_ ABSL_GUARDED_BY(mutex_);

    uint32_t target_bitrate_bps_ ABSL_GUARDED_BY(mutex_);
    uint32_t target_fps_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace goldfish::videobridge
