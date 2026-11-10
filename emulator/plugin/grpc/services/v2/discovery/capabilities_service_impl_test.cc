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

#include "capabilities_service_impl.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "android/goldfish/fake_hardware_config.h"
#include "capabilities_service.grpc.pb.h"
#include "goldfish/avd/avd_properties.h"
#include "goldfish/avd/avd_universe.h"
#include "goldfish/system/testing/fake_clocks.h"

#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::AccessMode;
using ::android::emulation::v2::CapabilitiesService;
using ::android::emulation::v2::ChargerSource;
using ::android::emulation::v2::DeviceCapabilities;
using ::android::emulation::v2::Fidelity;
using ::android::emulation::v2::FoldAxis;
using ::android::emulation::v2::GetDeviceCapabilitiesRequest;
using ::android::emulation::v2::InputFeature;
using ::android::emulation::v2::SampleFormat;
using ::android::emulation::v2::VmOperation;
using ::android::goldfish::FakeHardwareConfig;
using ::goldfish::avd::AvdProperties;
using ::goldfish::avd::AvdUniverse;
using ::grpc::ServerContext;
using ::grpc::Status;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Not;

class TestUniverseClockHolder {
  protected:
    ::goldfish::system::testing::FakeVirtualClock virtual_clock_;
};

class TestUniverse : private TestUniverseClockHolder, public AvdUniverse {
  public:
    explicit TestUniverse(std::unique_ptr<AvdProperties> props)
            : AvdUniverse(std::move(props), virtual_clock_) {}

    async::EventLoop& GetQemuEventLoop() override {
        LOG(FATAL) << "GetQemuEventLoop not implemented in test";
    }

    metrics::MetricsReporter& GetMetricsReporter() override {
        LOG(FATAL) << "GetMetricsReporter not implemented in test";
    }

    android::goldfish::VmOperations& GetVmOperations() override {
        LOG(FATAL) << "GetVmOperations not implemented in test";
    }

    display::IMultiDisplay& GetMultiDisplay() const override {
        LOG(FATAL) << "GetMultiDisplay not implemented in test";
    }

    std::shared_ptr<IInputDispatcher> GetInputDispatcher() override {
        LOG(FATAL) << "GetInputDispatcher not implemented in test";
    }

    absl::Status OnSave(archive::IWriter&) const override { return absl::OkStatus(); }
    absl::Status OnLoad(archive::IReader&) override { return absl::OkStatus(); }
};

class CapabilitiesServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto props = std::make_unique<AvdProperties>();
        props->avd_name = "test-pixel-9";
        props->avd_id = "pixel_9_api_35";
        props->avd_api = 35;
        props->avd_abi = "x86_64";
        props->build_id = "AP2A.240805.005";
        props->hw_config = FakeHardwareConfig::GetHwConfig();
        props->hw_config.hw_cpu_ncore = 4;
        props->hw_config.hw_ramSize = 4096;
        props->hw_config.hw_lcd_width = 1080;
        props->hw_config.hw_lcd_height = 2424;
        props->hw_config.hw_lcd_density = 428;
        props->hw_config.hw_rotaryInput = true;

        universe_ = std::make_unique<TestUniverse>(std::move(props));
        service_ = std::make_unique<CapabilitiesServiceImpl>(*universe_);
    }

    std::unique_ptr<TestUniverse> universe_;
    std::unique_ptr<CapabilitiesServiceImpl> service_;
};

