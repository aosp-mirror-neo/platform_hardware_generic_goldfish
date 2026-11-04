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
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "goldfish/videobridge/media_provider.h"
#include "goldfish/videobridge/rtc_connection.h"
#include "nlohmann/json.hpp"

namespace goldfish::videobridge {

class Participant;

/**
 * @class Switchboard
 * @brief Thread-safe manager coordinating participant connection lifecycles, ICE settings, and
 * signal exchanges.
 *
 * Switchboard is the concrete implementation of RtcConnection. It maintains active Participant
 * sessions, handles signaling transport queue allocations, and coordinates callback-driven or
 * blocking signaling retrievals for gRPC service layers.
 */
class Switchboard : public RtcConnection {
  public:
    using InputSenderFactory = std::function<std::unique_ptr<InputSender>(DataChannelLabel)>;

    /**
     * @brief Constructs the Switchboard coordinator.
     *
     * @param media_provider Optional provider injected to set up audio/video streams for
     * participants.
     * @param input_sender_factory Optional factory callback for instantiating custom InputSender
     * channels.
     * @param audio_device Optional audio device module that received audio plays into, for
     * callers that pull it (see GoldfishAudioDeviceModule::PullPlayout()). Null creates one.
     */
    explicit Switchboard(std::shared_ptr<MediaProvider> media_provider = nullptr,
                         InputSenderFactory input_sender_factory = nullptr,
                         ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device = nullptr);

    ~Switchboard() override;

  protected:
    /**
     * @brief Constructs a Switchboard on a caller-supplied network substrate.
     *
     * Lets a subclass run participants over an in-memory network instead of
     * the host's. Behaves identically to the public constructor otherwise.
     */
    Switchboard(std::shared_ptr<MediaProvider> media_provider,
                InputSenderFactory input_sender_factory, NetworkSubstrate substrate,
                ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device = nullptr);

  public:
    /**
     * @brief Establishes a new Participant connection session, initializing its PeerConnection.
     * Thread-safe.
     *
     * @param identity The unique identifier of the client.
     * @param turn_config Serialized JSON string containing TURN/STUN settings.
     * @return true if connection creation was successful.
     */
    bool Connect(const std::string& identity, const std::string& turn_config = "");

    /**
     * @brief Disconnects the participant, purges their signaling queues, and notifies closed
     * callbacks. Thread-safe.
     *
     * @param identity The client identifier to disconnect.
     */
    void Disconnect(const std::string& identity);

    /**
     * @brief Routes an incoming signaling packet (SDP or ICE) from the client to the participant's
     * PeerConnection. Thread-safe.
     *
     * @param identity The client identifier.
     * @param msg The raw JSEP signaling JSON payload.
     * @return absl::Status indicating routing and parsing success.
     */
    absl::Status AcceptJsepMessage(const std::string& identity, const std::string& msg);

    /**
     * @brief Reports whether a participant session is currently connected. Thread-safe.
     *
     * Lets signaling services reject operations on unknown or already torn down sessions rather
     * than silently accepting them.
     *
     * @param identity The client identifier.
     * @return true if the participant has an active connection.
     */
    bool HasSession(const std::string& identity);

    /**
     * @brief Synchronously blocks the calling thread waiting for the next outgoing JSEP message
     * from the participant. Used by unary gRPC or poll-based signaling transports.
     *
     * Returns Cancelled if the session is closed while waiting, and DeadlineExceeded if the
     * timeout elapses first. Callers driving a stream should treat only the latter as a reason
     * to keep waiting.
     *
     * @param identity Client identifier.
     * @param timeout Maximum duration to block.
     * @return absl::StatusOr<std::string> The retrieved signaling payload, or an error status.
     */
    absl::StatusOr<std::string> NextMessage(const std::string& identity, absl::Duration timeout);

    // RtcConnection overrides
    /**
     * @brief Handles terminal closed state transitions for a participant.
     */
    void RtcConnectionClosed(std::string participant) override;

    /**
     * @brief Enqueues an outgoing WebRTC JSEP signaling JSON message from a participant.
     * Immediately dispatches message if an async callback is registered.
     */
    void Send(std::string to, const nlohmann::json& msg) override;

    /**
     * @brief Factory creating a GrpcInputSender stream for the data channel.
     */
    std::unique_ptr<InputSender> CreateInputSender(DataChannelLabel label) override;

  private:
    struct ParticipantQueue {
        absl::Mutex mutex;
        std::queue<std::string> queue;

        // Sticky flag to wake blocked readers on teardown.
        bool closed ABSL_GUARDED_BY(mutex) = false;

        // Unblocks readers waiting on this queue.
        void Close() {
            const absl::MutexLock lock(&mutex);
            closed = true;
        }
    };

    std::shared_ptr<ParticipantQueue> GetOrCreateQueue(const std::string& identity);
    std::shared_ptr<ParticipantQueue> GetQueue(const std::string& identity);
    void RemoveQueue(const std::string& identity);

    std::shared_ptr<MediaProvider> media_provider_;
    InputSenderFactory input_sender_factory_;

    absl::Mutex connections_mutex_;
    absl::flat_hash_map<std::string, std::shared_ptr<Participant>> connections_
            ABSL_GUARDED_BY(connections_mutex_);

    absl::Mutex queues_mutex_;
    absl::flat_hash_map<std::string, std::shared_ptr<ParticipantQueue>> message_queues_
            ABSL_GUARDED_BY(queues_mutex_);
};

}  // namespace goldfish::videobridge
