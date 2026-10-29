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

// External WebRTC headers included transitively by in_process_rtc_service.h
// lack complete nullability annotations, triggering Clang warnings without this pragma.
#pragma clang diagnostic ignored "-Wnullability-completeness"

#include "android/emulation/control/in_process_rtc_service.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"

#include "android/emulation/control/in_process_media_provider.h"
#include "api/make_ref_counted.h"
#include "emulator/plugin/grpc/input/dispatcher_session.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/v1_input_translator.h"
#include "emulator/plugin/grpc/input/v2_input_validator.h"
#include "emulator/plugin/grpc/services/v2/environment/sensors_service_impl.h"
#include "emulator/plugin/grpc/services/v2/webrtc/rtc_service_impl.h"
#include "goldfish/audio/qemu_audio_source.h"
#include "goldfish/audio/webrtc_microphone.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/videobridge/goldfish_audio_device_module.h"
#include "goldfish/videobridge/in_process_input_sender.h"
#include "goldfish/videobridge/in_process_video_source.h"
#include "goldfish/videobridge/media_track_provider.h"
#include "goldfish/videobridge/rtc_service.h"
#include "goldfish/videobridge/switchboard.h"

namespace android::emulation::control {

using ::goldfish::audio::QemuAudioSource;
using ::goldfish::audio::WebRtcMicrophone;
using ::goldfish::avd::AvdUniverse;
using ::goldfish::display::IMultiDisplay;
using ::goldfish::grpc::v2::DispatcherSession;
using ::goldfish::grpc::v2::IInputDispatcher;
using ::goldfish::grpc::v2::V1InputTranslator;
using ::goldfish::grpc::v2::V2InputValidator;
using ::goldfish::videobridge::DataChannelLabel;
using ::goldfish::videobridge::GoldfishAudioDeviceModule;
using ::goldfish::videobridge::InProcessInputSender;
using ::goldfish::videobridge::InProcessVideoSource;
using ::goldfish::videobridge::InputEvent;
using ::goldfish::videobridge::InputSender;
using ::goldfish::videobridge::InputSenderFactory;
using ::goldfish::videobridge::MediaTrackProvider;
using ::goldfish::videobridge::RtcService;
using ::goldfish::videobridge::Switchboard;

namespace {

void LogRejectedV1InputEvent(IInputDispatcher::DispatchResult result) {
    if (!result.status.ok()) {
        VLOG(1) << "Dropped WebRTC v1 input event: " << result.status;
    }
}

std::unique_ptr<InputSender> CreateV1InputSender(IMultiDisplay& multidisplay,
                                                 std::shared_ptr<IInputDispatcher> input_dispatcher,
                                                 V2InputValidator::Options v2_validator_options) {
    const IInputDispatcher::SessionOptions v1_session_options{
        .human = true, .source_id = 0, .reject_down_on_held = false};
    auto session_holder = std::make_shared<std::unique_ptr<DispatcherSession>>(
            std::make_unique<DispatcherSession>(input_dispatcher, v1_session_options));
    auto translator =
            std::make_shared<V1InputTranslator>(multidisplay, v2_validator_options.hw_sensor_hinge);
    return std::make_unique<InProcessInputSender>(
            [input_dispatcher = std::move(input_dispatcher), session_holder, translator,
             v1_session_options](const InputEvent& request) {
                VLOG(2) << "InProcess InputEvent: " << request.ShortDebugString();
                if (*session_holder == nullptr) {
                    *session_holder = std::make_unique<DispatcherSession>(input_dispatcher,
                                                                          v1_session_options);
                }
                translator->SyncResetTracker((*session_holder)->ResetTracker());
                for (const auto& event : translator->TranslateInputEvent(request)) {
                    (*session_holder)->Dispatch(event, LogRejectedV1InputEvent);
                }
                return absl::OkStatus();
            },
            /*v2_dispatcher=*/nullptr,
            /*v2_sensors_dispatcher=*/nullptr,
            /*on_stop=*/
            [session_holder, translator]() {
                session_holder->reset();
                translator->Reset();
            });
}

// WebRTC has no way to report a rejected event to the client.
void LogRejectedV2InputEvent(IInputDispatcher::DispatchResult result) {
    if (!result.status.ok()) {
        VLOG(1) << "Dropped WebRTC v2 input event: " << result.status;
    }
}

// One WebRTC v2 input channel: a human session on `input_dispatcher` that
// closes immediately on DataChannel close (`Stop()`) or sender destruction, so
// closing the channel releases its input without waiting for PeerConnection
// teardown. As for every v2 client, DOWN for a pointer it holds is rejected.
std::unique_ptr<InputSender> CreateV2InputSender(IMultiDisplay& multidisplay,
                                                 std::shared_ptr<IInputDispatcher> input_dispatcher,
                                                 V2InputValidator::Options v2_validator_options) {
    const IInputDispatcher::SessionOptions v2_session_options{
        .human = true, .source_id = 0, .reject_down_on_held = true};
    auto session_holder = std::make_shared<std::unique_ptr<DispatcherSession>>(
            std::make_unique<DispatcherSession>(input_dispatcher, v2_session_options));
    V2InputValidator validator(multidisplay, v2_validator_options);
    return std::make_unique<InProcessInputSender>(
            /*dispatcher=*/nullptr,
            [input_dispatcher = std::move(input_dispatcher), session_holder, validator,
             v2_session_options](const ::android::emulation::v2::InputEvent& event) {
                if (absl::Status status = validator.Validate(event); !status.ok()) {
                    VLOG(1) << "Dropped invalid WebRTC v2 input event: " << status;
                    return absl::OkStatus();
                }
                if (*session_holder == nullptr) {
                    *session_holder = std::make_unique<DispatcherSession>(input_dispatcher,
                                                                          v2_session_options);
                }
                (*session_holder)->Dispatch(event, LogRejectedV2InputEvent);
                return absl::OkStatus();
            },
            /*v2_sensors_dispatcher=*/nullptr,
            /*on_stop=*/[session_holder]() { session_holder->reset(); });
}

static_assert(WebRtcMicrophone::kConfig.sample_rate_hz ==
                      GoldfishAudioDeviceModule::kPlayoutSampleRateHz &&
              WebRtcMicrophone::kConfig.channels == GoldfishAudioDeviceModule::kPlayoutChannels);

// Streams the display and speaker to each participant, and plays what participants send, such
// as a browser microphone, into the guest microphone.
class InProcessMediaProvider : public MediaTrackProvider {
  public:
    InProcessMediaProvider(::webrtc::scoped_refptr<InProcessVideoSource> video_source,
                           ::webrtc::scoped_refptr<QemuAudioSource> audio_source,
                           ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device,
                           std::shared_ptr<WebRtcMicrophone> microphone)
            : MediaTrackProvider(std::move(video_source), std::move(audio_source))
            , audio_device_(std::move(audio_device))
            , microphone_(std::move(microphone)) {
        // WebRTC plays out while a peer offers to send audio, so the guest microphone gets a
        // "webrtc" input for as long as a microphone-capable session is connected.
        audio_device_->SetPlayoutListener([microphone = microphone_.get()](bool playing) {
            microphone->SetPlaying(playing);
        });
    }

