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

#include "audio/audio_service_impl.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/simple_async_grpc.h"

namespace goldfish::grpc::v2 {

using ::android::emulation::control::AbslStatusToGrpcStatus;
using ::android::emulation::v2::AudioChunk;
using ::android::emulation::v2::InjectAudioClipResponse;
using ::android::emulation::v2::SampleFormat;
using ::android::emulation::v2::StreamMicrophoneAudioResponse;
using ::android::emulation::v2::StreamSpeakerAudioRequest;
using ::grpc::CallbackServerContext;
using ::grpc::ServerContext;
using ::grpc::ServerReadReactor;
using ::grpc::ServerWriteReactor;
using ::grpc::Status;
using ::grpc::StatusCode;

namespace {

constexpr absl::Duration kClipSliceDuration = absl::Milliseconds(20);
constexpr absl::Duration kCancellationPollInterval = absl::Milliseconds(50);
constexpr uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr uint64_t kMillisPerSecond = 1'000ULL;

class ErrorServerReader final : public ServerReadReactor<AudioChunk> {
  public:
    explicit ErrorServerReader(Status status) { this->Finish(std::move(status)); }
    void OnDone() override { delete this; }
};

Status SpeakerUnsupportedStatus() {
    return Status(StatusCode::FAILED_PRECONDITION,
                  "Failed to stream speaker audio: speaker audio output is disabled on this "
                  "device (hw.audioOutput=no, AudioCapabilities.speaker_profile is unset). Check "
                  "DeviceCapabilities.audio.speaker_profile before calling StreamSpeakerAudio.");
}

size_t BytesPerSample(SampleFormat format) {
    return (format == SampleFormat::SAMPLE_FORMAT_F32_BIT) ? sizeof(float) : sizeof(int16_t);
}

absl::Status ValidateFloatSamplesFinite(absl::string_view pcm_data, absl::string_view operation) {
    const size_t num_samples = pcm_data.size() / sizeof(float);
    for (size_t i = 0; i < num_samples; ++i) {
        float sample = 0.0F;
        std::memcpy(&sample, pcm_data.data() + (i * sizeof(float)), sizeof(float));
        if (!std::isfinite(sample)) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to validate audio chunk in %s: non-finite float sample (%g) at "
                    "sample index %zu. Pass finite IEEE 754 float samples in [-1.0, 1.0].",
                    operation, sample, i));
        }
    }
    return absl::OkStatus();
}

}  // namespace

AudioServiceImpl::AudioServiceImpl(Options options) : options_(std::move(options)) {}

bool AudioServiceImpl::IsSupportedSampleRate(int32_t sample_rate_hz) {
    return std::ranges::find(kSupportedSampleRatesHz, sample_rate_hz) !=
           kSupportedSampleRatesHz.end();
}

bool AudioServiceImpl::IsSupportedChannelCount(int32_t channel_count) {
    return std::ranges::find(kSupportedChannelCounts, channel_count) !=
           kSupportedChannelCounts.end();
}

bool AudioServiceImpl::IsSupportedSampleFormat(SampleFormat format) {
    return format == SampleFormat::SAMPLE_FORMAT_PCM16_BIT ||
           format == SampleFormat::SAMPLE_FORMAT_F32_BIT;
}

Status AudioServiceImpl::CheckMicrophoneWritable(absl::string_view operation) const {
    if (options_.read_only) {
        return Status(StatusCode::PERMISSION_DENIED,
                      absl::StrCat("Failed to execute ", operation,
                                   ": microphone audio injection is disabled on this device "
                                   "(ACCESS_MODE_READ_ONLY). Use StreamSpeakerAudio for read-only "
                                   "speaker observation."));
    }
    if (!options_.has_audio_input) {
        return Status(
                StatusCode::FAILED_PRECONDITION,
                absl::StrCat("Failed to execute ", operation,
                             ": audio input hardware is disabled in the AVD configuration "
                             "(hw.audioInput=no, AudioCapabilities.microphone_profile is unset). "
                             "Check DeviceCapabilities.audio.microphone_profile before calling ",
                             operation, "."));
    }
    if (!options_.microphone_sink) {
        return Status(StatusCode::FAILED_PRECONDITION,
                      absl::StrCat("Failed to execute ", operation,
                                   ": this device has no injectable microphone endpoint "
                                   "(AudioCapabilities.microphone_profile is unset). Check "
                                   "DeviceCapabilities.audio.microphone_profile before calling ",
                                   operation, "."));
    }
    return Status::OK;
}

