// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "absl/types/optional.h"

#include "../fourcc_utils.h"
#include "VerLogging.h"
#include "android/base/scoped_com_initializer.h"
#include "android/base/win32_unicode_string.h"
#include "webcam_source.h"

using namespace Microsoft::WRL;
using namespace android::base;

namespace android {
namespace ver {

// Media Foundation Attribute GUIDs
static constexpr GUID ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE = {
    0xc60ac5fe, 0x252a, 0x478f, {0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3}};
static constexpr GUID ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK = {
    0x58f0aad8, 0x22bf, 0x4f8a, {0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f}};
static constexpr GUID ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID = {
    0x8ac3587a, 0x4ae7, 0x42d8, {0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f}};

static constexpr GUID ANDROID_MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = {
    0xf81da2c, 0xb537, 0x4672, {0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x7, 0xa3}};
static constexpr GUID ANDROID_CLSID_VideoProcessorMFT = {
    0x88753b26, 0x5b24, 0x49bd, {0xb2, 0xe7, 0xc, 0x44, 0x5c, 0x78, 0xc9, 0x82}};

static constexpr GUID kGuidNull = {};

struct CameraFormatMapping {
    GUID mfSubtype;
    uint32_t v4l2Format;
    const char* humanReadableName;
};

static const CameraFormatMapping kSupportedPixelFormats[] = {
    {MFVideoFormat_NV12, V4L2_PIX_FMT_NV12, "NV12"},
    {MFVideoFormat_I420, V4L2_PIX_FMT_YUV420, "I420"},
    {MFVideoFormat_YUY2, V4L2_PIX_FMT_YUY2, "YUY2"},
    {MFVideoFormat_RGB32, V4L2_PIX_FMT_BGR32, "RGB32"},
    {MFVideoFormat_ARGB32, V4L2_PIX_FMT_BGR32, "ARGB32"},
    {MFVideoFormat_RGB24, V4L2_PIX_FMT_BGR24, "RGB24"},
};

static uint32_t subtypeToPixelFormat(REFGUID subtype) {
    for (const auto& supportedFormat : kSupportedPixelFormats) {
        if (supportedFormat.mfSubtype == subtype) {
            return supportedFormat.v4l2Format;
        }
    }
    return 0;
}

static std::string hrToString(HRESULT hr) {
    char buf[11] = {};
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buf);
}

static std::string guidToString(REFGUID guid) {
    char buf[37] = {};
    snprintf(buf, sizeof(buf),
             "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%"
             "02hhX",
             guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
             guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::string(buf);
}

static std::string subtypeHumanReadableName(REFGUID subtype) {
    if (subtype == kGuidNull) {
        return "GUID_NULL";
    }

    for (const auto& supportedFormat : kSupportedPixelFormats) {
        if (supportedFormat.mfSubtype == subtype) {
            return supportedFormat.humanReadableName;
        }
    }

    struct SubtypeToName {
        GUID mfSubtype;
        const char* humanReadableName;
    };

    // Convert common subtypes as well, this list is not extensive.
    // https://docs.microsoft.com/en-us/windows/desktop/medfound/video-subtype-guids
    static const SubtypeToName kSubtypeToName[] = {
        // RGB formats, except for supported formats handled above.
        {MFVideoFormat_RGB8, "RGB8"},
        {MFVideoFormat_RGB555, "RGB555"},
        {MFVideoFormat_RGB565, "RGB565"},
        // YUV formats, except for supported formats handled above.
        {MFVideoFormat_AI44, "AI44"},
        {MFVideoFormat_AYUV, "AYUV"},
        {MFVideoFormat_IYUV, "IYUV"},
        {MFVideoFormat_NV11, "NV11"},
        {MFVideoFormat_UYVY, "UYVY"},
        {MFVideoFormat_YUY2, "YUY2"},
        {MFVideoFormat_YV12, "YV12"},
        {MFVideoFormat_YVYU, "YVYU"},
        // Compressed formats.
        {MFVideoFormat_H264, "H264"},
        {MFVideoFormat_H265, "H265"},
        {MFVideoFormat_H264_ES, "H264_ES"},
        {MFVideoFormat_HEVC, "HEVC"},
        {MFVideoFormat_HEVC_ES, "HEVC_ES"},
        {MFVideoFormat_MP4V, "MP4V"},
    };

    for (const auto& mapping : kSubtypeToName) {
        if (mapping.mfSubtype == subtype) {
            return mapping.humanReadableName;
        }
    }

    return guidToString(subtype);
}

static HRESULT getMediaHandler(const ComPtr<IMFMediaSource>& source,
                               ComPtr<IMFMediaTypeHandler>* outMediaHandler) {
    ComPtr<IMFPresentationDescriptor> presentationDesc;
    HRESULT hr = source->CreatePresentationDescriptor(&presentationDesc);

    if (SUCCEEDED(hr)) {
        BOOL selected;
        ComPtr<IMFStreamDescriptor> streamDesc;
        hr = presentationDesc->GetStreamDescriptorByIndex(0, &selected, &streamDesc);

        if (SUCCEEDED(hr)) {
            hr = streamDesc->GetMediaTypeHandler(outMediaHandler->ReleaseAndGetAddressOf());
        }
    }

    return hr;
}

template <typename Type>
class DelayLoad {
  public:
    DelayLoad(const char* dll, const char* name) {
        mModule = LoadLibraryA(dll);
        if (mModule) {
            FARPROC fn = GetProcAddress(mModule, name);
            if (fn) {
                mFunction = reinterpret_cast<Type*>(fn);
            }
        }
    }
    ~DelayLoad() {
        if (mModule) FreeLibrary(mModule);
    }
    bool isValid() const { return mFunction != nullptr; }
    template <typename... Args>
    HRESULT operator()(Args... args) {
        return mFunction ? mFunction(std::forward<Args>(args)...) : E_NOTIMPL;
    }

  private:
    HMODULE mModule = nullptr;
    Type* mFunction = nullptr;
};

class MFApi {
  public:
    MFApi()
            : mfStartup("mfplat.dll", "MFStartup")
            , mfShutdown("mfplat.dll", "MFShutdown")
            , mfCreateAttributes("mfplat.dll", "MFCreateAttributes")
            , mfEnumDeviceSources("mf.dll", "MFEnumDeviceSources")
            , mfCreateDeviceSource("mf.dll", "MFCreateDeviceSource")
            , mfCreateSourceReaderFromMediaSource("mfreadwrite.dll",
                                                  "MFCreateSourceReaderFromMediaSource")
            , mfCreateMediaType("mfplat.dll", "MFCreateMediaType") {
        if (isValid()) {
            ComPtr<IUnknown> obj;
            if (SUCCEEDED(CoCreateInstance(ANDROID_CLSID_VideoProcessorMFT, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&obj)))) {
                mSupportsAdvancedVideoProcessor = true;
            }
        }
    }
    bool isValid() {
        return mfStartup.isValid() && mfShutdown.isValid() && mfCreateAttributes.isValid() &&
               mfEnumDeviceSources.isValid() && mfCreateDeviceSource.isValid() &&
               mfCreateSourceReaderFromMediaSource.isValid() && mfCreateMediaType.isValid();
    }
    bool supportsAdvancedVideoProcessor() const { return mSupportsAdvancedVideoProcessor; }

    DelayLoad<decltype(MFStartup)> mfStartup;
    DelayLoad<HRESULT()> mfShutdown;
    DelayLoad<decltype(MFCreateAttributes)> mfCreateAttributes;
    DelayLoad<decltype(MFEnumDeviceSources)> mfEnumDeviceSources;
    DelayLoad<decltype(MFCreateDeviceSource)> mfCreateDeviceSource;
    DelayLoad<decltype(MFCreateSourceReaderFromMediaSource)> mfCreateSourceReaderFromMediaSource;
    DelayLoad<decltype(MFCreateMediaType)> mfCreateMediaType;

  private:
    bool mSupportsAdvancedVideoProcessor = false;
};

static MFApi& getMFApiInstance() {
    static MFApi* const sMFApi = new MFApi();
    return *sMFApi;
}

class MFInitialize {
  public:
    MFInitialize() : mComInit(COINIT_MULTITHREADED) {
        if (!mComInit) {
            LOG(INFO) << "CoInitializeEx failed, hr=" << hrToString(mComInit.hr());
            return;
        }
        if (!getMFApiInstance().isValid()) {
            LOG(INFO) << "MFApi is not valid";
            return;
        }
        HRESULT hr = getMFApiInstance().mfStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hr)) {
            LOG(INFO) << "MFStartup failed, hr=" << hrToString(hr);
            return;
        }
        mMfInit = true;
    }
    ~MFInitialize() {
        if (mMfInit) getMFApiInstance().mfShutdown();
    }
    operator bool() { return mMfInit; }
    MFApi& getMFApi() { return getMFApiInstance(); }

  private:
    const ScopedComInitializer mComInit;
    bool mMfInit = false;
};

