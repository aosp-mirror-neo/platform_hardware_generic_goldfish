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
#include <memory>
#include <optional>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/simple_async_grpc.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/qemu_audio_inject.h"
#include "goldfish/socket_buffer.h"

namespace goldfish::grpc::v2 {

using ::android::emulation::control::AbslStatusToGrpcStatus;
using ::android::emulation::v2::AudioChunk;
using ::android::emulation::v2::InjectAudioClipResponse;
using ::android::emulation::v2::SampleFormat;
using ::android::emulation::v2::StreamMicrophoneAudioResponse;
using ::android::emulation::v2::StreamSpeakerAudioRequest;
using ::goldfish::audio::AudioStreamConfig;
using ::goldfish::audio::QemuAudioInject;
using ::grpc::CallbackServerContext;
using ::grpc::ServerContext;
using ::grpc::ServerReadReactor;
using ::grpc::ServerWriteReactor;
using ::grpc::Status;
using ::grpc::StatusCode;

namespace {

constexpr absl::Duration kClipSliceDuration = absl::Milliseconds(20);
constexpr absl::Duration kCancellationPollInterval = absl::Milliseconds(50);
constexpr absl::Duration kClipDrainGraceDuration = absl::Milliseconds(40);
constexpr absl::Duration kClipDrainPollInterval = absl::Milliseconds(5);
constexpr uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr uint64_t kMillisPerSecond = 1'000ULL;

constexpr AudioStreamConfig kDefaultMicrophoneConfig = {
    .sample_rate_hz = 48000,
    .channels = 1,
    .format = ::goldfish::audio::SampleFormat::kS16,
};

Status SpeakerUnsupportedStatus() {
    return Status(StatusCode::FAILED_PRECONDITION,
                  "Failed to stream speaker audio: speaker audio output is disabled on this "
                  "device (hw.audioOutput=no, AudioCapabilities.speaker_profile is unset). Check "
                  "DeviceCapabilities.audio.speaker_profile before calling StreamSpeakerAudio.");
}

Status CancelledClipStatus() {
    return Status(StatusCode::CANCELLED,
                  "Failed to complete InjectAudioClip: RPC was cancelled by the client "
                  "during audio clip playback.");
}

size_t BytesPerSample(SampleFormat format) {
    return (format == SampleFormat::SAMPLE_FORMAT_F32_BIT) ? sizeof(float) : sizeof(int16_t);
}

size_t BytesPerStreamFrame(const AudioStreamConfig& config) {
    return static_cast<size_t>(config.channels) * ::goldfish::audio::BytesPerSample(config.format);
}

size_t MaxBufferedMicrophoneBytes(const AudioStreamConfig& config) {
    return static_cast<size_t>(config.sample_rate_hz) *
           AudioServiceImpl::kMaxBufferedMicrophoneSeconds * BytesPerStreamFrame(config);
}

AudioStreamConfig ToAudioStreamConfig(const AudioChunk& chunk) {
    const ::goldfish::audio::SampleFormat pcm_format =
            (chunk.format() == SampleFormat::SAMPLE_FORMAT_F32_BIT)
                    ? ::goldfish::audio::SampleFormat::kF32
                    : ::goldfish::audio::SampleFormat::kS16;
    return AudioStreamConfig{
        .sample_rate_hz = static_cast<uint32_t>(chunk.sample_rate_hz()),
        .channels = static_cast<uint32_t>(chunk.channel_count()),
        .format = pcm_format,
    };
}

Status TranslateInjectorStartStatus(const absl::Status& status, absl::string_view operation,
                                    const AudioStreamConfig& config) {
    switch (status.code()) {
    case absl::StatusCode::kUnavailable:
    case absl::StatusCode::kFailedPrecondition:
        return Status(
                StatusCode::FAILED_PRECONDITION,
                absl::StrFormat(
                        "Failed to start microphone injection in %s (%u Hz, %u channel(s)): %s "
                        "Ensure the emulator was started without -no-audio and check "
                        "DeviceCapabilities.audio.microphone_profile before calling %s.",
                        operation, config.sample_rate_hz, config.channels, status.message(),
                        operation));
    case absl::StatusCode::kAlreadyExists:
        return Status(
                StatusCode::ALREADY_EXISTS,
                absl::StrFormat(
                        "Failed to start microphone injection in %s (%u Hz, %u channel(s)): %s "
                        "Close or wait for the active StreamMicrophoneAudio, InjectAudioClip, or "
                        "WebRTC microphone session before retrying.",
                        operation, config.sample_rate_hz, config.channels, status.message()));
    default:
        return AbslStatusToGrpcStatus(status);
    }
}

void DiscardSocketBufferBytes(::goldfish::SocketBuffer& buffer, size_t byte_count) {
    size_t remaining = std::min(byte_count, buffer.Size());
    while (remaining > 0) {
        const auto [ptr, contiguous] = buffer.Peek();
        (void)ptr;
        if (contiguous == 0) {
            break;
        }
        const size_t step = std::min(remaining, contiguous);
        (void)buffer.Consume(step);
        remaining -= step;
    }
}

size_t PopSocketBufferBytes(::goldfish::SocketBuffer& buffer, void* dst, size_t max_bytes) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t copied = 0;
    while (copied < max_bytes && buffer.Size() > 0) {
        const auto [ptr, contiguous] = buffer.Peek();
        if (contiguous == 0) {
            break;
        }
        const size_t step = std::min(max_bytes - copied, contiguous);
        std::memcpy(out + copied, ptr, step);
        (void)buffer.Consume(step);
        copied += step;
    }
    return copied;
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

/**
 * Zero-copy pull state for a single unary `InjectAudioClip` invocation.
 *
 * Class Invariant: `cursor_frames_ <= total_frames_` holds whenever `clip_mutex_` is not held.
 * `clip_mutex_` is a pure leaf lock and is never held across QEMU or gRPC calls.
 */
class ClipPullSource final {
  public:
    ClipPullSource(absl::string_view pcm_data, size_t total_frames, size_t bytes_per_frame)
            : pcm_data_(pcm_data), total_frames_(total_frames), bytes_per_frame_(bytes_per_frame) {}

