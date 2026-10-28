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

#include "goldfish/audio/audio_input_mux.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "emulator/plugin/vminterface/vm_lock.h"
#include "test_bql.h"

namespace goldfish::audio {
namespace {

class FakeHostAudioInput final : public HostAudioInputInterface {
  public:
    struct FakeHostVoice {
        std::string name;
        GuestAudioConfig config;
        GuestChannelVolume volume;
        GuestDataCallback on_host_data;
        bool active = false;
    };

    HostVoiceHandle OpenHostVoice(HostVoiceHandle existing, const char* name,
                                  const GuestAudioConfig& config,
                                  GuestDataCallback on_host_data) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        HostVoiceHandle result = nullptr;
        {
            absl::MutexLock lock(fake_mutex_);
            if (fail_open_in_) {
                if (existing != nullptr) {
                    for (auto it = voices_.begin(); it != voices_.end(); ++it) {
                        if (it->get() == existing) {
                            voices_.erase(it);
                            ++closed_voices_;
                            break;
                        }
                    }
                }
            } else if (existing != nullptr) {
                auto* v = static_cast<FakeHostVoice*>(existing);
                v->name = name ? name : "";
                v->config = config;
                v->on_host_data = std::move(on_host_data);
                result = v;
            } else {
                auto v = std::make_unique<FakeHostVoice>();
                v->name = name ? name : "";
                v->config = config;
                v->on_host_data = std::move(on_host_data);
                FakeHostVoice* raw = v.get();
                voices_.push_back(std::move(v));
                result = raw;
            }
        }
        return result;
    }

    void CloseHostVoice(HostVoiceHandle voice) override ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        for (auto it = voices_.begin(); it != voices_.end(); ++it) {
            if (it->get() == voice) {
                voices_.erase(it);
                ++closed_voices_;
                return;
            }
        }
    }

    void SetHostVoiceActive(HostVoiceHandle voice, bool active) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        auto* v = static_cast<FakeHostVoice*>(voice);
        v->active = active;
    }

    void SetHostVoiceVolume(HostVoiceHandle voice, const GuestChannelVolume& volume) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        auto* v = static_cast<FakeHostVoice*>(voice);
        v->volume = volume;
    }

    size_t ReadHostVoice(HostVoiceHandle /*voice*/, void* dst, size_t byte_count) override
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        const size_t n = std::min(byte_count, host_pcm_bytes_.size());
        if (n > 0) {
            std::memcpy(dst, host_pcm_bytes_.data(), n);
            host_pcm_bytes_.erase(host_pcm_bytes_.begin(),
                                  host_pcm_bytes_.begin() + static_cast<ptrdiff_t>(n));
        }
        return n;
    }

    void SetTimerArmed(bool armed) override ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        timer_armed_ = armed;
    }

    absl::Duration NowVirtualTime() const override ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return now_;
    }

    void AdvanceTime(absl::Duration delta) ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        now_ += delta;
    }

    void SetNow(absl::Duration now) ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        now_ = now;
    }

    void SetFailOpenIn(bool fail) ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        fail_open_in_ = fail;
    }

    void EnqueueHostS16Samples(const std::vector<int16_t>& samples)
            ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        GuestDataCallback cb;
        int avail = 0;
        {
            absl::MutexLock lock(fake_mutex_);
            const size_t old_size = host_pcm_bytes_.size();
            const size_t add_bytes = samples.size() * sizeof(int16_t);
            host_pcm_bytes_.resize(old_size + add_bytes);
            std::memcpy(host_pcm_bytes_.data() + old_size, samples.data(), add_bytes);
            if (!voices_.empty() && voices_[0]->active) {
                cb = voices_[0]->on_host_data;
                avail = static_cast<int>(add_bytes);
            }
        }
        if (cb && avail > 0) {
            cb(avail);
        }
    }

    bool IsTimerArmed() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return timer_armed_;
    }

    bool IsFirstHostVoiceActive() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return !voices_.empty() && voices_[0]->active;
    }

    size_t OpenVoiceCount() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return voices_.size();
    }

    size_t ClosedVoiceCount() const ABSL_LOCKS_EXCLUDED(fake_mutex_) {
        absl::MutexLock lock(fake_mutex_);
        return closed_voices_;
    }

  private:
    mutable absl::Mutex fake_mutex_;
    bool fail_open_in_ ABSL_GUARDED_BY(fake_mutex_) = false;
    bool timer_armed_ ABSL_GUARDED_BY(fake_mutex_) = false;
    absl::Duration now_ ABSL_GUARDED_BY(fake_mutex_) = absl::Seconds(1);
    std::vector<std::unique_ptr<FakeHostVoice>> voices_ ABSL_GUARDED_BY(fake_mutex_);
    std::vector<uint8_t> host_pcm_bytes_ ABSL_GUARDED_BY(fake_mutex_);
    size_t closed_voices_ ABSL_GUARDED_BY(fake_mutex_) = 0;
};

