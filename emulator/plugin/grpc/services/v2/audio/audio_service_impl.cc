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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/simple_async_grpc.h"
#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/qemu_audio_inject.h"
#include "goldfish/socket_buffer.h"

namespace goldfish::grpc::v2 {

using ::android::emulation::control::AbslStatusToGrpcStatus;
using ::android::emulation::v2::AudioChunk;
using ::android::emulation::v2::AudioState;
using ::android::emulation::v2::GetAudioStateRequest;
using ::android::emulation::v2::InjectAudioClipResponse;
using ::android::emulation::v2::MicrophoneInput;
using ::android::emulation::v2::SampleFormat;
using ::android::emulation::v2::StreamAudioStateRequest;
using ::android::emulation::v2::StreamMicrophoneAudioResponse;
using ::android::emulation::v2::StreamSpeakerAudioRequest;
using ::android::emulation::v2::UpdateAudioStateRequest;
using ::goldfish::audio::AudioInputMux;
using ::goldfish::audio::AudioInputStateMutation;
using ::goldfish::audio::AudioInputStateSnapshot;
using ::goldfish::audio::AudioStreamConfig;
using ::goldfish::audio::GuestAudioConfig;
using ::goldfish::audio::GuestChannelVolume;
using ::goldfish::audio::GuestPcmFormat;
using ::goldfish::audio::HostAudioInputInterface;
using ::goldfish::audio::MicrophoneInputChannelUpdate;
using ::goldfish::audio::MicrophoneInputInfo;
using ::goldfish::audio::MicrophoneInputKind;
using ::goldfish::audio::MicrophoneSourceMetadata;
using ::goldfish::audio::QemuAudioInject;
using ::goldfish::audio::ResolveTargetAudioInputMux;
using ::goldfish::audio::ScaleGuestPcmBuffer;
using ::grpc::CallbackServerContext;
using ::grpc::ServerContext;
using ::grpc::ServerContextBase;
using ::grpc::ServerReadReactor;
using ::grpc::ServerWriteReactor;
using ::grpc::Status;
using ::grpc::StatusCode;

namespace {

constexpr char kClientIdMetadataKey[] = "x-client-id";
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

Status AudioSubsystemDisabledStatus(absl::string_view operation) {
    return Status(StatusCode::FAILED_PRECONDITION,
                  absl::StrCat("Failed to execute ", operation,
                               ": the audio subsystem is disabled on this device "
                               "(hw.audioOutput=no and hw.audioInput=no, DeviceCapabilities.audio "
                               "is unset). Check DeviceCapabilities.audio before calling ",
                               operation, "."));
}

Status SpeakerUnsupportedStatus() {
    return Status(StatusCode::FAILED_PRECONDITION,
                  "Failed to stream speaker audio: speaker audio output is disabled on this "
                  "device (hw.audioOutput=no, AudioCapabilities.speaker_profile is unset). Check "
                  "DeviceCapabilities.audio.speaker_profile before calling StreamSpeakerAudio.");
}

Status MissingMuxStatus(absl::string_view operation) {
    return Status(StatusCode::FAILED_PRECONDITION,
                  absl::StrFormat("Failed to execute %s: no active virtual microphone multiplexer "
                                  "is registered on the QEMU audio backend. Ensure the emulator "
                                  "was started without -no-audio and check "
                                  "DeviceCapabilities.audio.microphone_profile before calling %s.",
                                  operation, operation));
}

Status ReservedHostMetadataStatus(absl::string_view operation) {
    return Status(StatusCode::INVALID_ARGUMENT,
                  absl::StrFormat("Failed to execute %s: x-client-id metadata header cannot use "
                                  "reserved source_id \"host\". Choose a distinct x-client-id "
                                  "identifier.",
                                  operation));
}

Status CancelledClipStatus() {
    return Status(StatusCode::CANCELLED,
                  "Failed to complete InjectAudioClip: RPC was cancelled by the client "
                  "during audio clip playback.");
}

absl::string_view ExtractClientMetadataSourceId(const ServerContextBase* context) {
    if (context == nullptr) {
        return {};
    }
    const auto& metadata = context->client_metadata();
    const auto it = metadata.find(kClientIdMetadataKey);
    if (it == metadata.end() || it->second.empty()) {
        return {};
    }
    return absl::string_view(it->second.data(), it->second.length());
}

std::string ResolveInitialSourceId(const AudioChunk& chunk, absl::string_view client_source_id) {
    if (chunk.has_source_id() && !chunk.source_id().empty()) {
        return chunk.source_id();
    }
    return std::string(client_source_id);
}

MicrophoneInput::Kind ToProtoMicrophoneInputKind(MicrophoneInputKind kind) {
    switch (kind) {
    case MicrophoneInputKind::kHost:
        return MicrophoneInput::HOST;
    case MicrophoneInputKind::kGrpcClip:
        return MicrophoneInput::GRPC_CLIP;
    case MicrophoneInputKind::kGrpcStream:
        return MicrophoneInput::GRPC_STREAM;
    case MicrophoneInputKind::kWebRtc:
        return MicrophoneInput::WEBRTC;
    case MicrophoneInputKind::kUnspecified:
    default:
        return MicrophoneInput::KIND_UNSPECIFIED;
    }
}

void PopulateAudioStateFromSnapshot(const AudioInputStateSnapshot& snapshot, absl::Duration now,
                                    AudioState* out) {
    out->set_microphone_muted(snapshot.microphone_muted);
    out->set_microphone_in_use_by_guest(snapshot.microphone_in_use_by_guest);
    out->set_timestamp_nanos(absl::ToInt64Nanoseconds(now));
    out->clear_microphone_inputs();
    out->mutable_microphone_inputs()->Reserve(static_cast<int>(snapshot.microphone_inputs.size()));
    for (const MicrophoneInputInfo& channel : snapshot.microphone_inputs) {
        MicrophoneInput* proto_input = out->add_microphone_inputs();
        proto_input->set_source_id(channel.source_id);
        proto_input->set_kind(ToProtoMicrophoneInputKind(channel.kind));
        proto_input->set_muted(channel.muted);
        proto_input->set_gain(channel.gain);
    }
}

float ComputeEffectiveChannelGain(const AudioInputStateSnapshot& snapshot,
                                  std::string_view source_id) {
    if (snapshot.microphone_muted) {
        return 0.0F;
    }
    for (const MicrophoneInputInfo& channel : snapshot.microphone_inputs) {
        if (channel.source_id == source_id) {
            return channel.muted ? 0.0F : channel.gain;
        }
    }
    return 1.0F;
}

void ApplySinkChunkGain(float effective_gain, AudioChunk* chunk) {
    if (effective_gain == 1.0F || chunk->pcm_data().empty()) {
        return;
    }
    const GuestPcmFormat guest_format = (chunk->format() == SampleFormat::SAMPLE_FORMAT_F32_BIT)
                                                ? GuestPcmFormat::kF32
                                                : GuestPcmFormat::kS16;
    std::string* pcm_bytes = chunk->mutable_pcm_data();
    ScaleGuestPcmBuffer(guest_format, /*big_endian=*/false, effective_gain, pcm_bytes->data(),
                        pcm_bytes->size());
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
                        "Set a distinct AudioChunk.source_id (or x-client-id), or close the "
                        "stream that uses it.",
                        operation, config.sample_rate_hz, config.channels, status.message()));
    case absl::StatusCode::kResourceExhausted:
        return Status(
                StatusCode::RESOURCE_EXHAUSTED,
                absl::StrFormat(
                        "Failed to start microphone injection in %s (%u Hz, %u channel(s)): %s "
                        "Close a StreamMicrophoneAudio, InjectAudioClip, or WebRTC microphone "
                        "session before retrying.",
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

class NullHostAudioInput final : public HostAudioInputInterface {
  public:
    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* /*name*/,
                                  const GuestAudioConfig& /*config*/,
                                  GuestDataCallback /*on_host_data*/) override {
        return existing;
    }

    void CloseHostVoice(HostVoiceHandle /*voice*/) override {}
    void SetHostVoiceActive(HostVoiceHandle /*voice*/, bool /*active*/) override {}
    void SetHostVoiceVolume(HostVoiceHandle /*voice*/,
                            const GuestChannelVolume& /*volume*/) override {}

    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override {
        std::memset(dst, 0, byte_count);
        return byte_count;
    }

    void SetTimerArmed(bool /*armed*/) override {}
    absl::Duration NowVirtualTime() const override { return absl::ZeroDuration(); }
};

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
 * Bridges a client-streaming `StreamMicrophoneAudio` RPC into `Options::microphone_sink` while
 * tracking mixer channel ownership, `source_id`, mute, and gain in a fallback `AudioInputMux`.
 */
class SinkMicrophoneSession final {
  public:
    using ChunkValidator = absl::Status (*)(const AudioChunk&, absl::string_view);

    SinkMicrophoneSession(std::shared_ptr<AudioInputMux> mux, AudioServiceImpl::MicrophoneSink sink,
                          const ::goldfish::system::SteadyClock& clock,
                          std::string initial_source_id, ChunkValidator validator)
            : mux_(std::move(mux))
            , sink_(std::move(sink))
            , clock_(clock)
            , initial_source_id_(std::move(initial_source_id))
            , validator_(validator) {}

    ~SinkMicrophoneSession() { Stop(); }

    Status StartInitial() ABSL_LOCKS_EXCLUDED(session_mutex_) {
        absl::MutexLock session_lock(session_mutex_);
        android::goldfish::RecursiveScopedVmLock vmlock;
        absl::StatusOr<std::string> source_id = mux_->RegisterInjector(
                this, kDefaultMicrophoneConfig,
                [](void* /*dst*/, size_t /*max_frames*/) { return size_t{0}; }, nullptr,
                MicrophoneSourceMetadata{
                    .source_id = initial_source_id_,
                    .kind = MicrophoneInputKind::kGrpcStream,
                });
        if (!source_id.ok()) {
            stopped_ = true;
            return TranslateInjectorStartStatus(source_id.status(), "StreamMicrophoneAudio",
                                                kDefaultMicrophoneConfig);
        }
        registered_ = true;
        active_source_id_ = *std::move(source_id);
        return Status::OK;
    }

    Status WriteChunk(const AudioChunk& chunk) ABSL_LOCKS_EXCLUDED(session_mutex_) {
        if (absl::Status valid = validator_(chunk, "StreamMicrophoneAudio"); !valid.ok()) {
            Stop();
            return AbslStatusToGrpcStatus(valid);
        }

        AudioChunk rebased = chunk;
        float effective_gain = 1.0F;
        {
            absl::MutexLock session_lock(session_mutex_);
            if (stopped_) {
                return Status(StatusCode::CANCELLED,
                              "Failed to write AudioChunk in StreamMicrophoneAudio: stream is "
                              "already closed.");
            }

            const AudioStreamConfig chunk_config = ToAudioStreamConfig(chunk);
            std::optional<std::string_view> new_source_id;
            if (chunk.has_source_id() && !chunk.source_id().empty() &&
                chunk.source_id() != active_source_id_) {
                new_source_id = chunk.source_id();
            }

            if (chunk_config != active_config_ || new_source_id.has_value()) {
                android::goldfish::RecursiveScopedVmLock vmlock;
                absl::Status reconfig_status =
                        mux_->ReconfigureInjector(this, chunk_config, new_source_id);
                if (!reconfig_status.ok()) {
                    stopped_ = true;
                    registered_ = false;
                    mux_->UnregisterInjector(this);
                    return AbslStatusToGrpcStatus(reconfig_status);
                }
                active_config_ = chunk_config;
                if (new_source_id.has_value()) {
                    active_source_id_ = std::string(*new_source_id);
                }
            }

            const absl::Duration now = clock_.Now();
            int64_t rebased_nanos = absl::ToInt64Nanoseconds(now);
            if (!first_client_timestamp_nanos_.has_value()) {
                first_client_timestamp_nanos_ = chunk.timestamp_nanos();
                first_host_time_ = now;
            } else if (chunk.timestamp_nanos() > *first_client_timestamp_nanos_) {
                const absl::Duration client_delta =
                        absl::Nanoseconds(chunk.timestamp_nanos() - *first_client_timestamp_nanos_);
                rebased_nanos = absl::ToInt64Nanoseconds(first_host_time_ + client_delta);
            }
            rebased.set_timestamp_nanos(rebased_nanos);
            if (!active_source_id_.empty()) {
                rebased.set_source_id(active_source_id_);
            }
            effective_gain =
                    ComputeEffectiveChannelGain(mux_->GetStateSnapshot(), active_source_id_);
        }

        ApplySinkChunkGain(effective_gain, &rebased);
        return AbslStatusToGrpcStatus(sink_(rebased));
    }

    void Stop() ABSL_LOCKS_EXCLUDED(session_mutex_) {
        absl::MutexLock session_lock(session_mutex_);
        stopped_ = true;
        if (registered_) {
            registered_ = false;
            android::goldfish::RecursiveScopedVmLock vmlock;
            mux_->UnregisterInjector(this);
        }
    }

  private:
    const std::shared_ptr<AudioInputMux> mux_;
    const AudioServiceImpl::MicrophoneSink sink_;
    const ::goldfish::system::SteadyClock& clock_;
    const std::string initial_source_id_;
    const ChunkValidator validator_;

    absl::Mutex session_mutex_;
    bool stopped_ ABSL_GUARDED_BY(session_mutex_) = false;
    bool registered_ ABSL_GUARDED_BY(session_mutex_) = false;
    AudioStreamConfig active_config_ ABSL_GUARDED_BY(session_mutex_) = kDefaultMicrophoneConfig;
    std::string active_source_id_ ABSL_GUARDED_BY(session_mutex_);
    std::optional<int64_t> first_client_timestamp_nanos_ ABSL_GUARDED_BY(session_mutex_);
    absl::Duration first_host_time_ ABSL_GUARDED_BY(session_mutex_) = absl::ZeroDuration();
};

/**
 * Bridges a client-streaming `StreamMicrophoneAudio` RPC into a pull-driven `QemuAudioInject`.
 *
 * Lock Hierarchy & Invariants:
 *   - Global acquisition order: `stream_mutex_` -> QEMU BQL -> `buffer_mutex_` (leaf, taken by
 *     `PullFrames` inside `ReadCallback`).
 *   - `buffer_mutex_` is never held while calling `QemuAudioInject::Start()`, `Reconfigure()`, or
 *     `Stop()`.
 *   - `buffer_.Size() % bytes_per_frame_ == 0` and `buffer_.Size() <= max_buffer_bytes_` hold
 *     whenever `buffer_mutex_` is not held.
 */
class QemuMicrophoneSession final {
  public:
    using ChunkValidator = absl::Status (*)(const AudioChunk&, absl::string_view);

    QemuMicrophoneSession(AudioBackend* audio_backend, std::string initial_source_id,
                          ChunkValidator validator)
            : audio_backend_(audio_backend)
            , initial_source_id_(std::move(initial_source_id))
            , validator_(validator) {}

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
        std::optional<std::string_view> new_source_id;
        if (chunk.has_source_id() && !chunk.source_id().empty() &&
            chunk.source_id() != active_source_id_) {
            new_source_id = chunk.source_id();
        }

        if (chunk_config != active_config_ || new_source_id.has_value()) {
            if (Status reconfig_status = ReconfigureLocked(chunk_config, new_source_id);
                !reconfig_status.ok()) {
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
                config, nullptr, audio_backend_,
                MicrophoneSourceMetadata{
                    .source_id = initial_source_id_,
                    .kind = MicrophoneInputKind::kGrpcStream,
                });
        if (absl::Status start_status = injector->Start(); !start_status.ok()) {
            stopped_ = true;
            return TranslateInjectorStartStatus(start_status, "StreamMicrophoneAudio", config);
        }
        active_config_ = config;
        active_source_id_ = injector->metadata().source_id;
        injector_ = std::move(injector);
        return Status::OK;
    }

    Status ReconfigureLocked(const AudioStreamConfig& new_config,
                             std::optional<std::string_view> new_source_id)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(stream_mutex_) ABSL_LOCKS_EXCLUDED(buffer_mutex_) {
        if (injector_ == nullptr) {
            return StartInjectorLocked(new_config);
        }

        const bool format_changed = (new_config != active_config_);
        if (format_changed) {
            // Drain buffered old-format bytes before rebuilding VoicePipelines so any concurrent
            // PullFrames call returns 0 frames without writing old-sized frames into dst.
            absl::MutexLock buffer_lock(buffer_mutex_);
            buffer_.Clear();
        }

        if (absl::Status reconfig_status = injector_->Reconfigure(new_config, new_source_id);
            !reconfig_status.ok()) {
            injector_->Stop();
            injector_.reset();
            stopped_ = true;
            return TranslateInjectorStartStatus(reconfig_status, "StreamMicrophoneAudio",
                                                new_config);
        }

        if (format_changed) {
            absl::MutexLock buffer_lock(buffer_mutex_);
            bytes_per_frame_ = BytesPerStreamFrame(new_config);
            max_buffer_bytes_ = MaxBufferedMicrophoneBytes(new_config);
        }
        active_config_ = new_config;
        active_source_id_ = injector_->metadata().source_id;
        return Status::OK;
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
    const std::string initial_source_id_;
    const ChunkValidator validator_;

    absl::Mutex stream_mutex_;
    bool stopped_ ABSL_GUARDED_BY(stream_mutex_) = false;
    AudioStreamConfig active_config_ ABSL_GUARDED_BY(stream_mutex_) = kDefaultMicrophoneConfig;
    std::string active_source_id_ ABSL_GUARDED_BY(stream_mutex_);
    std::unique_ptr<QemuAudioInject> injector_ ABSL_GUARDED_BY(stream_mutex_);

    absl::Mutex buffer_mutex_ ABSL_ACQUIRED_AFTER(stream_mutex_);
    ::goldfish::SocketBuffer buffer_ ABSL_GUARDED_BY(buffer_mutex_);
    size_t bytes_per_frame_ ABSL_GUARDED_BY(buffer_mutex_) =
            BytesPerStreamFrame(kDefaultMicrophoneConfig);
    size_t max_buffer_bytes_ ABSL_GUARDED_BY(buffer_mutex_) =
            MaxBufferedMicrophoneBytes(kDefaultMicrophoneConfig);
};

}  // namespace

struct AudioServiceImpl::SinkMuxState {
    SinkMuxState() : mux(std::make_shared<AudioInputMux>(&host)) {}
    ~SinkMuxState() {
        android::goldfish::RecursiveScopedVmLock vmlock;
        mux->Shutdown();
    }

    NullHostAudioInput host;
    std::shared_ptr<AudioInputMux> mux;
};

AudioServiceImpl::AudioServiceImpl(Options options)
        : options_(std::move(options)), clock_(options_.clock.get()) {
    if (options_.microphone_sink != nullptr && options_.has_audio_input) {
        sink_mux_state_ = std::make_shared<SinkMuxState>();
        (void)BindActiveMux();
    }
}

AudioServiceImpl::~AudioServiceImpl() {
    std::shared_ptr<AudioInputMux> mux_to_detach;
    AudioInputMux::StateObserverId id_to_remove = 0;
    {
        absl::MutexLock serialize_lock(bind_serialize_mutex_);
        absl::MutexLock state_lock(state_binding_mutex_);
        mux_to_detach = std::move(observed_mux_);
        id_to_remove = std::exchange(observer_id_, 0);
    }
    if (mux_to_detach != nullptr && id_to_remove != 0) {
        mux_to_detach->RemoveStateObserver(id_to_remove);
    }
}

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

std::shared_ptr<AudioInputMux> AudioServiceImpl::GetBoundMux() const {
    absl::MutexLock state_lock(state_binding_mutex_);
    if (observed_mux_ != nullptr && !observed_mux_->IsShutdown()) {
        return observed_mux_;
    }
    return nullptr;
}

std::shared_ptr<AudioInputMux> AudioServiceImpl::BindActiveMux() const {
    if (!options_.has_audio_input) {
        return nullptr;
    }
    if (std::shared_ptr<AudioInputMux> existing = GetBoundMux(); existing != nullptr) {
        return existing;
    }

    std::shared_ptr<AudioInputMux> bound;
    bool notify_initial_bind = false;
    {
        absl::MutexLock serialize_lock(bind_serialize_mutex_);
        std::shared_ptr<AudioInputMux> stale_mux;
        AudioInputMux::StateObserverId stale_observer_id = 0;
        {
            absl::MutexLock state_lock(state_binding_mutex_);
            if (observed_mux_ != nullptr && !observed_mux_->IsShutdown()) {
                return observed_mux_;
            }
            stale_mux = std::move(observed_mux_);
            stale_observer_id = std::exchange(observer_id_, 0);
        }
        if (stale_mux != nullptr && stale_observer_id != 0) {
            stale_mux->RemoveStateObserver(stale_observer_id);
        }

        std::shared_ptr<AudioInputMux> candidate =
                (sink_mux_state_ != nullptr) ? sink_mux_state_->mux
                                             : ResolveTargetAudioInputMux(options_.audio_backend);
        if (candidate == nullptr || candidate->IsShutdown()) {
            return nullptr;
        }

        {
            absl::MutexLock state_lock(state_binding_mutex_);
            observed_mux_ = candidate;
        }
        const AudioInputMux::StateObserverId new_id = candidate->AddStateObserver(
                [this](const AudioInputStateSnapshot& /*snapshot*/) { OnMuxStateChanged(); });
        {
            absl::MutexLock state_lock(state_binding_mutex_);
            observer_id_ = new_id;
        }
        bound = std::move(candidate);
        notify_initial_bind = true;
    }

    if (notify_initial_bind) {
        OnMuxStateChanged();
    }
    return bound;
}

void AudioServiceImpl::OnMuxStateChanged() const {
    state_events_.FireEvent(AudioStateEvent{});
}

void AudioServiceImpl::PopulateAudioState(AudioState* out) const {
    const absl::Duration now = clock_.Now();
    out->clear_microphone_inputs();
    out->set_timestamp_nanos(absl::ToInt64Nanoseconds(now));

    if (!options_.has_audio_input) {
        out->set_microphone_muted(false);
        out->set_microphone_in_use_by_guest(false);
        return;
    }

    if (std::shared_ptr<AudioInputMux> mux = GetBoundMux(); mux != nullptr) {
        PopulateAudioStateFromSnapshot(mux->GetStateSnapshot(), now, out);
        return;
    }

    out->set_microphone_muted(false);
    out->set_microphone_in_use_by_guest(false);
    MicrophoneInput* host_input = out->add_microphone_inputs();
    host_input->set_source_id(std::string(::goldfish::audio::kHostMicrophoneSourceId));
    host_input->set_kind(MicrophoneInput::HOST);
    host_input->set_muted(false);
    host_input->set_gain(1.0F);
}

Status AudioServiceImpl::GetAudioState(ServerContext* /*context*/,
                                       const GetAudioStateRequest* /*request*/, AudioState* reply) {
    if (!HasAudioSubsystem()) {
        return AudioSubsystemDisabledStatus("GetAudioState");
    }
    (void)BindActiveMux();
    PopulateAudioState(reply);
    return Status::OK;
}

ServerWriteReactor<AudioState>* AudioServiceImpl::StreamAudioState(
        CallbackServerContext* /*context*/, const StreamAudioStateRequest* /*request*/) {
    if (!HasAudioSubsystem()) {
        return new ::ErrorServerWriter<AudioState>(
                AudioSubsystemDisabledStatus("StreamAudioState"));
    }
    return CreateAudioStateWriter();
}

Status AudioServiceImpl::UpdateAudioState(ServerContext* /*context*/,
                                          const UpdateAudioStateRequest* request,
                                          AudioState* reply) {
    if (!HasAudioSubsystem()) {
        return AudioSubsystemDisabledStatus("UpdateAudioState");
    }
    if (Status writable = CheckMicrophoneWritable("UpdateAudioState"); !writable.ok()) {
        return writable;
    }

    std::shared_ptr<AudioInputMux> mux = BindActiveMux();
    if (mux == nullptr) {
        return MissingMuxStatus("UpdateAudioState");
    }

    AudioInputStateMutation mutation;
    if (request->has_microphone_muted()) {
        mutation.microphone_muted = request->microphone_muted();
    }
    mutation.input_updates.reserve(static_cast<size_t>(request->microphone_inputs_size()));
    for (const auto& update : request->microphone_inputs()) {
        MicrophoneInputChannelUpdate channel_update;
        channel_update.source_id = update.source_id();
        if (update.has_muted()) {
            channel_update.muted = update.muted();
        }
        if (update.has_gain()) {
            channel_update.gain = update.gain();
        }
        mutation.input_updates.push_back(std::move(channel_update));
    }

    auto updated = ::goldfish::audio::UpdateAudioInputMuxState(*mux, mutation);
    if (!updated.ok()) {
        if (updated.status().code() == absl::StatusCode::kUnavailable ||
            updated.status().code() == absl::StatusCode::kFailedPrecondition) {
            return MissingMuxStatus("UpdateAudioState");
        }
        return AbslStatusToGrpcStatus(updated.status());
    }

    PopulateAudioStateFromSnapshot(*updated, clock_.Now(), reply);
    return Status::OK;
}

Status AudioServiceImpl::CheckMicrophoneWritable(absl::string_view operation) const {
    if (options_.read_only) {
        return Status(
                StatusCode::PERMISSION_DENIED,
                absl::StrCat("Failed to execute ", operation,
                             ": microphone mutations and audio injection are disabled on this "
                             "device (ACCESS_MODE_READ_ONLY). Use GetAudioState, StreamAudioState, "
                             "or StreamSpeakerAudio for read-only observation."));
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
    if (chunk.has_source_id() && chunk.source_id() == ::goldfish::audio::kHostMicrophoneSourceId) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to validate audio chunk in %s: source_id=\"host\" is reserved for the "
                "physical host microphone channel. Choose a distinct source_id or omit source_id.",
                operation));
    }
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

bool AudioServiceImpl::SleepUntil(const ServerContext* context, absl::Duration target) const {
    while (true) {
        if (context != nullptr && context->IsCancelled()) {
            return false;
        }
        const absl::Duration now = clock_.Now();
        if (now >= target) {
            return true;
        }
        const absl::Duration remaining = target - now;
        options_.sleep_fn(std::min(remaining, kCancellationPollInterval));
    }
}

Status AudioServiceImpl::InjectClipViaSink(ServerContext* context, const AudioChunk& request,
                                           absl::string_view client_source_id,
                                           InjectAudioClipResponse* reply) const {
    const AudioStreamConfig config = ToAudioStreamConfig(request);
    const std::string resolved_source_id = ResolveInitialSourceId(request, client_source_id);
    const int owner_token = 0;

    std::string active_source_id;
    if (sink_mux_state_ != nullptr) {
        android::goldfish::RecursiveScopedVmLock vmlock;
        absl::StatusOr<std::string> source_id = sink_mux_state_->mux->RegisterInjector(
                &owner_token, config,
                [](void* /*dst*/, size_t /*max_frames*/) { return size_t{0}; }, nullptr,
                MicrophoneSourceMetadata{
                    .source_id = resolved_source_id,
                    .kind = MicrophoneInputKind::kGrpcClip,
                });
        if (!source_id.ok()) {
            return TranslateInjectorStartStatus(source_id.status(), "InjectAudioClip", config);
        }
        active_source_id = *std::move(source_id);
    }
    absl::Cleanup unregister = [this, &owner_token]() {
        if (sink_mux_state_ != nullptr) {
            android::goldfish::RecursiveScopedVmLock vmlock;
            sink_mux_state_->mux->UnregisterInjector(&owner_token);
        }
    };

    const size_t bytes_per_frame =
            static_cast<size_t>(request.channel_count()) * BytesPerSample(request.format());
    const size_t total_frames = request.pcm_data().size() / bytes_per_frame;
    const auto sample_rate = static_cast<uint64_t>(request.sample_rate_hz());
    const size_t frames_per_slice = std::max<size_t>(
            1, static_cast<size_t>((sample_rate * absl::ToInt64Milliseconds(kClipSliceDuration)) /
                                   kMillisPerSecond));

    const absl::Duration start_time = clock_.Now();
    for (size_t offset_frames = 0; offset_frames < total_frames;
         offset_frames += frames_per_slice) {
        const absl::Duration slice_target =
                start_time + absl::Nanoseconds((offset_frames * kNanosPerSecond) / sample_rate);
        if (!SleepUntil(context, slice_target)) {
            return CancelledClipStatus();
        }

        const size_t slice_frames = std::min(frames_per_slice, total_frames - offset_frames);
        AudioChunk slice;
        slice.set_sample_rate_hz(request.sample_rate_hz());
        slice.set_channel_count(request.channel_count());
        slice.set_format(request.format());
        slice.set_timestamp_nanos(absl::ToInt64Nanoseconds(clock_.Now()));
        if (!active_source_id.empty()) {
            slice.set_source_id(active_source_id);
        }
        slice.set_pcm_data(request.pcm_data().data() + (offset_frames * bytes_per_frame),
                           slice_frames * bytes_per_frame);

        if (sink_mux_state_ != nullptr) {
            const float gain = ComputeEffectiveChannelGain(sink_mux_state_->mux->GetStateSnapshot(),
                                                           active_source_id);
            ApplySinkChunkGain(gain, &slice);
        }

        if (absl::Status sink_status = options_.microphone_sink(slice); !sink_status.ok()) {
            return AbslStatusToGrpcStatus(sink_status);
        }
    }

    const absl::Duration end_target =
            start_time + absl::Nanoseconds((total_frames * kNanosPerSecond) / sample_rate);
    if (!SleepUntil(context, end_target)) {
        return CancelledClipStatus();
    }

    reply->set_duration_ms((total_frames * kMillisPerSecond) / sample_rate);
    return Status::OK;
}

Status AudioServiceImpl::InjectClipViaQemuAudioInject(ServerContext* context,
                                                      const AudioChunk& request,
                                                      absl::string_view client_source_id,
                                                      InjectAudioClipResponse* reply) const {
    (void)BindActiveMux();
    const AudioStreamConfig config = ToAudioStreamConfig(request);
    const size_t bytes_per_frame = BytesPerStreamFrame(config);
    const size_t total_frames = request.pcm_data().size() / bytes_per_frame;
    const auto sample_rate = static_cast<uint64_t>(config.sample_rate_hz);

    ClipPullSource source(request.pcm_data(), total_frames, bytes_per_frame);
    QemuAudioInject injector(
            [&source](void* dst, size_t max_frames) { return source.PullFrames(dst, max_frames); },
            config, [&source](bool active) { source.SetGuestActive(active); },
            options_.audio_backend,
            MicrophoneSourceMetadata{
                .source_id = ResolveInitialSourceId(request, client_source_id),
                .kind = MicrophoneInputKind::kGrpcClip,
            });
    if (absl::Status start_status = injector.Start(); !start_status.ok()) {
        return TranslateInjectorStartStatus(start_status, "InjectAudioClip", config);
    }

    const absl::Duration start_time = clock_.Now();
    const absl::Duration end_target =
            start_time + absl::Nanoseconds((total_frames * kNanosPerSecond) / sample_rate);
    if (!SleepUntil(context, end_target)) {
        injector.Stop();
        return CancelledClipStatus();
    }

    const absl::Duration grace_deadline = end_target + kClipDrainGraceDuration;
    while (source.NeedsDrainGrace() && clock_.Now() < grace_deadline) {
        const absl::Duration step_target =
                std::min(grace_deadline, clock_.Now() + kClipDrainPollInterval);
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
    const absl::string_view client_source_id = ExtractClientMetadataSourceId(context);
    if ((!request->has_source_id() || request->source_id().empty()) &&
        client_source_id == ::goldfish::audio::kHostMicrophoneSourceId) {
        return ReservedHostMetadataStatus("InjectAudioClip");
    }
    if (options_.microphone_sink != nullptr) {
        return InjectClipViaSink(context, *request, client_source_id, reply);
    }
    return InjectClipViaQemuAudioInject(context, *request, client_source_id, reply);
}

AudioServiceImpl::MicrophoneStreamStart AudioServiceImpl::StartSinkMicrophoneStream(
        absl::string_view client_source_id) const {
    auto session = std::make_shared<SinkMicrophoneSession>(
            sink_mux_state_->mux, options_.microphone_sink, clock_,
            std::string(client_source_id), &ValidateMicrophoneChunk);
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

AudioServiceImpl::MicrophoneStreamStart AudioServiceImpl::StartMicrophoneStream(
        absl::string_view client_source_id) const {
    if (Status writable = CheckMicrophoneWritable("StreamMicrophoneAudio"); !writable.ok()) {
        return MicrophoneStreamStart{.status = std::move(writable)};
    }
    if (client_source_id == ::goldfish::audio::kHostMicrophoneSourceId) {
        return MicrophoneStreamStart{.status = ReservedHostMetadataStatus("StreamMicrophoneAudio")};
    }
    if (options_.microphone_sink != nullptr) {
        return StartSinkMicrophoneStream(client_source_id);
    }

    (void)BindActiveMux();
    auto session = std::make_shared<QemuMicrophoneSession>(
            options_.audio_backend, std::string(client_source_id), &ValidateMicrophoneChunk);
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
        CallbackServerContext* context, StreamMicrophoneAudioResponse* /*reply*/) {
    return CreateMicrophoneAudioReader<>(ExtractClientMetadataSourceId(context));
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