std::vector<std::shared_ptr<WebcamSource::WebcamInfo>> WebcamSource::EnumerateWebcams() {
    std::vector<std::shared_ptr<WebcamSource::WebcamInfo>> webcams;
    MFInitialize mf;
    if (!mf) return webcams;

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = mf.getMFApi().mfCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        LOG(ERROR) << "Failed to create attributes, hr=" << hrToString(hr);
        return webcams;
    }
    hr = attributes->SetGUID(ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        LOG(ERROR) << "Failed setting attributes, hr=" << hrToString(hr);
        return webcams;
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    hr = mf.getMFApi().mfEnumDeviceSources(attributes.Get(), &devices, &deviceCount);
    if (FAILED(hr)) {
        LOG(ERROR) << "MFEnumDeviceSources, hr=" << hrToString(hr);
        return webcams;
    }

    for (UINT32 i = 0; i < deviceCount; ++i) {
        ComPtr<IMFActivate> device;
        device.Attach(devices[i]);

        WCHAR* symbolicLink = nullptr;
        UINT32 len = 0;
        std::shared_ptr<WebcamInfo> info = std::make_shared<WebcamInfo>();
        hr = device->GetAllocatedString(
                ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink,
                &len);
        if (SUCCEEDED(hr)) {
            info->os_name = Win32UnicodeString(symbolicLink).toString();
            info->os_alias = info->os_name;
            CoTaskMemFree(symbolicLink);
        } else {
            LOG(WARNING) << "Failed to get webcam info for device, hr=" << hrToString(hr);
        }

        WCHAR* friendlyName = nullptr;
        hr = device->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &len);
        if (SUCCEEDED(hr)) {
            info->friendly_name = Win32UnicodeString(friendlyName).toString();
            CoTaskMemFree(friendlyName);
        } else {
            LOG(WARNING) << "Failed to get user friendly name for device, hr=" << hrToString(hr);
        }

        info->preferred_format_index = -1;

        std::map<uint32_t, WebcamPixelFormat> format_map;
        std::set<std::pair<unsigned int, unsigned int>> all_resolutions;
        ComPtr<IMFMediaSource> source;
        HRESULT hr_act = device->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr_act)) {
            LOG(WARNING) << "Failed to activate device object, hr=" << hrToString(hr_act);
        }
        if (SUCCEEDED(hr_act)) {
            ComPtr<IMFMediaTypeHandler> handler;
            if (SUCCEEDED(getMediaHandler(source, &handler))) {
                DWORD count = 0;
                HRESULT hr_count = handler->GetMediaTypeCount(&count);
                if (FAILED(hr_count)) {
                    LOG(WARNING) << "Failed to get media type count, hr=" << hrToString(hr_count);
                    count = 0;
                }
                for (DWORD j = 0; j < count; ++j) {
                    ComPtr<IMFMediaType> type;
                    if (SUCCEEDED(handler->GetMediaTypeByIndex(j, &type))) {
                        GUID subtype;
                        HRESULT hr_guid = type->GetGUID(MF_MT_SUBTYPE, &subtype);
                        if (FAILED(hr_guid)) {
                            LOG(WARNING)
                                    << "Failed to get media subtype, hr=" << hrToString(hr_guid);
                            continue;
                        }
                        // The webcam on the HTC Vive reports H264 with
                        // !IsCompressedFormat(), blacklist H264 explicitly.
                        if (subtype == MFVideoFormat_H264) {
                            continue;
                        }
                        UINT32 w = 0, h = 0;
                        HRESULT hr_size = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
                        if (FAILED(hr_size)) {
                            LOG(WARNING) << "Failed to get frame size, hr=" << hrToString(hr_size);
                            continue;
                        }
                        all_resolutions.insert(
                                {static_cast<unsigned int>(w), static_cast<unsigned int>(h)});
                        uint32_t pixel_format = subtypeToPixelFormat(subtype);
                        if (pixel_format == 0) {
                            VLOG(1) << "Skipping unsupported webcam pixel format subtype: "
                                    << subtypeHumanReadableName(subtype);
                            continue;
                        }
                        WebcamPixelFormat& wpfmt = format_map[pixel_format];
                        wpfmt.pixel_format = pixel_format;
                        wpfmt.compressed = false;

                        bool duplicate = false;
                        for (const auto& res : wpfmt.resolutions) {
                            if (res.resolution.width == static_cast<unsigned int>(w) &&
                                res.resolution.height == static_cast<unsigned int>(h)) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate) {
                            wpfmt.resolutions.push_back(
                                    {{static_cast<unsigned int>(w), static_cast<unsigned int>(h)},
                                     {}});
                        }
                    }
                }
                if (format_map.empty() && !all_resolutions.empty()) {
                    WebcamPixelFormat& wpfmt = format_map[V4L2_PIX_FMT_BGR32];
                    wpfmt.pixel_format = V4L2_PIX_FMT_BGR32;
                    wpfmt.compressed = false;
                    for (const auto& res : all_resolutions) {
                        wpfmt.resolutions.push_back({{res.first, res.second}, {}});
                    }
                    VLOG(1) << "No native supported formats found for webcam, falling back to "
                               "RGB32 via transcoding.";
                }
            }
            device->ShutdownObject();
        }

        for (auto& entry : format_map) {
            WebcamPixelFormat& wpfmt = entry.second;
            /* Sort resolutions by pixel count */
            std::sort(wpfmt.resolutions.begin(), wpfmt.resolutions.end(),
                      [](const WebcamResolutions& a, const WebcamResolutions& b) {
                          return a.resolution.width * a.resolution.height <
                                 b.resolution.width * b.resolution.height;
                      });
            info->supported_formats.push_back(std::move(wpfmt));
        }

        if (!info->supported_formats.empty()) {
            info->preferred_format_index = GetPreferredFormatIndex(info->supported_formats);
            webcams.push_back(std::move(info));
        }
    }
    CoTaskMemFree(devices);
    return webcams;
}