using android::goldfish::ScopedVmLock;
using android::goldfish::ScopedVmUnlock;

// Every test body runs with the VM lock held, as QEMU device callbacks do. Multithreaded tests
// release it with ScopedVmUnlock and take it per operation.
class AudioInputMuxTest : public ::testing::Test {
  protected:
    TestBql bql_;
    ScopedVmLock vm_lock_;
};

TEST_F(AudioInputMuxTest, DelegatesToHostMicrophoneWhenNoInjectorActive) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t));
                const size_t read = mux.ReadVoice(voice, buf.data(), buf.size() * sizeof(int16_t));
                buf.resize(read / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);
    EXPECT_FALSE(mux.IsVoiceActive(voice));
    EXPECT_FALSE(host.IsFirstHostVoiceActive());

    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(mux.IsVoiceActive(voice));
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    const std::vector<int16_t> host_samples = {101, -202, 303, -404};
    host.EnqueueHostS16Samples(host_samples);
    EXPECT_EQ(guest_received, host_samples);

    mux.CloseVoice(voice);
    EXPECT_EQ(host.OpenVoiceCount(), 0U);
}

TEST_F(AudioInputMuxTest, HotSwapsMidStreamBetweenHostMicAndPacedInjector) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                // Read in two split chunks (simulating virtio-snd ring-buffer wrap-around).
                const size_t total = static_cast<size_t>(avail_bytes);
                const size_t first = total / 3;
                const size_t aligned_first = first - (first % sizeof(int16_t));
                const size_t second = total - aligned_first;

                std::vector<uint8_t> raw(total, 0);
                const size_t r1 = mux.ReadVoice(voice, raw.data(), aligned_first);
                const size_t r2 = mux.ReadVoice(voice, raw.data() + r1, second);
                ASSERT_EQ(r1 + r2, total);
                const size_t count = total / sizeof(int16_t);
                const auto* s16 = reinterpret_cast<const int16_t*>(raw.data());
                guest_received.insert(guest_received.end(), s16, s16 + count);
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());

    int owner_tag = 1;
    std::vector<bool> active_events;
    absl::Status status = mux.RegisterInjector(
            &owner_tag,
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            [](void* dst, size_t max_frames) -> size_t {
                auto* out = static_cast<int16_t*>(dst);
                std::fill(out, out + max_frames, static_cast<int16_t>(7777));
                return max_frames;
            },
            [&active_events](bool active) { active_events.push_back(active); });
    ASSERT_TRUE(status.ok()) << status;
    // The guest was already recording, so registration reports it immediately.
    EXPECT_EQ(active_events, std::vector<bool>{true});
    EXPECT_FALSE(host.IsFirstHostVoiceActive());
    EXPECT_TRUE(host.IsTimerArmed());

    // Advance virtual clock by 10 ms -> 480 frames at 48 kHz.
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 480U);
    EXPECT_EQ(guest_received.front(), 7777);
    EXPECT_EQ(guest_received.back(), 7777);

    // Unregistering mid-stream restores the host microphone and disarms the virtual timer.
    mux.UnregisterInjector(&owner_tag);
    EXPECT_TRUE(host.IsFirstHostVoiceActive());
    EXPECT_FALSE(host.IsTimerArmed());

    guest_received.clear();
    host.EnqueueHostS16Samples({-1234, 5678});
    EXPECT_EQ(guest_received, (std::vector<int16_t>{-1234, 5678}));

    mux.SetVoiceActive(voice, false);
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, PacesSilenceWhenHostHasNoPhysicalMicAndUpgradesToInjector) {
    FakeHostAudioInput host;
    host.SetFailOpenIn(true);
    AudioInputMux mux(&host);

    std::vector<int16_t> guest_received;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice, &guest_received](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 99);
                const size_t r = mux.ReadVoice(voice, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                guest_received.insert(guest_received.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);

    mux.SetVoiceActive(voice, true);
    EXPECT_TRUE(host.IsTimerArmed());

    // 10 ms at 16 kHz -> 160 frames of paced silence even without a host mic or injector.
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 160U);
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    // Attach injector mid-stream -> delivers injected samples.
    guest_received.clear();
    int owner_tag = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner_tag,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(4242));
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(guest_received.size(), 160U);
    EXPECT_EQ(guest_received.front(), 4242);

    // Detach injector -> stays armed and returns to paced silence because host_voice is null.
    guest_received.clear();
    mux.UnregisterInjector(&owner_tag);
    EXPECT_TRUE(host.IsTimerArmed());
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    EXPECT_EQ(guest_received, std::vector<int16_t>(160, 0));

    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, RejectsSecondInjectorWithAlreadyExistsAndTracksActiveCallback) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<bool> active_events;
    int owner_a = 1;
    int owner_b = 2;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner_a,
                       AudioStreamConfig{
                           .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
                       [](void*, size_t) -> size_t { return 0; },
                       [&active_events](bool active) { active_events.push_back(active); })
                    .ok());

    absl::Status conflict = mux.RegisterInjector(
            &owner_b,
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            [](void*, size_t) -> size_t { return 0; }, nullptr);
    EXPECT_EQ(conflict.code(), absl::StatusCode::kAlreadyExists);

    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);

    mux.SetVoiceActive(voice, true);
    // Registering the same owner again is an idempotent no-op.
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner_a,
                       AudioStreamConfig{
                           .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
                       [](void*, size_t) -> size_t { return 0; }, nullptr)
                    .ok());
    mux.SetVoiceActive(voice, false);
    EXPECT_EQ(active_events, (std::vector<bool>{true, false}));

    mux.UnregisterInjector(&owner_a);
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, VmRunStateAndShutdownHandleSnapshotJumpAndLateInjectorTeardown) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
                       [](void*, size_t n) -> size_t { return n; }, nullptr)
                    .ok());
    EXPECT_TRUE(host.IsTimerArmed());

    // Pause VM -> disarms timer; snapshot load jumps clock backwards -> resume re-arms cleanly.
    mux.OnVmRunningChanged(false);
    EXPECT_FALSE(host.IsTimerArmed());
    host.SetNow(absl::Milliseconds(200));
    mux.OnVmRunningChanged(true);
    EXPECT_TRUE(host.IsTimerArmed());

    // Shutdown (simulating audio_cleanup before ~QemuAudioInject) disarms timer & closes host
    // voice.
    mux.Shutdown();
    EXPECT_TRUE(mux.IsShutdown());
    EXPECT_FALSE(host.IsTimerArmed());
    EXPECT_EQ(host.ClosedVoiceCount(), 1U);

    // Late UnregisterInjector after Shutdown is a safe no-op, while RegisterInjector fails with
    // FAILED_PRECONDITION because Shutdown() is terminal.
    mux.UnregisterInjector(&owner);
    absl::Status after_shutdown = mux.RegisterInjector(
            &owner,
            AudioStreamConfig{.sample_rate_hz = 48000, .channels = 1, .format = SampleFormat::kS16},
            [](void*, size_t) -> size_t { return 0; }, nullptr);
    EXPECT_EQ(after_shutdown.code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(AudioInputMuxTest, ConcurrentInjectorChurnAndReadsNeverInvokeCallbackAfterUnregister) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
            [&mux, &voice](int avail_bytes) {
                std::vector<uint8_t> buf(static_cast<size_t>(avail_bytes));
                mux.ReadVoice(voice, buf.data(), buf.size());
            });
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    ScopedVmUnlock unlock;
    std::atomic<bool> stop_threads{false};
    std::thread reader_thread([&mux, &host, &stop_threads]() {
        while (!stop_threads.load(std::memory_order_relaxed)) {
            {
                ScopedVmLock lock;
                host.AdvanceTime(absl::Milliseconds(10));
                mux.OnTimerTick();
            }
            std::this_thread::yield();
        }
    });

    for (int iter = 0; iter < 50; ++iter) {
        std::atomic<bool> unregistered{false};
        std::atomic<int> post_unregister_calls{0};
        int owner = iter;
        ScopedVmLock lock;
        ASSERT_TRUE(mux.RegisterInjector(
                               &owner,
                               AudioStreamConfig{.sample_rate_hz = 16000,
                                                 .channels = 1,
                                                 .format = SampleFormat::kS16},
                               [&unregistered, &post_unregister_calls](
                                       void* dst, size_t max_frames) -> size_t {
                                   if (unregistered.load(std::memory_order_acquire)) {
                                       post_unregister_calls.fetch_add(1,
                                                                       std::memory_order_relaxed);
                                   }
                                   std::memset(dst, 0, max_frames * sizeof(int16_t));
                                   return max_frames;
                               },
                               nullptr)
                            .ok());
        {
            ScopedVmUnlock let_reader_run;
            std::this_thread::yield();
        }
        mux.UnregisterInjector(&owner);
        unregistered.store(true, std::memory_order_release);
        EXPECT_EQ(post_unregister_calls.load(std::memory_order_relaxed), 0);
    }

    stop_threads.store(true, std::memory_order_relaxed);
    reader_thread.join();
    ScopedVmLock lock;
    mux.CloseVoice(voice);
}

