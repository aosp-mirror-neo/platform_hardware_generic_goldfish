// Copyright 2026 The Android Open Source Project
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

#include "goldfish/wasapi/audio_wasapi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"

#include "pcm_converter.h"
#include "wasapi_win32.h"

// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"

extern "C" {
#include "audio/audio_int.h"
#include "qemu/module.h"
#include "qom/object.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace goldfish::wasapi {
namespace {

constexpr char kTypeAudioWasapi[] = "audio-wasapi";

constexpr REFERENCE_TIME kHnsPerSecond = 10'000'000LL;
constexpr REFERENCE_TIME kDefaultPeriodHns = 100'000LL;  // 10 ms in 100-ns units
constexpr DWORD kFallbackPollMs = 10U;
constexpr DWORD kStreamingWaitTimeoutMs = 2000U;
constexpr size_t kRingPeriodCount = 4U;
constexpr size_t kMinRingDurationMs = 40U;
constexpr size_t kMinRingFrames = 256U;
constexpr int kDefaultFallbackSampleRateHz = 48000;
constexpr int kMinMixengChannels = 1;
constexpr int kMaxMixengChannels = 2;
constexpr int64_t kNsPerSecond = 1'000'000'000LL;
constexpr int64_t kMaxFallbackStepNs = 100'000'000LL;  // Cap catch-up burst at 100 ms

bool TryToQemuAudioFormat(PcmSampleFormat fmt, AudioFormat* out) {
    switch (fmt) {
    case PcmSampleFormat::kU8:
        *out = AUDIO_FORMAT_U8;
        return true;
    case PcmSampleFormat::kS8:
        *out = AUDIO_FORMAT_S8;
        return true;
    case PcmSampleFormat::kS16:
        *out = AUDIO_FORMAT_S16;
        return true;
    case PcmSampleFormat::kS32:
        *out = AUDIO_FORMAT_S32;
        return true;
    case PcmSampleFormat::kF32:
        *out = AUDIO_FORMAT_F32;
        return true;
    case PcmSampleFormat::kS24Packed:
    case PcmSampleFormat::kS24In32:
    case PcmSampleFormat::kUnknown:
    default:
        return false;
    }
}

AudioFormat ToSafeQemuAudioFormat(AudioFormat fmt) {
    switch (fmt) {
    case AUDIO_FORMAT_S8:
    case AUDIO_FORMAT_U8:
    case AUDIO_FORMAT_S16:
    case AUDIO_FORMAT_U16:
        return AUDIO_FORMAT_S16;
    case AUDIO_FORMAT_S32:
    case AUDIO_FORMAT_U32:
        return AUDIO_FORMAT_S32;
    case AUDIO_FORMAT_F32:
    case AUDIO_FORMAT__MAX:
    default:
        return AUDIO_FORMAT_F32;
    }
}

PcmSampleFormat FromQemuAudioFormat(AudioFormat fmt) {
    switch (fmt) {
    case AUDIO_FORMAT_U8:
        return PcmSampleFormat::kU8;
    case AUDIO_FORMAT_S8:
        return PcmSampleFormat::kS8;
    case AUDIO_FORMAT_S16:
        return PcmSampleFormat::kS16;
    case AUDIO_FORMAT_S32:
        return PcmSampleFormat::kS32;
    case AUDIO_FORMAT_F32:
        return PcmSampleFormat::kF32;
    case AUDIO_FORMAT_U16:
    case AUDIO_FORMAT_U32:
    case AUDIO_FORMAT__MAX:
    default:
        return PcmSampleFormat::kUnknown;
    }
}

bool IsDeviceInvalidatedError(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return true;
    default:
        return false;
    }
}

struct RingBufferRefs {
    void** buf_emul = nullptr;
    size_t* pos_emul = nullptr;
    size_t* pending_emul = nullptr;
    size_t* size_emul = nullptr;
    size_t* samples = nullptr;
    audio_pcm_info* info = nullptr;
};

struct EndpointState {
    goldfish::base::IntrusivePtr<IMMDevice> device;
    goldfish::base::IntrusivePtr<IAudioClient> audio_client;
    goldfish::base::IntrusivePtr<IAudioRenderClient> render_client;
    goldfish::base::IntrusivePtr<IAudioCaptureClient> capture_client;
    PcmFormat hw_fmt{};
    REFERENCE_TIME default_period_hns = kDefaultPeriodHns;
    UINT32 buffer_frames = 0U;
    bool client_started = false;

    bool IsOpen() const { return static_cast<bool>(audio_client); }

    void Reset() {
        if (client_started && audio_client) {
            audio_client->Stop();
        }
        client_started = false;
        capture_client.reset();
        render_client.reset();
        audio_client.reset();
        device.reset();
        hw_fmt = PcmFormat{};
        default_period_hns = kDefaultPeriodHns;
        buffer_frames = 0U;
    }
};

class WasapiStream {
  public:
    WasapiStream(bool is_output, RingBufferRefs ring) : is_output_(is_output), ring_(ring) {}

    ~WasapiStream() {
        StopWorker();
        if (*ring_.buf_emul != nullptr) {
            g_free(*ring_.buf_emul);
            *ring_.buf_emul = nullptr;
            *ring_.size_emul = 0U;
            *ring_.samples = 0U;
        }
    }

    WasapiStream(const WasapiStream&) = delete;
    WasapiStream& operator=(const WasapiStream&) = delete;

    bool Initialize(const audsettings& mixeng_as) {
        requested_mixeng_as_ = mixeng_as;
        wake_event_ = android::base::ScopedEventHandle(
                ::CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE, nullptr));
        buffer_event_ = android::base::ScopedEventHandle(
                ::CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE, nullptr));
        if (!wake_event_.ok() || !buffer_event_.ok()) {
            const DWORD err = ::GetLastError();
            LOG(ERROR) << "Failed to create Win32 event handles for WASAPI audio "
                       << DirectionName() << ": " << android::base::Win32Utils::getErrorString(err);
            return false;
        }

        std::promise<void> init_done;
        std::future<void> init_future = init_done.get_future();
        worker_thread_ = std::thread(&WasapiStream::WorkerMain, this, std::move(init_done));
        init_future.get();
        return true;
    }

    void SetEnabled(bool enable) {
        desired_enabled_.store(enable, std::memory_order_release);
        if (wake_event_.ok()) {
            ::SetEvent(wake_event_.get());
        }
    }

    std::mutex& BufferMutex() { return buf_mutex_; }

  private:
    std::string_view DirectionName() const { return is_output_ ? "output" : "input"; }

    std::string_view DirectionDeviceNoun() const {
        return is_output_ ? "playback device (speakers/headphones)"
                          : "recording device (microphone)";
    }

    void StopWorker() {
        stopping_.store(true, std::memory_order_release);
        if (wake_event_.ok()) {
            ::SetEvent(wake_event_.get());
        }
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    void NotifyDeviceChanged() {
        device_changed_.store(true, std::memory_order_release);
        if (wake_event_.ok()) {
            ::SetEvent(wake_event_.get());
        }
    }

    void EnsureRingBufferInitialized(const EndpointState* ep) {
        if (ring_initialized_) {
            return;
        }

        audsettings chosen_as{};
        chosen_as.big_endian = false;

        if (ep != nullptr && ep->hw_fmt.IsValid()) {
            if (!TryToQemuAudioFormat(ep->hw_fmt.format, &chosen_as.fmt)) {
                chosen_as.fmt = AUDIO_FORMAT_F32;
            }
            chosen_as.freq = static_cast<int>(ep->hw_fmt.sample_rate);
            chosen_as.nchannels = std::clamp(std::min<int>(requested_mixeng_as_.nchannels,
                                                           static_cast<int>(ep->hw_fmt.channels)),
                                             kMinMixengChannels, kMaxMixengChannels);
        } else {
            chosen_as.freq = requested_mixeng_as_.freq > 0 ? requested_mixeng_as_.freq
                                                           : kDefaultFallbackSampleRateHz;
            chosen_as.fmt = ToSafeQemuAudioFormat(requested_mixeng_as_.fmt);
            chosen_as.nchannels = std::clamp(requested_mixeng_as_.nchannels, kMinMixengChannels,
                                             kMaxMixengChannels);
        }

        audio_pcm_init_info(ring_.info, &chosen_as);
        sw_fmt_ = PcmFormat{
            .format = FromQemuAudioFormat(ring_.info->af),
            .sample_rate = static_cast<uint32_t>(ring_.info->freq),
            .channels = static_cast<uint32_t>(ring_.info->nchannels),
        };

        const REFERENCE_TIME period_hns = (ep != nullptr && ep->default_period_hns > 0)
                                                  ? ep->default_period_hns
                                                  : kDefaultPeriodHns;
        const size_t period_frames = std::max<size_t>(
                1U, static_cast<size_t>((static_cast<uint64_t>(sw_fmt_.sample_rate) *
                                         static_cast<uint64_t>(period_hns)) /
                                        static_cast<uint64_t>(kHnsPerSecond)));
        const size_t min_duration_frames = std::max<size_t>(
                kMinRingFrames,
                (static_cast<size_t>(sw_fmt_.sample_rate) * kMinRingDurationMs) / 1000U);

        *ring_.samples = std::max(kRingPeriodCount * period_frames, min_duration_frames);
        *ring_.size_emul = (*ring_.samples) * static_cast<size_t>(ring_.info->bytes_per_frame);
        *ring_.buf_emul = g_malloc(*ring_.size_emul);
        FillSilenceFrames(*ring_.buf_emul, *ring_.samples, sw_fmt_);
        *ring_.pos_emul = 0U;
        *ring_.pending_emul = 0U;
        ring_initialized_ = true;
    }

    bool TryOpenEndpoint(IMMDeviceEnumerator* enumerator, EndpointState* ep, bool log_on_failure) {
        ep->Reset();
        if (enumerator == nullptr) {
            return false;
        }

        const EDataFlow data_flow = is_output_ ? eRender : eCapture;
        HRESULT hr = QueryComOutPtr(&ep->device, [&](IMMDevice** raw) {
            return enumerator->GetDefaultAudioEndpoint(data_flow, eConsole, raw);
        });
        if (FAILED(hr) || !ep->device) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: failed to get default audio endpoint: "
                           << FormatHresult(hr) << ". Emulator will continue without host audio "
                           << DirectionName() << "; connect or enable a Windows "
                           << DirectionDeviceNoun() << " to restore audio.";
            }
            return false;
        }

        hr = QueryComOutPtr(&ep->audio_client, [&](IAudioClient** raw) {
            return ep->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(raw));
        });
        if (FAILED(hr) || !ep->audio_client) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: IAudioClient activation failed: "
                           << FormatHresult(hr) << ". Emulator will continue without host audio "
                           << DirectionName() << ".";
            }
            ep->Reset();
            return false;
        }

        WAVEFORMATEX* raw_mix_fmt = nullptr;
        hr = ep->audio_client->GetMixFormat(&raw_mix_fmt);
        ScopedCoTaskMem<WAVEFORMATEX> mix_fmt(raw_mix_fmt);
        if (FAILED(hr) || !mix_fmt.ok()) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: IAudioClient::GetMixFormat failed: "
                           << FormatHresult(hr) << ". Emulator will continue without host audio "
                           << DirectionName() << ".";
            }
            ep->Reset();
            return false;
        }

        if (!ExtractWaveFormat(mix_fmt.get(), &ep->hw_fmt)) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: unsupported shared-mode mix format (tag="
                           << absl::StrFormat("0x%04X", mix_fmt.get()->wFormatTag)
                           << ", rate=" << mix_fmt.get()->nSamplesPerSec
                           << " Hz, channels=" << mix_fmt.get()->nChannels
                           << ", bits=" << mix_fmt.get()->wBitsPerSample
                           << "). Check the default device format in Windows Sound settings.";
            }
            ep->Reset();
            return false;
        }

        REFERENCE_TIME default_period = 0LL;
        REFERENCE_TIME min_period = 0LL;
        if (SUCCEEDED(ep->audio_client->GetDevicePeriod(&default_period, &min_period)) &&
            default_period > 0LL) {
            ep->default_period_hns = default_period;
        } else {
            ep->default_period_hns = kDefaultPeriodHns;
        }

        // Initialize QEMU's voice format directly from the endpoint's mix format on first open.
        EnsureRingBufferInitialized(ep);

        DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (ep->hw_fmt.sample_rate != sw_fmt_.sample_rate || ep->hw_fmt.format != sw_fmt_.format) {
            if (!ConfigureWaveFormatForSoftwareFormat(mix_fmt.get(), sw_fmt_)) {
                ep->Reset();
                return false;
            }
            ep->hw_fmt.sample_rate = sw_fmt_.sample_rate;
            ep->hw_fmt.format = sw_fmt_.format;
            stream_flags |=
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }

        hr = ep->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                          ep->default_period_hns, 0, mix_fmt.get(), nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR || hr == E_INVALIDARG) {
            hr = ep->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0,
                                              mix_fmt.get(), nullptr);
        }
        if (FAILED(hr)) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: IAudioClient::Initialize failed (rate="
                           << ep->hw_fmt.sample_rate << " Hz, channels=" << ep->hw_fmt.channels
                           << ", format=" << SampleFormatName(ep->hw_fmt.format)
                           << "): " << FormatHresult(hr)
                           << ". Emulator will continue without host audio " << DirectionName()
                           << ".";
            }
            ep->Reset();
            return false;
        }

        hr = ep->audio_client->SetEventHandle(buffer_event_.get());
        if (FAILED(hr)) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: IAudioClient::SetEventHandle failed: "
                           << FormatHresult(hr) << ".";
            }
            ep->Reset();
            return false;
        }

        hr = ep->audio_client->GetBufferSize(&ep->buffer_frames);
        if (FAILED(hr) || ep->buffer_frames == 0U) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: IAudioClient::GetBufferSize returned "
                           << ep->buffer_frames << " frames: " << FormatHresult(hr) << ".";
            }
            ep->Reset();
            return false;
        }

        if (is_output_) {
            hr = QueryComOutPtr(&ep->render_client, [&](IAudioRenderClient** raw) {
                return ep->audio_client->GetService(__uuidof(IAudioRenderClient),
                                                    reinterpret_cast<void**>(raw));
            });
        } else {
            hr = QueryComOutPtr(&ep->capture_client, [&](IAudioCaptureClient** raw) {
                return ep->audio_client->GetService(__uuidof(IAudioCaptureClient),
                                                    reinterpret_cast<void**>(raw));
            });
        }
        if (FAILED(hr) || (is_output_ ? !ep->render_client : !ep->capture_client)) {
            if (log_on_failure) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: IAudioClient::GetService failed: "
                           << FormatHresult(hr) << ".";
            }
            ep->Reset();
            return false;
        }

        return true;
    }

    bool UpdateEndpointRunningState(EndpointState* ep, bool want_running) {
        if (!ep->IsOpen() || ep->client_started == want_running) {
            return ep->IsOpen();
        }
        if (want_running) {
            if (is_output_) {
                // Prime render buffer before Start() to avoid an initial underrun click.
                const HRESULT prime_hr = PumpOutput(ep);
                if (FAILED(prime_hr)) {
                    HandleStreamFailure(ep, prime_hr, "IAudioRenderClient prime");
                    return false;
                }
            }
            const HRESULT hr = ep->audio_client->Start();
            if (FAILED(hr)) {
                HandleStreamFailure(ep, hr, "IAudioClient::Start");
                return false;
            }
            ep->client_started = true;
            return true;
        }

        const HRESULT hr = ep->audio_client->Stop();
        ep->client_started = false;
        if (FAILED(hr) && !IsDeviceInvalidatedError(hr)) {
            LOG(WARNING) << "WASAPI audio " << DirectionName() << " IAudioClient::Stop returned "
                         << FormatHresult(hr);
        }
        return true;
    }

    void HandleStreamFailure(EndpointState* ep, HRESULT hr, std::string_view stage) {
        if (IsDeviceInvalidatedError(hr)) {
            LOG(WARNING) << "Windows WASAPI audio " << DirectionName() << " device changed during "
                         << stage << ": " << FormatHresult(hr)
                         << ". Switching to current default endpoint.";
            device_changed_.store(true, std::memory_order_release);
        } else {
            LOG(ERROR) << "Windows WASAPI audio " << DirectionName() << " failed during " << stage
                       << ": " << FormatHresult(hr)
                       << ". Falling back to silent emulation until default device changes.";
        }
        ep->Reset();
    }

    HRESULT PumpOutput(EndpointState* ep) {
        UINT32 padding_frames = 0U;
        HRESULT hr = ep->audio_client->GetCurrentPadding(&padding_frames);
        if (FAILED(hr)) {
            return hr;
        }
        const UINT32 avail_frames =
                ep->buffer_frames > padding_frames ? (ep->buffer_frames - padding_frames) : 0U;
        if (avail_frames == 0U) {
            return S_OK;
        }

        BYTE* hw_data = nullptr;
        hr = ep->render_client->GetBuffer(avail_frames, &hw_data);
        if (FAILED(hr)) {
            return hr;
        }

        size_t produced = 0U;
        {
            std::lock_guard<std::mutex> lock(buf_mutex_);
            produced = ReadRingToHardwareChannelOnly(
                    static_cast<const uint8_t*>(*ring_.buf_emul), *ring_.size_emul, *ring_.pos_emul,
                    ring_.pending_emul, static_cast<uint8_t*>(hw_data), avail_frames, sw_fmt_,
                    ep->hw_fmt.channels);
        }

        const DWORD release_flags = (produced == 0U) ? AUDCLNT_BUFFERFLAGS_SILENT : 0U;
        return ep->render_client->ReleaseBuffer(avail_frames, release_flags);
    }

    HRESULT PumpInput(EndpointState* ep) {
        for (;;) {
            UINT32 packet_frames = 0U;
            HRESULT hr = ep->capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                return hr;
            }
            if (packet_frames == 0U) {
                return S_OK;
            }

            BYTE* hw_data = nullptr;
            UINT32 num_frames = 0U;
            DWORD flags = 0U;
            hr = ep->capture_client->GetBuffer(&hw_data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                return hr;
            }

            if (num_frames > 0U) {
                const bool hw_is_silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U;
                std::lock_guard<std::mutex> lock(buf_mutex_);
                WriteHardwareToRingChannelOnly(static_cast<const uint8_t*>(hw_data), num_frames,
                                               hw_is_silent, static_cast<uint8_t*>(*ring_.buf_emul),
                                               *ring_.size_emul, ring_.pos_emul, ring_.pending_emul,
                                               sw_fmt_, ep->hw_fmt.channels);
            }

            hr = ep->capture_client->ReleaseBuffer(num_frames);
            if (FAILED(hr)) {
                return hr;
            }
        }
    }

    void RunDisconnectedFallbackTick(std::chrono::steady_clock::time_point* last_tick,
                                     int64_t* rem_scaled_ns) {
        const auto now = std::chrono::steady_clock::now();
        const int64_t elapsed_ns = std::clamp<int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - *last_tick).count(), 0LL,
                kMaxFallbackStepNs);
        *last_tick = now;

        const int64_t total_scaled =
                elapsed_ns * static_cast<int64_t>(sw_fmt_.sample_rate) + *rem_scaled_ns;
        const auto frames = static_cast<size_t>(total_scaled / kNsPerSecond);
        *rem_scaled_ns = total_scaled % kNsPerSecond;
        if (frames == 0U) {
            return;
        }

        std::lock_guard<std::mutex> lock(buf_mutex_);
        if (is_output_) {
            const size_t drain_bytes = frames * sw_fmt_.BytesPerFrame();
            *ring_.pending_emul -= std::min(*ring_.pending_emul, drain_bytes);
        } else {
            WriteHardwareToRingChannelOnly(/*hw_src=*/nullptr, frames, /*hw_is_silent=*/true,
                                           static_cast<uint8_t*>(*ring_.buf_emul), *ring_.size_emul,
                                           ring_.pos_emul, ring_.pending_emul, sw_fmt_,
                                           sw_fmt_.channels);
        }
    }

    void WorkerMain(std::promise<void> init_done) {
        const android::base::ScopedComInitializer com_init(COINIT_MULTITHREADED);
        DWORD avrt_task_index = 0U;
        ScopedAvrtHandle avrt_handle(
                ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrt_task_index));

        goldfish::base::IntrusivePtr<IMMDeviceEnumerator> enumerator;
        goldfish::base::IntrusivePtr<WasapiNotificationClient> notification_client;
        bool notification_registered = false;

        if (!com_init.ok()) {
            LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                       << " is unavailable: CoInitializeEx failed: " << FormatHresult(com_init.hr())
                       << ". Emulator will continue without host audio " << DirectionName() << ".";
        } else {
            const HRESULT enum_hr = QueryComOutPtr(&enumerator, [&](IMMDeviceEnumerator** raw) {
                return ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(raw));
            });
            if (FAILED(enum_hr) || !enumerator) {
                LOG(ERROR) << "Windows WASAPI audio " << DirectionName()
                           << " is unavailable: failed to create IMMDeviceEnumerator: "
                           << FormatHresult(enum_hr)
                           << ". Check that the Windows Audio service is enabled.";
            } else {
                notification_client = goldfish::base::IntrusivePtr<WasapiNotificationClient>(
                        new WasapiNotificationClient(is_output_ ? eRender : eCapture,
                                                     [this]() { NotifyDeviceChanged(); }));
                const HRESULT reg_hr =
                        enumerator->RegisterEndpointNotificationCallback(notification_client.get());
                if (SUCCEEDED(reg_hr)) {
                    notification_registered = true;
                } else {
                    LOG(WARNING) << "WASAPI audio " << DirectionName()
                                 << " could not register endpoint change listener: "
                                 << FormatHresult(reg_hr);
                }
            }
        }

        EndpointState ep;
        const bool opened_at_init =
                TryOpenEndpoint(enumerator.get(), &ep, /*log_on_failure=*/enumerator != nullptr);
        EnsureRingBufferInitialized(opened_at_init ? &ep : nullptr);
        bool had_working_endpoint = opened_at_init;

        init_done.set_value();

        auto fallback_last_tick = std::chrono::steady_clock::now();
        int64_t fallback_rem_scaled_ns = 0LL;
        bool was_enabled = false;

        while (!stopping_.load(std::memory_order_acquire)) {
            if (device_changed_.exchange(false, std::memory_order_acq_rel)) {
                ep.Reset();
                if (TryOpenEndpoint(enumerator.get(), &ep,
                                    /*log_on_failure=*/had_working_endpoint)) {
                    had_working_endpoint = true;
                    LOG(INFO) << "Windows WASAPI audio " << DirectionName()
                              << " connected to default endpoint (" << ep.hw_fmt.sample_rate
                              << " Hz, " << ep.hw_fmt.channels << " ch, "
                              << SampleFormatName(ep.hw_fmt.format) << ")";
                } else {
                    had_working_endpoint = false;
                    fallback_last_tick = std::chrono::steady_clock::now();
                    fallback_rem_scaled_ns = 0LL;
                }
            }

            const bool want_enabled = desired_enabled_.load(std::memory_order_acquire);
            if (want_enabled && !was_enabled) {
                fallback_last_tick = std::chrono::steady_clock::now();
                fallback_rem_scaled_ns = 0LL;
            }
            was_enabled = want_enabled;

            if (ep.IsOpen()) {
                UpdateEndpointRunningState(&ep, want_enabled);
            }

            if (want_enabled && ep.IsOpen() && ep.client_started) {
                const HANDLE wait_handles[2] = {wake_event_.get(), buffer_event_.get()};
                const DWORD wait_rc = ::WaitForMultipleObjects(2U, wait_handles, /*bWaitAll=*/FALSE,
                                                               kStreamingWaitTimeoutMs);
                if (stopping_.load(std::memory_order_acquire)) {
                    break;
                }
                if (wait_rc == WAIT_OBJECT_0 + 1U || wait_rc == WAIT_TIMEOUT) {
                    const HRESULT io_hr = is_output_ ? PumpOutput(&ep) : PumpInput(&ep);
                    if (FAILED(io_hr)) {
                        HandleStreamFailure(&ep, io_hr, is_output_ ? "PumpOutput" : "PumpInput");
                        fallback_last_tick = std::chrono::steady_clock::now();
                        fallback_rem_scaled_ns = 0LL;
                    }
                }
            } else if (want_enabled) {
                ::WaitForSingleObject(wake_event_.get(), kFallbackPollMs);
                if (!stopping_.load(std::memory_order_acquire) &&
                    desired_enabled_.load(std::memory_order_acquire) && !ep.IsOpen()) {
                    RunDisconnectedFallbackTick(&fallback_last_tick, &fallback_rem_scaled_ns);
                }
            } else {
                ::WaitForSingleObject(wake_event_.get(), INFINITE);
            }
        }

        ep.Reset();
        if (notification_registered && enumerator && notification_client) {
            enumerator->UnregisterEndpointNotificationCallback(notification_client.get());
        }
    }

    bool is_output_;
    RingBufferRefs ring_;
    audsettings requested_mixeng_as_{};
    bool ring_initialized_ = false;
    PcmFormat sw_fmt_{};
    std::mutex buf_mutex_;
    android::base::ScopedEventHandle wake_event_;
    android::base::ScopedEventHandle buffer_event_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> desired_enabled_{false};
    std::atomic<bool> device_changed_{false};
    std::thread worker_thread_;
};

