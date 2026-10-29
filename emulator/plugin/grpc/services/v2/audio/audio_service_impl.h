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
#include <type_traits>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/grpc_event_stream_support.h"
#include "android/emulation/control/simple_async_grpc.h"
#include "audio_service.grpc.pb.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/qemu_audio_capture.h"
#include "goldfish/eventing/event_sources.h"
#include "goldfish/system/steady_clock.h"

extern "C" {
typedef struct AudioBackend AudioBackend;
}

namespace goldfish::grpc::v2 {

using AudioServiceBase =
        ::android::emulation::v2::AudioService::WithCallbackMethod_StreamAudioState<
                ::android::emulation::v2::AudioService::WithCallbackMethod_StreamMicrophoneAudio<
                        ::android::emulation::v2::AudioService::
                                WithCallbackMethod_StreamSpeakerAudio<
                                        ::android::emulation::v2::AudioService::Service>>>;

/**
 * Implements the AEMU v2 `AudioService` gRPC interface (`android.emulation.v2.AudioService`).
 *
 * Bridges host gRPC clients to QEMU's speaker capture mixer and virtual microphone mix bus:
 * - `GetAudioState`, `StreamAudioState`, and `UpdateAudioState` observe and mutate the virtual
 *   microphone mix bus (`goldfish::audio::AudioInputMux`), covering master virtual microphone mute
 *   (paced PCM silence across all inputs), built-in `"host"` physical microphone detach, guest
 *   `AudioRecord` capture activity, and per-channel mute/gain controls.
 * - `StreamSpeakerAudio` registers a pull-free capture voice via
 *   `goldfish::audio::QemuAudioCapture` at the requested sample rate and channel count
 *   (defaulting to 48000 Hz, 2 channels, `SAMPLE_FORMAT_PCM16_BIT`), converts samples to
 *   `SAMPLE_FORMAT_F32_BIT` when requested, and stamps each `AudioChunk` with a
 *   `CLOCK_MONOTONIC` nanosecond timestamp.
 * - `InjectAudioClip` and `StreamMicrophoneAudio` feed host PCM audio into QEMU's
 *   `GoldfishAudioBackend` via `goldfish::audio::QemuAudioInject` by default (or delegate to
 *   `Options::microphone_sink` when a custom sink is supplied). Up to
 *   `max_concurrent_microphone_streams` injections are mixed; a duplicate `source_id` fails with
 *   `ALREADY_EXISTS`, and one past the limit with `RESOURCE_EXHAUSTED`.
 *
 * ### Streaming & Backpressure:
 * - `StreamAudioState` sends an initial `AudioState` snapshot immediately upon connection and
 *   coalesces rapid intermediate state updates via `StateStreamWriter`.
 * - Speaker capture queues at most `kMaxPendingSpeakerChunks` unread chunks per client so slow
 *   readers cannot cause unbounded memory growth or stall the QEMU audio thread.
 * - Live microphone streaming buffers at most `kMaxBufferedMicrophoneSeconds` of whole PCM frames
 *   in a `goldfish::SocketBuffer`, dropping the oldest unread frames on overflow.
 *
 * ### Thread Safety:
 * - Thread-safe across concurrent gRPC calls and QEMU audio capture/pull/state callbacks.
 */
class AudioServiceImpl final : public AudioServiceBase {
  public:
    struct AudioStateEvent {};
    using AudioStateEventSource = ::android::base::eventing::CallbackEventSource<AudioStateEvent>;

    using AudioChunk = ::android::emulation::v2::AudioChunk;
    using AudioState = ::android::emulation::v2::AudioState;
    using GetAudioStateRequest = ::android::emulation::v2::GetAudioStateRequest;
    using InjectAudioClipResponse = ::android::emulation::v2::InjectAudioClipResponse;
    using MicrophoneInput = ::android::emulation::v2::MicrophoneInput;
    using MicrophoneInputUpdate = ::android::emulation::v2::MicrophoneInputUpdate;
    using SampleFormat = ::android::emulation::v2::SampleFormat;
    using StreamAudioStateRequest = ::android::emulation::v2::StreamAudioStateRequest;
    using StreamMicrophoneAudioResponse = ::android::emulation::v2::StreamMicrophoneAudioResponse;
    using StreamSpeakerAudioRequest = ::android::emulation::v2::StreamSpeakerAudioRequest;
    using UpdateAudioStateRequest = ::android::emulation::v2::UpdateAudioStateRequest;

    using MicrophoneSink = std::function<absl::Status(const AudioChunk&)>;
    using SleepFn = std::function<void(absl::Duration)>;

