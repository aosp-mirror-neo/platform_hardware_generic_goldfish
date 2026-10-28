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

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "audio_service.grpc.pb.h"
#include "emulator/plugin/audio/test_qemu_audio_stub.h"
#include "test/grpc_v2_service_test.h"

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::AudioChunk;
using ::android::emulation::v2::AudioService;
using ::android::emulation::v2::InjectAudioClipResponse;
using ::android::emulation::v2::SampleFormat;
using ::android::emulation::v2::StreamMicrophoneAudioResponse;
using ::android::emulation::v2::StreamSpeakerAudioRequest;
using ::grpc::ClientContext;
using ::grpc::Server;
using ::grpc::ServerBuilder;
using ::grpc::ServerContext;
using ::grpc::Status;
using ::grpc::StatusCode;

std::string MakePcm16Bytes(const std::vector<int16_t>& samples) {
    std::string out(samples.size() * sizeof(int16_t), '\0');
    if (!samples.empty()) {
        std::memcpy(out.data(), samples.data(), out.size());
    }
    return out;
}

std::string MakeF32Bytes(const std::vector<float>& samples) {
    std::string out(samples.size() * sizeof(float), '\0');
    if (!samples.empty()) {
        std::memcpy(out.data(), samples.data(), out.size());
    }
    return out;
}

template <typename W>
class ThreadSafeFakeWriteReactor : public test::FakeWriteReactor<W> {
  public:
    void StartWrite(const W* msg) {
        absl::MutexLock lock(mu_);
        test::FakeWriteReactor<W>::StartWrite(msg);
    }

    void Finish(Status status) {
        absl::MutexLock lock(mu_);
        test::FakeWriteReactor<W>::Finish(std::move(status));
    }

    bool IsFinishedThreadSafe() const {
        absl::MutexLock lock(mu_);
        return this->finish_called;
    }

    Status FinishStatusThreadSafe() const {
        absl::MutexLock lock(mu_);
        return this->finish_status;
    }

  private:
    mutable absl::Mutex mu_;
};

class AudioServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_reset_audio_stubs();
        {
            absl::MutexLock lock(clock_mu_);
            current_time_ = absl::FromUnixNanos(1'000'000'000LL);
            total_slept_ = absl::ZeroDuration();
        }
        RecreateService(MakeDefaultOptions());
    }

    void TearDown() override {
        ShutdownServer();
        service_.reset();
        test_reset_audio_stubs();
    }

    AudioServiceImpl::Options MakeDefaultOptions() {
        AudioServiceImpl::Options opts;
        opts.has_audio_output = true;
        opts.has_audio_input = true;
        opts.read_only = false;
        opts.now_fn = [this]() {
            absl::MutexLock lock(clock_mu_);
            return current_time_;
        };
        opts.sleep_fn = [this](absl::Duration d) {
            absl::MutexLock lock(clock_mu_);
            current_time_ += d;
            total_slept_ += d;
        };
        opts.microphone_sink = [this](const AudioChunk& chunk) {
            absl::MutexLock lock(sink_mu_);
            if (!sink_status_.ok()) {
                return sink_status_;
            }
            injected_chunks_.push_back(chunk);
            return absl::OkStatus();
        };
        return opts;
    }

    void RecreateService(AudioServiceImpl::Options opts) {
        ShutdownServer();
        service_ = std::make_unique<AudioServiceImpl>(std::move(opts));
        ServerBuilder builder;
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = AudioService::NewStub(server_->InProcessChannel(::grpc::ChannelArguments()));
    }

    void ShutdownServer() {
        if (server_) {
            server_->Shutdown();
            server_->Wait();
            server_.reset();
        }
        stub_.reset();
    }

    void AdvanceClock(absl::Duration delta) {
        absl::MutexLock lock(clock_mu_);
        current_time_ += delta;
    }

    std::vector<AudioChunk> GetInjectedChunks() const {
        absl::MutexLock lock(sink_mu_);
        return injected_chunks_;
    }

    void SetSinkStatus(absl::Status status) {
        absl::MutexLock lock(sink_mu_);
        sink_status_ = std::move(status);
    }

    mutable absl::Mutex clock_mu_;
    absl::Time current_time_ ABSL_GUARDED_BY(clock_mu_);
    absl::Duration total_slept_ ABSL_GUARDED_BY(clock_mu_);

    mutable absl::Mutex sink_mu_;
    std::vector<AudioChunk> injected_chunks_ ABSL_GUARDED_BY(sink_mu_);
    absl::Status sink_status_ ABSL_GUARDED_BY(sink_mu_) = absl::OkStatus();

    std::unique_ptr<AudioServiceImpl> service_;
    std::unique_ptr<Server> server_;
    std::unique_ptr<AudioService::Stub> stub_;
};

