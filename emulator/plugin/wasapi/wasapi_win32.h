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

#pragma once

#if !defined(_WIN32)
#error "Only compile this file when targeting Windows!"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef NOBITMAP
#define NOBITMAP 1
#endif

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "absl/strings/str_format.h"

#include "android/base/scoped_file_handle.h"
#include "android/base/win32_utils.h"
#include "goldfish/base/intrusive_ptr.h"
#include "goldfish/base/unique_handle.h"
#include "pcm_converter.h"

#ifndef GOLDFISH_IUNKNOWN_INTRUSIVE_PTR_HOOKS_DEFINED
#define GOLDFISH_IUNKNOWN_INTRUSIVE_PTR_HOOKS_DEFINED
inline void IntrusivePtrAddRef(IUnknown* ptr) {
    ptr->AddRef();
}
inline void IntrusivePtrRelease(IUnknown* ptr) {
    ptr->Release();
}
inline void IntrusivePtrCtor(IUnknown* /*ptr*/) {}
#endif

namespace goldfish::wasapi {

template <typename T>
struct CoTaskMemDeleter {
    struct Empty {};
    CoTaskMemDeleter() = default;
    explicit CoTaskMemDeleter(Empty /*empty*/) {}
    void operator()(T* ptr) const noexcept {
        if (ptr != nullptr) {
            ::CoTaskMemFree(ptr);
        }
    }
};

template <typename T>
using ScopedCoTaskMem = goldfish::base::UniqueHandle<T*, nullptr, CoTaskMemDeleter<T>>;

struct AvrtHandleDeleter {
    struct Empty {};
    AvrtHandleDeleter() = default;
    explicit AvrtHandleDeleter(Empty /*empty*/) {}
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr) {
            ::AvRevertMmThreadCharacteristics(handle);
        }
    }
};

using ScopedAvrtHandle = goldfish::base::UniqueHandle<HANDLE, nullptr, AvrtHandleDeleter>;

// RAII wrapper for per-thread COM initialization. Treats RPC_E_CHANGED_MODE as
// usable (the calling thread already initialized COM in another apartment)
// without calling CoUninitialize on destruction.
class ScopedComInitializer {
  public:
    explicit ScopedComInitializer(DWORD coinit = COINIT_MULTITHREADED)
            : hr_(::CoInitializeEx(nullptr, coinit))
            , should_uninitialize_(hr_ == S_OK || hr_ == S_FALSE) {}

    ~ScopedComInitializer() {
        if (should_uninitialize_) {
            ::CoUninitialize();
        }
    }

    ScopedComInitializer(const ScopedComInitializer&) = delete;
    ScopedComInitializer& operator=(const ScopedComInitializer&) = delete;
    ScopedComInitializer(ScopedComInitializer&&) = delete;
    ScopedComInitializer& operator=(ScopedComInitializer&&) = delete;

    bool Usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
    HRESULT Result() const noexcept { return hr_; }

  private:
    HRESULT hr_;
    bool should_uninitialize_;
};

// Adopts a newly returned COM interface pointer (refcount already incremented
// by the COM callee) into a goldfish::base::IntrusivePtr<Interface>.
template <typename Interface, typename Fn>
HRESULT QueryComOutPtr(goldfish::base::IntrusivePtr<Interface>* out, Fn&& fn) {
    out->reset();
    Interface* raw = nullptr;
    const HRESULT hr = fn(&raw);
    if (SUCCEEDED(hr) && raw != nullptr) {
        *out = goldfish::base::IntrusivePtr<Interface>(raw);
    } else if (raw != nullptr) {
        raw->Release();
    }
    return hr;
}

inline RawWaveFormatFields ExtractWaveFormatFields(const WAVEFORMATEX* wf) {
    if (wf == nullptr) {
        return RawWaveFormatFields{};
    }
    RawWaveFormatFields raw{
        .format_tag = wf->wFormatTag,
        .channels = wf->nChannels,
        .samples_per_sec = wf->nSamplesPerSec,
        .block_align = wf->nBlockAlign,
        .bits_per_sample = wf->wBitsPerSample,
        .cb_size = wf->cbSize,
    };
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        raw.valid_bits_per_sample = ext->Samples.wValidBitsPerSample;
        raw.subformat_is_pcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
        raw.subformat_is_ieee_float =
                IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    return raw;
}

inline bool ExtractWaveFormat(const WAVEFORMATEX* wf, PcmFormat* out) {
    if (wf == nullptr) {
        return false;
    }
    return ParseWaveFormat(ExtractWaveFormatFields(wf), out);
}

