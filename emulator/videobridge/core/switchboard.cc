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

#include "goldfish/videobridge/switchboard.h"

#include <utility>

#include "absl/log/log.h"

#include "goldfish/videobridge/webrtc_logging.h"
#include "participant.h"

namespace goldfish::videobridge {

Switchboard::Switchboard(std::shared_ptr<MediaProvider> media_provider,
                         InputSenderFactory input_sender_factory,
                         ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device)
        : Switchboard(std::move(media_provider), std::move(input_sender_factory),
                      NetworkSubstrate{}, std::move(audio_device)) {}

Switchboard::Switchboard(std::shared_ptr<MediaProvider> media_provider,
                         InputSenderFactory input_sender_factory, NetworkSubstrate substrate,
                         ::webrtc::scoped_refptr<GoldfishAudioDeviceModule> audio_device)
        : RtcConnection(std::move(substrate), std::move(audio_device))
        , media_provider_(std::move(media_provider))
        , input_sender_factory_(std::move(input_sender_factory)) {
    ConfigureWebRtcLogging();
}

Switchboard::~Switchboard() {
    std::vector<std::shared_ptr<Participant>> active_connections;
    {
        const absl::MutexLock lock(&connections_mutex_);
        for (auto& [_, participant] : connections_) {
            active_connections.push_back(participant);
        }
    }
    for (auto& participant : active_connections) {
        participant->Close();
        participant->WaitForClose();
    }
    const absl::MutexLock lock(&connections_mutex_);
    connections_.clear();
}

bool Switchboard::Connect(const std::string& identity, const std::string& turn_config) {
    const absl::MutexLock lock(&connections_mutex_);
    if (connections_.count(identity) > 0) {
        VLOG(1) << "Connect called for already connected WebRTC participant: " << identity;
        return true;
    }

    auto participant = std::make_shared<Participant>(*this, identity, turn_config, media_provider_);
    auto status = participant->Initialize();
    if (!status.ok()) {
        LOG(ERROR) << "Failed to initialize WebRTC participant session for '" << identity
                   << "': " << status;
        return false;
    }

    connections_[identity] = participant;
    GetOrCreateQueue(identity);
    LOG(INFO) << "Successfully created WebRTC participant session for: " << identity;
    return true;
}

void Switchboard::Disconnect(const std::string& identity) {
    std::shared_ptr<Participant> participant;
    {
        const absl::MutexLock lock(&connections_mutex_);
        auto it = connections_.find(identity);
        if (it == connections_.end()) {
            VLOG(1) << "Disconnect called for inactive/unknown participant: " << identity;
            return;
        }
        participant = std::move(it->second);
        connections_.erase(it);
    }

    if (participant) {
        participant->Close();
    }
    RemoveQueue(identity);
    LOG(INFO) << "Successfully disconnected and cleaned up WebRTC session for participant: "
              << identity;
}

absl::Status Switchboard::AcceptJsepMessage(const std::string& identity, const std::string& msg) {
    std::shared_ptr<Participant> participant;
    {
        const absl::MutexLock lock(&connections_mutex_);
        auto it = connections_.find(identity);
        if (it == connections_.end()) {
            return absl::NotFoundError(absl::StrCat(
                    "JSEP message received for unknown/inactive participant: ", identity));
        }
        participant = it->second;
    }

    auto json_msg = nlohmann::json::parse(msg, nullptr, false);
    if (json_msg.is_discarded()) {
        return absl::InvalidArgumentError(
                absl::StrCat("Failed to parse JSEP signal message from participant ", identity,
                             " (Invalid JSON payload): ", msg));
    }

    VLOG(1) << "Routing incoming JSEP message to participant " << identity;
    participant->IncomingMessage(json_msg);
    return absl::OkStatus();
}

bool Switchboard::HasSession(const std::string& identity) {
    const absl::MutexLock lock(&connections_mutex_);
    return connections_.contains(identity);
}

absl::StatusOr<std::string> Switchboard::NextMessage(const std::string& identity,
                                                     absl::Duration timeout) {
    auto queue = GetQueue(identity);
    if (!queue) {
        return absl::NotFoundError(absl::StrCat("Participant queue not found for: ", identity));
    }
    const absl::MutexLock lock(&queue->mutex);
    auto ready = [&queue]() {
        queue->mutex.AssertHeld();
        return !queue->queue.empty() || queue->closed;
    };
    if (queue->mutex.AwaitWithTimeout(absl::Condition(&ready), timeout)) {
        // Drain remaining messages before reporting closure.
        if (!queue->queue.empty()) {
            std::string next_message = std::move(queue->queue.front());
            queue->queue.pop();
            return next_message;
        }
        return absl::CancelledError(
                absl::StrCat("Session closed while waiting for participant: ", identity));
    }
    return absl::DeadlineExceededError(
            absl::StrCat("Timeout waiting for next JSEP message from participant: ", identity));
}

void Switchboard::RtcConnectionClosed(std::string participant) {
    LOG(INFO) << "WebRTC connection closed notification received for participant: " << participant;
}

std::unique_ptr<InputSender> Switchboard::CreateInputSender(DataChannelLabel label) {
    if (input_sender_factory_) {
        return input_sender_factory_(label);
    }
    return nullptr;
}

void Switchboard::Send(std::string to, const nlohmann::json& msg) {
    auto queue = GetQueue(to);
    if (!queue) {
        VLOG(1) << "Send: Dropping message to already disconnected participant: " << to;
        return;
    }

    // Enqueueing is the whole delivery path: a reader blocked in NextMessage
    // re-evaluates its await condition as soon as this lock is released, so it
    // sees the message immediately rather than on its next poll.
    const absl::MutexLock lock(&queue->mutex);
    queue->queue.push(msg.dump());
}

std::shared_ptr<Switchboard::ParticipantQueue> Switchboard::GetOrCreateQueue(
        const std::string& identity) {
    const absl::MutexLock lock(&queues_mutex_);
    auto it = message_queues_.find(identity);
    if (it != message_queues_.end()) {
        return it->second;
    }
    auto queue = std::make_shared<ParticipantQueue>();
    message_queues_[identity] = queue;
    return queue;
}

std::shared_ptr<Switchboard::ParticipantQueue> Switchboard::GetQueue(const std::string& identity) {
    const absl::MutexLock lock(&queues_mutex_);
    auto it = message_queues_.find(identity);
    if (it != message_queues_.end()) {
        return it->second;
    }
    return nullptr;
}

void Switchboard::RemoveQueue(const std::string& identity) {
    std::shared_ptr<ParticipantQueue> queue;
    {
        const absl::MutexLock lock(&queues_mutex_);
        auto node = message_queues_.extract(identity);
        if (node.empty()) {
            return;
        }
        queue = std::move(node.mapped());
    }
    // Wake blocked readers; close outside queues_mutex_ to avoid lock nesting.
    queue->Close();
}

}  // namespace goldfish::videobridge
