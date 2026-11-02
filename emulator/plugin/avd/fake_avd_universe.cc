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

#include "goldfish/avd/fake_avd_universe.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "emulator/plugin/grpc/input/contact_table.h"
#include "emulator/plugin/grpc/input/input_dispatcher.h"
#include "emulator/plugin/grpc/input/keyboard_sink.h"
#include "emulator/plugin/grpc/input/touch_device.h"
#include "goldfish/avd/avd_info.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "input_service.pb.h"

namespace goldfish::avd {

namespace {

class NoTouchDevices : public grpc::v2::TouchDevices {
  public:
    absl::StatusOr<grpc::v2::TouchDeviceId> Resolve(int32_t display_id) override {
        return absl::UnavailableError(absl::StrCat(
                "Cannot resolve touch display ", display_id,
                " because FakeAvdUniverse has no displays; configure a touch-capable universe "
                "for touch tests."));
    }
    grpc::v2::TouchDeviceSink* Find(grpc::v2::TouchDeviceId /*device*/) override { return nullptr; }
    std::vector<grpc::v2::TouchDeviceId> Devices() override { return {}; }
};

class NoOpKeyboard : public grpc::v2::KeyboardSink {
  public:
    void WriteKey(uint32_t /*evdev_code*/, bool /*down*/) override {}
    void TypeText(std::string_view /*utf8*/) override {}
    void Reset() override {}
};

// Maps only `evdev_code`, without the QEMU key tables.
uint32_t ResolveEvdevCode(const ::android::emulation::v2::KeyEvent& key) {
    return key.key_identifier_case() == ::android::emulation::v2::KeyEvent::kEvdevCode &&
                           key.evdev_code() > 0
                   ? static_cast<uint32_t>(key.evdev_code())
                   : 0U;
}

}  // namespace

FakeAvdUniverse::FakeAvdUniverse()
        : AvdUniverse(std::make_unique<AvdProperties>(), virtual_clock_)
        , input_loop_(async::testing::TestEventLoop::Create("FakeAvdUniverseInput"))
        , touch_devices_(std::make_unique<NoTouchDevices>())
        , keyboard_(std::make_unique<NoOpKeyboard>())
        , input_dispatcher_(std::make_shared<grpc::v2::InputDispatcher>(
                  *input_loop_, *touch_devices_, keyboard_.get(), ResolveEvdevCode)) {
    SetGlobalAvdUniverse(this);
}

FakeAvdUniverse::~FakeAvdUniverse() {
    input_dispatcher_->Shutdown();
    SetGlobalAvdUniverse(nullptr);
}

async::EventLoop& FakeAvdUniverse::GetQemuEventLoop() {
    LOG(FATAL) << "GetQemuEventLoop not implemented";
}

metrics::MetricsReporter& FakeAvdUniverse::GetMetricsReporter() {
    return metrics_reporter_;
}

android::goldfish::VmOperations& FakeAvdUniverse::GetVmOperations() {
    LOG(FATAL) << "GetVmOperations not implemented";
}

display::IMultiDisplay& FakeAvdUniverse::GetMultiDisplay() const {
    LOG(FATAL) << "GetMultiDisplay not implemented";
}

std::shared_ptr<grpc::v2::IInputDispatcher> FakeAvdUniverse::GetInputDispatcher() {
    return input_dispatcher_;
}

void FakeAvdUniverse::OnPreGrpcShutdown() {
    input_dispatcher_->Shutdown();
}

absl::Status FakeAvdUniverse::OnSave(archive::IWriter&) const {
    return absl::UnimplementedError(__func__);
}

absl::Status FakeAvdUniverse::OnLoad(archive::IReader&) {
    return absl::UnimplementedError(__func__);
}

}  // namespace goldfish::avd
