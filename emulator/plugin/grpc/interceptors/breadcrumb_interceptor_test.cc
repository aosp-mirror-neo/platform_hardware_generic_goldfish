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
#include "android/control/interceptor/breadcrumb_interceptor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

#include "absl/time/time.h"
#include "grpc_diagnostic.pb.h"

#include "android/crashreport/thread.h"
#include "breadcrumb.pb.h"
#include "emulator/crashreport/include/android/crashreport/breadcrumb_proto.h"
#include "emulator_controller.pb.h"

namespace android::control::interceptor {

using namespace testing;
using namespace grpc::experimental;
using ::android::control::breadcrumbs::Breadcrumb;

static_assert(static_cast<int>(grpc::StatusCode::OK) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::OK),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::CANCELLED) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::CANCELLED),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::UNKNOWN) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::UNKNOWN),
              "gRPC status code mismatch");
static_assert(
        static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT) ==
                static_cast<int>(android::control::breadcrumbs::GrpcPayload::INVALID_ARGUMENT),
        "gRPC status code mismatch");
static_assert(
        static_cast<int>(grpc::StatusCode::DEADLINE_EXCEEDED) ==
                static_cast<int>(android::control::breadcrumbs::GrpcPayload::DEADLINE_EXCEEDED),
        "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::NOT_FOUND) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::NOT_FOUND),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::ALREADY_EXISTS) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::ALREADY_EXISTS),
              "gRPC status code mismatch");
static_assert(
        static_cast<int>(grpc::StatusCode::PERMISSION_DENIED) ==
                static_cast<int>(android::control::breadcrumbs::GrpcPayload::PERMISSION_DENIED),
        "gRPC status code mismatch");
static_assert(
        static_cast<int>(grpc::StatusCode::RESOURCE_EXHAUSTED) ==
                static_cast<int>(android::control::breadcrumbs::GrpcPayload::RESOURCE_EXHAUSTED),
        "gRPC status code mismatch");
static_assert(
        static_cast<int>(grpc::StatusCode::FAILED_PRECONDITION) ==
                static_cast<int>(android::control::breadcrumbs::GrpcPayload::FAILED_PRECONDITION),
        "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::ABORTED) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::ABORTED),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::OUT_OF_RANGE) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::OUT_OF_RANGE),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::UNIMPLEMENTED) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::UNIMPLEMENTED),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::INTERNAL) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::INTERNAL),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::UNAVAILABLE) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::UNAVAILABLE),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::DATA_LOSS) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::DATA_LOSS),
              "gRPC status code mismatch");
static_assert(static_cast<int>(grpc::StatusCode::UNAUTHENTICATED) ==
                      static_cast<int>(android::control::breadcrumbs::GrpcPayload::UNAUTHENTICATED),
              "gRPC status code mismatch");

class MockInterceptorBatchMethods : public grpc::experimental::InterceptorBatchMethods {
  public:
    MOCK_METHOD(bool, QueryInterceptionHookPoint, (grpc::experimental::InterceptionHookPoints),
                (override));
    MOCK_METHOD(void, Proceed, (), (override));
    MOCK_METHOD(void, Hijack, (), (override));
    MOCK_METHOD(grpc::ByteBuffer*, GetSerializedSendMessage, (), (override));
    MOCK_METHOD(const void*, GetSendMessage, (), (override));
    MOCK_METHOD(void, ModifySendMessage, (const void*), (override));
    MOCK_METHOD(bool, GetSendMessageStatus, (), (override));
    MOCK_METHOD((std::multimap<grpc::string, grpc::string>*), GetSendInitialMetadata, (),
                (override));
    MOCK_METHOD(grpc::Status, GetSendStatus, (), (override));
    MOCK_METHOD(void, ModifySendStatus, (const grpc::Status&), (override));
    MOCK_METHOD((std::multimap<grpc::string, grpc::string>*), GetSendTrailingMetadata, (),
                (override));
    MOCK_METHOD(void*, GetRecvMessage, (), (override));
    MOCK_METHOD((std::multimap<grpc::string_ref, grpc::string_ref>*), GetRecvInitialMetadata, (),
                (override));
    MOCK_METHOD(grpc::Status*, GetRecvStatus, (), (override));
    MOCK_METHOD((std::multimap<grpc::string_ref, grpc::string_ref>*), GetRecvTrailingMetadata, (),
                (override));
    MOCK_METHOD((std::unique_ptr<grpc::ChannelInterface>), GetInterceptedChannel, (), (override));
    MOCK_METHOD(void, FailHijackedRecvMessage, (), (override));
    MOCK_METHOD(void, FailHijackedSendMessage, (), (override));
};

