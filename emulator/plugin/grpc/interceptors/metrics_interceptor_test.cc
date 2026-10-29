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
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "android/control/interceptor/logging_interceptor.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/metrics/metrics_reporter.h"
#include "goldfish/metrics/testing/fake_metrics_writer.h"

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
    std::unique_ptr<::goldfish::async::testing::TestEventLoop> loop_ =
            ::goldfish::async::testing::TestEventLoop::Create();
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
    MetricsInterceptorFactory factory(*reporter_, loop_.get());
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
        MetricsInterceptorFactory factory(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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
        MetricsInterceptorFactory metrics(*reporter_, loop_.get());
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

namespace {

constexpr absl::Duration kEventWaitTimeout = absl::Seconds(5);

struct PeriodicTestHarness {
    PeriodicTestHarness()
            : reporter(::goldfish::metrics::Uuid::Generate())
            , loop(::goldfish::async::testing::TestEventLoop::Create()) {
        auto fake_writer = std::make_unique<::goldfish::metrics::testing::FakeMetricsWriter>();
        writer = fake_writer.get();
        reporter.SetWriter(std::move(fake_writer));
    }

    // Enqueues a non-gRPC sentinel event on the FIFO MetricsReporter queue and waits for
    // `expected_total_events` to reach `writer`, proving all prior periodic/tail flushes
    // have already been drained without relying on sleep delays.
    bool FlushReporterBarrier(size_t expected_total_events) {
        reporter.Report([](android_studio::AndroidStudioEvent& event) {
            event.set_kind(android_studio::AndroidStudioEvent::EMULATOR_HOST);
        });
        return writer->WaitForEvents(expected_total_events, kEventWaitTimeout);
    }

    ::goldfish::metrics::MetricsReporter reporter;
    ::goldfish::metrics::testing::FakeMetricsWriter* writer = nullptr;
    std::unique_ptr<::goldfish::async::testing::TestEventLoop> loop;
};

uint32_t MethodHash(const std::string& method) {
    return crc32(0, reinterpret_cast<const uint8_t*>(method.data()),
                 static_cast<uInt>(method.size()));
}

// Runs `on_first_write` on the reporter's worker thread before storing the first event.
class HookedFakeMetricsWriter : public ::goldfish::metrics::testing::FakeMetricsWriter {
  public:
    void Write(::goldfish::metrics::MetricsEvent event) override {
        if (auto hook = std::exchange(on_first_write, nullptr)) {
            hook();
        }
        FakeMetricsWriter::Write(std::move(event));
    }

    // Set before the first event is reported; afterwards only the worker thread touches it.
    std::function<void()> on_first_write;
};

}  // namespace

TEST(MetricsInterceptorPeriodicTest, PeriodicFlushReportsAtIntervalAndCancelsOnDestruction) {
    PeriodicTestHarness harness;
    const std::string method = "/android.emulation.control.EmulatorController/getStatus";
    auto factory =
            std::make_unique<MetricsInterceptorFactory>(harness.reporter, harness.loop.get());

    InvocationRecord record;
    record.method = method;
    record.status = Status::OK;
    factory->Record(record);
    record.status = Status::CANCELLED;
    factory->Record(record);

    // Advancing just short of the 5-minute interval must not flush metrics.
    harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval - absl::Seconds(1));
    ASSERT_TRUE(harness.FlushReporterBarrier(/*expected_total_events=*/1));
    {
        absl::MutexLock lock(harness.writer->mutex);
        ASSERT_EQ(harness.writer->events.size(), 1);
        EXPECT_FALSE(harness.writer->events[0].emulator_details().has_grpc());
    }

    // Reaching the 5-minute mark flushes the active method while the factory stays alive.
    harness.loop->AdvanceClock(absl::Seconds(1));
    ASSERT_TRUE(harness.writer->WaitForEvents(2, kEventWaitTimeout));
    {
        absl::MutexLock lock(harness.writer->mutex);
        ASSERT_EQ(harness.writer->events.size(), 2);
        const auto& grpc = harness.writer->events[1].emulator_details().grpc();
        EXPECT_EQ(grpc.type(), android_studio::EmulatorGrpc::SERVER);
        EXPECT_EQ(grpc.call_id(), MethodHash(method));
        EXPECT_EQ(grpc.requests(), 2);
        EXPECT_EQ(grpc.failures(), 1);
    }

    // Tail traffic recorded after the periodic flush is flushed on destruction.
    record.status = Status::OK;
    factory->Record(record);
    factory->Record(record);
    factory->Record(record);
    factory.reset();

    ASSERT_TRUE(harness.writer->WaitForEvents(3, kEventWaitTimeout));
    {
        absl::MutexLock lock(harness.writer->mutex);
        ASSERT_EQ(harness.writer->events.size(), 3);
        const auto& grpc = harness.writer->events[2].emulator_details().grpc();
        EXPECT_EQ(grpc.call_id(), MethodHash(method));
        EXPECT_EQ(grpc.requests(), 3);
        EXPECT_EQ(grpc.failures(), 0);
    }

    // Advancing the event loop after factory destruction must not trigger a dangling timer.
    harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
    ASSERT_TRUE(harness.FlushReporterBarrier(/*expected_total_events=*/4));
    {
        absl::MutexLock lock(harness.writer->mutex);
        ASSERT_EQ(harness.writer->events.size(), 4);
        EXPECT_FALSE(harness.writer->events[3].emulator_details().has_grpc());
    }
}