    static constexpr int32_t kDefaultSpeakerSampleRateHz = 48000;
    static constexpr int32_t kDefaultSpeakerChannelCount = 2;
    static constexpr SampleFormat kDefaultSpeakerFormat = SampleFormat::SAMPLE_FORMAT_PCM16_BIT;
    static constexpr size_t kMaxPendingStateUpdates = 1;
    static constexpr size_t kMaxPendingSpeakerChunks = 50;
    static constexpr size_t kMaxBufferedMicrophoneSeconds = 5;

    static constexpr std::array<int32_t, 8> kSupportedSampleRatesHz = {8000,  11025, 16000, 22050,
                                                                       24000, 32000, 44100, 48000};
    static constexpr std::array<int32_t, 2> kSupportedChannelCounts = {1, 2};

    template <typename Reactor = ::grpc::ServerWriteReactor<AudioState>>
    using AudioStateWriter =
            ::android::emulation::control::StateStreamWriter<AudioState, AudioStateEvent,
                                                             AudioStateEventSource,
                                                             kMaxPendingStateUpdates, 0, Reactor>;

    struct Options {
        bool has_audio_output{true};
        bool has_audio_input{false};
        bool read_only{false};
        AudioBackend* audio_backend{nullptr};
        MicrophoneSink microphone_sink{nullptr};
        std::reference_wrapper<const ::goldfish::system::SteadyClock> clock{
                ::goldfish::system::SteadyClock::GetRealClock()};
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
                           AudioBackend* audio_backend,
                           const ::goldfish::system::SteadyClock& clock)
                : sample_rate_hz_(sample_rate_hz)
                , channel_count_(channel_count)
                , format_(format)
                , clock_(clock) {
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
            chunk.set_timestamp_nanos(absl::ToInt64Nanoseconds(clock_.Now()));

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
        const ::goldfish::system::SteadyClock& clock_;
        std::unique_ptr<::goldfish::audio::QemuAudioCapture> capture_;
    };

    explicit AudioServiceImpl(Options options);
    AudioServiceImpl() : AudioServiceImpl(Options{}) {}
    ~AudioServiceImpl() override;

    ::grpc::Status GetAudioState(::grpc::ServerContext* context,
                                 const GetAudioStateRequest* request, AudioState* reply) override;

    /**
     * Constructs a `StateStreamWriter` for streaming `AudioState` snapshots (templated for tests).
     */
    template <typename Reactor = ::grpc::ServerWriteReactor<AudioState>>
    auto* CreateAudioStateWriter() {
        (void)BindActiveMux();
        return new AudioStateWriter<Reactor>(
                &state_events_, [this](AudioState* state) { PopulateAudioState(state); });
    }

    ::grpc::ServerWriteReactor<AudioState>* StreamAudioState(
            ::grpc::CallbackServerContext* context,
            const StreamAudioStateRequest* request) override;

    ::grpc::Status UpdateAudioState(::grpc::ServerContext* context,
                                    const UpdateAudioStateRequest* request,
                                    AudioState* reply) override;

    ::grpc::Status InjectAudioClip(::grpc::ServerContext* context, const AudioChunk* request,
                                   InjectAudioClipResponse* reply) override;

    ::grpc::ServerReadReactor<AudioChunk>* StreamMicrophoneAudio(
            ::grpc::CallbackServerContext* context, StreamMicrophoneAudioResponse* reply) override;

    /**
     * Constructs a client-streaming microphone reader reactor (templated for hermetic testing).
     */
    template <typename Reactor = ::grpc::ServerReadReactor<AudioChunk>>
    Reactor* CreateMicrophoneAudioReader(absl::string_view client_source_id = "") {
        MicrophoneStreamStart started = StartMicrophoneStream(client_source_id);
        if (!started.status.ok()) {
            return new ErrorMicrophoneReader<Reactor>(std::move(started.status));
        }
        auto* reader = new MicrophoneAudioReader<Reactor>(std::move(started.on_chunk),
                                                          std::move(started.stop_stream));
        if constexpr (!std::is_base_of_v<::grpc::internal::ServerReactor, Reactor>) {
            reader->StartRead();
        }
        return reader;
    }

    /**
     * Constructs a SpeakerAudioWriter for streaming AudioChunk messages.
     */
    template <typename Reactor = ::grpc::ServerWriteReactor<AudioChunk>>
    auto* CreateSpeakerAudioWriter(int32_t sample_rate_hz = kDefaultSpeakerSampleRateHz,
                                   int32_t channel_count = kDefaultSpeakerChannelCount,
                                   SampleFormat format = kDefaultSpeakerFormat) {
        return new SpeakerAudioWriter<Reactor>(sample_rate_hz, channel_count, format,
                                               options_.audio_backend, clock_);
    }

    ::grpc::ServerWriteReactor<AudioChunk>* StreamSpeakerAudio(
            ::grpc::CallbackServerContext* context,
            const StreamSpeakerAudioRequest* request) override;