std::vector<Breadcrumb> GetAllCrumbs() {
    using android::control::breadcrumbs::GrpcPayload;
    using android::crashreport::BreadcrumbEnvelope;
    using android::crashreport::BreadcrumbPhase;
    using android::crashreport::PayloadType;

    std::vector<Breadcrumb> result;
    BreadcrumbInterceptor::GetLogForTesting()->ForEach([&](const void* data, uint16_t size) {
        if (size < sizeof(BreadcrumbEnvelope)) return true;

        const auto* envelope = static_cast<const BreadcrumbEnvelope*>(data);
        Breadcrumb event;
        event.set_flow_id(envelope->flow_id);
        event.set_timestamp_ns(envelope->timestamp_ns);
        event.set_thread_id(envelope->thread_id);

        switch (envelope->phase) {
        case BreadcrumbPhase::kFlowBegin:
            event.set_phase(Breadcrumb::FLOW_BEGIN);
            break;
        case BreadcrumbPhase::kFlowStep:
            event.set_phase(Breadcrumb::FLOW_STEP);
            break;
        case BreadcrumbPhase::kFlowEnd:
            event.set_phase(Breadcrumb::FLOW_END);
            break;
        default:
            event.set_phase(Breadcrumb::INSTANT);
            break;
        }

        const char* payload_ptr = static_cast<const char*>(data) + sizeof(BreadcrumbEnvelope);
        uint16_t payload_len = envelope->payload_len;

        if (payload_len > 0 && envelope->payload_type == PayloadType::kGrpcProto) {
            GrpcPayload grpc;
            if (grpc.ParseFromArray(payload_ptr, payload_len)) {
                *event.mutable_grpc() = grpc;
            }
        }

        result.push_back(event);
        return true;
    });
    return result;
}

class BreadcrumbInterceptorTest : public ::testing::Test {};

TEST_F(BreadcrumbInterceptorTest, LogsStartOnCreation) {
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>();

    {
        auto interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
                factory->CreateClientInterceptor(nullptr));
    }

    auto crumbs = GetAllCrumbs();

    ASSERT_GE(crumbs.size(), 2);
    // The last two should be BEGIN and END for the interceptor we just destroyed.
    EXPECT_EQ(crumbs[crumbs.size() - 2].phase(), Breadcrumb::FLOW_BEGIN);
    EXPECT_EQ(crumbs.back().phase(), Breadcrumb::FLOW_END);

    EXPECT_EQ(crumbs[crumbs.size() - 2].grpc().grpc_phase(),
              android::control::breadcrumbs::GrpcPayload::START);
    EXPECT_EQ(crumbs.back().grpc().grpc_phase(),
              android::control::breadcrumbs::GrpcPayload::END_OF_CALL);
}

TEST_F(BreadcrumbInterceptorTest, HandlesLargePayloadsByDroppingDetail) {
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>(/*capture_payload=*/true);
    auto interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
            factory->CreateClientInterceptor(nullptr));

    MockInterceptorBatchMethods methods;
    EXPECT_CALL(methods, QueryInterceptionHookPoint(
                                 grpc::experimental::InterceptionHookPoints::PRE_SEND_MESSAGE))
            .WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(methods, QueryInterceptionHookPoint(::testing::Ne(
                                 grpc::experimental::InterceptionHookPoints::PRE_SEND_MESSAGE)))
            .WillRepeatedly(::testing::Return(false));
    EXPECT_CALL(methods, Proceed());

    // Use EmulatorStatus and fill it with garbage to exceed 200 bytes.
    android::emulation::control::EmulatorStatus large_status;
    large_status.set_version(std::string(300, 'A'));
    EXPECT_CALL(methods, GetSendMessage()).WillOnce(::testing::Return(&large_status));

    interceptor->Intercept(&methods);

    auto crumbs = GetAllCrumbs();

    bool found_large = false;
    for (const auto& c : crumbs) {
        if (c.phase() == Breadcrumb::FLOW_STEP && c.has_grpc() &&
            c.grpc().grpc_phase() == android::control::breadcrumbs::GrpcPayload::PRE_SEND_MESSAGE &&
            c.grpc().msg_size() > 200) {
            EXPECT_FALSE(c.grpc().has_payload());  // Payload should be dropped as it's too big
            found_large = true;
        }
    }
    EXPECT_TRUE(found_large);
}