TEST_F(CapabilitiesServiceTest, ReturnsAccurateDeviceMetadata) {
    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext context;

    Status status = service_->GetDeviceCapabilities(&context, &request, &reply);
    ASSERT_TRUE(status.ok()) << "gRPC error: " << status.error_message();

    ASSERT_TRUE(reply.has_metadata());
    const auto& meta = reply.metadata();
    EXPECT_EQ(meta.display_name(), "test-pixel-9");
    EXPECT_EQ(meta.model_id(), "pixel_9_api_35");
    EXPECT_EQ(meta.api_level(), 35);
    EXPECT_EQ(meta.server_version(), "2.0.0");
    EXPECT_EQ(meta.server_implementation(), "goldfish-grpc-v2");
    EXPECT_EQ(meta.cpu_architecture(), "x86_64");
    EXPECT_EQ(meta.cpu_core_count(), 4);
    EXPECT_EQ(meta.total_ram_bytes(), 4096LL * 1024 * 1024);
    EXPECT_TRUE(meta.supports_play_store());
    EXPECT_EQ(meta.manufacturer(), "Google");
    EXPECT_EQ(meta.brand(), "Google");
    EXPECT_GT(meta.total_storage_bytes(), 0);
}

TEST_F(CapabilitiesServiceTest, ReturnsDisplayCapabilities) {
    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext context;

    Status status = service_->GetDeviceCapabilities(&context, &request, &reply);
    ASSERT_TRUE(status.ok());

    ASSERT_TRUE(reply.has_display());
    const auto& display = reply.display();
    ASSERT_GE(display.built_in_displays_size(), 1);
    EXPECT_EQ(display.built_in_displays(0).width(), 1080);
    EXPECT_EQ(display.built_in_displays(0).height(), 2424);
    EXPECT_EQ(display.built_in_displays(0).density_dpi(), 428);
    EXPECT_TRUE(display.supports_virtual_display_creation());
    EXPECT_TRUE(display.has_screen_capture());
}

TEST_F(CapabilitiesServiceTest, ReturnsSubsystemsCapabilities) {
    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext context;

    Status status = service_->GetDeviceCapabilities(&context, &request, &reply);
    ASSERT_TRUE(status.ok());

    // Battery
    ASSERT_TRUE(reply.has_battery());
    EXPECT_EQ(reply.battery().access_mode(), AccessMode::ACCESS_MODE_READ_WRITE);
    EXPECT_EQ(reply.battery().fidelity(), Fidelity::FIDELITY_HARDWARE);
    EXPECT_THAT(reply.battery().supported_chargers(),
                ElementsAre(ChargerSource::CHARGER_SOURCE_AC));

    // Location
    ASSERT_TRUE(reply.has_location());
    EXPECT_EQ(reply.location().access_mode(), AccessMode::ACCESS_MODE_READ_WRITE);
    EXPECT_EQ(reply.location().fidelity(), Fidelity::FIDELITY_HARDWARE);

    // Audio
    ASSERT_TRUE(reply.has_audio());
    EXPECT_EQ(reply.audio().access_mode(), AccessMode::ACCESS_MODE_READ_WRITE);
    EXPECT_EQ(reply.audio().fidelity(), Fidelity::FIDELITY_HARDWARE);
    ASSERT_TRUE(reply.audio().has_microphone_profile());
    ASSERT_TRUE(reply.audio().has_max_concurrent_microphone_streams());
    EXPECT_EQ(reply.audio().max_concurrent_microphone_streams(), 4);
    EXPECT_TRUE(reply.audio().supports_wired_headset());
    EXPECT_THAT(reply.audio().microphone_profile().supported_formats(),
                ElementsAre(SampleFormat::SAMPLE_FORMAT_PCM16_BIT,
                            SampleFormat::SAMPLE_FORMAT_F32_BIT));
    EXPECT_THAT(reply.audio().microphone_profile().supported_sample_rates_hz(),
                ElementsAre(8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000));
    EXPECT_THAT(reply.audio().microphone_profile().supported_channel_counts(), ElementsAre(1, 2));
    ASSERT_TRUE(reply.audio().has_speaker_profile());
    EXPECT_THAT(reply.audio().speaker_profile().supported_formats(),
                ElementsAre(SampleFormat::SAMPLE_FORMAT_PCM16_BIT,
                            SampleFormat::SAMPLE_FORMAT_F32_BIT));
    EXPECT_THAT(reply.audio().speaker_profile().supported_sample_rates_hz(),
                ElementsAre(8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000));
    EXPECT_THAT(reply.audio().speaker_profile().supported_channel_counts(), ElementsAre(1, 2));

    // System
    ASSERT_TRUE(reply.has_system());
    EXPECT_EQ(reply.system().access_mode(), AccessMode::ACCESS_MODE_READ_WRITE);
    EXPECT_TRUE(reply.system().supports_reboot());
    EXPECT_TRUE(reply.system().has_biometrics());
    EXPECT_TRUE(reply.system().has_clipboard());

    // VM
    ASSERT_TRUE(reply.has_vm());
    EXPECT_EQ(reply.vm().access_mode(), AccessMode::ACCESS_MODE_READ_WRITE);
    EXPECT_THAT(reply.vm().supported_operations(),
                Contains(VmOperation::VM_OPERATION_PAUSE_RESUME));

    // Input
    ASSERT_TRUE(reply.has_input());
    EXPECT_EQ(reply.input().max_touch_point_count(), 10);
    EXPECT_TRUE(reply.input().supports_input_activity_observation());
    EXPECT_EQ(reply.input().max_batch_duration_ms(), 30000);
    EXPECT_THAT(reply.input().supported_features(), Contains(InputFeature::INPUT_FEATURE_TOUCH));
    EXPECT_THAT(reply.input().supported_features(),
                Contains(InputFeature::INPUT_FEATURE_MULTI_TOUCH));
    EXPECT_THAT(reply.input().supported_features(), Contains(InputFeature::INPUT_FEATURE_KEYBOARD));
    EXPECT_THAT(reply.input().supported_features(),
                Not(Contains(InputFeature::INPUT_FEATURE_STYLUS)));
    EXPECT_THAT(reply.input().supported_features(),
                Not(Contains(InputFeature::INPUT_FEATURE_MOUSE_POINTER)));
    EXPECT_THAT(reply.input().supported_features(),
                Not(Contains(InputFeature::INPUT_FEATURE_RELATIVE_MOUSE)));
    EXPECT_THAT(reply.input().supported_features(),
                Not(Contains(InputFeature::INPUT_FEATURE_ROTARY_ENCODER)));
}