TEST(MetricsInterceptorPeriodicTest, IntervalResetClearsCountersAndPercentileReservoir) {
    PeriodicTestHarness harness;
    MetricsInterceptorFactory factory(harness.reporter, harness.loop.get());

    InvocationRecord record;
    record.method = "StreamMethod";
    record.type = CallType::kBidiStreaming;
    record.rcv_bytes = 1024;
    record.snd_bytes = 2048;

    // Exceed the 32-sample raw reservoir in window 1 so percentiles switch to bucketized mode.
    constexpr int kWindowOneRequests = 33;
    constexpr int kWindowOneFailures = 5;
    for (int i = 0; i < kWindowOneRequests; ++i) {
        record.duration = static_cast<uint64_t>(i + 1) * 1000ULL;
        record.status = (i < kWindowOneFailures) ? Status(StatusCode::INTERNAL, "err") : Status::OK;
        factory.Record(record);
    }

    harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
    ASSERT_TRUE(harness.writer->WaitForEvents(1, kEventWaitTimeout));
    {
        absl::MutexLock lock(harness.writer->mutex);
        ASSERT_EQ(harness.writer->events.size(), 1);
        const auto& grpc = harness.writer->events[0].emulator_details().grpc();
        EXPECT_EQ(grpc.requests(), kWindowOneRequests);
        EXPECT_EQ(grpc.failures(), kWindowOneFailures);
        EXPECT_GT(grpc.duration().bucket_size(), 0);
        EXPECT_EQ(grpc.duration().raw_sample_size(), 0);
        EXPECT_GT(grpc.rcv_bytes_estimate().bucket_size(), 0);
        EXPECT_EQ(grpc.rcv_bytes_estimate().raw_sample_size(), 0);
        EXPECT_GT(grpc.snd_bytes_estimate().bucket_size(), 0);
        EXPECT_EQ(grpc.snd_bytes_estimate().raw_sample_size(), 0);
    }

    // Window 2 starts with a fresh 32-sample reservoir and zeroed counters.
    record.status = Status::OK;
    record.rcv_bytes = 128;
    record.snd_bytes = 256;
    record.duration = 5000;
    factory.Record(record);

    record.rcv_bytes = 512;
    record.snd_bytes = 1024;
    record.duration = 15000;
    factory.Record(record);

    harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
    ASSERT_TRUE(harness.writer->WaitForEvents(2, kEventWaitTimeout));
    {
        absl::MutexLock lock(harness.writer->mutex);
        ASSERT_EQ(harness.writer->events.size(), 2);
        const auto& grpc = harness.writer->events[1].emulator_details().grpc();
        EXPECT_EQ(grpc.requests(), 2);
        EXPECT_EQ(grpc.failures(), 0);
        EXPECT_EQ(grpc.duration().bucket_size(), 0);
        ASSERT_EQ(grpc.duration().raw_sample_size(), 2);
        EXPECT_DOUBLE_EQ(grpc.duration().raw_sample(0), 5.0);
        EXPECT_DOUBLE_EQ(grpc.duration().raw_sample(1), 15.0);
        EXPECT_EQ(grpc.rcv_bytes_estimate().bucket_size(), 0);
        ASSERT_EQ(grpc.rcv_bytes_estimate().raw_sample_size(), 2);
        EXPECT_DOUBLE_EQ(grpc.rcv_bytes_estimate().raw_sample(0), 128.0);
        EXPECT_DOUBLE_EQ(grpc.rcv_bytes_estimate().raw_sample(1), 512.0);
        EXPECT_EQ(grpc.snd_bytes_estimate().bucket_size(), 0);
        ASSERT_EQ(grpc.snd_bytes_estimate().raw_sample_size(), 2);
        EXPECT_DOUBLE_EQ(grpc.snd_bytes_estimate().raw_sample(0), 256.0);
        EXPECT_DOUBLE_EQ(grpc.snd_bytes_estimate().raw_sample(1), 1024.0);
    }
}

