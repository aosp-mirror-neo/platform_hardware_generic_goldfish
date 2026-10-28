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

#include <grpcpp/grpcpp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/base/clock.h"
#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/simple_async_grpc.h"
#include "audio_service.grpc.pb.h"
#include "goldfish/audio/qemu_audio_capture.h"

extern "C" {
typedef struct AudioBackend AudioBackend;
}

namespace goldfish::grpc::v2 {

using AudioServiceBase =
        ::android::emulation::v2::AudioService::WithCallbackMethod_StreamMicrophoneAudio<
                ::android::emulation::v2::AudioService::WithCallbackMethod_StreamSpeakerAudio<
                        ::android::emulation::v2::AudioService::Service>>;

/**
 * @brief Implements the AEMU v2 AudioService gRPC interface.
 *
 * Streams uncompressed PCM audio from the guest speaker mixer via
 * `goldfish::audio::QemuAudioCapture` (`StreamSpeakerAudio`) and supports
 * host-to-guest PCM microphone injection (`InjectAudioClip` and
 * `StreamMicrophoneAudio`) when a virtual microphone sink is configured.
 *
 * ### Streaming & Backpressure:
 * - `StreamSpeakerAudio` registers a capture voice on QEMU's `AudioBackend` at
 *   the requested sample rate and channel count (defaulting to the native mixer
 *   settings of 48000 Hz, 2 channels, `SAMPLE_FORMAT_PCM16_BIT`), converts
 *   samples to `SAMPLE_FORMAT_F32_BIT` when requested, and stamps each
 *   `AudioChunk` with a `CLOCK_MONOTONIC` nanosecond timestamp.
 * - Under client backpressure, pending chunks are bounded by
 *   `kMaxPendingSpeakerChunks` so slow clients cannot cause unbounded memory
 *   growth or stall the QEMU audio thread.
 *
 * ### Thread Safety:
 * - Thread-safe across concurrent gRPC calls and QEMU audio capture callbacks.
 */
class AudioServiceImpl final : public AudioServiceBase {
  public:
    using AudioChunk = ::android::emulation::v2::AudioChunk;
    using InjectAudioClipResponse = ::android::emulation::v2::InjectAudioClipResponse;
    using SampleFormat = ::android::emulation::v2::SampleFormat;
    using StreamMicrophoneAudioResponse = ::android::emulation::v2::StreamMicrophoneAudioResponse;
    using StreamSpeakerAudioRequest = ::android::emulation::v2::StreamSpeakerAudioRequest;

    using MicrophoneSink = std::function<absl::Status(const AudioChunk&)>;
    using NowFn = std::function<absl::Time()>;
    using SleepFn = std::function<void(absl::Duration)>;

    static constexpr int32_t kDefaultSpeakerSampleRateHz = 48000;
    static constexpr int32_t kDefaultSpeakerChannelCount = 2;
    static constexpr SampleFormat kDefaultSpeakerFormat = SampleFormat::SAMPLE_FORMAT_PCM16_BIT;
    static constexpr size_t kMaxPendingSpeakerChunks = 50;

    static constexpr std::array<int32_t, 8> kSupportedSampleRatesHz = {8000,  11025, 16000, 22050,
                                                                       24000, 32000, 44100, 48000};
    static constexpr std::array<int32_t, 2> kSupportedChannelCounts = {1, 2};

    struct Options {
        bool has_audio_output{true};
        bool has_audio_input{false};
        bool read_only{false};
        AudioBackend* audio_backend{nullptr};
        MicrophoneSink microphone_sink{nullptr};
        NowFn now_fn{[]() { return ::android::base::IClock::RealtimeNow(); }};
        SleepFn sleep_fn{[](absl::Duration d) { absl::SleepFor(d); }};
    };