TEST_F(BreadcrumbInterceptorTest, DefaultConfigurationBehavior) {
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>();
    auto interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
            factory->CreateServerInterceptor(nullptr));

    MockInterceptorBatchMethods methods;
    EXPECT_CALL(methods, QueryInterceptionHookPoint(
                                 grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE))
            .WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(methods, QueryInterceptionHookPoint(::testing::Ne(
                                 grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE)))
            .WillRepeatedly(::testing::Return(false));
    EXPECT_CALL(methods, Proceed());

    android::emulation::control::BatteryState battery_state;
    battery_state.set_chargelevel(80);
    EXPECT_CALL(methods, GetRecvMessage()).WillOnce(::testing::Return(&battery_state));

    interceptor->Intercept(&methods);

    auto crumbs = GetAllCrumbs();

    bool found = false;
    for (const auto& c : crumbs) {
        if (c.phase() == Breadcrumb::FLOW_STEP && c.has_grpc() &&
            c.grpc().grpc_phase() ==
                    android::control::breadcrumbs::GrpcPayload::POST_RECV_MESSAGE) {
#if defined(CONFIG_TRACE_GOLDFISH)
            EXPECT_TRUE(c.grpc().has_payload());
#else
            EXPECT_FALSE(c.grpc().has_payload());
            EXPECT_GT(c.grpc().msg_size(), 0);
#endif
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(BreadcrumbInterceptorTest, ExplicitDisableDropsAllPayloads) {
    const size_t start_index = GetAllCrumbs().size();
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>(/*capture_payload=*/false);
    auto interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
            factory->CreateServerInterceptor(nullptr));

    MockInterceptorBatchMethods methods;
    EXPECT_CALL(methods, QueryInterceptionHookPoint(
                                 grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE))
            .WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(methods, QueryInterceptionHookPoint(::testing::Ne(
                                 grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE)))
            .WillRepeatedly(::testing::Return(false));
    EXPECT_CALL(methods, Proceed()).Times(2);

    android::emulation::control::BatteryState battery_state;
    battery_state.set_chargelevel(80);
    EXPECT_CALL(methods, GetRecvMessage()).WillOnce(::testing::Return(&battery_state));
    interceptor->Intercept(&methods);

    android::emulation::control::KeyboardEvent key_event;
    key_event.set_key("SecretPassword");
    EXPECT_CALL(methods, GetRecvMessage()).WillOnce(::testing::Return(&key_event));
    interceptor->Intercept(&methods);

    auto crumbs = GetAllCrumbs();
    int dropped_count = 0;
    for (size_t i = start_index; i < crumbs.size(); ++i) {
        const auto& c = crumbs[i];
        if (c.phase() == Breadcrumb::FLOW_STEP && c.has_grpc() &&
            c.grpc().grpc_phase() ==
                    android::control::breadcrumbs::GrpcPayload::POST_RECV_MESSAGE) {
            EXPECT_FALSE(c.grpc().has_payload());
            EXPECT_GT(c.grpc().msg_size(), 0);
            dropped_count++;
        }
    }
    EXPECT_EQ(dropped_count, 2);
}

TEST_F(BreadcrumbInterceptorTest, ExplicitEnableCapturesAllPayloads) {
    const size_t start_index = GetAllCrumbs().size();
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>(/*capture_payload=*/true);
    auto interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
            factory->CreateServerInterceptor(nullptr));

    MockInterceptorBatchMethods methods;
    EXPECT_CALL(methods, QueryInterceptionHookPoint(
                                 grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE))
            .WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(methods, QueryInterceptionHookPoint(::testing::Ne(
                                 grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE)))
            .WillRepeatedly(::testing::Return(false));
    EXPECT_CALL(methods, Proceed()).Times(2);

    android::emulation::control::BatteryState battery_state;
    battery_state.set_chargelevel(80);
    EXPECT_CALL(methods, GetRecvMessage()).WillOnce(::testing::Return(&battery_state));
    interceptor->Intercept(&methods);

    android::emulation::control::KeyboardEvent key_event;
    key_event.set_key("A");
    EXPECT_CALL(methods, GetRecvMessage()).WillOnce(::testing::Return(&key_event));
    interceptor->Intercept(&methods);

    auto crumbs = GetAllCrumbs();
    int captured_count = 0;
    for (size_t i = start_index; i < crumbs.size(); ++i) {
        const auto& c = crumbs[i];
        if (c.phase() == Breadcrumb::FLOW_STEP && c.has_grpc() &&
            c.grpc().grpc_phase() ==
                    android::control::breadcrumbs::GrpcPayload::POST_RECV_MESSAGE) {
            EXPECT_TRUE(c.grpc().has_payload());
            captured_count++;
        }
    }
    EXPECT_EQ(captured_count, 2);
}