TEST(MetricsInterceptorPeriodicTest, SkipsZeroRequestMethodsAcrossIntervals) {
    PeriodicTestHarness harness;
    {
        MetricsInterceptorFactory factory(harness.reporter, harness.loop.get());

        // Tick 1: No methods recorded yet -> zero events emitted.
        harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);

        // Tick 2: Both server and client methods active -> both emitted.
        InvocationRecord server_record;
        server_record.method = "ServerMethod";
        server_record.direction = Direction::kIncoming;
        server_record.status = Status::OK;
        factory.Record(server_record);

        InvocationRecord client_record;
        client_record.method = "ClientMethod";
        client_record.direction = Direction::kOutgoing;
        client_record.status = Status::OK;
        factory.Record(client_record);

        harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
        ASSERT_TRUE(harness.writer->WaitForEvents(2, kEventWaitTimeout));

        // Tick 3: Only ClientMethod receives traffic; ServerMethod has 0 requests and is skipped.
        factory.Record(client_record);
        factory.Record(client_record);
        harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
        ASSERT_TRUE(harness.writer->WaitForEvents(3, kEventWaitTimeout));

        // Tick 4: Both tracked methods have 0 requests -> zero events emitted.
        harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
    }
    // Factory destructor also skips methods with 0 tail requests.
    ASSERT_TRUE(harness.FlushReporterBarrier(/*expected_total_events=*/4));

    absl::MutexLock lock(harness.writer->mutex);
    ASSERT_EQ(harness.writer->events.size(), 4);

    const auto& tick2_first = harness.writer->events[0].emulator_details().grpc();
    const auto& tick2_second = harness.writer->events[1].emulator_details().grpc();
    EXPECT_EQ(tick2_first.type(), android_studio::EmulatorGrpc::SERVER);
    EXPECT_EQ(tick2_first.call_id(), MethodHash("ServerMethod"));
    EXPECT_EQ(tick2_first.requests(), 1);
    EXPECT_EQ(tick2_second.type(), android_studio::EmulatorGrpc::CLIENT);
    EXPECT_EQ(tick2_second.call_id(), MethodHash("ClientMethod"));
    EXPECT_EQ(tick2_second.requests(), 1);

    const auto& tick3_only = harness.writer->events[2].emulator_details().grpc();
    EXPECT_EQ(tick3_only.type(), android_studio::EmulatorGrpc::CLIENT);
    EXPECT_EQ(tick3_only.call_id(), MethodHash("ClientMethod"));
    EXPECT_EQ(tick3_only.requests(), 2);

    EXPECT_FALSE(harness.writer->events[3].emulator_details().has_grpc());
}

TEST(MetricsInterceptorPeriodicTest, FlushesUtf8MethodNamesCorrectly) {
    PeriodicTestHarness harness;
    MetricsInterceptorFactory factory(harness.reporter, harness.loop.get());
    const std::string utf8_method = "/emulator.Service/メトリクス_🚀";

    InvocationRecord record;
    record.method = utf8_method;
    record.status = Status::OK;
    factory.Record(record);

    harness.loop->AdvanceClock(MetricsInterceptorFactory::kFlushInterval);
    ASSERT_TRUE(harness.writer->WaitForEvents(1, kEventWaitTimeout));

    absl::MutexLock lock(harness.writer->mutex);
    ASSERT_EQ(harness.writer->events.size(), 1);
    const auto& grpc = harness.writer->events[0].emulator_details().grpc();
    EXPECT_EQ(grpc.call_id(), MethodHash(utf8_method));
    EXPECT_EQ(grpc.requests(), 1);
}

TEST(MetricsInterceptorPeriodicTest, RecordDuringReportGoesToNextInterval) {
    ::goldfish::metrics::MetricsReporter reporter(::goldfish::metrics::Uuid::Generate());
    auto loop = ::goldfish::async::testing::TestEventLoop::Create();
    auto hooked_writer = std::make_unique<HookedFakeMetricsWriter>();
    HookedFakeMetricsWriter* writer = hooked_writer.get();
    MetricsInterceptorFactory factory(reporter, loop.get());

    InvocationRecord record;
    record.method = "FirstIntervalMethod";
    record.status = Status::OK;
    factory.Record(record);

    // Record() from the reporter's worker thread while it writes the first report. The record
    // belongs to the next interval.
    InvocationRecord next_record = record;
    next_record.method = "NextIntervalMethod";
    writer->on_first_write = [&factory, &next_record] { factory.Record(next_record); };
    reporter.SetWriter(std::move(hooked_writer));

    factory.ReportMetrics();
    ASSERT_TRUE(writer->WaitForEvents(1, kEventWaitTimeout));

    factory.ReportMetrics();
    ASSERT_TRUE(writer->WaitForEvents(2, kEventWaitTimeout));

    absl::MutexLock lock(writer->mutex);
    ASSERT_EQ(writer->events.size(), 2);
    const auto& first = writer->events[0].emulator_details().grpc();
    EXPECT_EQ(first.call_id(), MethodHash("FirstIntervalMethod"));
    EXPECT_EQ(first.requests(), 1);
    const auto& next = writer->events[1].emulator_details().grpc();
    EXPECT_EQ(next.call_id(), MethodHash("NextIntervalMethod"));
    EXPECT_EQ(next.requests(), 1);
}

}  // namespace android::control::interceptor
