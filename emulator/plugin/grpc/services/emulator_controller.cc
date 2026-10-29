// Copyright (C) 2018 The Android Open Source Project
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

#include "android/emulation/control/emulator_controller.h"

#include <grpcpp/grpcpp.h>

#include <memory>

#include "absl/log/log.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/event_sender.h"
#include "android/emulation/control/keyboard/key_event_sender.h"
#include "android/goldfish/hardware_config.h"
#include "android/goldfish/vm_interface.h"
#include "emulator_controller.grpc.pb.h"
#include "emulator_controller_methods/audio_stream_writer.h"
#include "emulator_controller_methods/battery_service.h"
#include "emulator_controller_methods/clipboard_service.h"
#include "emulator_controller_methods/display_service.h"
#include "emulator_controller_methods/fingerprint_service.h"
#include "emulator_controller_methods/gps_service.h"
#include "emulator_controller_methods/microphone_service.h"
#include "emulator_controller_methods/notification_store.h"
#include "emulator_controller_methods/notification_stream_writer.h"
#include "emulator_controller_methods/sensor_service.h"
#include "emulator_controller_methods/status_service.h"
#include "emulator_controller_methods/vm_service.h"
#include "goldfish/avd/global_event_loop.h"
#include "goldfish/parsing/resizable_display_config.h"