    size_t PullFrames(void* dst, size_t max_frames) ABSL_LOCKS_EXCLUDED(clip_mutex_) {
        absl::MutexLock clip_lock(clip_mutex_);
        const size_t available = total_frames_ - cursor_frames_;
        const size_t to_copy = std::min(max_frames, available);
        if (to_copy > 0) {
            std::memcpy(dst, pcm_data_.data() + (cursor_frames_ * bytes_per_frame_),
                        to_copy * bytes_per_frame_);
            cursor_frames_ += to_copy;
        }
        return to_copy;
    }

    void SetGuestActive(bool active) ABSL_LOCKS_EXCLUDED(clip_mutex_) {
        absl::MutexLock clip_lock(clip_mutex_);
        guest_active_ = active;
    }

    bool NeedsDrainGrace() const ABSL_LOCKS_EXCLUDED(clip_mutex_) {
        absl::MutexLock clip_lock(clip_mutex_);
        return guest_active_ && cursor_frames_ < total_frames_;
    }

  private:
    const absl::string_view pcm_data_;
    const size_t total_frames_;
    const size_t bytes_per_frame_;

    mutable absl::Mutex clip_mutex_;
    size_t cursor_frames_ ABSL_GUARDED_BY(clip_mutex_) = 0;
    bool guest_active_ ABSL_GUARDED_BY(clip_mutex_) = false;
};

/**
 * Bridges a client-streaming `StreamMicrophoneAudio` RPC into a pull-driven `QemuAudioInject`.
 *
 * Lock Hierarchy & Invariants:
 *   - Global acquisition order: `stream_mutex_` -> QEMU BQL -> `buffer_mutex_` (leaf, taken by
 *     `PullFrames` inside `ReadCallback`).
 *   - `buffer_mutex_` is never held while calling `QemuAudioInject::Start()` or `Stop()`.
 *   - `buffer_.Size() % bytes_per_frame_ == 0` and `buffer_.Size() <= max_buffer_bytes_` hold
 *     whenever `buffer_mutex_` is not held.
 */
class QemuMicrophoneSession final {
  public:
    using ChunkValidator = absl::Status (*)(const AudioChunk&, absl::string_view);