absl::Status AudioServiceImpl::ValidateMicrophoneChunk(const AudioChunk& chunk,
                                                       absl::string_view operation) {
    if (!IsSupportedSampleRate(chunk.sample_rate_hz())) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate audio chunk in %s: unsupported sample_rate_hz=%d. Supported "
                "sample rates are 8000, 11025, 16000, 22050, 24000, 32000, 44100, or 48000 Hz.",
                operation, chunk.sample_rate_hz()));
    }
    if (!IsSupportedChannelCount(chunk.channel_count())) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate audio chunk in %s: unsupported channel_count=%d. Pass 1 "
                "(Mono) or 2 (Stereo).",
                operation, chunk.channel_count()));
    }
    if (!IsSupportedSampleFormat(chunk.format())) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate audio chunk in %s: invalid or unspecified format=%d. Pass "
                "SAMPLE_FORMAT_PCM16_BIT (1) or SAMPLE_FORMAT_F32_BIT (2).",
                operation, static_cast<int>(chunk.format())));
    }
    if (chunk.pcm_data().empty()) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate audio chunk in %s: pcm_data is empty (0 bytes). Provide at "
                "least one complete audio frame.",
                operation));
    }

    const size_t bytes_per_frame =
            static_cast<size_t>(chunk.channel_count()) * BytesPerSample(chunk.format());
    if (chunk.pcm_data().size() % bytes_per_frame != 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate audio chunk in %s: pcm_data size (%zu bytes) is not a "
                "multiple of the frame size (%zu bytes for %d channel(s)). Pass whole interleaved "
                "audio frames.",
                operation, chunk.pcm_data().size(), bytes_per_frame, chunk.channel_count()));
    }

    if (chunk.format() == SampleFormat::SAMPLE_FORMAT_F32_BIT) {
        return ValidateFloatSamplesFinite(chunk.pcm_data(), operation);
    }
    return absl::OkStatus();
}

absl::StatusOr<AudioServiceImpl::ResolvedSpeakerConfig> AudioServiceImpl::ResolveSpeakerRequest(
        const StreamSpeakerAudioRequest& request) {
    int32_t sample_rate_hz = kDefaultSpeakerSampleRateHz;
    if (request.has_sample_rate_hz()) {
        if (!IsSupportedSampleRate(request.sample_rate_hz())) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to start StreamSpeakerAudio: unsupported sample_rate_hz=%d. Pass one "
                    "of the rates in AudioCapabilities.speaker_profile (8000, 11025, 16000, "
                    "22050, 24000, 32000, 44100, or 48000 Hz), or omit sample_rate_hz to use the "
                    "native rate (%d Hz).",
                    request.sample_rate_hz(), kDefaultSpeakerSampleRateHz));
        }
        sample_rate_hz = request.sample_rate_hz();
    }

    int32_t channel_count = kDefaultSpeakerChannelCount;
    if (request.has_channel_count()) {
        if (!IsSupportedChannelCount(request.channel_count())) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to start StreamSpeakerAudio: unsupported channel_count=%d. Pass 1 "
                    "(Mono) or 2 (Stereo), or omit channel_count to use the native layout (%d).",
                    request.channel_count(), kDefaultSpeakerChannelCount));
        }
        channel_count = request.channel_count();
    }

    SampleFormat format = kDefaultSpeakerFormat;
    if (request.has_format() && request.format() != SampleFormat::SAMPLE_FORMAT_UNSPECIFIED) {
        if (!IsSupportedSampleFormat(request.format())) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to start StreamSpeakerAudio: unsupported format=%d. Pass "
                    "SAMPLE_FORMAT_PCM16_BIT (1) or SAMPLE_FORMAT_F32_BIT (2), or omit format to "
                    "use the native format.",
                    static_cast<int>(request.format())));
        }
        format = request.format();
    }

    return ResolvedSpeakerConfig{
        .sample_rate_hz = sample_rate_hz,
        .channel_count = channel_count,
        .format = format,
    };
}

bool AudioServiceImpl::SleepUntil(const ServerContext* context, absl::Time target) const {
    while (true) {
        if (context != nullptr && context->IsCancelled()) {
            return false;
        }
        const absl::Time now = options_.now_fn();
        if (now >= target) {
            return true;
        }
        const absl::Duration remaining = target - now;
        options_.sleep_fn(std::min(remaining, kCancellationPollInterval));
    }
}

