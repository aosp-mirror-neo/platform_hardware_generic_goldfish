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

#include "legacy_console_controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <thread>

#include "absl/synchronization/notification.h"

#include "goldfish/async/async_socket_server.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/testing/fake_async_socket.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/network/endpoint.h"

namespace goldfish::telnet {
namespace {

using ::goldfish::async::LibuvEventLoop;
using ::goldfish::async::testing::MockAsyncSocketFactory;
using ::goldfish::network::Endpoint;
using ::goldfish::network::ToEndpoint;
using ::goldfish::network::ToIpv4Address;
using ::goldfish::network::ToIpv6Address;
using ::testing::_;
using ::testing::Return;

class MockAsyncSocketServer : public goldfish::async::AsyncSocketServer {
  public:
    MOCK_METHOD(goldfish::network::Endpoint, GetEndpoint, (), (const, override));
    MOCK_METHOD(void, Close, (), (override));
    MOCK_METHOD(goldfish::async::EventLoop*, GetLoop, (), (const, override));
};

using ::goldfish::async::ThreadedEventLoop;

class LegacyConsoleControllerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_done_ = std::make_unique<absl::Notification>();
        watchdog_ = std::thread([this]() {
            if (!test_done_->WaitForNotificationWithTimeout(absl::Seconds(30))) {
                const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
                fprintf(stderr, "FATAL: Test timed out after 30 seconds. Possible hang in %s.%s\n",
                        info->test_suite_name(), info->name());
                std::abort();
            }
        });
        loop_ = ThreadedEventLoop::Create(LibuvEventLoop::Create("TestLoop"));
    }

    void TearDown() override {
        if (test_done_) {
            test_done_->Notify();
        }
        if (watchdog_.joinable()) {
            watchdog_.join();
        }
    }

    goldfish::metrics::MetricsReporter metrics_reporter_;
    std::unique_ptr<ThreadedEventLoop> loop_;
    std::unique_ptr<absl::Notification> test_done_;
    std::thread watchdog_;
};

TEST_F(LegacyConsoleControllerTest, StartsBothServers) {
    ::testing::NiceMock<MockAsyncSocketFactory> factory;
    int port = 5554;
    LegacyConsoleController controller(factory, loop_.get(), metrics_reporter_);

    const auto k_ipv4_loopback = ToIpv4Address(127, 0, 0, 1);
    auto ipv4_endpoint = ToEndpoint(k_ipv4_loopback, port);

    const auto k_ipv6_loopback = ToIpv6Address(0, 0, 0, 0, 0, 0, 0, 1);
    auto ipv6_endpoint = ToEndpoint(k_ipv6_loopback, port);

    auto mock_ipv4_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();
    auto mock_ipv6_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();

    // Expect IPv4 server creation
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv4_endpoint, _, _))
            .WillOnce(Return(mock_ipv4_server));

    // Expect IPv6 server creation
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv6_endpoint, _, _))
            .WillOnce(Return(mock_ipv6_server));

    auto status = controller.Start(port);
    EXPECT_TRUE(status.ok());
}

TEST_F(LegacyConsoleControllerTest, HandlesIpv4Failure) {
    ::testing::NiceMock<MockAsyncSocketFactory> factory;
    int port = 5554;
    LegacyConsoleController controller(factory, loop_.get(), metrics_reporter_);

    const auto k_ipv4_loopback = ToIpv4Address(127, 0, 0, 1);
    auto ipv4_endpoint = ToEndpoint(k_ipv4_loopback, port);

    const auto k_ipv6_loopback = ToIpv6Address(0, 0, 0, 0, 0, 0, 0, 1);
    auto ipv6_endpoint = ToEndpoint(k_ipv6_loopback, port);

    auto mock_ipv6_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();

    // Fail IPv4
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv4_endpoint, _, _)).WillOnce(Return(nullptr));

    // IPv6 server creation is not expected, as we exit fast.
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv6_endpoint, _, _)).Times(0);

    auto status = controller.Start(port);
    EXPECT_FALSE(status.ok());  // Fails because IPv4 failed
}

TEST_F(LegacyConsoleControllerTest, AnyFamilyHandlesIpv4Failure) {
    ::testing::NiceMock<MockAsyncSocketFactory> factory;
    int port = 5554;
    LegacyConsoleController controller(factory, loop_.get(), metrics_reporter_);

    auto ipv4_endpoint = ToEndpoint(ToIpv4Address(127, 0, 0, 1), port);
    auto ipv6_endpoint = ToEndpoint(ToIpv6Address(0, 0, 0, 0, 0, 0, 0, 1), port);

    auto mock_ipv6_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();

    // IPv4 fails, e.g. on an IPv6-only host, but IPv6 still starts.
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv4_endpoint, _, _)).WillOnce(Return(nullptr));
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv6_endpoint, _, _))
            .WillOnce(Return(mock_ipv6_server));

    auto status = controller.Start(port, LegacyConsoleController::LoopbackRequirement::kAnyFamily);
    EXPECT_TRUE(status.ok()) << status;
}

TEST_F(LegacyConsoleControllerTest, HandlesIpv6Failure) {
    ::testing::NiceMock<MockAsyncSocketFactory> factory;
    int port = 5554;
    LegacyConsoleController controller(factory, loop_.get(), metrics_reporter_);

    auto ipv4_endpoint = ToEndpoint(ToIpv4Address(127, 0, 0, 1), port);
    auto ipv6_endpoint = ToEndpoint(ToIpv6Address(0, 0, 0, 0, 0, 0, 0, 1), port);

    auto mock_ipv4_server = std::make_shared<::testing::NiceMock<MockAsyncSocketServer>>();

    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv4_endpoint, _, _))
            .WillOnce(Return(mock_ipv4_server));
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv6_endpoint, _, _)).WillOnce(Return(nullptr));

    auto status = controller.Start(port);
    EXPECT_TRUE(status.ok()) << status;
}

TEST_F(LegacyConsoleControllerTest, AnyFamilyFailsWhenBothFamiliesFail) {
    ::testing::NiceMock<MockAsyncSocketFactory> factory;
    int port = 5554;
    LegacyConsoleController controller(factory, loop_.get(), metrics_reporter_);

    auto ipv4_endpoint = ToEndpoint(ToIpv4Address(127, 0, 0, 1), port);
    auto ipv6_endpoint = ToEndpoint(ToIpv6Address(0, 0, 0, 0, 0, 0, 0, 1), port);

    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv4_endpoint, _, _)).WillOnce(Return(nullptr));
    EXPECT_CALL(factory, CreateServer(loop_.get(), ipv6_endpoint, _, _)).WillOnce(Return(nullptr));

    auto status = controller.Start(port, LegacyConsoleController::LoopbackRequirement::kAnyFamily);
    EXPECT_FALSE(status.ok());
}

}  // namespace
}  // namespace goldfish::telnet