    QemuMicrophoneSession(AudioBackend* audio_backend, ChunkValidator validator)
            : audio_backend_(audio_backend), validator_(validator) {}

    ~QemuMicrophoneSession() { Stop(); }

    Status StartInitial() ABSL_LOCKS_EXCLUDED(stream_mutex_, buffer_mutex_) {
        absl::MutexLock stream_lock(stream_mutex_);
        return StartInjectorLocked(kDefaultMicrophoneConfig);
    }

    Status WriteChunk(const AudioChunk& chunk) ABSL_LOCKS_EXCLUDED(stream_mutex_, buffer_mutex_) {
        if (absl::Status valid = validator_(chunk, "StreamMicrophoneAudio"); !valid.ok()) {
            Stop();
            return AbslStatusToGrpcStatus(valid);
        }

        absl::MutexLock stream_lock(stream_mutex_);
        if (stopped_) {
            return Status(StatusCode::CANCELLED,
                          "Failed to write AudioChunk in StreamMicrophoneAudio: stream is already "
                          "closed.");
        }

        const AudioStreamConfig chunk_config = ToAudioStreamConfig(chunk);
        if (chunk_config != active_config_) {
            if (Status reconfig_status = ReconfigureLocked(chunk_config); !reconfig_status.ok()) {
                return reconfig_status;
            }
        }

        AppendFrames(chunk.pcm_data());
        return Status::OK;
    }

    void Stop() ABSL_LOCKS_EXCLUDED(stream_mutex_, buffer_mutex_) {
        absl::MutexLock stream_lock(stream_mutex_);
        stopped_ = true;
        if (injector_ != nullptr) {
            injector_->Stop();
            injector_.reset();
        }
        absl::MutexLock buffer_lock(buffer_mutex_);
        buffer_.Clear(true);
    }

  private:
    Status StartInjectorLocked(const AudioStreamConfig& config)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(stream_mutex_) ABSL_LOCKS_EXCLUDED(buffer_mutex_) {
        {
            absl::MutexLock buffer_lock(buffer_mutex_);
            buffer_.Clear();
            bytes_per_frame_ = BytesPerStreamFrame(config);
            max_buffer_bytes_ = MaxBufferedMicrophoneBytes(config);
        }
        auto injector = std::make_unique<QemuAudioInject>(
                [this](void* dst, size_t max_frames) { return PullFrames(dst, max_frames); },
                config, nullptr, audio_backend_);
        if (absl::Status start_status = injector->Start(); !start_status.ok()) {
            stopped_ = true;
            return TranslateInjectorStartStatus(start_status, "StreamMicrophoneAudio", config);
        }
        active_config_ = config;
        injector_ = std::move(injector);
        return Status::OK;
    }

    Status ReconfigureLocked(const AudioStreamConfig& new_config)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(stream_mutex_) ABSL_LOCKS_EXCLUDED(buffer_mutex_) {
        if (injector_ != nullptr) {
            injector_->Stop();
            injector_.reset();
        }
        return StartInjectorLocked(new_config);
    }

    void AppendFrames(absl::string_view pcm_data) ABSL_LOCKS_EXCLUDED(buffer_mutex_) {
        absl::MutexLock buffer_lock(buffer_mutex_);
        if (max_buffer_bytes_ == 0 || bytes_per_frame_ == 0) {
            return;
        }
        if (pcm_data.size() >= max_buffer_bytes_) {
            buffer_.Clear();
            const size_t tail_offset = pcm_data.size() - max_buffer_bytes_;
            (void)buffer_.Append(pcm_data.data() + tail_offset, max_buffer_bytes_);
            return;
        }
        if (buffer_.Size() + pcm_data.size() > max_buffer_bytes_) {
            const size_t overflow = (buffer_.Size() + pcm_data.size()) - max_buffer_bytes_;
            const size_t drop_frames = (overflow + bytes_per_frame_ - 1) / bytes_per_frame_;
            DiscardSocketBufferBytes(buffer_, drop_frames * bytes_per_frame_);
        }
        (void)buffer_.Append(pcm_data.data(), pcm_data.size());
    }