struct AudioWasapi {
    AudioMixengBackend parent_obj;
};

struct WasapiVoiceOut {
    HWVoiceOut hw;
    WasapiStream* stream;
};
static_assert(offsetof(WasapiVoiceOut, hw) == 0U);

struct WasapiVoiceIn {
    HWVoiceIn hw;
    WasapiStream* stream;
};
static_assert(offsetof(WasapiVoiceIn, hw) == 0U);

int WasapiInitOut(HWVoiceOut* hw, struct audsettings* as) {
    auto* voice = reinterpret_cast<WasapiVoiceOut*>(hw);
    const RingBufferRefs refs{
        .buf_emul = &hw->buf_emul,
        .pos_emul = &hw->pos_emul,
        .pending_emul = &hw->pending_emul,
        .size_emul = &hw->size_emul,
        .samples = &hw->samples,
        .info = &hw->info,
    };
    auto stream = std::make_unique<WasapiStream>(/*is_output=*/true, refs);
    if (!stream->Initialize(*as)) {
        return -1;
    }
    voice->stream = stream.release();
    return 0;
}

void WasapiFiniOut(HWVoiceOut* hw) {
    auto* voice = reinterpret_cast<WasapiVoiceOut*>(hw);
    delete voice->stream;
    voice->stream = nullptr;
}

