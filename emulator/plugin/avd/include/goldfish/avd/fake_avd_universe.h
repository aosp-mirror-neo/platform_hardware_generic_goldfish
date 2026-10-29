// Copyright 2026 The Android Open Source Project
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

#include "absl/status/status.h"

#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/system/testing/fake_clocks.h"

namespace goldfish::grpc::v2 {
class KeyboardSink;
class TouchDevices;
}  // namespace goldfish::grpc::v2

namespace goldfish::avd {

namespace detail {
class FakeAvdVirtualClockHolder {
  protected:
    system::testing::FakeVirtualClock virtual_clock_;
};
}  // namespace detail

/**
 * @brief Minimal implementation of `AvdUniverse` for unit tests.
 *
 * Registers itself as the global `AvdUniverse` on construction and clears it on
 * destruction.
 *
 * Its input dispatcher is real, but has no displays and drops keys and text:
 * touches complete with `UNAVAILABLE`, keys and text with `OK`. Keys map only
 * their `evdev_code`. It runs on `input_loop()`, which runs only when the test
 * runs it.
 */
class FakeAvdUniverse : private detail::FakeAvdVirtualClockHolder, public AvdUniverse {
  public:
    FakeAvdUniverse();
    ~FakeAvdUniverse() override;

    async::EventLoop& GetQemuEventLoop() override;
    metrics::MetricsReporter& GetMetricsReporter() override;
    android::goldfish::VmOperations& GetVmOperations() override;
    display::IMultiDisplay& GetMultiDisplay() const override;
    std::shared_ptr<grpc::v2::IInputDispatcher> GetInputDispatcher() override;

    /** @brief Returns the test event loop driving `GetInputDispatcher()`. */
    async::testing::TestEventLoop& input_loop() { return *input_loop_; }

    /** @brief Returns the fake virtual clock backing `GetSensorsPhysicalModel()`. */
    system::testing::FakeVirtualClock& virtual_clock() { return virtual_clock_; }

    absl::Status OnSave(archive::IWriter&) const override;
    absl::Status OnLoad(archive::IReader&) override;

  private:
    metrics::MetricsReporter metrics_reporter_;
    // In this order, so the dispatcher is released first; the destructor
    // shuts it down before, for holders that outlive the universe.
    std::unique_ptr<async::testing::TestEventLoop> input_loop_;
    std::unique_ptr<grpc::v2::TouchDevices> touch_devices_;
    std::unique_ptr<grpc::v2::KeyboardSink> keyboard_;
    std::shared_ptr<grpc::v2::IInputDispatcher> input_dispatcher_;
};

}  // namespace goldfish::avd