    size_t PullFrames(void* dst, size_t max_frames) ABSL_LOCKS_EXCLUDED(buffer_mutex_) {
        absl::MutexLock buffer_lock(buffer_mutex_);
        if (bytes_per_frame_ == 0 || max_frames == 0 || buffer_.Size() < bytes_per_frame_) {
            return 0;
        }
        const size_t available_frames = buffer_.Size() / bytes_per_frame_;
        const size_t frames_to_read = std::min(max_frames, available_frames);
        const size_t copied = PopSocketBufferBytes(buffer_, dst, frames_to_read * bytes_per_frame_);
        return copied / bytes_per_frame_;
    }

    AudioBackend* const audio_backend_;
    const ChunkValidator validator_;

    absl::Mutex stream_mutex_;
    bool stopped_ ABSL_GUARDED_BY(stream_mutex_) = false;
    AudioStreamConfig active_config_ ABSL_GUARDED_BY(stream_mutex_) = kDefaultMicrophoneConfig;
    std::unique_ptr<QemuAudioInject> injector_ ABSL_GUARDED_BY(stream_mutex_);

    absl::Mutex buffer_mutex_ ABSL_ACQUIRED_AFTER(stream_mutex_);
    ::goldfish::SocketBuffer buffer_ ABSL_GUARDED_BY(buffer_mutex_);
    size_t bytes_per_frame_ ABSL_GUARDED_BY(buffer_mutex_) =
            BytesPerStreamFrame(kDefaultMicrophoneConfig);
    size_t max_buffer_bytes_ ABSL_GUARDED_BY(buffer_mutex_) =
            MaxBufferedMicrophoneBytes(kDefaultMicrophoneConfig);
};

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

Status AudioServiceImpl::InjectClipViaSink(ServerContext* context, const AudioChunk& request,
                                           InjectAudioClipResponse* reply) const {
    const size_t bytes_per_frame =
            static_cast<size_t>(request.channel_count()) * BytesPerSample(request.format());
    const size_t total_frames = request.pcm_data().size() / bytes_per_frame;
    const auto sample_rate = static_cast<uint64_t>(request.sample_rate_hz());
    const size_t frames_per_slice = std::max<size_t>(
            1, static_cast<size_t>((sample_rate * absl::ToInt64Milliseconds(kClipSliceDuration)) /
                                   kMillisPerSecond));

    const absl::Time start_time = options_.now_fn();
    for (size_t offset_frames = 0; offset_frames < total_frames;
         offset_frames += frames_per_slice) {
        const absl::Time slice_target =
                start_time + absl::Nanoseconds((offset_frames * kNanosPerSecond) / sample_rate);
        if (!SleepUntil(context, slice_target)) {
            return CancelledClipStatus();
        }

        const size_t slice_frames = std::min(frames_per_slice, total_frames - offset_frames);
        AudioChunk slice;
        slice.set_sample_rate_hz(request.sample_rate_hz());
        slice.set_channel_count(request.channel_count());
        slice.set_format(request.format());
        slice.set_timestamp_nanos(absl::ToUnixNanos(options_.now_fn()));
        slice.set_pcm_data(request.pcm_data().data() + (offset_frames * bytes_per_frame),
                           slice_frames * bytes_per_frame);

        if (absl::Status sink_status = options_.microphone_sink(slice); !sink_status.ok()) {
            return AbslStatusToGrpcStatus(sink_status);
        }
    }

    const absl::Time end_target =
            start_time + absl::Nanoseconds((total_frames * kNanosPerSecond) / sample_rate);
    if (!SleepUntil(context, end_target)) {
        return CancelledClipStatus();
    }

    reply->set_duration_ms((total_frames * kMillisPerSecond) / sample_rate);
    return Status::OK;
}

Status AudioServiceImpl::InjectClipViaQemuAudioInject(ServerContext* context,
                                                      const AudioChunk& request,
                                                      InjectAudioClipResponse* reply) const {
    const AudioStreamConfig config = ToAudioStreamConfig(request);
    const size_t bytes_per_frame = BytesPerStreamFrame(config);
    const size_t total_frames = request.pcm_data().size() / bytes_per_frame;
    const auto sample_rate = static_cast<uint64_t>(config.sample_rate_hz);

    ClipPullSource source(request.pcm_data(), total_frames, bytes_per_frame);
    QemuAudioInject injector(
            [&source](void* dst, size_t max_frames) { return source.PullFrames(dst, max_frames); },
            config, [&source](bool active) { source.SetGuestActive(active); },
            options_.audio_backend);
    if (absl::Status start_status = injector.Start(); !start_status.ok()) {
        return TranslateInjectorStartStatus(start_status, "InjectAudioClip", config);
    }

    const absl::Time start_time = options_.now_fn();
    const absl::Time end_target =
            start_time + absl::Nanoseconds((total_frames * kNanosPerSecond) / sample_rate);
    if (!SleepUntil(context, end_target)) {
        injector.Stop();
        return CancelledClipStatus();
    }

    const absl::Time grace_deadline = end_target + kClipDrainGraceDuration;
    while (source.NeedsDrainGrace() && options_.now_fn() < grace_deadline) {
        const absl::Time step_target =
                std::min(grace_deadline, options_.now_fn() + kClipDrainPollInterval);
        if (!SleepUntil(context, step_target)) {
            injector.Stop();
            return CancelledClipStatus();
        }
    }

    injector.Stop();
    reply->set_duration_ms((total_frames * kMillisPerSecond) / sample_rate);
    return Status::OK;
}

Status AudioServiceImpl::InjectAudioClip(ServerContext* context, const AudioChunk* request,
                                         InjectAudioClipResponse* reply) {
    if (Status writable = CheckMicrophoneWritable("InjectAudioClip"); !writable.ok()) {
        return writable;
    }
    if (absl::Status valid = ValidateMicrophoneChunk(*request, "InjectAudioClip"); !valid.ok()) {
        return AbslStatusToGrpcStatus(valid);
    }
    if (options_.microphone_sink != nullptr) {
        return InjectClipViaSink(context, *request, reply);
    }
    return InjectClipViaQemuAudioInject(context, *request, reply);
}

AudioServiceImpl::MicrophoneStreamStart AudioServiceImpl::StartSinkMicrophoneStream() const {
    return MicrophoneStreamStart{
        .status = Status::OK,
        .on_chunk = [this, first_client_timestamp_nanos = std::optional<int64_t>{},
                     first_host_time =
                             absl::InfinitePast()](const AudioChunk* chunk) mutable -> Status {
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
                const absl::Duration client_delta =
                        absl::Nanoseconds(chunk->timestamp_nanos() - *first_client_timestamp_nanos);
                rebased_nanos = absl::ToUnixNanos(first_host_time + client_delta);
            }

            AudioChunk rebased = *chunk;
            rebased.set_timestamp_nanos(rebased_nanos);
            return AbslStatusToGrpcStatus(options_.microphone_sink(rebased));
        },
        .stop_stream = nullptr,
    };
}

AudioServiceImpl::MicrophoneStreamStart AudioServiceImpl::StartMicrophoneStream() const {
    if (Status writable = CheckMicrophoneWritable("StreamMicrophoneAudio"); !writable.ok()) {
        return MicrophoneStreamStart{.status = std::move(writable)};
    }
    if (options_.microphone_sink != nullptr) {
        return StartSinkMicrophoneStream();
    }

    auto session = std::make_shared<QemuMicrophoneSession>(options_.audio_backend,
                                                           &ValidateMicrophoneChunk);
    if (Status start_status = session->StartInitial(); !start_status.ok()) {
        return MicrophoneStreamStart{.status = std::move(start_status)};
    }
    return MicrophoneStreamStart{
        .status = Status::OK,
        .on_chunk = [session](const AudioChunk* chunk) -> Status {
            return session->WriteChunk(*chunk);
        },
        .stop_stream = [session]() { session->Stop(); },
    };
}

ServerReadReactor<AudioChunk>* AudioServiceImpl::StreamMicrophoneAudio(
        CallbackServerContext* /*context*/, StreamMicrophoneAudioResponse* /*reply*/) {
    return CreateMicrophoneAudioReader<>();
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
