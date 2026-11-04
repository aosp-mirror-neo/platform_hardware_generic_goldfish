// Copyright (C) 2017 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "goldfish/devices/sensor/sensor_device.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "absl/strings/str_format.h"
#include "absl/time/time.h"

#include "android/goldfish/fake_hardware_config.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/devices/qemud/qemud.h"
#include "goldfish/devices/test_connector_registry.h"
#include "goldfish/system/testing/fake_clocks.h"

namespace goldfish::devices::sensor {

using async::testing::TestEventLoop;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::HasSubstr;

namespace {
int CountOccurrences(const std::string& text, const std::string& target) {
    int count = 0;
    std::string::size_type pos = 0;

    while ((pos = text.find(target, pos)) != std::string::npos) {
        ++count;
        pos += target.length();
    }

    return count;
}
}  // namespace

class SensorDeviceTest : public ::testing::Test {
    void SetUp() override {
        hw_ = android::goldfish::FakeHardwareConfig::GetHwConfig();
        physical_model_ = std::make_unique<PhysicalModel>(hw_, clock_);
        client_loop_ = TestEventLoop::Create();
        qemu_loop_ = TestEventLoop::Create();

        ISensorDevice::RegisterDevice(physical_model_.get(), &registry_,
                                      /*avd_type=*/android::goldfish::DeviceType::kPhone,
                                      /*avd_api=*/30, hw_, client_loop_.get(), qemu_loop_.get(),
                                      clock_);
        device_ = registry_.ConstructHalDevice<ISensorDevice>();
        test_socket_ = registry_.HalSocket();
        Clear();
        device_->OnConnect();
    }

    void TearDown() override {
        registry_.Close();
        client_loop_->RunAll();
    }

  public:
    void Receive(std::string_view msg) {
        (void)client_loop_->Post([&, this] { device_->OnReceive(qemud::EncodeQemudPacket(msg)); });
        client_loop_->RunAll();
    }
    void Clear() { test_socket_->storage.clear(); }

  protected:
    android::goldfish::HardwareConfig hw_;
    std::unique_ptr<PhysicalModel> physical_model_;
    std::unique_ptr<TestEventLoop> client_loop_;
    std::unique_ptr<TestEventLoop> qemu_loop_;
    TestConnectorRegistry registry_;
    ::goldfish::system::testing::FakeVirtualClock clock_;
    ISensorDevice* device_;
    TestHalSocket* test_socket_;
};

TEST_F(SensorDeviceTest, canCreateDevice) {
    EXPECT_NE(device_, nullptr);
}

TEST_F(SensorDeviceTest, canListSensors) {
    Receive("list-sensors");
    EXPECT_THAT(test_socket_->storage, Eq("0006133119"));
}

TEST_F(SensorDeviceTest, canSetSensors) {
    Receive("set:acceleration:0");
    Receive("set:gyroscope:0");
    Clear();

    // The active set should have changed.
    Receive("list-sensors");
    EXPECT_THAT(test_socket_->storage, Eq("0006133116"));
}

TEST_F(SensorDeviceTest, setDelayCausesATick) {
    clock_.Set(absl::Nanoseconds(1234567890));
    Receive("set-delay:10");
    // The looper keeps ticking so just check for the first few digits.
    EXPECT_THAT(test_socket_->storage, HasSubstr("0015guest-sync:1234"));
}

TEST_F(SensorDeviceTest, setTimeOffset) {
    clock_.Set(absl::Nanoseconds(1234567890));
    Receive("time:100");
    Receive("set-delay:1");
    EXPECT_THAT(test_socket_->storage, HasSubstr("000Eguest-sync:10"));
}

TEST_F(SensorDeviceTest, timeKeepsOnRolling) {
    clock_.Set(absl::Nanoseconds(1234567890));
    Receive("set-delay:1");
    Clear();
    EXPECT_THAT(test_socket_->storage, Eq(""));
    for (int i = 0; i < 11; i++) client_loop_->AdvanceClock(absl::Milliseconds(10));

    // We should see a sync several times.
    EXPECT_THAT(CountOccurrences(test_socket_->storage, "guest-sync:"), Gt(10));
}

}  // namespace goldfish::devices::sensor
