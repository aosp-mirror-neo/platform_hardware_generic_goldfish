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

#include "goldfish/videobridge/in_process_input_sender.h"

#include <string_view>
#include <utility>

#include "absl/log/log.h"

namespace goldfish::videobridge {
namespace {

template <typename Dispatcher, typename Event>
void DispatchIfStarted(bool started, const Dispatcher& dispatcher, const Event& event,
                       std::string_view label) {
    if (!started) {
        LOG(WARNING) << "Dropping WebRTC " << label << " (" << event.ShortDebugString()
                     << "): delivery is not active.";
        return;
    }
    if (dispatcher) {
        if (auto status = dispatcher(event); !status.ok()) {
            LOG(WARNING) << "Failed to dispatch WebRTC " << label << " ("
                         << event.ShortDebugString() << "): " << status;
        }
    }
}

}  // namespace

InProcessInputSender::InProcessInputSender(EventDispatcher dispatcher,
                                           V2EventDispatcher v2_dispatcher,
                                           V2SensorsDispatcher v2_sensors_dispatcher,
                                           StopCallback on_stop)
        : dispatcher_(std::move(dispatcher))
        , v2_dispatcher_(std::move(v2_dispatcher))
        , v2_sensors_dispatcher_(std::move(v2_sensors_dispatcher))
        , on_stop_(std::move(on_stop)) {}

absl::Status InProcessInputSender::Start() {
    started_.store(true, std::memory_order_release);
    VLOG(1) << "InProcessInputSender started.";
    return absl::OkStatus();
}

void InProcessInputSender::SendEvent(const InputEvent& event) {
    DispatchIfStarted(started_.load(std::memory_order_acquire), dispatcher_, event, "input event");
}

void InProcessInputSender::SendV2Event(const V2InputEvent& event) {
    DispatchIfStarted(started_.load(std::memory_order_acquire), v2_dispatcher_, event,
                      "v2 input event");
}

void InProcessInputSender::SendV2SensorsUpdate(const V2SensorsUpdateRequest& request) {
    DispatchIfStarted(started_.load(std::memory_order_acquire), v2_sensors_dispatcher_, request,
                      "v2 sensors update");
}

void InProcessInputSender::Stop() {
    const bool was_started = started_.exchange(false, std::memory_order_acq_rel);
    VLOG(1) << "InProcessInputSender stopped.";
    if (was_started && on_stop_) {
        on_stop_();
    }
}

}  // namespace goldfish::videobridge