Status AudioServiceImpl::InjectAudioClip(ServerContext* context, const AudioChunk* request,
                                         InjectAudioClipResponse* reply) {
    if (Status writable = CheckMicrophoneWritable("InjectAudioClip"); !writable.ok()) {
        return writable;
    }
    if (absl::Status valid = ValidateMicrophoneChunk(*request, "InjectAudioClip"); !valid.ok()) {
        return AbslStatusToGrpcStatus(valid);
    }

    const size_t bytes_per_frame =
            static_cast<size_t>(request->channel_count()) * BytesPerSample(request->format());
    const size_t total_frames = request->pcm_data().size() / bytes_per_frame;
    const auto sample_rate = static_cast<uint64_t>(request->sample_rate_hz());
    const size_t frames_per_slice = std::max<size_t>(
            1, static_cast<size_t>((sample_rate * absl::ToInt64Milliseconds(kClipSliceDuration)) /
                                   kMillisPerSecond));

    const absl::Time start_time = options_.now_fn();
    for (size_t offset_frames = 0; offset_frames < total_frames;
         offset_frames += frames_per_slice) {
        const absl::Time slice_target =
                start_time + absl::Nanoseconds((offset_frames * kNanosPerSecond) / sample_rate);
        if (!SleepUntil(context, slice_target)) {
            return Status(StatusCode::CANCELLED,
                          "Failed to complete InjectAudioClip: RPC was cancelled by the client "
                          "during audio clip playback.");
        }

        const size_t slice_frames = std::min(frames_per_slice, total_frames - offset_frames);
        AudioChunk slice;
        slice.set_sample_rate_hz(request->sample_rate_hz());
        slice.set_channel_count(request->channel_count());
        slice.set_format(request->format());
        slice.set_timestamp_nanos(absl::ToUnixNanos(options_.now_fn()));
        slice.set_pcm_data(request->pcm_data().data() + (offset_frames * bytes_per_frame),
                           slice_frames * bytes_per_frame);

        if (absl::Status sink_status = options_.microphone_sink(slice); !sink_status.ok()) {
            return AbslStatusToGrpcStatus(sink_status);
        }
    }

    const absl::Time end_target =
            start_time + absl::Nanoseconds((total_frames * kNanosPerSecond) / sample_rate);
    if (!SleepUntil(context, end_target)) {
        return Status(StatusCode::CANCELLED,
                      "Failed to complete InjectAudioClip: RPC was cancelled by the client "
                      "during audio clip playback.");
    }

    reply->set_duration_ms((total_frames * kMillisPerSecond) / sample_rate);
    return Status::OK;
}

ServerReadReactor<AudioChunk>* AudioServiceImpl::StreamMicrophoneAudio(
        CallbackServerContext* /*context*/, StreamMicrophoneAudioResponse* /*reply*/) {
    if (Status writable = CheckMicrophoneWritable("StreamMicrophoneAudio"); !writable.ok()) {
        return new ErrorServerReader(std::move(writable));
    }

    return new ::SimpleServerLambdaReader<AudioChunk>(
            [this, first_client_timestamp_nanos = std::optional<int64_t>{},
             first_host_time = absl::InfinitePast()](const AudioChunk* chunk) mutable -> Status {
                if (absl::Status valid = ValidateMicrophoneChunk(*chunk, "StreamMicrophoneAudio");
                    !valid.ok()) {
                    return AbslStatusToGrpcStatus(valid);
                }

                const absl::Time now = options_.now_fn();
                int64_t rebased_nanos = absl::ToUnixNanos(now);
                if (!first_client_timestamp_nanos.has_value()) {
                    first_client_timestamp_nanos = chunk->timestamp_nanos();
                    first_host_time = now;
                } else if (chunk->timestamp_nanos() > *first_client_timestamp_nanos) {
                    const absl::Duration client_delta = absl::Nanoseconds(
                            chunk->timestamp_nanos() - *first_client_timestamp_nanos);
                    rebased_nanos = absl::ToUnixNanos(first_host_time + client_delta);
                }

                AudioChunk rebased = *chunk;
                rebased.set_timestamp_nanos(rebased_nanos);
                return AbslStatusToGrpcStatus(options_.microphone_sink(rebased));
            });
}

ServerWriteReactor<AudioChunk>* AudioServiceImpl::StreamSpeakerAudio(
        CallbackServerContext* /*context*/, const StreamSpeakerAudioRequest* request) {
    if (!options_.has_audio_output) {
        return new ::ErrorServerWriter<AudioChunk>(SpeakerUnsupportedStatus());
    }
    auto resolved = ResolveSpeakerRequest(*request);
    if (!resolved.ok()) {
        return new ::ErrorServerWriter<AudioChunk>(AbslStatusToGrpcStatus(resolved.status()));
    }
    return CreateSpeakerAudioWriter(resolved->sample_rate_hz, resolved->channel_count,
                                    resolved->format);
}

}  // namespace goldfish::grpc::v2
