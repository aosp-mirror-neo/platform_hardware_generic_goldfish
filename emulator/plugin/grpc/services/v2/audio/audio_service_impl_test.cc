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

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
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
#include "emulator/plugin/audio/test_bql.h"
#include "emulator/plugin/audio/test_qemu_audio_stub.h"
#include "emulator/plugin/vminterface/vm_lock.h"
#include "goldfish/audio/audio_input_mux.h"
#include "goldfish/audio/pcm_format.h"
#include "goldfish/audio/qemu_audio_inject.h"
#include "test/grpc_v2_service_test.h"

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
AudioBackend* audio_be_by_name(const char* name, void* errp);
}

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::AudioChunk;
using ::android::emulation::v2::AudioService;
using ::android::emulation::v2::InjectAudioClipResponse;
using ::android::emulation::v2::SampleFormat;
using ::android::emulation::v2::StreamMicrophoneAudioResponse;
using ::android::emulation::v2::StreamSpeakerAudioRequest;
using ::goldfish::audio::AudioInputMux;
using ::goldfish::audio::AudioStreamConfig;
using ::goldfish::audio::GuestAudioConfig;
using ::goldfish::audio::GuestChannelVolume;
using ::goldfish::audio::GuestPcmFormat;
using ::goldfish::audio::HostAudioInputInterface;
using ::goldfish::audio::MuxVoiceHandle;
using ::goldfish::audio::QemuAudioInject;
using ::goldfish::audio::RegisterAudioInputMux;
using ::goldfish::audio::UnregisterAudioInputMux;
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
    void StartWrite(const W* msg) ABSL_LOCKS_EXCLUDED(reactor_state_mutex_) {
        absl::MutexLock reactor_lock(reactor_state_mutex_);
        test::FakeWriteReactor<W>::StartWrite(msg);
    }

    void Finish(Status status) ABSL_LOCKS_EXCLUDED(reactor_state_mutex_) {
        absl::MutexLock reactor_lock(reactor_state_mutex_);
        test::FakeWriteReactor<W>::Finish(std::move(status));
    }

    bool IsFinishedThreadSafe() const ABSL_LOCKS_EXCLUDED(reactor_state_mutex_) {
        absl::MutexLock reactor_lock(reactor_state_mutex_);
        return this->finish_called;
    }

    Status FinishStatusThreadSafe() const ABSL_LOCKS_EXCLUDED(reactor_state_mutex_) {
        absl::MutexLock reactor_lock(reactor_state_mutex_);
        return this->finish_status;
    }

  private:
    mutable absl::Mutex reactor_state_mutex_;
};

template <typename R>
class FakeReadReactor {
  public:
    virtual ~FakeReadReactor() = default;
    virtual void OnReadDone(bool /*ok*/) {}
    virtual void OnDone() {}
    virtual void OnCancel() {}

    void StartRead(R* msg) {
        start_read_called = true;
        last_read_msg = msg;
    }

    void DeliverMessage(const R& msg) {
        if (last_read_msg == nullptr) {
            return;
        }
        *last_read_msg = msg;
        start_read_called = false;
        OnReadDone(true);
    }

    void Finish(Status status) {
        finish_called = true;
        finish_status = std::move(status);
    }

    bool start_read_called = false;
    bool finish_called = false;
    Status finish_status = Status::OK;
    R* last_read_msg = nullptr;
};

// Records whether the host microphone was already restored when the stream finished, so a test
// fails if Finish() ever unblocks the client before the injector is released.
// Templated like FakeReadReactor because WithSimpleReader reads the message type from it.
template <typename R>
class FinishOrderReadReactor : public FakeReadReactor<R> {
  public:
    void Finish(Status status) {
        host_active_at_finish = probe ? std::optional<bool>(probe()) : std::nullopt;
        FakeReadReactor<R>::Finish(std::move(status));
    }

    std::function<bool()> probe;
    std::optional<bool> host_active_at_finish;
};