namespace android {
namespace emulation {
namespace control {

using ::goldfish::avd::AvdUniverse;
using ::goldfish::avd_universe::grpc::GrpcNotificationEventSource;
using ::goldfish::display::IMultiDisplay;
using ::google::protobuf::Empty;
using grpc::ServerContext;
using grpc::Status;

namespace {
std::vector<::goldfish::parsing::ResizableDisplayConfig> GetResizableConfig(
        const AvdUniverse& avd_universe) {
    auto configs = ::goldfish::parsing::ParseResizableDisplayConfig(
            avd_universe.Props().hw_config.hw_resizable_configs);
    if (configs) {
        return *std::move(configs);
    }

    return {};
}
}  // namespace

// Logic and data behind the server's behavior.
class EmulatorControllerImpl final
        : public EmulatorController::WithCallbackMethod_streamAudio<
                  EmulatorController::WithCallbackMethod_streamClipboard<
                          EmulatorController::WithCallbackMethod_streamInputEvent<
                                  EmulatorController::WithCallbackMethod_injectWheel<
                                          EmulatorController::WithCallbackMethod_streamNotification<
                                                  EmulatorController::Service>>>>> {
  public:
    EmulatorControllerImpl(VmOperations* vm, QemuConsole* keyboard_console,
                           AvdUniverse* avd_universe, IMultiDisplay* multidisplay)
            : vm_service_(vm)
            , grpc_notification_channel_(avd_universe->GetGrpcNotificationChannel())
            , notification_store_(&grpc_notification_channel_)
            , microphone_service_(grpc_notification_channel_, *::goldfish::avd::globalEventLoop())
            , key_event_sender_(keyboard::createKeyEventSender(keyboard_console,
                                                               &avd_universe->GetQemuEventLoop()))
            , status_service_(avd_universe->GetGuestStatus(), avd_universe->Props(), avd_universe)
            , battery_service_(avd_universe->GetBattery())
            , sensor_service_(avd_universe->GetSensorsPhysicalModel(), &grpc_notification_channel_)
            , gps_service_(avd_universe->GetLocation())
            , fingerprint_service_(avd_universe->GetFingerprintSensor())
            , clipboard_service_(avd_universe->GetClipboardChannel())
            , input_event_sender_(multidisplay)
            , display_service_(multidisplay, &avd_universe->GetSensorsPhysicalModel(),
                               GetResizableConfig(*avd_universe)) {}

    Status getBattery(ServerContext* /*context*/, const Empty* /*request*/,
                      BatteryState* reply) override {
        return battery_service_.getBattery(reply);
    }

    Status setBattery(ServerContext* /*context*/, const BatteryState* request,
                      Empty* /*reply*/) override {
        return battery_service_.setBattery(*request);
    }

    Status getMicrophoneState(ServerContext* /*context*/, const Empty* /*request*/,
                              MicrophoneState* reply) override {
        return microphone_service_.getMicrophoneState(reply);
    }

    Status setMicrophoneState(ServerContext* /*context*/, const MicrophoneState* request,
                              Empty* /*reply*/) override {
        return microphone_service_.setMicrophoneState(*request);
    }

    Status getStatus(ServerContext* /*context*/, const Empty* /*request*/,
                     EmulatorStatus* reply) override {
        return status_service_.getStatus(reply);
    }

    Status setVmState(ServerContext* /*context*/, const VmRunState* request,
                      Empty* /*reply*/) override {
        return vm_service_.setVmState(*request);
    }

    Status getVmState(ServerContext* /*context*/, const Empty* /*request*/,
                      VmRunState* reply) override {
        return vm_service_.getVmState(reply);
    }

    Status getGps(ServerContext* /*context*/, const Empty* /*request*/, GpsState* reply) override {
        return gps_service_.getGps(reply);
    }

    Status setGps(ServerContext* context, const GpsState* request, Empty* /*reply*/) override {
        return gps_service_.setGps(*request);
    }

    Status setSensor(ServerContext* /*context*/, const SensorValue* request,
                     Empty* /*reply*/) override {
        return sensor_service_.setSensor(*request);
    }

    Status getSensor(ServerContext* /*context*/, const SensorValue* request,
                     SensorValue* reply) override {
        return sensor_service_.getSensor(*request, reply);
    }

    Status setPhysicalModel(ServerContext* /*context*/, const PhysicalModelValue* request,
                            google::protobuf::Empty* /*reply*/) override {
        return sensor_service_.setPhysicalModel(*request);
    }

    Status getPhysicalModel(ServerContext* /*context*/, const PhysicalModelValue* request,
                            PhysicalModelValue* reply) override {
        return sensor_service_.getPhysicalModel(*request, reply);
    }

    Status rotateVirtualSceneCamera(ServerContext* /*context*/, const RotationRadian* request,
                                    Empty* /*reply*/) override {
        return sensor_service_.rotateVirtualSceneCamera(*request);
    }

    Status setVirtualSceneCameraVelocity(ServerContext* /*context*/, const Velocity* request,
                                         Empty* /*reply*/) override {
        return sensor_service_.setVirtualSceneCameraVelocity(*request);
    }

    ::grpc::ServerWriteReactor<ClipData>* streamClipboard(::grpc::CallbackServerContext* context,
                                                          const Empty* /*request*/) override {
        return clipboard_service_.streamClipboard(ClipboardServiceImpl::getPeerId(*context));
    }

    Status sendFingerprint(ServerContext* /*context*/, const Fingerprint* request,
                           Empty* /*reply*/) override {
        return fingerprint_service_.sendFingerprint(*request);
    }

    Status sendKey(ServerContext* context, const KeyboardEvent* request,
                   Empty* /*reply*/) override {
        key_event_sender_->send(*request);
        return Status::OK;
    }

    Status sendMouse(ServerContext* context, const MouseEvent* request, Empty* /*reply*/) override {
        return AbslStatusToGrpcStatus(input_event_sender_.Send(*request));
    }

    Status sendTouch(ServerContext* context, const TouchEvent* request, Empty* /*reply*/) override {
        return AbslStatusToGrpcStatus(input_event_sender_.Send(*request));
    }

    ::grpc::ServerReadReactor<WheelEvent>* injectWheel(::grpc::CallbackServerContext* /*context*/,
                                                       Empty* /*response*/) override {
        return new SimpleServerLambdaReader<WheelEvent>([this](auto request) {
            return AbslStatusToGrpcStatus(input_event_sender_.Send(*request));
        });
    }

    ::grpc::ServerReadReactor<InputEvent>* streamInputEvent(
            ::grpc::CallbackServerContext* /*context*/, Empty* /*response*/) override {
        SimpleServerLambdaReader<InputEvent>* event_reader =
                new SimpleServerLambdaReader<InputEvent>([this](auto request) -> grpc::Status {
                    VLOG(2) << "InputEvent:" << request->ShortDebugString();
                    absl::Status status;
                    if (request->has_key_event()) {
                        key_event_sender_->send(request->key_event());
                        status = absl::OkStatus();
                    } else if (request->has_mouse_event()) {
                        status = input_event_sender_.Send(request->mouse_event());
                    } else if (request->has_touch_event()) {
                        status = input_event_sender_.Send(request->touch_event());
                    } else if (request->has_android_event()) {
                        status = input_event_sender_.Send(request->android_event());
                    } else if (request->has_pen_event()) {
                        status = input_event_sender_.Send(request->pen_event());
                    } else if (request->has_wheel_event()) {
                        status = input_event_sender_.Send(request->wheel_event());
                    } else {
                        status = absl::InvalidArgumentError(
                                "Unknown event, is the emulator out of date?.");
                    }
                    return AbslStatusToGrpcStatus(status);
                });
        // Note that the event reader will delete itself on completion of
        // the request.
        return event_reader;
    }

    Status getClipboard(ServerContext* context, const Empty* /*request*/,
                        ClipData* reply) override {
        return clipboard_service_.getClipboard(reply);
    }

    Status setClipboard(ServerContext* context, const ClipData* request,
                        Empty* /*reply*/) override {
        return clipboard_service_.setClipboard(ClipboardServiceImpl::getPeerId(*context), *request);
    }

    Status getDisplayConfigurations(ServerContext* context,
                                    const ::google::protobuf::Empty* request,
                                    DisplayConfigurations* reply) override {
        return display_service_.getDisplayConfigurations(context, request, reply);
    }

    Status setDisplayConfigurations(ServerContext* context, const DisplayConfigurations* request,
                                    DisplayConfigurations* reply) override {
        return display_service_.setDisplayConfigurations(context, request, reply);
    }

    Status getDisplayMode(ServerContext* context, const Empty* request,
                          DisplayMode* reply) override {
        return display_service_.getDisplayMode(context, request, reply);
    }

    Status setDisplayMode(ServerContext* context, const DisplayMode* request,
                          Empty* reply) override {
        return display_service_.setDisplayMode(context, request, reply);
    }

    Status streamScreenshot(ServerContext* context, const ImageFormat* request,
                            grpc::ServerWriter<Image>* writer) override {
        return display_service_.streamScreenshot(context, request, writer);
    }

    Status getScreenshot(ServerContext* context, const ImageFormat* request,
                         Image* reply) override {
        return display_service_.getScreenshot(context, request, reply);
    }

    ::grpc::ServerWriteReactor<Notification>* streamNotification(
            ::grpc::CallbackServerContext* /*context*/, const Empty* /*request*/) override {
        // Starts microphone notifications, so the store holds the state this stream starts with.
        microphone_service_.Watch();
        return CreateNotificationStreamWriter(&grpc_notification_channel_, &notification_store_);
    }

    ::grpc::ServerWriteReactor<AudioPacket>* streamAudio(::grpc::CallbackServerContext* /*context*/,
                                                         const AudioFormat* request) override {
        return new AudioStreamWriter(*request);
    }

  private:
    VmServiceImpl vm_service_;
    GrpcNotificationEventSource& grpc_notification_channel_;
    NotificationStore notification_store_;
    // After notification_store_, so the store hears its first notification.
    MicrophoneServiceImpl microphone_service_;
    const std::unique_ptr<keyboard::IKeyEventSender> key_event_sender_;
    StatusServiceImpl status_service_;
    BatteryServiceImpl battery_service_;
    SensorServiceImpl sensor_service_;
    GpsServiceImpl gps_service_;
    FingerprintServiceImpl fingerprint_service_;
    ClipboardServiceImpl clipboard_service_;
    InputEventSender input_event_sender_;
    DisplayServiceImpl display_service_;
};

std::shared_ptr<grpc::Service> getEmulatorController(VmOperations* vm,
                                                     QemuConsole* keyboard_console,
                                                     AvdUniverse* avd_universe,
                                                     IMultiDisplay* multidisplay) {
    return std::make_shared<EmulatorControllerImpl>(vm, keyboard_console, avd_universe,
                                                    multidisplay);
}

}  // namespace control
}  // namespace emulation
}  // namespace android
