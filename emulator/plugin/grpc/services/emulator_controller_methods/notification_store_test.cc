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

#include "notification_store.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "emulator_controller.grpc.pb.h"
#include "goldfish/avd_universe/grpc/grpc_notification_channel.h"
#include "notification_stream_writer.h"
#include "test/grpc_service_test.h"

namespace android {
namespace emulation {
namespace control {

using ::goldfish::avd_universe::grpc::GrpcNotification;
using ::goldfish::avd_universe::grpc::GrpcNotificationEventSource;
using ::google::protobuf::Empty;
using ::testing::UnorderedElementsAre;

class NotificationStoreTest : public ::testing::Test {
  protected:
    void SetUp() override { notification_store_ = std::make_unique<NotificationStore>(&source_); }

    GrpcNotificationEventSource source_;
    std::unique_ptr<NotificationStore> notification_store_;
};

TEST_F(NotificationStoreTest, StoreStickyNotifications) {
    GrpcNotification boot_event;
    boot_event.mutable_booted()->set_time(1000);
    source_.FireEvent(boot_event);

    GrpcNotification posture_event;
    posture_event.mutable_posture()->set_value(Posture::POSTURE_OPENED);
    source_.FireEvent(posture_event);

    auto latest = notification_store_->GetLatest();
    EXPECT_EQ(latest.size(), 2);

    bool found_boot = false;
    bool found_posture = false;
    for (const auto& n : latest) {
        if (n.has_booted()) {
            found_boot = true;
            EXPECT_EQ(n.booted().time(), 1000);
        } else if (n.has_posture()) {
            found_posture = true;
            EXPECT_EQ(n.posture().value(), Posture::POSTURE_OPENED);
        }
    }
    EXPECT_TRUE(found_boot);
    EXPECT_TRUE(found_posture);
}

TEST_F(NotificationStoreTest, KeepsOnlyTheLatestMicrophoneState) {
    GrpcNotification enabled;
    enabled.mutable_microphonestate()->set_realaudioenabled(true);
    source_.FireEvent(enabled);
    GrpcNotification disabled;
    disabled.mutable_microphonestate()->set_realaudioenabled(false);
    source_.FireEvent(disabled);

    auto latest = notification_store_->GetLatest();
    ASSERT_EQ(latest.size(), 1);
    ASSERT_TRUE(latest[0].has_microphonestate());
    EXPECT_FALSE(latest[0].microphonestate().realaudioenabled());
}

TEST_F(NotificationStoreTest, NonStickyNotStored) {
    GrpcNotification brightness_event;
    brightness_event.mutable_brightness()->set_value(50);
    source_.FireEvent(brightness_event);

    auto latest = notification_store_->GetLatest();
    EXPECT_EQ(latest.size(), 0);
}

class MockEmulatorControllerService
        : public EmulatorController::WithCallbackMethod_streamNotification<
                  EmulatorController::Service> {
  public:
    MockEmulatorControllerService(GrpcNotificationEventSource* source, NotificationStore* store)
            : source_(source), store_(store) {}

    ::grpc::ServerWriteReactor<Notification>* streamNotification(
            ::grpc::CallbackServerContext* /*context*/, const Empty* /*request*/) override {
        return CreateNotificationStreamWriter(source_, store_);
    }

  private:
    GrpcNotificationEventSource* source_;
    NotificationStore* store_;
};

class NotificationReplayTest : public GrcpServiceTest {
  protected:
    void SetUp() override {
        notification_store_ = std::make_unique<NotificationStore>(&source_);
        service_ = std::make_unique<MockEmulatorControllerService>(&source_,
                                                                   notification_store_.get());
        GrcpServiceTest::SetUp();
    }

    EmulatorController::Service* getService() override { return service_.get(); }

    GrpcNotificationEventSource source_;
    std::unique_ptr<NotificationStore> notification_store_;
    std::unique_ptr<MockEmulatorControllerService> service_;
};

TEST_F(NotificationReplayTest, ReplaysStoredNotifications) {
    // 1. Fire some notifications BEFORE the client connects.
    GrpcNotification boot_event;
    boot_event.mutable_booted()->set_time(1234);
    source_.FireEvent(boot_event);

    GrpcNotification posture_event;
    posture_event.mutable_posture()->set_value(Posture::POSTURE_CLOSED);
    source_.FireEvent(posture_event);

    // 2. Connect a client and start streaming.
    Empty request;
    auto context = getContextWithTimeout();
    std::unique_ptr<::grpc::ClientReader<Notification>> reader(
            mStub->streamNotification(context.get(), request));

    // 3. Verify the client immediately receives the stored notifications.
    Notification n1, n2;
    EXPECT_TRUE(reader->Read(&n1));
    EXPECT_TRUE(reader->Read(&n2));

    // The order might depend on the map iteration, so we check both.
    std::vector<Notification> received = {n1, n2};
    bool found_boot = false;
    bool found_posture = false;
    for (const auto& n : received) {
        if (n.has_booted()) {
            found_boot = true;
            EXPECT_EQ(n.booted().time(), 1234);
        } else if (n.has_posture()) {
            found_posture = true;
            EXPECT_EQ(n.posture().value(), Posture::POSTURE_CLOSED);
        }
    }
    EXPECT_TRUE(found_boot);
    EXPECT_TRUE(found_posture);

    // 4. Fire a new event and verify the client receives it.
    GrpcNotification camera_event;
    camera_event.mutable_cameranotification()->set_active(true);
    source_.FireEvent(camera_event);

    Notification n3;
    EXPECT_TRUE(reader->Read(&n3));
    EXPECT_TRUE(n3.has_cameranotification());
    EXPECT_EQ(n3.cameranotification().active(), true);
}

}  // namespace control
}  // namespace emulation
}  // namespace android
