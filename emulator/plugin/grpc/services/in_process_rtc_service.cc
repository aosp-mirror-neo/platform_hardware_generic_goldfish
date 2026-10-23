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

#include <memory>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"

#include "android/emulation/control/event_sender.h"
#include "api/make_ref_counted.h"
#include "emulator/plugin/grpc/input/dispatcher_session.h"
#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/v2_input_validator.h"
#include "emulator/plugin/grpc/services/v2/environment/sensors_service_impl.h"
#include "emulator/plugin/grpc/services/v2/webrtc/rtc_service_impl.h"
#include "goldfish/audio/qemu_audio_source.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/videobridge/in_process_input_sender.h"
#include "goldfish/videobridge/in_process_video_source.h"
#include "goldfish/videobridge/media_track_provider.h"
#include "goldfish/videobridge/rtc_service.h"
#include "goldfish/videobridge/switchboard.h"
extern "C" {
typedef struct QemuConsole QemuConsole;
// NOLINTNEXTLINE(readability-identifier-naming)
QemuConsole* qemu_console_lookup_by_index(int index);
// NOLINTNEXTLINE(readability-identifier-naming)
QemuConsole* qemu_console_lookup_default(void);
}

namespace android::emulation::control {

using ::goldfish::audio::QemuAudioSource;
using ::goldfish::avd::AvdUniverse;
using ::goldfish::display::IMultiDisplay;
using ::goldfish::grpc::v2::DispatcherSession;
using ::goldfish::grpc::v2::IInputDispatcher;
using ::goldfish::grpc::v2::V2InputValidator;
using ::goldfish::videobridge::DataChannelLabel;
using ::goldfish::videobridge::InProcessInputSender;
using ::goldfish::videobridge::InProcessVideoSource;
using ::goldfish::videobridge::InputEvent;
using ::goldfish::videobridge::InputSender;
using ::goldfish::videobridge::InputSenderFactory;
using ::goldfish::videobridge::MediaTrackProvider;
using ::goldfish::videobridge::RtcService;
using ::goldfish::videobridge::Switchboard;
using keyboard::IKeyEventSender;

namespace {

// Clean, testable event routing isolated from factory plumbing.
absl::Status DispatchInProcessInputEvent(const InputEvent& request,
                                         InputEventSender& input_event_sender,
                                         IKeyEventSender& key_event_sender) {
    VLOG(2) << "InProcess InputEvent: " << request.ShortDebugString();

    switch (request.type_case()) {
    case InputEvent::kKeyEvent:
        key_event_sender.send(request.key_event());
        return absl::OkStatus();

    case InputEvent::kMouseEvent:
        return input_event_sender.Send(request.mouse_event());

    case InputEvent::kTouchEvent:
        return input_event_sender.Send(request.touch_event());

    case InputEvent::kAndroidEvent:
        return input_event_sender.Send(request.android_event());

    case InputEvent::kPenEvent:
        return input_event_sender.Send(request.pen_event());

    case InputEvent::kWheelEvent:
        return input_event_sender.Send(request.wheel_event());

    case InputEvent::TYPE_NOT_SET:
    default:
        return absl::InvalidArgumentError("Unknown or unset InputEvent payload");
    }
}

// WebRTC has no way to report a rejected event to the client.
void LogRejectedV2InputEvent(IInputDispatcher::DispatchResult result) {
    if (!result.status.ok()) {
        VLOG(1) << "Dropped WebRTC v2 input event: " << result.status;
    }
}

// One WebRTC v2 input channel: a human session on `input_dispatcher` that
// lives as long as the sender, so closing the channel releases its input. As
// for every v2 client, DOWN for a pointer it holds is rejected.
std::unique_ptr<InputSender> CreateV2InputSender(IMultiDisplay& multidisplay,
                                                 std::shared_ptr<IInputDispatcher> input_dispatcher,
                                                 V2InputValidator::Options v2_validator_options) {
    auto session = std::make_shared<DispatcherSession>(
            std::move(input_dispatcher),
            IInputDispatcher::SessionOptions{
                .human = true, .source_id = 0, .reject_down_on_held = true});
    V2InputValidator validator(multidisplay, v2_validator_options);
    return std::make_unique<InProcessInputSender>(
            /*dispatcher=*/nullptr, [session = std::move(session),
                                     validator](const ::android::emulation::v2::InputEvent& event) {
                if (absl::Status status = validator.Validate(event); !status.ok()) {
                    VLOG(1) << "Dropped invalid WebRTC v2 input event: " << status;
                    return absl::OkStatus();
                }
                session->Dispatch(event, LogRejectedV2InputEvent);
                return absl::OkStatus();
            });
}

}  // namespace

InputSenderFactory CreateInProcessInputSenderFactory(
        IMultiDisplay& multidisplay, std::shared_ptr<IKeyEventSender> key_event_sender,
        std::shared_ptr<IInputDispatcher> input_dispatcher,
        ::goldfish::sensors::PhysicalModel* physical_model,
        V2InputValidator::Options v2_validator_options) {
    CHECK_NE(key_event_sender, nullptr) << "key_event_sender must not be null";
    auto input_event_sender = std::make_shared<InputEventSender>(&multidisplay);
    std::shared_ptr<::goldfish::grpc::v2::SensorsServiceImpl> sensors_service;
    if (physical_model != nullptr) {
        sensors_service =
                std::make_shared<::goldfish::grpc::v2::SensorsServiceImpl>(*physical_model);
    }

    return [&multidisplay, input_event_sender, key_event_sender = std::move(key_event_sender),
            input_dispatcher = std::move(input_dispatcher), sensors_service,
            v2_validator_options](DataChannelLabel label) -> std::unique_ptr<InputSender> {
        if (label == DataChannelLabel::kInputV2 && input_dispatcher != nullptr) {
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

        return std::make_unique<InProcessInputSender>([input_event_sender, key_event_sender](
                                                              const InputEvent& request) {
            return DispatchInProcessInputEvent(request, *input_event_sender, *key_event_sender);
        });
    };
}

static std::shared_ptr<Switchboard> CreateInProcessSwitchboard(AvdUniverse& avd_universe,
                                                               uint32_t display_id,
                                                               uint32_t console_index,
                                                               AudioBackend* audio_backend) {
    auto video_source = ::webrtc::make_ref_counted<InProcessVideoSource>(
            avd_universe.GetMultiDisplay(), display_id);

    // QemuAudioSource handles a null audio_backend gracefully by resolving the default active
    // backend.
    auto audio_source = ::webrtc::make_ref_counted<QemuAudioSource>(audio_backend);

    auto media_provider = std::make_shared<MediaTrackProvider>(video_source, audio_source);

    QemuConsole* console = qemu_console_lookup_by_index(console_index);
    if (console == nullptr) {
        LOG(WARNING) << "Failed to find QemuConsole for console index " << console_index
                     << ". Input injection will fallback to default console.";
        console = qemu_console_lookup_default();
    }

    // keyboard::createKeyEventSender explicitly supports a null QemuConsole* gracefully.
    std::shared_ptr<IKeyEventSender> key_event_sender =
            keyboard::createKeyEventSender(console, &avd_universe.GetQemuEventLoop());
    const auto& hw = avd_universe.Props().hw_config;
    auto input_sender_factory = CreateInProcessInputSenderFactory(
            avd_universe.GetMultiDisplay(), std::move(key_event_sender),
            avd_universe.GetInputDispatcher(), &avd_universe.GetSensorsPhysicalModel(),
            {.hw_sensor_hinge = hw.hw_sensor_hinge, .hw_keyboard = hw.hw_keyboard});
    return std::make_shared<Switchboard>(media_provider, input_sender_factory);
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