TEST_F(CapabilitiesServiceTest, ReturnsPostureCapabilitiesWhenConfigured) {
    auto props = std::make_unique<AvdProperties>();
    props->hw_config = FakeHardwareConfig::GetHwConfig();
    props->hw_config.hw_sensor_hinge = true;
    props->hw_config.hw_sensor_hinge_count = 1;
    props->hw_config.hw_sensor_hinge_type = 1;      // Vertical (book fold)
    props->hw_config.hw_sensor_hinge_sub_type = 0;  // Fold on screen
    props->hw_config.hw_sensor_hinge_defaults = "180";
    props->hw_config.hw_sensor_hinge_ranges = "0-180";
    props->hw_config.hw_sensor_hinge_areas = "50-0";
    props->hw_config.hw_sensor_posture_list = "1, 2, 3";
    props->hw_config.hw_sensor_hinge_angles_posture_definitions = "0-30, 30-150, 150-180";

    auto universe = std::make_unique<TestUniverse>(std::move(props));
    CapabilitiesServiceImpl service(*universe);

    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext server_context;

    Status status = service.GetDeviceCapabilities(&server_context, &request, &reply);
    ASSERT_TRUE(status.ok());

    ASSERT_TRUE(reply.sensors().has_posture());
    const auto& posture = reply.sensors().posture();
    EXPECT_EQ(posture.access_mode(), AccessMode::ACCESS_MODE_READ_WRITE);
    ASSERT_GE(posture.hinges_size(), 1);
    EXPECT_EQ(posture.hinges(0).hinge_id(), 0);
    EXPECT_EQ(posture.hinges(0).fold_axis(), FoldAxis::FOLD_AXIS_VERTICAL);
}