void WasapiEnableOut(HWVoiceOut* hw, bool enable) {
    auto* voice = reinterpret_cast<WasapiVoiceOut*>(hw);
    if (voice->stream != nullptr) {
        voice->stream->SetEnabled(enable);
    }
}

size_t WasapiBufferGetFree(HWVoiceOut* hw) {
    auto* voice = reinterpret_cast<WasapiVoiceOut*>(hw);
    std::lock_guard<std::mutex> lock(voice->stream->BufferMutex());
    return audio_generic_buffer_get_free(hw);
}

void* WasapiGetBufferOut(HWVoiceOut* hw, size_t* size) {
    auto* voice = reinterpret_cast<WasapiVoiceOut*>(hw);
    std::lock_guard<std::mutex> lock(voice->stream->BufferMutex());
    return audio_generic_get_buffer_out(hw, size);
}

size_t WasapiPutBufferOut(HWVoiceOut* hw, void* buf, size_t size) {
    auto* voice = reinterpret_cast<WasapiVoiceOut*>(hw);
    std::lock_guard<std::mutex> lock(voice->stream->BufferMutex());
    return audio_generic_put_buffer_out(hw, buf, size);
}

int WasapiInitIn(HWVoiceIn* hw, struct audsettings* as) {
    auto* voice = reinterpret_cast<WasapiVoiceIn*>(hw);
    const RingBufferRefs refs{
        .buf_emul = &hw->buf_emul,
        .pos_emul = &hw->pos_emul,
        .pending_emul = &hw->pending_emul,
        .size_emul = &hw->size_emul,
        .samples = &hw->samples,
        .info = &hw->info,
    };
    auto stream = std::make_unique<WasapiStream>(/*is_output=*/false, refs);
    if (!stream->Initialize(*as)) {
        return -1;
    }
    voice->stream = stream.release();
    return 0;
}

