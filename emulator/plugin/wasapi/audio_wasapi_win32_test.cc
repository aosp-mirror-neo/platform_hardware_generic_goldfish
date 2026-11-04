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

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>

#include "wasapi_win32.h"

namespace goldfish::wasapi {
namespace {

TEST(WasapiWin32Test, ScopedCoTaskMemAllocAndRelease) {
    auto* raw = static_cast<WAVEFORMATEX*>(::CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    ASSERT_NE(raw, nullptr);
    std::memset(raw, 0, sizeof(*raw));
    raw->wFormatTag = WAVE_FORMAT_PCM;
    raw->nChannels = 2U;
    raw->nSamplesPerSec = 48000U;
    raw->wBitsPerSample = 16U;
    raw->nBlockAlign = 4U;

    ScopedCoTaskMem<WAVEFORMATEX> mem(raw);
    ASSERT_TRUE(mem.ok());
    EXPECT_EQ(mem.get()->nSamplesPerSec, 48000U);

    ScopedCoTaskMem<WAVEFORMATEX> moved(std::move(mem));
    EXPECT_FALSE(mem.ok());
    ASSERT_TRUE(moved.ok());
    EXPECT_EQ(moved.get()->nChannels, 2U);
}

TEST(WasapiWin32Test, ScopedComInitializerIsUsableWhenNested) {
    const android::base::ScopedComInitializer outer(COINIT_MULTITHREADED);
    EXPECT_TRUE(outer.ok());

    const android::base::ScopedComInitializer inner(COINIT_MULTITHREADED);
    EXPECT_TRUE(inner.ok());
}

TEST(WasapiWin32Test, ExtractAndConfigureWaveFormatExtensibleAndStandard) {
    WAVEFORMATEXTENSIBLE ext{};
    ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.nChannels = 6U;
    ext.Format.nSamplesPerSec = 96000U;
    ext.Format.wBitsPerSample = 32U;
    ext.Format.nBlockAlign = 24U;
    ext.Format.nAvgBytesPerSec = 96000UL * 24UL;
    ext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ext.Samples.wValidBitsPerSample = 24U;
    ext.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    PcmFormat parsed{};
    ASSERT_TRUE(ExtractWaveFormat(&ext.Format, &parsed));
    EXPECT_EQ(parsed.format, PcmSampleFormat::kS24In32);
    EXPECT_EQ(parsed.sample_rate, 96000U);
    EXPECT_EQ(parsed.channels, 6U);

    // Reconfigure extensible 6-channel 96 kHz 24-bit mix format to 48 kHz S16 while
    // preserving the 6 hardware channels and channel mask for AUTOCONVERTPCM.
    const PcmFormat sw_fmt{PcmSampleFormat::kS16, 48000U, 2U};
    ASSERT_TRUE(ConfigureWaveFormatForSoftwareFormat(&ext.Format, sw_fmt));
    EXPECT_EQ(ext.Format.nChannels, 6U);
    EXPECT_EQ(ext.dwChannelMask, static_cast<DWORD>(KSAUDIO_SPEAKER_5POINT1));
    ASSERT_TRUE(ExtractWaveFormat(&ext.Format, &parsed));
    EXPECT_EQ(parsed.format, PcmSampleFormat::kS16);
    EXPECT_EQ(parsed.sample_rate, 48000U);
    EXPECT_EQ(parsed.channels, 6U);
}

TEST(WasapiWin32Test, NotificationClientIntrusivePtrAndDefaultDeviceFiltering) {
    std::atomic<int> notifications{0};
    goldfish::base::IntrusivePtr<WasapiNotificationClient> client(
            new WasapiNotificationClient(eRender, [&]() { notifications.fetch_add(1); }));
    ASSERT_TRUE(static_cast<bool>(client));

    // QueryInterface validation (null out-ptr, unsupported IID, IUnknown, IMMNotificationClient).
    EXPECT_EQ(client->QueryInterface(IID_IUnknown, nullptr), E_POINTER);
    void* unknown_iface = nullptr;
    EXPECT_EQ(client->QueryInterface(__uuidof(IAudioClient), &unknown_iface), E_NOINTERFACE);
    EXPECT_EQ(unknown_iface, nullptr);

    goldfish::base::IntrusivePtr<IMMNotificationClient> mm_iface;
    const HRESULT qi_hr = QueryComOutPtr(&mm_iface, [&](IMMNotificationClient** raw) {
        return client->QueryInterface(__uuidof(IMMNotificationClient),
                                      reinterpret_cast<void**>(raw));
    });
    ASSERT_EQ(qi_hr, S_OK);
    ASSERT_TRUE(static_cast<bool>(mm_iface));

    // Non-matching role or flow should be ignored; matching eRender + eConsole triggers callback,
    // including non-ASCII UTF-16 endpoint IDs.
    constexpr wchar_t kUnicodeEndpointId[] = L"{0.0.0.00000000}.\u65E5\u672C\u8A9E-\u00FC-audio";
    EXPECT_EQ(mm_iface->OnDefaultDeviceChanged(eCapture, eConsole, kUnicodeEndpointId), S_OK);
    EXPECT_EQ(mm_iface->OnDefaultDeviceChanged(eRender, eCommunications, kUnicodeEndpointId), S_OK);
    EXPECT_EQ(notifications.load(), 0);

    EXPECT_EQ(mm_iface->OnDefaultDeviceChanged(eRender, eConsole, kUnicodeEndpointId), S_OK);
    EXPECT_EQ(notifications.load(), 1);
}

TEST(WasapiWin32Test, FormatHresultIncludesNamedWasapiDiagnostics) {
    const std::string not_found = FormatHresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    EXPECT_NE(not_found.find("E_NOTFOUND"), std::string::npos);

    const std::string invalidated = FormatHresult(AUDCLNT_E_DEVICE_INVALIDATED);
    EXPECT_NE(invalidated.find("AUDCLNT_E_DEVICE_INVALIDATED"), std::string::npos);
}

}  // namespace
}  // namespace goldfish::wasapi