TEST_F(AudioInputMuxTest, MultipleConcurrentGuestVoicesTrackAggregateActiveStateAndHostFallback) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    std::vector<int16_t> voice_a_samples;
    MuxVoiceHandle* voice_a = nullptr;
    voice_a = mux.OpenVoice(
            nullptr, "virtio-sound.in.0",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice_a, &voice_a_samples](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 77);
                const size_t r =
                        mux.ReadVoice(voice_a, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                voice_a_samples.insert(voice_a_samples.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice_a, nullptr);

    MuxVoiceHandle* voice_b = mux.OpenVoice(
            nullptr, "virtio-sound.in.1",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice_b, nullptr);
    EXPECT_EQ(host.OpenVoiceCount(), 2U);

    std::vector<bool> active_events;
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<int16_t*>(dst);
                           std::fill(out, out + max_frames, static_cast<int16_t>(1234));
                           return max_frames;
                       },
                       [&active_events](bool active) { active_events.push_back(active); })
                    .ok());

    // Activating the first voice fires ActiveCallback(true); activating the second voice does not
    // fire a duplicate true event.
    mux.SetVoiceActive(voice_a, true);
    EXPECT_EQ(active_events, (std::vector<bool>{true}));
    mux.SetVoiceActive(voice_b, true);
    EXPECT_EQ(active_events, (std::vector<bool>{true}));

    // Deactivating voice_a while voice_b remains active must NOT fire ActiveCallback(false).
    mux.SetVoiceActive(voice_a, false);
    EXPECT_EQ(active_events, (std::vector<bool>{true}));
    EXPECT_TRUE(host.IsTimerArmed());

    // Closing the last active voice (voice_b) transitions aggregate active state to false.
    mux.CloseVoice(voice_b);
    EXPECT_EQ(active_events, (std::vector<bool>{true, false}));
    EXPECT_FALSE(host.IsTimerArmed());

    mux.UnregisterInjector(&owner);

    // Simulate host reopen failure on existing voice_a: OpenHostVoice consumes the old host handle
    // and returns nullptr, so AudioInputMux must fall back to virtual-clock-paced silence.
    host.SetFailOpenIn(true);
    MuxVoiceHandle* reopened_a = mux.OpenVoice(
            voice_a, "virtio-sound.in.0",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [&mux, &voice_a, &voice_a_samples](int avail_bytes) {
                std::vector<int16_t> buf(static_cast<size_t>(avail_bytes) / sizeof(int16_t), 77);
                const size_t r =
                        mux.ReadVoice(voice_a, buf.data(), static_cast<size_t>(avail_bytes));
                buf.resize(r / sizeof(int16_t));
                voice_a_samples.insert(voice_a_samples.end(), buf.begin(), buf.end());
            });
    EXPECT_EQ(reopened_a, voice_a);
    EXPECT_EQ(host.OpenVoiceCount(), 0U);

    voice_a_samples.clear();
    mux.SetVoiceActive(voice_a, true);
    EXPECT_TRUE(host.IsTimerArmed());
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(voice_a_samples.size(), 160U);
    EXPECT_EQ(voice_a_samples, std::vector<int16_t>(160, 0));

    mux.CloseVoice(voice_a);
}