void WasapiFiniIn(HWVoiceIn* hw) {
    auto* voice = reinterpret_cast<WasapiVoiceIn*>(hw);
    delete voice->stream;
    voice->stream = nullptr;
}

void WasapiEnableIn(HWVoiceIn* hw, bool enable) {
    auto* voice = reinterpret_cast<WasapiVoiceIn*>(hw);
    if (voice->stream != nullptr) {
        voice->stream->SetEnabled(enable);
    }
}

void* WasapiGetBufferIn(HWVoiceIn* hw, size_t* size) {
    auto* voice = reinterpret_cast<WasapiVoiceIn*>(hw);
    std::lock_guard<std::mutex> lock(voice->stream->BufferMutex());
    return audio_generic_get_buffer_in(hw, size);
}

void WasapiPutBufferIn(HWVoiceIn* hw, void* buf, size_t size) {
    auto* voice = reinterpret_cast<WasapiVoiceIn*>(hw);
    std::lock_guard<std::mutex> lock(voice->stream->BufferMutex());
    audio_generic_put_buffer_in(hw, buf, size);
}

void AudioWasapiClassInit(ObjectClass* klass, const void* /*data*/) {
    auto* backend_class = AUDIO_MIXENG_BACKEND_CLASS(klass);
    backend_class->max_voices_out = INT_MAX;
    backend_class->max_voices_in = INT_MAX;
    backend_class->voice_size_out = sizeof(WasapiVoiceOut);
    backend_class->voice_size_in = sizeof(WasapiVoiceIn);

    backend_class->init_out = WasapiInitOut;
    backend_class->fini_out = WasapiFiniOut;
    backend_class->write = audio_generic_write;
    backend_class->buffer_get_free = WasapiBufferGetFree;
    backend_class->get_buffer_out = WasapiGetBufferOut;
    backend_class->put_buffer_out = WasapiPutBufferOut;
    backend_class->enable_out = WasapiEnableOut;

    backend_class->init_in = WasapiInitIn;
    backend_class->fini_in = WasapiFiniIn;
    backend_class->read = audio_generic_read;
    backend_class->run_buffer_in = nullptr;
    backend_class->get_buffer_in = WasapiGetBufferIn;
    backend_class->put_buffer_in = WasapiPutBufferIn;
    backend_class->enable_in = WasapiEnableIn;
}

const TypeInfo kAudioWasapiTypeInfo = [] {
    TypeInfo info{};
    info.name = kTypeAudioWasapi;
    info.parent = TYPE_AUDIO_MIXENG_BACKEND;
    info.instance_size = sizeof(AudioWasapi);
    info.class_init = AudioWasapiClassInit;
    return info;
}();

}  // namespace

void WasapiRegisterTypes() {
    type_register_static(&kAudioWasapiTypeInfo);
}

}  // namespace goldfish::wasapi
