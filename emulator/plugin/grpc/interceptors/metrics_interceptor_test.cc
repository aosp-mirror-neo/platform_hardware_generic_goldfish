/* Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "android/control/interceptor/metrics_interceptor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "android/control/interceptor/logging_interceptor.h"
#include "goldfish/metrics/metrics_reporter.h"

namespace android::control::interceptor {

using namespace testing;
using namespace grpc;
using namespace grpc::experimental;

class MockMetricsWriter : public ::goldfish::metrics::MetricsWriter {
  public:
    virtual ~MockMetricsWriter() = default;
    MOCK_METHOD(void, Write, (::goldfish::metrics::MetricsEvent event), (override));
};

struct Completion {
    void Notify() ABSL_LOCKS_EXCLUDED(mutex) {
        const absl::MutexLock lock(mutex);
        notified = true;
    }

    void Wait() ABSL_LOCKS_EXCLUDED(mutex) {
        const absl::MutexLock lock(mutex);
        mutex.Await(absl::Condition(&notified));
    }

    bool notified ABSL_GUARDED_BY(mutex) = false;
    absl::Mutex mutex;
};

class MetricsInterceptorTest : public Test {
  protected:
    void SetUp() override {
        auto writer = std::make_unique<NiceMock<MockMetricsWriter>>();
        writer_ptr_ = writer.get();
        reporter_ = std::make_unique<::goldfish::metrics::MetricsReporter>(
                ::goldfish::metrics::Uuid::Generate());
        reporter_->SetWriter(std::move(writer));
    }

    std::unique_ptr<::goldfish::metrics::MetricsReporter> reporter_;
    MockMetricsWriter* writer_ptr_;
};

class MockInterceptorBatchMethods : public InterceptorBatchMethods {
  public:
    MOCK_METHOD(bool, QueryInterceptionHookPoint, (InterceptionHookPoints), (override));
    MOCK_METHOD(void, Proceed, (), (override));
    MOCK_METHOD(void, Hijack, (), (override));
    MOCK_METHOD(ByteBuffer*, GetSerializedSendMessage, (), (override));
    MOCK_METHOD(const void*, GetSendMessage, (), (override));
    MOCK_METHOD(void, ModifySendMessage, (const void*), (override));
    MOCK_METHOD(bool, GetSendMessageStatus, (), (override));
    MOCK_METHOD((std::multimap<grpc::string, grpc::string>*), GetSendInitialMetadata, (),
                (override));
    MOCK_METHOD(Status, GetSendStatus, (), (override));
    MOCK_METHOD(void, ModifySendStatus, (const Status&), (override));
    MOCK_METHOD((std::multimap<grpc::string, grpc::string>*), GetSendTrailingMetadata, (),
                (override));
    MOCK_METHOD(void*, GetRecvMessage, (), (override));
    MOCK_METHOD((std::multimap<grpc::string_ref, grpc::string_ref>*), GetRecvInitialMetadata, (),
                (override));
    MOCK_METHOD(Status*, GetRecvStatus, (), (override));
    MOCK_METHOD((std::multimap<grpc::string_ref, grpc::string_ref>*), GetRecvTrailingMetadata, (),
                (override));
    MOCK_METHOD((std::unique_ptr<ChannelInterface>), GetInterceptedChannel, (), (override));
    MOCK_METHOD(void, FailHijackedRecvMessage, (), (override));
    MOCK_METHOD(void, FailHijackedSendMessage, (), (override));
};

TEST_F(MetricsInterceptorTest, FactoryCreatesInterceptor) {
    MetricsInterceptorFactory factory(*reporter_);
    auto interceptor = std::unique_ptr<Interceptor>(factory.CreateServerInterceptor(nullptr));
    EXPECT_NE(interceptor, nullptr);
}

TEST_F(MetricsInterceptorTest, RecordAndFlushMetrics) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_EQ(grpc.requests(), 1);
                EXPECT_EQ(grpc.failures(), 0);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory factory(*reporter_);
        auto interceptor = std::unique_ptr<Interceptor>(factory.CreateServerInterceptor(nullptr));

        MockInterceptorBatchMethods batchMethods;
        EXPECT_CALL(batchMethods, Proceed());
        EXPECT_CALL(batchMethods, GetSendStatus()).WillOnce(Return(Status::OK));
        EXPECT_CALL(batchMethods, QueryInterceptionHookPoint(_)).WillRepeatedly(Return(true));
        EXPECT_CALL(batchMethods, GetRecvMessage()).WillRepeatedly(Return(nullptr));
        EXPECT_CALL(batchMethods, GetSendMessage()).WillRepeatedly(Return(nullptr));

        interceptor->Intercept(&batchMethods);
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, InvocationMetricsAggregates) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_EQ(grpc.requests(), 2);
                EXPECT_EQ(grpc.failures(), 1);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "M";
        r.status = Status::OK;
        metrics.Record(r);
        r.status = Status::CANCELLED;
        metrics.Record(r);
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, RecordClientMetrics) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_EQ(grpc.type(), android_studio::EmulatorGrpc::CLIENT);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "ClientMethod";
        r.direction = Direction::kOutgoing;
        r.status = Status::OK;
        metrics.Record(r);
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, StreamingCollectsByteCounts) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                // Percentile Estimator should have 1 raw sample (since we only added 1)
                EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample_size(), 1);
                EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample(0), 1024);
                EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample_size(), 1);
                EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample(0), 2048);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "StreamMethod";
        r.type = CallType::kBidiStreaming;
        r.rcv_bytes = 1024;
        r.snd_bytes = 2048;
        r.status = Status::OK;
        metrics.Record(r);
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, UnarySkipsByteCounts) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample_size(), 0);
                EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample_size(), 0);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "UnaryMethod";
        r.type = CallType::kUnary;
        r.rcv_bytes = 1024;
        r.snd_bytes = 2048;
        r.status = Status::OK;
        metrics.Record(r);
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, BucketizedMetricsExported) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_GT(grpc.duration().bucket_size(), 0);
                EXPECT_EQ(grpc.duration().raw_sample_size(), 0);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "BucketizedMethod";
        r.type = CallType::kUnary;
        r.status = Status::OK;
        for (int i = 0; i < 33; ++i) {
            r.duration = i * 1000;
            metrics.Record(r);
        }
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, ServerStreamingSkipsRcvByteCounts) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample_size(), 0);
                EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample_size(), 1);
                EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample(0), 2048);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "ServerStreamMethod";
        r.type = CallType::kServerStreaming;
        r.rcv_bytes = 1024;
        r.snd_bytes = 2048;
        r.status = Status::OK;
        metrics.Record(r);
    }

    completion->Wait();
}

TEST_F(MetricsInterceptorTest, ClientStreamingSkipsSndByteCounts) {
    auto completion = std::make_shared<Completion>();

    EXPECT_CALL(*writer_ptr_, Write(_))
            .WillOnce(Invoke([completion](::goldfish::metrics::MetricsEvent event) {
                auto& grpc = event.as_event.emulator_details().grpc();
                EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample_size(), 1);
                EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample(0), 1024);
                EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample_size(), 0);
                completion->Notify();
            }));

    {
        MetricsInterceptorFactory metrics(*reporter_);
        InvocationRecord r;
        r.method = "ClientStreamMethod";
        r.type = CallType::kClientStreaming;
        r.rcv_bytes = 1024;
        r.snd_bytes = 2048;
        r.status = Status::OK;
        metrics.Record(r);
    }

    completion->Wait();
}

}  // namespace android::control::interceptor