TEST_F(AudioInputMuxTest, HandlesBoundaryReads) {
    // Boundary ReadVoice calls (nullptr handle, nullptr buffer, 0 bytes, sub-frame bytes).
    {
        FakeHostAudioInput host;
        AudioInputMux mux(&host);

        MuxVoiceHandle* stereo_voice = mux.OpenVoice(
                nullptr, "virtio-sound.in",
                GuestAudioConfig{
                    .sample_rate_hz = 48000, .channels = 2, .format = GuestPcmFormat::kS16},
                [](int) {});
        ASSERT_NE(stereo_voice, nullptr);
        mux.SetVoiceActive(stereo_voice, true);

        uint8_t scratch[16] = {};
        EXPECT_EQ(mux.ReadVoice(nullptr, scratch, sizeof(scratch)), 0U);
        EXPECT_EQ(mux.ReadVoice(stereo_voice, nullptr, sizeof(scratch)), 0U);
        EXPECT_EQ(mux.ReadVoice(stereo_voice, scratch, 0), 0U);

        // Switch to paced mode and verify sub-frame ReadVoice (< 4 bytes for stereo S16) returns 0
        // without invoking the injector callback.
        size_t injector_calls = 0;
        int owner = 1;
        ASSERT_TRUE(mux.RegisterInjector(
                               &owner,
                               AudioStreamConfig{.sample_rate_hz = 48000,
                                                 .channels = 2,
                                                 .format = SampleFormat::kS16},
                               [&injector_calls](void* /*dst*/, size_t max_frames) -> size_t {
                                   ++injector_calls;
                                   return max_frames;
                               },
                               nullptr)
                            .ok());
        host.AdvanceTime(absl::Milliseconds(10));
        mux.OnTimerTick();
        EXPECT_EQ(mux.ReadVoice(stereo_voice, scratch, 3), 0U);
        EXPECT_EQ(injector_calls, 0U);

        mux.UnregisterInjector(&owner);
        mux.CloseVoice(stereo_voice);
    }
}