    /**
     * @brief Asynchronous gRPC writer reactor that captures guest speaker audio from QEMU.
     */
    template <typename Reactor = ::grpc::ServerWriteReactor<AudioChunk>>
    class SpeakerAudioWriter final
            : public ::WithSimpleQueueWriter<Reactor, kMaxPendingSpeakerChunks> {
      public:
        SpeakerAudioWriter(int32_t sample_rate_hz, int32_t channel_count, SampleFormat format,
                           AudioBackend* audio_backend, NowFn now_fn)
                : sample_rate_hz_(sample_rate_hz)
                , channel_count_(channel_count)
                , format_(format)
                , now_fn_(std::move(now_fn)) {
            capture_ = std::make_unique<::goldfish::audio::QemuAudioCapture>(
                    [this](const int16_t* pcm_data, size_t num_samples) {
                        OnAudioData(pcm_data, num_samples);
                    },
                    static_cast<uint32_t>(sample_rate_hz_), static_cast<uint32_t>(channel_count_),
                    audio_backend);
            if (absl::Status status = capture_->Start(); !status.ok()) {
                const ::grpc::Status grpc_status =
                        ::android::emulation::control::AbslStatusToGrpcStatus(status);
                this->Finish(::grpc::Status(
                        grpc_status.error_code(),
                        absl::StrFormat("Failed to start speaker audio capture at %d Hz (%d "
                                        "channels): %s Ensure the emulator was started with audio "
                                        "enabled (without -no-audio).",
                                        sample_rate_hz_, channel_count_, status.message())));
            }
        }

        ~SpeakerAudioWriter() override {
            if (capture_) {
                capture_->Stop();
            }
        }

        void OnCancel() override {
            if (capture_) {
                capture_->Stop();
            }
            if (this->SetCancelled()) {
                this->Finish(::grpc::Status::CANCELLED);
            }
        }

        void OnDone() override {
            if (capture_) {
                capture_->Stop();
            }
            delete this;
        }

      private:
        static constexpr float kPcm16ToFloatScale = 1.0F / 32768.0F;

        void OnAudioData(const int16_t* pcm_data, size_t num_samples) {
            if (pcm_data == nullptr || num_samples == 0 || this->IsFinished() ||
                this->IsCancelled()) {
                return;
            }

            AudioChunk chunk;
            chunk.set_sample_rate_hz(sample_rate_hz_);
            chunk.set_channel_count(channel_count_);
            chunk.set_format(format_);
            chunk.set_timestamp_nanos(absl::ToUnixNanos(now_fn_()));

            if (format_ == SampleFormat::SAMPLE_FORMAT_F32_BIT) {
                std::string* out = chunk.mutable_pcm_data();
                out->resize(num_samples * sizeof(float));
                char* dst = out->data();
                for (size_t i = 0; i < num_samples; ++i) {
                    const float sample = static_cast<float>(pcm_data[i]) * kPcm16ToFloatScale;
                    std::memcpy(dst + (i * sizeof(float)), &sample, sizeof(float));
                }
            } else {
                chunk.set_pcm_data(pcm_data, num_samples * sizeof(int16_t));
            }

            this->Write(std::move(chunk));
        }

        const int32_t sample_rate_hz_;
        const int32_t channel_count_;
        const SampleFormat format_;
        const NowFn now_fn_;
        std::unique_ptr<::goldfish::audio::QemuAudioCapture> capture_;
    };

    explicit AudioServiceImpl(Options options);
    AudioServiceImpl() : AudioServiceImpl(Options{}) {}
    ~AudioServiceImpl() override = default;

    ::grpc::Status InjectAudioClip(::grpc::ServerContext* context, const AudioChunk* request,
                                   InjectAudioClipResponse* reply) override;

    ::grpc::ServerReadReactor<AudioChunk>* StreamMicrophoneAudio(
            ::grpc::CallbackServerContext* context, StreamMicrophoneAudioResponse* reply) override;

    /**
     * @brief Constructs a SpeakerAudioWriter for streaming AudioChunk messages.
     */
    template <typename Reactor = ::grpc::ServerWriteReactor<AudioChunk>>
    auto* CreateSpeakerAudioWriter(int32_t sample_rate_hz = kDefaultSpeakerSampleRateHz,
                                   int32_t channel_count = kDefaultSpeakerChannelCount,
                                   SampleFormat format = kDefaultSpeakerFormat) {
        return new SpeakerAudioWriter<Reactor>(sample_rate_hz, channel_count, format,
                                               options_.audio_backend, options_.now_fn);
    }

    ::grpc::ServerWriteReactor<AudioChunk>* StreamSpeakerAudio(
            ::grpc::CallbackServerContext* context,
            const StreamSpeakerAudioRequest* request) override;

    static bool IsSupportedSampleRate(int32_t sample_rate_hz);
    static bool IsSupportedChannelCount(int32_t channel_count);
    static bool IsSupportedSampleFormat(SampleFormat format);

  private:
    struct ResolvedSpeakerConfig {
        int32_t sample_rate_hz;
        int32_t channel_count;
        SampleFormat format;
    };

    ::grpc::Status CheckMicrophoneWritable(absl::string_view operation) const;
    static absl::Status ValidateMicrophoneChunk(const AudioChunk& chunk,
                                                absl::string_view operation);
    static absl::StatusOr<ResolvedSpeakerConfig> ResolveSpeakerRequest(
            const StreamSpeakerAudioRequest& request);
    bool SleepUntil(const ::grpc::ServerContext* context, absl::Time target) const;

    const Options options_;
};

}  // namespace goldfish::grpc::v2
