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

#include <atomic>
#include <functional>

#include "goldfish/videobridge/input_sender.h"

namespace goldfish::videobridge {

/**
 * @class InProcessInputSender
 * @brief In-process implementation of InputSender interface for direct memory injection.
 *
 * Delegates InputEvent dispatching directly to in-process C++ callbacks
 * (e.g. InputDispatcher / SensorsServiceImpl) and invokes an optional stop
 * callback when an active data channel transitions to closing/closed.
 */
class InProcessInputSender : public InputSender {
  public:
    using EventDispatcher = std::function<absl::Status(const InputEvent&)>;
    using V2EventDispatcher = std::function<absl::Status(const V2InputEvent&)>;
    using V2SensorsDispatcher = std::function<absl::Status(const V2SensorsUpdateRequest&)>;
    using StopCallback = std::function<void()>;

    explicit InProcessInputSender(EventDispatcher dispatcher,
                                  V2EventDispatcher v2_dispatcher = nullptr,
                                  V2SensorsDispatcher v2_sensors_dispatcher = nullptr,
                                  StopCallback on_stop = nullptr);
    ~InProcessInputSender() override = default;

    absl::Status Start() override;
    void SendEvent(const InputEvent& event) override;
    void SendV2Event(const V2InputEvent& event) override;
    void SendV2SensorsUpdate(const V2SensorsUpdateRequest& request) override;
    void Stop() override;

  private:
    EventDispatcher dispatcher_;
    V2EventDispatcher v2_dispatcher_;
    V2SensorsDispatcher v2_sensors_dispatcher_;
    StopCallback on_stop_;
    std::atomic_bool started_{false};
};

}  // namespace goldfish::videobridge