TEST_F(AudioInputMuxTest, MutationsWithoutTheVmLockFailTheLockCheck) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);
    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);

    ScopedVmUnlock unlock;
    EXPECT_DEBUG_DEATH(mux.SetVoiceActive(voice, true), "must hold the VM lock");
    EXPECT_DEBUG_DEATH(mux.OnTimerTick(), "must hold the VM lock");
    // IsShutdown() stays usable from any thread.
    EXPECT_FALSE(mux.IsShutdown());
}

TEST_F(AudioInputMuxTest, ReopeningVoiceAndUpdatingVolumeReconfiguresInjectedPipeline) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host);

    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 16000, .channels = 1, .format = SampleFormat::kS16},
                       [](void* dst, size_t max_frames) -> size_t {
                           auto* s = static_cast<int16_t*>(dst);
                           std::fill(s, s + max_frames, static_cast<int16_t>(16384));
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    MuxVoiceHandle* voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 1, .format = GuestPcmFormat::kS16},
            [](int) {});
    ASSERT_NE(voice, nullptr);
    mux.SetVoiceActive(voice, true);

    // Reopen existing voice as stereo F32 at 16 kHz and mute right channel via SetVoiceVolume.
    std::vector<float> stereo_out;
    MuxVoiceHandle* reopened = nullptr;
    reopened = mux.OpenVoice(
            voice, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 16000, .channels = 2, .format = GuestPcmFormat::kF32},
            [&mux, &reopened, &stereo_out](int avail_bytes) {
                std::vector<float> buf(static_cast<size_t>(avail_bytes) / sizeof(float));
                const size_t r = mux.ReadVoice(reopened, buf.data(), buf.size() * sizeof(float));
                buf.resize(r / sizeof(float));
                stereo_out.insert(stereo_out.end(), buf.begin(), buf.end());
            });
    EXPECT_EQ(reopened, voice);

    GuestChannelVolume vol;
    vol.channels = 2;
    vol.gain[0] = 255;
    vol.gain[1] = 0;
    mux.SetVoiceVolume(reopened, vol);

    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    // 10 ms at 16 kHz stereo -> 160 frames * 2 channels = 320 floats.
    ASSERT_EQ(stereo_out.size(), 320U);
    EXPECT_NEAR(stereo_out[0], 0.5F, 0.01F);
    EXPECT_FLOAT_EQ(stereo_out[1], 0.0F);

    mux.UnregisterInjector(&owner);
    mux.CloseVoice(reopened);
}