class SourceReaderCallback : public IMFSourceReaderCallback {
  public:
    SourceReaderCallback() { mFrameAvailable = CreateEvent(nullptr, false, false, nullptr); }
    virtual ~SourceReaderCallback() { CloseHandle(mFrameAvailable); }

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) ||
            iid == IID_IMFSourceReaderCallback) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&mRefCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG refs = InterlockedDecrement(&mRefCount);
        if (refs == 0) delete this;
        return refs;
    }

    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD streamFlags, LONGLONG,
                              IMFSample* sample) override {
        std::lock_guard<std::mutex> lock(mLock);
        VLOG(1) << "OnReadSample, hr = " << hrToString(hr);
        mNewSample = true;
        mLastHr = hr;
        mStreamFlags = streamFlags;
        mLastSample = sample;
        SetEvent(mFrameAvailable);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

    bool TryGetSample(HRESULT* hr, DWORD* streamFlags, IMFSample** sample) {
        std::lock_guard<std::mutex> lock(mLock);
        bool isNew = mNewSample;
        *hr = mLastHr;
        *streamFlags = mStreamFlags;
        *sample = mLastSample.Detach();
        mNewSample = false;
        return isNew;
    }
    HANDLE getEvent() { return mFrameAvailable; }

  private:
    volatile ULONG mRefCount = 1;
    std::mutex mLock;
    bool mNewSample = false;
    HRESULT mLastHr = S_OK;
    DWORD mStreamFlags = 0;
    ComPtr<IMFSample> mLastSample;
    HANDLE mFrameAvailable;
};