class StubHostAudioInput final : public HostAudioInputInterface {
  public:
    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* /*name*/,
                                  const GuestAudioConfig& /*config*/,
                                  GuestDataCallback /*on_host_data*/) override {
        return existing != nullptr ? existing : &dummy_voice_;
    }

    void CloseHostVoice(HostVoiceHandle /*voice*/) override {}

    void SetHostVoiceActive(HostVoiceHandle /*voice*/, bool active) override
            ABSL_LOCKS_EXCLUDED(host_state_mutex_) {
        absl::MutexLock host_lock(host_state_mutex_);
        host_active_ = active;
    }

    void SetHostVoiceVolume(HostVoiceHandle /*voice*/,
                            const GuestChannelVolume& /*volume*/) override {}

    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override {
        std::memset(dst, 0, byte_count);
        return byte_count;
    }

    void SetTimerArmed(bool armed) override ABSL_LOCKS_EXCLUDED(host_state_mutex_) {
        absl::MutexLock host_lock(host_state_mutex_);
        timer_armed_ = armed;
    }

    absl::Duration NowVirtualTime() const override ABSL_LOCKS_EXCLUDED(host_state_mutex_) {
        absl::MutexLock host_lock(host_state_mutex_);
        return virtual_now_;
    }

    void Reset() ABSL_LOCKS_EXCLUDED(host_state_mutex_) {
        absl::MutexLock host_lock(host_state_mutex_);
        host_active_ = false;
        timer_armed_ = false;
        virtual_now_ = absl::Seconds(1);
    }

    void AdvanceVirtualTime(absl::Duration delta) ABSL_LOCKS_EXCLUDED(host_state_mutex_) {
        absl::MutexLock host_lock(host_state_mutex_);
        virtual_now_ += delta;
    }

    bool IsHostActive() const ABSL_LOCKS_EXCLUDED(host_state_mutex_) {
        absl::MutexLock host_lock(host_state_mutex_);
        return host_active_;
    }

  private:
    int dummy_voice_ = 1;
    mutable absl::Mutex host_state_mutex_;
    bool host_active_ ABSL_GUARDED_BY(host_state_mutex_) = false;
    bool timer_armed_ ABSL_GUARDED_BY(host_state_mutex_) = false;
    absl::Duration virtual_now_ ABSL_GUARDED_BY(host_state_mutex_) = absl::Seconds(1);
};

// Calls the mux the way QEMU device code does: with the VM lock held.
class VmLockedMux {
  public:
    explicit VmLockedMux(const std::shared_ptr<AudioInputMux>* mux) : mux_(mux) {}

    MuxVoiceHandle* OpenVoice(MuxVoiceHandle* existing, std::string_view name,
                              const GuestAudioConfig& config,
                              AudioInputMux::GuestDataCallback guest_cb) {
        android::goldfish::RecursiveScopedVmLock lock;
        return (*mux_)->OpenVoice(existing, name, config, std::move(guest_cb));
    }
    void CloseVoice(MuxVoiceHandle* voice) {
        android::goldfish::RecursiveScopedVmLock lock;
        (*mux_)->CloseVoice(voice);
    }
    void SetVoiceActive(MuxVoiceHandle* voice, bool active) {
        android::goldfish::RecursiveScopedVmLock lock;
        (*mux_)->SetVoiceActive(voice, active);
    }
    size_t ReadVoice(MuxVoiceHandle* voice, void* dst, size_t byte_count) {
        android::goldfish::RecursiveScopedVmLock lock;
        return (*mux_)->ReadVoice(voice, dst, byte_count);
    }
    void OnTimerTick() {
        android::goldfish::RecursiveScopedVmLock lock;
        (*mux_)->OnTimerTick();
    }
    void Shutdown() {
        android::goldfish::RecursiveScopedVmLock lock;
        (*mux_)->Shutdown();
    }

  private:
    const std::shared_ptr<AudioInputMux>* const mux_;
};

class AudioServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_reset_audio_stubs();
        host_input_.Reset();
        {
            absl::MutexLock clock_lock(clock_mu_);
            current_time_ = absl::FromUnixNanos(1'000'000'000LL);
            total_slept_ = absl::ZeroDuration();
        }
        RecreateService(MakeDefaultOptions());
    }

    void TearDown() override {
        ShutdownServer();
        service_.reset();
        TeardownDefaultAudioInputMux();
        test_reset_audio_stubs();
    }

    void InstallDefaultAudioInputMux() {
        TeardownDefaultAudioInputMux();
        default_backend_ = audio_be_by_name("mainaudiodev", nullptr);
        ASSERT_NE(default_backend_, nullptr);
        mux_ = std::make_shared<AudioInputMux>(&host_input_);
        RegisterAudioInputMux(default_backend_, mux_);
    }

    void TeardownDefaultAudioInputMux() {
        if (default_backend_ != nullptr) {
            UnregisterAudioInputMux(default_backend_);
            default_backend_ = nullptr;
        }
        if (mux_ != nullptr) {
            guest_mux_.Shutdown();
            mux_.reset();
        }
    }

    AudioServiceImpl::Options MakeDefaultOptions() {
        AudioServiceImpl::Options opts;
        opts.has_audio_output = true;
        opts.has_audio_input = true;
        opts.read_only = false;
        opts.now_fn = [this]() {
            absl::MutexLock clock_lock(clock_mu_);
            return current_time_;
        };
        opts.sleep_fn = [this](absl::Duration d) {
            absl::MutexLock clock_lock(clock_mu_);
            current_time_ += d;
            total_slept_ += d;
        };
        opts.microphone_sink = [this](const AudioChunk& chunk) {
            absl::MutexLock sink_lock(sink_mu_);
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
        absl::MutexLock clock_lock(clock_mu_);
        current_time_ += delta;
    }

    std::vector<AudioChunk> GetInjectedChunks() const {
        absl::MutexLock sink_lock(sink_mu_);
        return injected_chunks_;
    }

    void SetSinkStatus(absl::Status status) {
        absl::MutexLock sink_lock(sink_mu_);
        sink_status_ = std::move(status);
    }

    // Declared first so it outlives the service and the mux.
    ::goldfish::audio::TestBql bql_;

    mutable absl::Mutex clock_mu_;
    absl::Time current_time_ ABSL_GUARDED_BY(clock_mu_);
    absl::Duration total_slept_ ABSL_GUARDED_BY(clock_mu_);

    mutable absl::Mutex sink_mu_;
    std::vector<AudioChunk> injected_chunks_ ABSL_GUARDED_BY(sink_mu_);
    absl::Status sink_status_ ABSL_GUARDED_BY(sink_mu_) = absl::OkStatus();

    StubHostAudioInput host_input_;
    AudioBackend* default_backend_ = nullptr;
    std::shared_ptr<AudioInputMux> mux_;
    VmLockedMux guest_mux_{&mux_};

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

    // No GoldfishAudioBackend mux registered (e.g., -no-audio) -> FAILED_PRECONDITION.
    {
        auto opts = MakeDefaultOptions();
        opts.microphone_sink = nullptr;
        AudioServiceImpl no_mux_service(std::move(opts));

        ServerContext context;
        InjectAudioClipResponse reply;
        Status status = no_mux_service.InjectAudioClip(&context, &valid_chunk, &reply);
        EXPECT_EQ(status.error_code(), StatusCode::FAILED_PRECONDITION);
        EXPECT_TRUE(absl::StrContains(status.error_message(), "-no-audio"));
        EXPECT_TRUE(absl::StrContains(status.error_message(), "microphone_profile"));
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

// ============================================================================
// QemuAudioInject Integration & Concurrency Tests (Default Production Path)
// ============================================================================

TEST_F(AudioServiceTest, InjectAudioClipPullsFramesThroughQemuAudioInjectAndRestoresHostMic) {
    InstallDefaultAudioInputMux();

    std::vector<int16_t> guest_captured;
    MuxVoiceHandle* voice = nullptr;
    voice = guest_mux_.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [this, &voice, &guest_captured](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t read_bytes =
                        guest_mux_.ReadVoice(voice, buf.data(), buf.size() * sizeof(int16_t));
                buf.resize(read_bytes / sizeof(int16_t));
                guest_captured.insert(guest_captured.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    guest_mux_.SetVoiceActive(voice, true);
    EXPECT_TRUE(host_input_.IsHostActive());

    bool saw_host_mic_deactivated = false;
    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    opts.sleep_fn = [this, &saw_host_mic_deactivated](absl::Duration duration) {
        if (!host_input_.IsHostActive()) {
            saw_host_mic_deactivated = true;
        }
        absl::Duration remaining = duration;
        constexpr absl::Duration kTickStep = absl::Milliseconds(10);
        while (remaining > absl::ZeroDuration()) {
            const absl::Duration step = std::min(remaining, kTickStep);
            AdvanceClock(step);
            host_input_.AdvanceVirtualTime(step);
            guest_mux_.OnTimerTick();
            remaining -= step;
        }
    };
    RecreateService(std::move(opts));

    constexpr size_t kTotalFrames = 800;  // 50 ms at 16000 Hz.
    std::vector<int16_t> expected_samples(kTotalFrames);
    for (size_t i = 0; i < kTotalFrames; ++i) {
        expected_samples[i] = static_cast<int16_t>(i + 1);
    }

    AudioChunk clip;
    clip.set_sample_rate_hz(16000);
    clip.set_channel_count(1);
    clip.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    clip.set_pcm_data(MakePcm16Bytes(expected_samples));

    ServerContext context;
    InjectAudioClipResponse reply;
    Status status = service_->InjectAudioClip(&context, &clip, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(reply.duration_ms(), 50U);
    EXPECT_TRUE(saw_host_mic_deactivated);
    EXPECT_TRUE(host_input_.IsHostActive());
    EXPECT_EQ(guest_captured, expected_samples);

    guest_mux_.CloseVoice(voice);
}

TEST_F(AudioServiceTest, InjectAudioClipCancellationAbortsEarlyAndRestoresHostMicImmediately) {
    InstallDefaultAudioInputMux();

    MuxVoiceHandle* voice = guest_mux_.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int /*avail_bytes*/) {});
    ASSERT_NE(voice, nullptr);
    guest_mux_.SetVoiceActive(voice, true);
    EXPECT_TRUE(host_input_.IsHostActive());

    ClientContext client_context;
    int sleep_slice_count = 0;
    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    opts.sleep_fn = [this, &client_context, &sleep_slice_count](absl::Duration duration) {
        ++sleep_slice_count;
        AdvanceClock(duration);
        host_input_.AdvanceVirtualTime(duration);
        guest_mux_.OnTimerTick();
        // Cancel the RPC during the first 20 ms pacing slice of a 100 ms clip.
        if (sleep_slice_count == 1) {
            client_context.TryCancel();
        }
    };
    RecreateService(std::move(opts));

    // 1600 frames at 16000 Hz = 100 ms (5 slices of 20 ms).
    AudioChunk clip;
    clip.set_sample_rate_hz(16000);
    clip.set_channel_count(1);
    clip.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    clip.set_pcm_data(MakePcm16Bytes(std::vector<int16_t>(1600, 1024)));

    InjectAudioClipResponse reply;
    Status status = stub_->InjectAudioClip(&client_context, clip, &reply);
    EXPECT_EQ(status.error_code(), StatusCode::CANCELLED);
    // Must abort after the first 20 ms slice rather than sleeping all 5 slices.
    EXPECT_EQ(sleep_slice_count, 1);
    // Scoped QemuAudioInject teardown must immediately restore physical host mic passthrough.
    EXPECT_TRUE(host_input_.IsHostActive());

    guest_mux_.CloseVoice(voice);
}

TEST_F(AudioServiceTest, StreamMicrophoneAudioFeedsBoundedPullBufferAndReconfiguresFormat) {
    InstallDefaultAudioInputMux();

    std::vector<int16_t> guest_captured;
    MuxVoiceHandle* voice = nullptr;
    voice = guest_mux_.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [this, &voice, &guest_captured](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t read_bytes =
                        guest_mux_.ReadVoice(voice, buf.data(), buf.size() * sizeof(int16_t));
                buf.resize(read_bytes / sizeof(int16_t));
                guest_captured.insert(guest_captured.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    guest_mux_.SetVoiceActive(voice, true);
    EXPECT_TRUE(host_input_.IsHostActive());

    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    RecreateService(std::move(opts));

    auto* reader = service_->CreateMicrophoneAudioReader<FakeReadReactor<AudioChunk>>();
    ASSERT_FALSE(reader->finish_called);
    EXPECT_FALSE(host_input_.IsHostActive());

    // Stream 20 ms (320 frames) of 16 kHz mono PCM16 samples.
    AudioChunk pcm16_chunk;
    pcm16_chunk.set_sample_rate_hz(16000);
    pcm16_chunk.set_channel_count(1);
    pcm16_chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    pcm16_chunk.set_pcm_data(MakePcm16Bytes(std::vector<int16_t>(320, 2048)));
    reader->DeliverMessage(pcm16_chunk);
    ASSERT_FALSE(reader->finish_called);

    host_input_.AdvanceVirtualTime(absl::Milliseconds(20));
    guest_mux_.OnTimerTick();
    ASSERT_EQ(guest_captured.size(), 320U);
    EXPECT_EQ(guest_captured.front(), 2048);
    EXPECT_EQ(guest_captured.back(), 2048);

    // Reconfigure mid-stream to 16 kHz mono F32 (+0.5f -> ~16384 in S16).
    guest_captured.clear();
    AudioChunk f32_chunk;
    f32_chunk.set_sample_rate_hz(16000);
    f32_chunk.set_channel_count(1);
    f32_chunk.set_format(SampleFormat::SAMPLE_FORMAT_F32_BIT);
    f32_chunk.set_pcm_data(MakeF32Bytes(std::vector<float>(320, 0.5F)));
    reader->DeliverMessage(f32_chunk);
    ASSERT_FALSE(reader->finish_called);

    host_input_.AdvanceVirtualTime(absl::Milliseconds(20));
    guest_mux_.OnTimerTick();
    ASSERT_EQ(guest_captured.size(), 320U);
    EXPECT_NEAR(guest_captured.front(), 16384, 2);

    // Verify bounded ring buffer drops the oldest whole frames when exceeding 5 seconds.
    guest_captured.clear();
    const size_t one_second_frames = 16000;
    for (size_t sec = 0; sec < AudioServiceImpl::kMaxBufferedMicrophoneSeconds + 1; ++sec) {
        AudioChunk one_sec_chunk;
        one_sec_chunk.set_sample_rate_hz(16000);
        one_sec_chunk.set_channel_count(1);
        one_sec_chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
        const auto marker = static_cast<int16_t>((sec + 1) * 100);
        one_sec_chunk.set_pcm_data(MakePcm16Bytes(std::vector<int16_t>(one_second_frames, marker)));
        reader->DeliverMessage(one_sec_chunk);
        ASSERT_FALSE(reader->finish_called);
    }

    host_input_.AdvanceVirtualTime(absl::Milliseconds(10));
    guest_mux_.OnTimerTick();
    ASSERT_EQ(guest_captured.size(), 160U);
    // The first 1-second chunk (marker 100) was evicted; the oldest retained sample is 200.
    EXPECT_EQ(guest_captured.front(), 200);

    reader->OnDone();
    EXPECT_TRUE(host_input_.IsHostActive());
    guest_mux_.CloseVoice(voice);
}

TEST_F(AudioServiceTest, StreamMicrophoneAudioClientCancelReleasesMicrophoneImmediately) {
    InstallDefaultAudioInputMux();
    MuxVoiceHandle* voice = guest_mux_.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int /*avail_bytes*/) {});
    ASSERT_NE(voice, nullptr);
    guest_mux_.SetVoiceActive(voice, true);

    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    RecreateService(std::move(opts));

    auto* first = service_->CreateMicrophoneAudioReader<FakeReadReactor<AudioChunk>>();
    ASSERT_FALSE(first->finish_called);
    EXPECT_FALSE(host_input_.IsHostActive());
    AudioChunk chunk;
    chunk.set_sample_rate_hz(16000);
    chunk.set_channel_count(1);
    chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    chunk.set_pcm_data(MakePcm16Bytes(std::vector<int16_t>(320, 1000)));
    first->DeliverMessage(chunk);

    // The client vanishes (crash or network drop): gRPC delivers OnCancel without a final
    // OnReadDone(false). The host microphone must come back right away.
    first->OnCancel();
    EXPECT_TRUE(host_input_.IsHostActive());

    // The microphone is free for the next client even before the old reactor's OnDone runs.
    auto* second = service_->CreateMicrophoneAudioReader<FakeReadReactor<AudioChunk>>();
    EXPECT_FALSE(second->finish_called) << second->finish_status.error_message();
    EXPECT_FALSE(host_input_.IsHostActive());

    // The cancelled stream's late OnDone must not stop the new owner.
    first->OnDone();
    EXPECT_FALSE(host_input_.IsHostActive());

    second->OnDone();
    EXPECT_TRUE(host_input_.IsHostActive());
    guest_mux_.CloseVoice(voice);
}

TEST_F(AudioServiceTest, StreamMicrophoneAudioReleasesMicrophoneBeforeFinishing) {
    InstallDefaultAudioInputMux();
    MuxVoiceHandle* voice = guest_mux_.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int /*avail_bytes*/) {});
    ASSERT_NE(voice, nullptr);
    guest_mux_.SetVoiceActive(voice, true);

    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    RecreateService(std::move(opts));

    // Client half-close (WritesDone): Finish(OK) lets the client start its next stream, so the
    // previous injector must already be gone or that stream fails with ALREADY_EXISTS.
    auto* eof_reader = service_->CreateMicrophoneAudioReader<FinishOrderReadReactor<AudioChunk>>();
    eof_reader->probe = [this]() { return host_input_.IsHostActive(); };
    ASSERT_FALSE(host_input_.IsHostActive());
    eof_reader->OnReadDone(false);
    ASSERT_TRUE(eof_reader->finish_called);
    EXPECT_EQ(eof_reader->host_active_at_finish, std::optional<bool>(true));
    eof_reader->OnDone();

    // An invalid chunk ends the stream with an error; the same ordering applies.
    auto* bad_reader = service_->CreateMicrophoneAudioReader<FinishOrderReadReactor<AudioChunk>>();
    bad_reader->probe = [this]() { return host_input_.IsHostActive(); };
    ASSERT_FALSE(host_input_.IsHostActive());
    AudioChunk invalid;
    invalid.set_sample_rate_hz(16000);
    invalid.set_channel_count(1);
    invalid.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    bad_reader->DeliverMessage(invalid);  // Empty pcm_data -> INVALID_ARGUMENT.
    ASSERT_TRUE(bad_reader->finish_called);
    EXPECT_EQ(bad_reader->finish_status.error_code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(bad_reader->host_active_at_finish, std::optional<bool>(true));
    bad_reader->OnDone();

    guest_mux_.CloseVoice(voice);
}

TEST_F(AudioServiceTest, ConcurrentMicrophoneInjectionReturnsAlreadyExists) {
    InstallDefaultAudioInputMux();

    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    RecreateService(std::move(opts));

    AudioChunk chunk;
    chunk.set_sample_rate_hz(48000);
    chunk.set_channel_count(1);
    chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    chunk.set_pcm_data(MakePcm16Bytes(std::vector<int16_t>(480, 512)));

    // Hold an external QemuAudioInject session (e.g., WebRTC inbound audio track).
    QemuAudioInject external_injector([](void* /*dst*/, size_t max_frames) { return max_frames; },
                                      AudioStreamConfig{});
    ASSERT_TRUE(external_injector.Start().ok());

    // Both InjectAudioClip and StreamMicrophoneAudio must fail with ALREADY_EXISTS.
    {
        ServerContext clip_context;
        InjectAudioClipResponse clip_reply;
        Status clip_status = service_->InjectAudioClip(&clip_context, &chunk, &clip_reply);
        EXPECT_EQ(clip_status.error_code(), StatusCode::ALREADY_EXISTS);
        EXPECT_TRUE(absl::StrContains(clip_status.error_message(), "WebRTC"));
    }
    {
        ClientContext stream_context;
        StreamMicrophoneAudioResponse stream_reply;
        auto second_writer = stub_->StreamMicrophoneAudio(&stream_context, &stream_reply);
        second_writer->WritesDone();
        Status stream_status = second_writer->Finish();
        EXPECT_EQ(stream_status.error_code(), StatusCode::ALREADY_EXISTS);
    }

    // Releasing the external injector immediately allows StreamMicrophoneAudio, while a concurrent
    // second gRPC caller is rejected with ALREADY_EXISTS until the first stream closes.
    external_injector.Stop();

    auto* active_reader = service_->CreateMicrophoneAudioReader<FakeReadReactor<AudioChunk>>();
    ASSERT_FALSE(active_reader->finish_called);
    active_reader->DeliverMessage(chunk);
    ASSERT_FALSE(active_reader->finish_called);

    {
        ServerContext clip_context;
        InjectAudioClipResponse clip_reply;
        Status clip_status = service_->InjectAudioClip(&clip_context, &chunk, &clip_reply);
        EXPECT_EQ(clip_status.error_code(), StatusCode::ALREADY_EXISTS);
    }

    // Client half-close (OnReadDone(false)) must release microphone ownership BEFORE Finish(OK)
    // unblocks the client, even while gRPC's callback executor has not yet invoked OnDone().
    active_reader->OnReadDone(false);

    ServerContext clip_context;
    InjectAudioClipResponse clip_reply;
    Status clip_status = service_->InjectAudioClip(&clip_context, &chunk, &clip_reply);
    EXPECT_TRUE(clip_status.ok()) << clip_status.error_message();

    active_reader->OnDone();
}

TEST_F(AudioServiceTest, ConcurrentMicrophoneStreamingAndGuestPullAreRaceFree) {
    InstallDefaultAudioInputMux();

    MuxVoiceHandle* voice = nullptr;
    voice = guest_mux_.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
            [this, &voice](int avail_bytes) {
                std::vector<uint8_t> scratch(static_cast<size_t>(avail_bytes));
                (void)guest_mux_.ReadVoice(voice, scratch.data(), scratch.size());
            });
    ASSERT_NE(voice, nullptr);
    guest_mux_.SetVoiceActive(voice, true);

    auto opts = MakeDefaultOptions();
    opts.microphone_sink = nullptr;
    RecreateService(std::move(opts));

    std::atomic<bool> start_barrier{false};
    std::atomic<bool> stop_puller{false};

    std::thread guest_puller([this, &start_barrier, &stop_puller]() {
        while (!start_barrier.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!stop_puller.load(std::memory_order_acquire)) {
            host_input_.AdvanceVirtualTime(absl::Milliseconds(10));
            guest_mux_.OnTimerTick();
            std::this_thread::yield();
        }
    });

    start_barrier.store(true, std::memory_order_release);

    AudioChunk pcm16_chunk;
    pcm16_chunk.set_sample_rate_hz(48000);
    pcm16_chunk.set_channel_count(2);
    pcm16_chunk.set_format(SampleFormat::SAMPLE_FORMAT_PCM16_BIT);
    pcm16_chunk.set_pcm_data(MakePcm16Bytes(std::vector<int16_t>(960, 321)));

    AudioChunk f32_chunk;
    f32_chunk.set_sample_rate_hz(16000);
    f32_chunk.set_channel_count(1);
    f32_chunk.set_format(SampleFormat::SAMPLE_FORMAT_F32_BIT);
    f32_chunk.set_pcm_data(MakeF32Bytes(std::vector<float>(320, 0.25F)));

    for (int iter = 0; iter < 10; ++iter) {
        ClientContext context;
        StreamMicrophoneAudioResponse response;
        auto writer = stub_->StreamMicrophoneAudio(&context, &response);
        for (int c = 0; c < 6; ++c) {
            (void)writer->Write((c % 2 == 0) ? pcm16_chunk : f32_chunk);
        }
        writer->WritesDone();
        Status status = writer->Finish();
        EXPECT_TRUE(status.ok()) << status.error_message();
    }

    stop_puller.store(true, std::memory_order_release);
    guest_puller.join();
    guest_mux_.CloseVoice(voice);
}

}  // namespace
}  // namespace goldfish::grpc::v2