TEST_F(AudioInputMuxTest, ResetsResamplerDelayLineWhenGuestVoiceReactivates) {
    FakeHostAudioInput host;
    AudioInputMux mux(&host, /*vm_running=*/true);

    float injected_value = 1.0F;
    int owner = 1;
    ASSERT_TRUE(
            mux.RegisterInjector(
                       &owner,
                       AudioStreamConfig{
                           .sample_rate_hz = 44100, .channels = 1, .format = SampleFormat::kF32},
                       [&injected_value](void* dst, size_t max_frames) -> size_t {
                           auto* out = static_cast<float*>(dst);
                           std::fill(out, out + max_frames, injected_value);
                           return max_frames;
                       },
                       nullptr)
                    .ok());

    std::vector<float> captured;
    MuxVoiceHandle* voice = nullptr;
    voice = mux.OpenVoice(
            nullptr, "virtio-sound.in",
            GuestAudioConfig{
                .sample_rate_hz = 48000, .channels = 1, .format = GuestPcmFormat::kF32},
            [&mux, &voice, &captured](int avail_bytes) {
                std::vector<float> buf(static_cast<size_t>(avail_bytes) / sizeof(float));
                const size_t r = mux.ReadVoice(voice, buf.data(), buf.size() * sizeof(float));
                buf.resize(r / sizeof(float));
                captured.insert(captured.end(), buf.begin(), buf.end());
            });
    ASSERT_NE(voice, nullptr);

    // Session 1: saturate the sinc resampler delay line with +1.0F DC samples.
    mux.SetVoiceActive(voice, true);
    host.AdvanceTime(absl::Milliseconds(20));
    mux.OnTimerTick();
    ASSERT_EQ(captured.size(), 960U);
    EXPECT_NEAR(captured.back(), 1.0F, 0.02F);

    // Pause guest capture, switch source to silence, and reactivate guest capture.
    mux.SetVoiceActive(voice, false);
    injected_value = 0.0F;
    captured.clear();

    mux.SetVoiceActive(voice, true);
    host.AdvanceTime(absl::Milliseconds(10));
    mux.OnTimerTick();
    ASSERT_EQ(captured.size(), 480U);
    for (size_t i = 0; i < captured.size(); ++i) {
        EXPECT_FLOAT_EQ(captured[i], 0.0F) << "Stale resampler bleed at frame " << i;
    }

    mux.UnregisterInjector(&owner);
    mux.CloseVoice(voice);
}

}  // namespace
}  // namespace goldfish::audio