    static bool IsSupportedSampleRate(int32_t sample_rate_hz);
    static bool IsSupportedChannelCount(int32_t channel_count);
    static bool IsSupportedSampleFormat(SampleFormat format);

  private:
    struct SinkMuxState;

    using ReadMicrophoneChunkFn = absl::AnyInvocable<::grpc::Status(const AudioChunk*)>;
    using StopMicrophoneStreamFn = std::function<void()>;

    struct MicrophoneStreamStart {
        ::grpc::Status status;
        ReadMicrophoneChunkFn on_chunk;
        StopMicrophoneStreamFn stop_stream;
    };

    template <typename Reactor = ::grpc::ServerReadReactor<AudioChunk>>
    class MicrophoneAudioReader final : public ::WithSimpleReader<Reactor> {
      public:
        MicrophoneAudioReader(ReadMicrophoneChunkFn on_chunk, StopMicrophoneStreamFn stop_stream)
                : on_chunk_(std::move(on_chunk)), stop_stream_(std::move(stop_stream)) {}

        bool Read(const AudioChunk* chunk) override {
            ::grpc::Status status = on_chunk_(chunk);
            if (!status.ok()) {
                StopStream();
                Reactor::Finish(status);
                return false;
            }
            return true;
        }

        void OnReadDone(bool ok) override {
            if (!ok) {
                // Release the microphone input before Finish(OK) unblocks the client.
                StopStream();
                if constexpr (!std::is_base_of_v<::grpc::internal::ServerReactor, Reactor>) {
                    Reactor::Finish(::grpc::Status::OK);
                }
            }
            ::WithSimpleReader<Reactor>::OnReadDone(ok);
        }

        void OnCancel() override { StopStream(); }

        void OnDone() override {
            StopStream();
            delete this;
        }

      private:
        void StopStream() {
            if (stop_stream_) {
                stop_stream_();
            }
        }

        ReadMicrophoneChunkFn on_chunk_;
        StopMicrophoneStreamFn stop_stream_;
    };

    template <typename Reactor = ::grpc::ServerReadReactor<AudioChunk>>
    class ErrorMicrophoneReader final : public Reactor {
      public:
        explicit ErrorMicrophoneReader(::grpc::Status status) { this->Finish(std::move(status)); }
        void OnDone() override { delete this; }
    };

    struct ResolvedSpeakerConfig {
        int32_t sample_rate_hz;
        int32_t channel_count;
        SampleFormat format;
    };

    bool HasAudioSubsystem() const { return options_.has_audio_output || options_.has_audio_input; }
    ::grpc::Status CheckMicrophoneWritable(absl::string_view operation) const;
    std::shared_ptr<::goldfish::audio::AudioInputMux> BindActiveMux() const
            ABSL_LOCKS_EXCLUDED(bind_serialize_mutex_, state_binding_mutex_);
    std::shared_ptr<::goldfish::audio::AudioInputMux> GetBoundMux() const
            ABSL_LOCKS_EXCLUDED(state_binding_mutex_);
    void PopulateAudioState(AudioState* out) const ABSL_LOCKS_EXCLUDED(state_binding_mutex_);
    void OnMuxStateChanged() const;

    ::grpc::Status InjectClipViaSink(::grpc::ServerContext* context, const AudioChunk& request,
                                     absl::string_view client_source_id,
                                     InjectAudioClipResponse* reply) const;
    ::grpc::Status InjectClipViaQemuAudioInject(::grpc::ServerContext* context,
                                                const AudioChunk& request,
                                                absl::string_view client_source_id,
                                                InjectAudioClipResponse* reply) const;
    MicrophoneStreamStart StartSinkMicrophoneStream(absl::string_view client_source_id) const;
    MicrophoneStreamStart StartMicrophoneStream(absl::string_view client_source_id) const;
    static absl::Status ValidateMicrophoneChunk(const AudioChunk& chunk,
                                                absl::string_view operation);
    static absl::StatusOr<ResolvedSpeakerConfig> ResolveSpeakerRequest(
            const StreamSpeakerAudioRequest& request);
    bool SleepUntil(const ::grpc::ServerContext* context, absl::Duration target) const;

    const Options options_;
    const ::goldfish::system::SteadyClock& clock_;
    std::shared_ptr<SinkMuxState> sink_mux_state_;
    mutable AudioStateEventSource state_events_;

    mutable absl::Mutex bind_serialize_mutex_;
    mutable absl::Mutex state_binding_mutex_ ABSL_ACQUIRED_AFTER(bind_serialize_mutex_);
    mutable std::shared_ptr<::goldfish::audio::AudioInputMux> observed_mux_
            ABSL_GUARDED_BY(state_binding_mutex_);
    mutable ::goldfish::audio::AudioInputMux::StateObserverId observer_id_
            ABSL_GUARDED_BY(state_binding_mutex_) = 0;
};

}  // namespace goldfish::grpc::v2