// ============================================================================
// Tier A: Deterministic SpeakerAudioWriter Reactor Tests
// ============================================================================

TEST_F(AudioServiceTest, SpeakerWriterStreamsPcm16WithMonotonicTimestamps) {
    auto* writer = service_->CreateSpeakerAudioWriter<test::FakeWriteReactor<AudioChunk>>();
    EXPECT_EQ(test_has_active_capture(), 1);

    const std::array<int16_t, 4> samples = {1000, -1000, 2000, -2000};
    test_simulate_qemu_audio_output(samples.data(), static_cast<int>(sizeof(samples)));

    ASSERT_EQ(writer->written_messages.size(), 1U);
    const AudioChunk& chunk = writer->written_messages[0];
    EXPECT_EQ(chunk.sample_rate_hz(), 48000);
    EXPECT_EQ(chunk.channel_count(), 2);
    EXPECT_EQ(chunk.format(), SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    EXPECT_EQ(chunk.timestamp_nanos(), 1'000'000'000LL);
    ASSERT_EQ(chunk.pcm_data().size(), sizeof(samples));
    EXPECT_EQ(std::memcmp(chunk.pcm_data().data(), samples.data(), sizeof(samples)), 0);

    writer->OnWriteDone(true);
    AdvanceClock(absl::Milliseconds(20));
    test_simulate_qemu_audio_output(samples.data(), static_cast<int>(sizeof(samples)));

    ASSERT_EQ(writer->written_messages.size(), 2U);
    EXPECT_EQ(writer->written_messages[1].timestamp_nanos(), 1'020'000'000LL);

    writer->OnDone();
    EXPECT_EQ(test_has_active_capture(), 0);
}

TEST_F(AudioServiceTest, SpeakerWriterConvertsPcm16ToFloat32WhenRequested) {
    auto* writer = service_->CreateSpeakerAudioWriter<test::FakeWriteReactor<AudioChunk>>(
            16000, 1, SampleFormat::SAMPLE_FORMAT_F32_BIT);
    EXPECT_EQ(test_has_active_capture(), 1);

    const std::array<int16_t, 4> samples = {-32768, 0, 16384, -16384};
    test_simulate_qemu_audio_output(samples.data(), static_cast<int>(sizeof(samples)));

    ASSERT_EQ(writer->written_messages.size(), 1U);
    const AudioChunk& chunk = writer->written_messages[0];
    EXPECT_EQ(chunk.sample_rate_hz(), 16000);
    EXPECT_EQ(chunk.channel_count(), 1);
    EXPECT_EQ(chunk.format(), SampleFormat::SAMPLE_FORMAT_F32_BIT);
    ASSERT_EQ(chunk.pcm_data().size(), samples.size() * sizeof(float));

    std::array<float, 4> decoded{};
    std::memcpy(decoded.data(), chunk.pcm_data().data(), sizeof(decoded));
    EXPECT_FLOAT_EQ(decoded[0], -1.0F);
    EXPECT_FLOAT_EQ(decoded[1], 0.0F);
    EXPECT_FLOAT_EQ(decoded[2], 0.5F);
    EXPECT_FLOAT_EQ(decoded[3], -0.5F);

    writer->OnDone();
    EXPECT_EQ(test_has_active_capture(), 0);
}

TEST_F(AudioServiceTest, SpeakerWriterBoundsQueueUnderClientBackpressure) {
    auto* writer = service_->CreateSpeakerAudioWriter<test::FakeWriteReactor<AudioChunk>>();
    const std::array<int16_t, 2> samples = {123, -123};

    // Enqueue 150 chunks without calling OnWriteDone(true); 1 chunk is in-flight
    // and at most kMaxPendingSpeakerChunks remain queued.
    for (int i = 0; i < 150; ++i) {
        int16_t tagged[2] = {static_cast<int16_t>(i), static_cast<int16_t>(-i)};
        test_simulate_qemu_audio_output(tagged, static_cast<int>(sizeof(tagged)));
        EXPECT_LE(writer->QueueSize(), AudioServiceImpl::kMaxPendingSpeakerChunks + 1);
    }

    EXPECT_EQ(writer->QueueSize(), AudioServiceImpl::kMaxPendingSpeakerChunks + 1);
    EXPECT_EQ(writer->written_messages.size(), 1U);

    // Drain all queued messages by completing writes.
    while (writer->QueueSize() > 0) {
        writer->OnWriteDone(true);
    }
    EXPECT_EQ(writer->written_messages.size(), AudioServiceImpl::kMaxPendingSpeakerChunks + 1);

    // Confirm the latest chunk (index 149) was preserved at the tail.
    int16_t last_samples[2] = {0, 0};
    std::memcpy(last_samples, writer->written_messages.back().pcm_data().data(),
                sizeof(last_samples));
    EXPECT_EQ(last_samples[0], 149);

    writer->OnDone();
}

TEST_F(AudioServiceTest, SpeakerWriterFinishesWithErrorWhenQemuCaptureStartFails) {
    test_set_fail_add_capture(1);
    auto* writer = service_->CreateSpeakerAudioWriter<test::FakeWriteReactor<AudioChunk>>();

    EXPECT_TRUE(writer->finish_called);
    EXPECT_EQ(writer->finish_status.error_code(), StatusCode::INTERNAL);
    EXPECT_TRUE(absl::StrContains(writer->finish_status.error_message(),
                                  "Failed to start speaker audio capture"));

    writer->OnDone();
}

// ============================================================================
// Tier B: Concurrency & In-Process gRPC Tests
// ============================================================================

TEST_F(AudioServiceTest, ConcurrentAudioCallbacksAndCancellationAreRaceFree) {
    auto* writer = service_->CreateSpeakerAudioWriter<ThreadSafeFakeWriteReactor<AudioChunk>>();
    std::atomic<bool> start{false};

    std::vector<std::thread> producers;
    producers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&start]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const std::array<int16_t, 4> frame = {10, -10, 20, -20};
            for (int i = 0; i < 100; ++i) {
                test_simulate_qemu_audio_output(frame.data(), static_cast<int>(sizeof(frame)));
            }
        });
    }

    std::thread canceller([&start, writer]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        writer->OnCancel();
    });

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    canceller.join();

    EXPECT_TRUE(writer->IsFinishedThreadSafe());
    EXPECT_EQ(writer->FinishStatusThreadSafe().error_code(), StatusCode::CANCELLED);
    EXPECT_EQ(test_has_active_capture(), 0);

    writer->OnDone();
}