    // The module may outlive this provider; once this returns, it no longer calls microphone_.
    ~InProcessMediaProvider() override { audio_device_->SetPlayoutListener(nullptr); }

  private:
    ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device_;
    std::shared_ptr<WebRtcMicrophone> microphone_;
};

}  // namespace

InputSenderFactory CreateInProcessInputSenderFactory(
        IMultiDisplay& multidisplay, std::shared_ptr<IInputDispatcher> input_dispatcher,
        ::goldfish::sensors::PhysicalModel* physical_model,
        V2InputValidator::Options v2_validator_options) {
    CHECK(input_dispatcher != nullptr)
            << "Failed to create in-process InputSenderFactory: input_dispatcher is null; ensure "
               "AvdUniverse initializes IInputDispatcher before WebRTC services";
    std::shared_ptr<::goldfish::grpc::v2::SensorsServiceImpl> sensors_service;
    if (physical_model != nullptr) {
        sensors_service =
                std::make_shared<::goldfish::grpc::v2::SensorsServiceImpl>(*physical_model);
    }

    return [&multidisplay, input_dispatcher = std::move(input_dispatcher), sensors_service,
            v2_validator_options](DataChannelLabel label) -> std::unique_ptr<InputSender> {
        if (label == DataChannelLabel::kInputV2) {
            return CreateV2InputSender(multidisplay, input_dispatcher, v2_validator_options);
        }
        if (label == DataChannelLabel::kSensorsV2 && sensors_service != nullptr) {
            return std::make_unique<InProcessInputSender>(
                    /*dispatcher=*/nullptr,
                    /*v2_dispatcher=*/nullptr,
                    [sensors_service](
                            const ::android::emulation::v2::UpdateSensorsStateRequest& request) {
                        return sensors_service->ApplySensorsUpdate(request);
                    });
        }
        return CreateV1InputSender(multidisplay, input_dispatcher, v2_validator_options);
    };
}

std::shared_ptr<::goldfish::videobridge::MediaProvider> CreateInProcessMediaProvider(
        AvdUniverse& avd_universe, uint32_t display_id, AudioBackend* audio_backend,
        ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device) {
    auto video_source = ::webrtc::make_ref_counted<InProcessVideoSource>(
            avd_universe.GetMultiDisplay(), display_id);

    // QemuAudioSource handles a null audio_backend gracefully by resolving the default active
    // backend.
    auto audio_source = ::webrtc::make_ref_counted<QemuAudioSource>(audio_backend);

    // The guest's capture clock pulls received audio out of WebRTC, which decodes it on demand.
    auto microphone = WebRtcMicrophone::Create(
            avd_universe.GetQemuEventLoop(),
            [audio_device](std::span<int16_t> dst) { return audio_device->PullPlayout(dst); },
            audio_backend);
    return std::make_shared<InProcessMediaProvider>(std::move(video_source),
                                                    std::move(audio_source),
                                                    std::move(audio_device), std::move(microphone));
}

static std::shared_ptr<Switchboard> CreateInProcessSwitchboard(AvdUniverse& avd_universe,
                                                               uint32_t display_id,
                                                               uint32_t /*console_index*/,
                                                               AudioBackend* audio_backend) {
    auto audio_device = GoldfishAudioDeviceModule::Create();
    auto media_provider =
            CreateInProcessMediaProvider(avd_universe, display_id, audio_backend, audio_device);

    const auto& hw = avd_universe.Props().hw_config;
    auto input_sender_factory = CreateInProcessInputSenderFactory(
            avd_universe.GetMultiDisplay(), avd_universe.GetInputDispatcher(),
            &avd_universe.GetSensorsPhysicalModel(),
            {.hw_sensor_hinge = hw.hw_sensor_hinge, .hw_keyboard = hw.hw_keyboard});
    return std::make_shared<Switchboard>(media_provider, input_sender_factory, audio_device);
}

std::vector<std::shared_ptr<::grpc::Service>> CreateInProcessRtcServices(
        AvdUniverse& avd_universe, uint32_t display_id, uint32_t console_index,
        AudioBackend* audio_backend) {
    LOG(INFO) << "Creating in-process WebRTC RtcServices (v1 and v2) for display " << display_id;
    auto switchboard =
            CreateInProcessSwitchboard(avd_universe, display_id, console_index, audio_backend);
    return {
        std::make_shared<RtcService>(switchboard),
        std::make_shared<::goldfish::grpc::v2::RtcServiceImpl>(switchboard),
    };
}

}  // namespace android::emulation::control