TEST_F(BreadcrumbInterceptorTest, VerifiedCircularBufferWrap) {
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>();

    // 8KB buffer, each message is ~20-30 bytes.
    // Logging 1000 times should definitely wrap.
    for (int i = 0; i < 1000; ++i) {
        auto interceptor = std::unique_ptr<Interceptor>(factory->CreateClientInterceptor(nullptr));
    }

    auto crumbs = GetAllCrumbs();

    // We expect some valid messages at the end of the buffer.
    ASSERT_GT(crumbs.size(), 10);
    EXPECT_EQ(crumbs.back().phase(), Breadcrumb::FLOW_END);
}

TEST_F(BreadcrumbInterceptorTest, CapturesCorrectThreadId) {
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>();
    uint64_t expected_id = android::crashreport::GetOsThreadId();

    {
        auto interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
                factory->CreateClientInterceptor(nullptr));
    }

    auto crumbs = GetAllCrumbs();
    ASSERT_GE(crumbs.size(), 2);

    // Check that the thread_id matches our current thread for both events.
    EXPECT_EQ(crumbs[crumbs.size() - 2].thread_id(), expected_id);
    EXPECT_EQ(crumbs.back().thread_id(), expected_id);
}

TEST_F(BreadcrumbInterceptorTest, CapturesThreadSwitches) {
    auto factory = std::make_unique<BreadcrumbInterceptorFactory>();

    uint64_t main_thread_id = android::crashreport::GetOsThreadId();
    static std::atomic<uint64_t> s_background_thread_id{0};

    std::unique_ptr<grpc::experimental::Interceptor> interceptor;

    interceptor = std::unique_ptr<grpc::experimental::Interceptor>(
            factory->CreateClientInterceptor(nullptr));

    MockInterceptorBatchMethods methods;
    EXPECT_CALL(methods,
                QueryInterceptionHookPoint(
                        grpc::experimental::InterceptionHookPoints::PRE_SEND_INITIAL_METADATA))
            .WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(methods,
                QueryInterceptionHookPoint(::testing::Ne(
                        grpc::experimental::InterceptionHookPoints::PRE_SEND_INITIAL_METADATA)))
            .WillRepeatedly(::testing::Return(false));
    EXPECT_CALL(methods, Proceed());

    std::thread t([&]() {
        s_background_thread_id.store(android::crashreport::GetOsThreadId(),
                                     std::memory_order_relaxed);
        interceptor->Intercept(&methods);
    });
    t.join();

    auto crumbs = GetAllCrumbs();
    ASSERT_GE(crumbs.size(), 2);

    bool found_begin = false;
    bool found_step = false;

    uint64_t bg_tid = s_background_thread_id.load(std::memory_order_relaxed);

    for (const auto& c : crumbs) {
        if (c.phase() == Breadcrumb::FLOW_BEGIN) {
            EXPECT_EQ(c.thread_id(), main_thread_id);
            found_begin = true;
        } else if (c.phase() == Breadcrumb::FLOW_STEP) {
            EXPECT_EQ(c.thread_id(), bg_tid);
            found_step = true;
        }
    }

    EXPECT_TRUE(found_begin);
    EXPECT_TRUE(found_step);
}

}  // namespace android::control::interceptor