class WindowsImpl : public WebcamSource::Impl {
  public:
    explicit WindowsImpl(std::shared_ptr<const WebcamSource::WebcamInfo> info)
            : webcam_info_(std::move(info)) {}
    ~WindowsImpl() override { StopLocked(); }

    HRESULT configureMediaSource(const ComPtr<IMFMediaSource>& source, uint32_t pixel_format,
                                 int frame_width, int frame_height, GUID* out_native_subtype) {
        auto format = webcam_info_->NegotiateFormat(pixel_format);
        if (!format.has_value()) {
            LOG(ERROR) << "Camera" << webcam_info_->friendly_name.c_str()
                       << " is not supported by the emulator";
            return E_FAIL;
        }

        WebcamSource::Resolution requested_res = {static_cast<unsigned int>(frame_width),
                                                  static_cast<unsigned int>(frame_height)};
        WebcamSource::Resolution best_res = format->FindBestMatchForResolution(requested_res);

        ComPtr<IMFMediaTypeHandler> mediaHandler;
        HRESULT hr = getMediaHandler(source, &mediaHandler);
        if (FAILED(hr)) return hr;

        DWORD count;
        hr = mediaHandler->GetMediaTypeCount(&count);
        if (FAILED(hr)) return hr;

        ComPtr<IMFMediaType> bestMediaType;
        for (DWORD j = 0; j < count; ++j) {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(mediaHandler->GetMediaTypeByIndex(j, &type))) {
                GUID subtype;
                HRESULT hr_guid = type->GetGUID(MF_MT_SUBTYPE, &subtype);
                if (FAILED(hr_guid)) {
                    continue;
                }
                UINT32 w = 0, h = 0;
                HRESULT hr_size = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
                if (FAILED(hr_size)) {
                    continue;
                }

                uint32_t pf = subtypeToPixelFormat(subtype);
                if (pf == 0) {
                    VLOG(1) << "Unsupported webcam pixel format subtype during configuration: "
                            << subtypeHumanReadableName(subtype);
                    continue;
                }

                if (pf == format->pixel_format && w == best_res.width && h == best_res.height) {
                    bestMediaType = type;
                    break;
                }
            }
        }

