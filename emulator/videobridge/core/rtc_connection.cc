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

#include "goldfish/videobridge/rtc_connection.h"

#include <utility>
// Disable compiler warnings for external third-party headers. We wrap these in localized
// pragma blocks rather than using target 'copts' so that thread-safety analysis remains
// active on our own local source files.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-reference-return"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "api/audio/audio_mixer.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/network.h"
#pragma clang diagnostic pop

#include "goldfish/videobridge/codec_factories.h"
#include "goldfish/videobridge/goldfish_audio_device_module.h"

namespace goldfish::videobridge {

RtcConnection::RtcConnection() : RtcConnection(NetworkSubstrate{}) {}

RtcConnection::RtcConnection(NetworkSubstrate substrate,
                             ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device)
        : task_factory_(::webrtc::CreateDefaultTaskQueueFactory())
        , network_thread_(substrate.socket_server ? std::make_unique<webrtc::Thread>(
                                                            std::move(substrate.socket_server))
                                                  : webrtc::Thread::CreateWithSocketServer())
        , worker_thread_(webrtc::Thread::Create())
        , signaling_thread_(webrtc::Thread::Create())
        , audio_device_(audio_device ? std::move(audio_device)
                                     : GoldfishAudioDeviceModule::Create()) {
    network_thread_->SetName("Sw-Network", nullptr);
    network_thread_->Start();
    worker_thread_->SetName("Sw-Worker", nullptr);
    worker_thread_->Start();
    signaling_thread_->SetName("Sw-Signaling", nullptr);
    signaling_thread_->Start();

    // Instantiate and cache the default NetworkManager and PacketSocketFactory.
    // They must remain valid for the lifetime of the connection (and any PortAllocators).
    ::webrtc::Environment env = ::webrtc::CreateEnvironment();
    network_manager_ = substrate.network_manager ? substrate.network_manager(network_thread_.get())
                                                 : std::make_unique<::webrtc::BasicNetworkManager>(
                                                           env, network_thread_->socketserver());
    // Reads back whichever socket server the network thread was given, so a
    // substituted one propagates to every port this connection allocates.
    socket_factory_ =
            std::make_unique<::webrtc::BasicPacketSocketFactory>(network_thread_->socketserver());

    connection_factory_ = ::webrtc::CreatePeerConnectionFactory(
            network_thread_.get(), worker_thread_.get(), signaling_thread_.get(), audio_device_,
            ::webrtc::CreateBuiltinAudioEncoderFactory(),
            ::webrtc::CreateBuiltinAudioDecoderFactory(), CreatePlatformVideoEncoderFactory(),
            CreatePlatformVideoDecoderFactory(), nullptr /* audio_mixer */,
            nullptr /* audio_processing */);
}

RtcConnection::~RtcConnection() {
    connection_factory_ = nullptr;
    // Both are bound to the network thread and may hop onto it as they go, so
    // they have to be released while it is still turning.
    socket_factory_.reset();
    network_manager_.reset();
    signaling_thread_->Stop();
    worker_thread_->Stop();
    network_thread_->Stop();
}

}  // namespace goldfish::videobridge
