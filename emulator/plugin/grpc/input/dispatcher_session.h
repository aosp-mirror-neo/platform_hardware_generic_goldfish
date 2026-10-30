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

#include <memory>

#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2 {

/**
 * @brief One input client's session with the input dispatcher: the client's
 * identity (the options its session was opened with) and the route its v2 input
 * events take to the guest.
 *
 * Constructing it opens the session; destroying it closes the session, which
 * releases only this client's contacts and keys.
 *
 * ### Thread Safety:
 * `Dispatch()` may be called from any thread, but the dispatcher applies one
 * client's events in the order they were dispatched, so a client should
 * dispatch from one thread to keep its order.
 *
 * ### Ownership:
 * Shares ownership of the dispatcher, so a session held by a long-lived
 * transport (e.g., a WebRTC channel) can safely close after the dispatcher's
 * owner has shut the dispatcher down and torn down its loop and sinks. Once
 * `Shutdown()` has run, events complete with `UNAVAILABLE` and closing is a
 * no-op without touching loop-only state.
 */
class DispatcherSession {
  public:
    using DispatchCallback = IInputDispatcher::DispatchCallback;

    /** @brief Opens a session on `dispatcher`, which must not be null. */
    DispatcherSession(std::shared_ptr<IInputDispatcher> dispatcher,
                      IInputDispatcher::SessionOptions options);
    /** @brief Closes the session. */
    ~DispatcherSession();

    DispatcherSession(const DispatcherSession&) = delete;
    DispatcherSession& operator=(const DispatcherSession&) = delete;

    /**
     * @brief Dispatches the pointer, key or text in `event` for this session,
     * as `IInputDispatcher::DispatchPointer()` and friends do.
     *
     * @param done Runs once with the result. An axis event completes with
     *     `UNIMPLEMENTED`, as no device takes rotary input, and an event
     *     without a payload with `INVALID_ARGUMENT`; both on the calling
     *     thread, without reaching the dispatcher.
     */
    void Dispatch(const ::android::emulation::v2::InputEvent& event,
                  DispatchCallback done = nullptr);

    /**
     * @brief Returns the tracker incremented whenever the dispatcher forcibly
     * clears contacts held by this session.
     */
    const SessionResetTracker& ResetTracker() const { return *reset_tracker_; }

  private:
    const std::shared_ptr<IInputDispatcher> dispatcher_;
    const std::shared_ptr<SessionResetTracker> reset_tracker_;
    const InputSessionId id_;
};

}  // namespace goldfish::grpc::v2