TEST_F(AudioServiceTest, StreamSpeakerAudioRejectsWhenAudioOutputDisabled) {
    auto opts = MakeDefaultOptions();
    opts.has_audio_output = false;
    RecreateService(std::move(opts));

    ClientContext context;
    StreamSpeakerAudioRequest request;
    auto reader = stub_->StreamSpeakerAudio(&context, request);

    AudioChunk chunk;
    EXPECT_FALSE(reader->Read(&chunk));
    Status status = reader->Finish();
    EXPECT_EQ(status.error_code(), StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(absl::StrContains(status.error_message(), "hw.audioOutput=no"));
}

TEST_F(AudioServiceTest, StreamSpeakerAudioRejectsUnsupportedParameters) {
    {
        ClientContext context;
        StreamSpeakerAudioRequest request;
        request.set_sample_rate_hz(12345);
        auto reader = stub_->StreamSpeakerAudio(&context, request);
        AudioChunk chunk;
        EXPECT_FALSE(reader->Read(&chunk));
        Status status = reader->Finish();
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "sample_rate_hz=12345"));
    }
    {
        ClientContext context;
        StreamSpeakerAudioRequest request;
        request.set_channel_count(5);
        auto reader = stub_->StreamSpeakerAudio(&context, request);
        AudioChunk chunk;
        EXPECT_FALSE(reader->Read(&chunk));
        Status status = reader->Finish();
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "channel_count=5"));
    }
    {
        ClientContext context;
        StreamSpeakerAudioRequest request;
        request.set_format(static_cast<SampleFormat>(99));
        auto reader = stub_->StreamSpeakerAudio(&context, request);
        AudioChunk chunk;
        EXPECT_FALSE(reader->Read(&chunk));
        Status status = reader->Finish();
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "format=99"));
    }
}