        if (bestMediaType) {
            hr = mediaHandler->SetCurrentMediaType(bestMediaType.Get());
            if (FAILED(hr)) {
                LOG(INFO) << "Failed to set media type on source, hr=" << hrToString(hr);
            } else {
                LOG(INFO) << "Configured webcam device to " << best_res.width << "x"
                          << best_res.height;
                hr = bestMediaType->GetGUID(MF_MT_SUBTYPE, out_native_subtype);
                if (FAILED(hr)) {
                    LOG(WARNING) << "Failed to get native subtype GUID, hr=" << hrToString(hr);
                    *out_native_subtype = kGuidNull;
                }
            }
        }

        return hr;
    }

    int StartLocked(uint32_t pixel_format, int frame_width, int frame_height) override {
        LOG(INFO) << "Starting webcam at " << frame_width << "x" << frame_height;
        if (!mMF) {
            LOG(ERROR) << "Media Foundation not initialized";
            return -1;
        }
        ComPtr<IMFAttributes> attr;
        HRESULT hr = mMF.getMFApi().mfCreateAttributes(&attr, 2);
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed to create attributes, hr=" << hrToString(hr);
            return -1;
        }
        // Specify that we want to create a webcam with a specific id.
        hr = attr->SetGUID(ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed setting attributes, hr=" << hrToString(hr);
            return -1;
        }
        hr = attr->SetString(ANDROID_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                             Win32UnicodeString(webcam_info_->os_name).data());
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed setting attributes, hr=" << hrToString(hr);
            return -1;
        }

        ComPtr<IMFMediaSource> source;
        hr = mMF.getMFApi().mfCreateDeviceSource(attr.Get(), &source);
        if (FAILED(hr)) {
            LOG(ERROR) << "Webcam source creation failed, hr=" << hrToString(hr);
            return -1;
        }

        GUID native_subtype = kGuidNull;
        hr = configureMediaSource(source, pixel_format, frame_width, frame_height, &native_subtype);
        if (FAILED(hr)) {
            return -1;
        }

        ComPtr<IMFAttributes> readerAttr;
        hr = mMF.getMFApi().mfCreateAttributes(&readerAttr, 1);
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed to create attributes, hr=" << hrToString(hr);
            return -1;
        }
        mCallback.Attach(new SourceReaderCallback());
        hr = readerAttr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, mCallback.Get());
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed setting attributes, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }

        if (mMF.getMFApi().supportsAdvancedVideoProcessor()) {
            hr = readerAttr->SetUINT32(ANDROID_MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                                       TRUE);
        } else {
            hr = readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed setting attributes, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }

        hr = mMF.getMFApi().mfCreateSourceReaderFromMediaSource(source.Get(), readerAttr.Get(),
                                                                &mSourceReader);
        if (FAILED(hr)) {
            LOG(ERROR) << "Configure source reader failed, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }

        ComPtr<IMFMediaType> type;
        hr = mMF.getMFApi().mfCreateMediaType(&type);
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed to create media type, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }
        hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) {
            if (native_subtype != kGuidNull) {
                hr = type->SetGUID(MF_MT_SUBTYPE, native_subtype);
            } else {
                hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            }
        }
        if (SUCCEEDED(hr)) {
            hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, frame_width, frame_height);
        }
        if (FAILED(hr)) {
            LOG(ERROR) << "Failed to configure media type attributes, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }

        hr = mSourceReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                                type.Get());
        if (FAILED(hr)) {
            LOG(ERROR) << "Configure webcam source failed, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }

        mPixelFormat = subtypeToPixelFormat(native_subtype);
        if (mPixelFormat == 0) {
            if (native_subtype == kGuidNull) {
                mPixelFormat = V4L2_PIX_FMT_BGR32;
            } else {
                LOG(ERROR) << "Unknown pixel format, could not configure webcam";
                StopLocked();
                return -1;
            }
        }
        mWidth = frame_width;
        mHeight = frame_height;

        hr = mSourceReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                       nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            LOG(ERROR) << "ReadSample failed, hr=" << hrToString(hr);
            StopLocked();
            return -1;
        }

        return 0;
    }

    int StopLocked() override {
        mSourceReader.Reset();
        mCallback.Reset();
        return 0;
    }

    absl::StatusOr<bool> FetchNextFrame(
            std::function<absl::Status(const RawImageBufferView*)> new_frame_cb) override {
        if (!mSourceReader || !mCallback) return absl::InternalError("Not started");

        HRESULT hr;
        DWORD flags;
        ComPtr<IMFSample> sample;
        bool isNew = mCallback->TryGetSample(&hr, &flags, &sample);

        if (FAILED(hr)) {
            LOG(ERROR) << "ReadSample failed, hr=" << hrToString(hr);
            return absl::InternalError("ReadSample failed");
        }

        if (isNew && (flags & MF_SOURCE_READERF_STREAMTICK)) {
            LOG(WARNING) << "Camera stream discontinuity detected.";
        }

        if (isNew) {
            hr = mSourceReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                           nullptr, nullptr, nullptr);
            if (FAILED(hr)) {
                LOG(ERROR) << "ReadSample failed in FetchNextFrame, hr=" << hrToString(hr);
                return absl::InternalError("ReadSample failed");
            }
        }

        if (!sample) return false;

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr_conv = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr_conv)) {
            LOG(ERROR) << "ConvertToContiguousBuffer failed, hr=" << hrToString(hr_conv);
            return absl::InternalError("ConvertToContiguousBuffer failed");
        }

        BYTE* data;
        DWORD len;
        HRESULT hr_lock = buffer->Lock(&data, nullptr, &len);
        if (FAILED(hr_lock)) {
            LOG(ERROR) << "Locking the webcam buffer failed, hr=" << hrToString(hr_lock);
            return absl::InternalError("Buffer lock failed");
        }

        RawImageBufferViewFourCC view = {data, static_cast<size_t>(len), mPixelFormat, mWidth,
                                         mHeight};
        if (view.pixel_format != V4L2_PIX_FMT_RGB32) {
            if (ConvertBufferToRGB32(view, conversion_storage_) != 0) {
                buffer->Unlock();
                return absl::InternalError("Format conversion failed");
            }
        }
        auto ver_view = RawImageBufferViewFourCCBridge(&view);
        absl::Status status = new_frame_cb(&ver_view);
        buffer->Unlock();

        if (!status.ok()) return status;

        return true;
    }

  private:
    MFInitialize mMF;
    std::shared_ptr<const WebcamSource::WebcamInfo> webcam_info_;
    ComPtr<SourceReaderCallback> mCallback;
    ComPtr<IMFSourceReader> mSourceReader;
    uint32_t mPixelFormat = 0;
    int mWidth = 0;
    int mHeight = 0;
    std::vector<uint8_t> conversion_storage_;
};

std::unique_ptr<WebcamSource::Impl> CreatePlatformWebcamImpl(
        std::shared_ptr<const WebcamSource::WebcamInfo> info) {
    return std::make_unique<WindowsImpl>(std::move(info));
}

}  // namespace ver
}  // namespace android