TEST_F(CapabilitiesServiceTest, OmitsLocationCapabilitiesWhenDisabled) {
    auto props = std::make_unique<AvdProperties>();
    props->hw_config = FakeHardwareConfig::GetHwConfig();
    props->hw_config.hw_gps = false;

    auto universe = std::make_unique<TestUniverse>(std::move(props));
    CapabilitiesServiceImpl service(*universe);

    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext server_context;

    Status status = service.GetDeviceCapabilities(&server_context, &request, &reply);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(reply.has_location());
}

TEST_F(CapabilitiesServiceTest, OmitsBatteryCapabilitiesWhenDisabled) {
    auto props = std::make_unique<AvdProperties>();
    props->hw_config = FakeHardwareConfig::GetHwConfig();
    props->hw_config.hw_battery = false;

    auto universe = std::make_unique<TestUniverse>(std::move(props));
    CapabilitiesServiceImpl service(*universe);

    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext server_context;

    Status status = service.GetDeviceCapabilities(&server_context, &request, &reply);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(reply.has_battery());
}

TEST_F(CapabilitiesServiceTest, OmitsAudioCapabilitiesWhenDisabled) {
    auto props = std::make_unique<AvdProperties>();
    props->hw_config = FakeHardwareConfig::GetHwConfig();
    props->hw_config.hw_audioOutput = false;
    props->hw_config.hw_audioInput = false;

    auto universe = std::make_unique<TestUniverse>(std::move(props));
    CapabilitiesServiceImpl service(*universe);

    GetDeviceCapabilitiesRequest request;
    DeviceCapabilities reply;
    ServerContext server_context;

    Status status = service.GetDeviceCapabilities(&server_context, &request, &reply);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(reply.has_audio());
}

TEST_F(CapabilitiesServiceTest, AudioProfilesFollowInputAndOutputHardwareFlags) {
    {
        auto props = std::make_unique<AvdProperties>();
        props->hw_config = FakeHardwareConfig::GetHwConfig();
        props->hw_config.hw_audioInput = true;
        props->hw_config.hw_audioOutput = false;

        auto universe = std::make_unique<TestUniverse>(std::move(props));
        CapabilitiesServiceImpl service(*universe);

        GetDeviceCapabilitiesRequest request;
        DeviceCapabilities reply;
        ServerContext server_context;
        ASSERT_TRUE(service.GetDeviceCapabilities(&server_context, &request, &reply).ok());
        ASSERT_TRUE(reply.has_audio());
        EXPECT_TRUE(reply.audio().has_microphone_profile());
        EXPECT_TRUE(reply.audio().has_max_concurrent_microphone_streams());
        EXPECT_EQ(reply.audio().max_concurrent_microphone_streams(), 4);
        EXPECT_FALSE(reply.audio().has_speaker_profile());
    }
    {
        auto props = std::make_unique<AvdProperties>();
        props->hw_config = FakeHardwareConfig::GetHwConfig();
        props->hw_config.hw_audioInput = false;
        props->hw_config.hw_audioOutput = true;

        auto universe = std::make_unique<TestUniverse>(std::move(props));
        CapabilitiesServiceImpl service(*universe);

        GetDeviceCapabilitiesRequest request;
        DeviceCapabilities reply;
        ServerContext server_context;
        ASSERT_TRUE(service.GetDeviceCapabilities(&server_context, &request, &reply).ok());
        ASSERT_TRUE(reply.has_audio());
        EXPECT_FALSE(reply.audio().has_microphone_profile());
        EXPECT_FALSE(reply.audio().has_max_concurrent_microphone_streams());
        EXPECT_TRUE(reply.audio().has_speaker_profile());
    }
}

}  // namespace
}  // namespace goldfish::grpc::v2