// ============================================================================
// Microphone Injection Tests (InjectAudioClip & StreamMicrophoneAudio)
// ============================================================================

TEST_F(AudioServiceTest, InjectAudioClipEnforcesReadOnlyAndCapabilityPreconditions) {
    AudioChunk valid_chunk;
    valid_chunk.set_sample_rate_hz(16000);
    valid_chunk.set_channel_count(1);
    valid_chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    valid_chunk.set_pcm_data(MakePcm16Bytes({100, -100, 200, -200}));

    // Read-only mode -> PERMISSION_DENIED.
    {
        auto opts = MakeDefaultOptions();
        opts.read_only = true;
        AudioServiceImpl read_only_service(std::move(opts));

        ServerContext context;
        InjectAudioClipResponse reply;
        Status status = read_only_service.InjectAudioClip(&context, &valid_chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::PERMISSION_DENIED);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "ACCESS_MODE_READ_ONLY"));
    }

    // Audio input hardware disabled -> FAILED_PRECONDITION.
    {
        auto opts = MakeDefaultOptions();
        opts.has_audio_input = false;
        AudioServiceImpl disabled_service(std::move(opts));

        ServerContext context;
        InjectAudioClipResponse reply;
        Status status = disabled_service.InjectAudioClip(&context, &valid_chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::FAILED_PRECONDITION);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "hw.audioInput=no"));
    }

    // No microphone sink configured -> FAILED_PRECONDITION.
    {
        auto opts = MakeDefaultOptions();
        opts.microphone_sink = nullptr;
        AudioServiceImpl no_sink_service(std::move(opts));

        ServerContext context;
        InjectAudioClipResponse reply;
        Status status = no_sink_service.InjectAudioClip(&context, &valid_chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::FAILED_PRECONDITION);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "microphone_profile is unset"));
    }
}