// Rewrites `wf` in-place to match `sw_fmt.sample_rate` and `sw_fmt.format`
// while preserving `wf->nChannels` (and `dwChannelMask` when extensible),
// for passing to `IAudioClient::Initialize` with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`.
inline bool ConfigureWaveFormatForSoftwareFormat(WAVEFORMATEX* wf, const PcmFormat& sw_fmt) {
    if (wf == nullptr || wf->nChannels == 0U || wf->nChannels > kMaxAudioChannels ||
        !sw_fmt.IsValid()) {
        return false;
    }

    WORD bits_per_sample = 0U;
    bool is_float = false;
    switch (sw_fmt.format) {
    case PcmSampleFormat::kU8:
        bits_per_sample = 8U;
        break;
    case PcmSampleFormat::kS16:
        bits_per_sample = 16U;
        break;
    case PcmSampleFormat::kS32:
        bits_per_sample = 32U;
        break;
    case PcmSampleFormat::kF32:
        bits_per_sample = 32U;
        is_float = true;
        break;
    case PcmSampleFormat::kS8:
    case PcmSampleFormat::kS24Packed:
    case PcmSampleFormat::kS24In32:
    case PcmSampleFormat::kUnknown:
    default:
        return false;
    }

    wf->nSamplesPerSec = sw_fmt.sample_rate;
    wf->wBitsPerSample = bits_per_sample;
    wf->nBlockAlign = static_cast<WORD>(wf->nChannels * (bits_per_sample / 8U));
    wf->nAvgBytesPerSec = wf->nSamplesPerSec * static_cast<DWORD>(wf->nBlockAlign);

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
        ext->Samples.wValidBitsPerSample = bits_per_sample;
        ext->SubFormat = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    } else {
        wf->wFormatTag = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        wf->cbSize = 0U;
    }
    return true;
}

inline std::string FormatHresult(HRESULT hr) {
    const char* name = nullptr;
    switch (hr) {
    case HRESULT_FROM_WIN32(ERROR_NOT_FOUND):
        name = "E_NOTFOUND (no audio endpoint connected or enabled)";
        break;
    case AUDCLNT_E_DEVICE_INVALIDATED:
        name = "AUDCLNT_E_DEVICE_INVALIDATED (audio endpoint disconnected or reconfigured)";
        break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        name = "AUDCLNT_E_UNSUPPORTED_FORMAT";
        break;
    case AUDCLNT_E_DEVICE_IN_USE:
        name = "AUDCLNT_E_DEVICE_IN_USE (endpoint locked in exclusive mode)";
        break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        name = "AUDCLNT_E_SERVICE_NOT_RUNNING (Windows Audio service is not running)";
        break;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        name = "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        break;
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        name = "AUDCLNT_E_RESOURCES_INVALIDATED";
        break;
    case AUDCLNT_E_BUFFER_SIZE_ERROR:
        name = "AUDCLNT_E_BUFFER_SIZE_ERROR";
        break;
    default:
        break;
    }
    if (name != nullptr) {
        return absl::StrFormat("0x%08X (%s)", static_cast<uint32_t>(hr), name);
    }
    return absl::StrFormat("0x%08X (%s)", static_cast<uint32_t>(hr),
                           android::base::Win32Utils::getErrorString(static_cast<DWORD>(hr)));
}

// Lightweight COM IMMNotificationClient that notifies a voice worker when the
// default eConsole endpoint for its EDataFlow (eRender / eCapture) changes.
// Managed via goldfish::base::IntrusivePtr<WasapiNotificationClient>.
class WasapiNotificationClient final : public IMMNotificationClient {
  public:
    WasapiNotificationClient(EDataFlow target_flow, std::function<void()> on_default_changed)
            : target_flow_(target_flow), on_default_changed_(std::move(on_default_changed)) {}

    WasapiNotificationClient(const WasapiNotificationClient&) = delete;
    WasapiNotificationClient& operator=(const WasapiNotificationClient&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1UL, std::memory_order_relaxed) + 1UL;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG prev = ref_count_.fetch_sub(1UL, std::memory_order_acq_rel);
        const ULONG remaining = prev - 1UL;
        if (remaining == 0UL) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out_object) override {
        if (out_object == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IMMNotificationClient))) {
            *out_object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out_object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR /*default_device_id*/) override {
        if (flow == target_flow_ && role == eConsole && on_default_changed_) {
            on_default_changed_();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*device_id*/) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*device_id*/) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*device_id*/,
                                                   DWORD /*new_state*/) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*device_id*/,
                                                     const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

  private:
    ~WasapiNotificationClient() = default;

    std::atomic<ULONG> ref_count_{1UL};
    EDataFlow target_flow_;
    std::function<void()> on_default_changed_;
};

}  // namespace goldfish::wasapi