TEST_F(AudioServiceTest, InjectAudioClipValidatesChunkParametersAndFrameAlignment) {
    ServerContext context;
    InjectAudioClipResponse reply;

    // Unsupported sample rate.
    {
        AudioChunk chunk;
        chunk.set_sample_rate_hz(0);
        chunk.set_channel_count(1);
        chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
        chunk.set_pcm_data(MakePcm16Bytes({1, 2}));
        Status status = service_->InjectAudioClip(&context, &chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
    }

    // Unsupported channel count.
    {
        AudioChunk chunk;
        chunk.set_sample_rate_hz(16000);
        chunk.set_channel_count(0);
        chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
        chunk.set_pcm_data(MakePcm16Bytes({1, 2}));
        Status status = service_->InjectAudioClip(&context, &chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
    }

    // Unspecified format.
    {
        AudioChunk chunk;
        chunk.set_sample_rate_hz(16000);
        chunk.set_channel_count(1);
        chunk.set_format(SampleFormat::SAMPLE_FORMAT_UNSPECIFIED);
        chunk.set_pcm_data(MakePcm16Bytes({1, 2}));
        Status status = service_->InjectAudioClip(&context, &chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
    }

    // Empty PCM buffer.
    {
        AudioChunk chunk;
        chunk.set_sample_rate_hz(16000);
        chunk.set_channel_count(1);
        chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
        Status status = service_->InjectAudioClip(&context, &chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
    }

    // Unaligned stereo PCM16 buffer (3 bytes is not a multiple of 4-byte stereo frame).
    {
        AudioChunk chunk;
        chunk.set_sample_rate_hz(16000);
        chunk.set_channel_count(2);
        chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
        chunk.set_pcm_data("abc");
        Status status = service_->InjectAudioClip(&context, &chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "multiple of the frame size"));
    }

    // Non-finite F32 sample.
    {
        AudioChunk chunk;
        chunk.set_sample_rate_hz(16000);
        chunk.set_channel_count(1);
        chunk.set_format(SampleFormat::SAMPLE_FORMAT_F32_BIT);
        chunk.set_pcm_data(MakeF32Bytes({0.25F, std::numeric_limits<float>::quiet_NaN()}));
        Status status = service_->InjectAudioClip(&context, &chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "non-finite float sample"));
    }
}

TEST_F(AudioServiceTest, InjectAudioClipSlicesAndPacesAudioAccordingToSampleRate) {
    // 800 frames at 16000 Hz = 50ms total clip duration (3 slices: 20ms + 20ms + 10ms).
    std::vector<int16_t> samples(800, 1024);
    AudioChunk chunk;
    chunk.set_sample_rate_hz(16000);
    chunk.set_channel_count(1);
    chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    chunk.set_pcm_data(MakePcm16Bytes(samples));

    ServerContext context;
    InjectAudioClipResponse reply;
    Status status = service_->InjectAudioClip(&context, &chunk, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(reply.duration_ms(), 50U);

    const std::vector<AudioChunk> slices = GetInjectedChunks();
    ASSERT_EQ(slices.size(), 3U);
    EXPECT_EQ(slices[0].timestamp_nanos(), 1'000'000'000LL);
    EXPECT_EQ(slices[1].timestamp_nanos(), 1'020'000'000LL);
    EXPECT_EQ(slices[2].timestamp_nanos(), 1'040'000'000LL);
    EXPECT_EQ(slices[0].pcm_data().size(), 320U * sizeof(int16_t));
    EXPECT_EQ(slices[1].pcm_data().size(), 320U * sizeof(int16_t));
    EXPECT_EQ(slices[2].pcm_data().size(), 160U * sizeof(int16_t));
}

TEST_F(AudioServiceTest, StreamMicrophoneAudioRebasesClientTimestampsAndStreamsChunks) {
    ClientContext context;
    StreamMicrophoneAudioResponse response;
    auto writer = stub_->StreamMicrophoneAudio(&context, &response);

    AudioChunk first;
    first.set_sample_rate_hz(48000);
    first.set_channel_count(2);
    first.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    first.set_timestamp_nanos(500'000'000LL);
    first.set_pcm_data(MakePcm16Bytes({10, -10, 20, -20}));
    ASSERT_TRUE(writer->Write(first));

    AudioChunk second = first;
    second.set_timestamp_nanos(520'000'000LL);
    ASSERT_TRUE(writer->Write(second));

    EXPECT_TRUE(writer->WritesDone());
    Status status = writer->Finish();
    ASSERT_TRUE(status.ok()) << status.error_message();

    const std::vector<AudioChunk> received = GetInjectedChunks();
    ASSERT_EQ(received.size(), 2U);
    EXPECT_EQ(received[0].timestamp_nanos(), 1'000'000'000LL);
    EXPECT_EQ(received[1].timestamp_nanos(), 1'020'000'000LL);
}

TEST_F(AudioServiceTest, StreamMicrophoneAudioRejectsWhenReadOnlyOrUnsupported) {
    {
        auto opts = MakeDefaultOptions();
        opts.read_only = true;
        RecreateService(std::move(opts));

        ClientContext context;
        StreamMicrophoneAudioResponse response;
        auto writer = stub_->StreamMicrophoneAudio(&context, &response);
        writer->WritesDone();
        Status status = writer->Finish();
        EXPECT_EQ(status.error_code(), StatusCode::PERMISSION_DENIED);
    }
    {
        auto opts = MakeDefaultOptions();
        opts.microphone_sink = nullptr;
        RecreateService(std::move(opts));

        ClientContext context;
        StreamMicrophoneAudioResponse response;
        auto writer = stub_->StreamMicrophoneAudio(&context, &response);
        writer->WritesDone();
        Status status = writer->Finish();
        EXPECT_EQ(status.error_code(), StatusCode::FAILED_PRECONDITION);
    }
}

TEST_F(AudioServiceTest, StreamMicrophoneAudioFailsOnInvalidChunk) {
    ClientContext context;
    StreamMicrophoneAudioResponse response;
    auto writer = stub_->StreamMicrophoneAudio(&context, &response);

    AudioChunk invalid;
    invalid.set_sample_rate_hz(48000);
    invalid.set_channel_count(2);
    invalid.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    // Empty pcm_data -> INVALID_ARGUMENT.
    writer->Write(invalid);
    writer->WritesDone();

    Status status = writer->Finish();
    EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(absl::StrContains(status.error_message(), "pcm_data is empty"));
}

}  // namespace
}  // namespace goldfish::grpc::v2
