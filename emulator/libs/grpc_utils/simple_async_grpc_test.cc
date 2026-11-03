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
#include "android/emulation/control/simple_async_grpc.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "android/emulation/control/grpc_event_stream_support.h"
#include "emulator_controller.grpc.pb.h"
#include "goldfish/eventing/event_sources.h"

namespace android::emulation::control {

template <typename R>
class FakeReactor {
  public:
    virtual ~FakeReactor() = default;
    virtual void OnReadDone(bool ok) = 0;
    virtual void OnDone() {}

    void StartRead(R* msg) {
        start_read_called = true;
        last_read_msg = msg;
    }

    void Finish(grpc::Status status) {
        finish_called = true;
        finish_status = status;
    }

    bool start_read_called = false;
    bool finish_called = false;
    grpc::Status finish_status = grpc::Status::OK;
    R* last_read_msg = nullptr;
};

TEST(SimpleAsyncGrpcTest, ReadErrorDoesNotCallStartRead) {
    struct MyMessage {
        int value;
    };

    bool read_called = false;
    SimpleServerLambdaReader<MyMessage, FakeReactor<MyMessage>> reader([&](const MyMessage* msg) {
        read_called = true;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "test error");
    });

    // Simulate gRPC calling OnReadDone with ok=true
    reader.OnReadDone(true);

    EXPECT_TRUE(read_called);
    EXPECT_TRUE(reader.finish_called);
    EXPECT_EQ(reader.finish_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    // This should be false, if it is true, we will crash.
    EXPECT_FALSE(reader.start_read_called);
}

TEST(SimpleAsyncGrpcTest, ReadSuccessCallsStartRead) {
    struct MyMessage {
        int value;
    };

    bool read_called = false;
    SimpleServerLambdaReader<MyMessage, FakeReactor<MyMessage>> reader([&](const MyMessage* msg) {
        read_called = true;
        return grpc::Status::OK;
    });

    // Simulate gRPC calling OnReadDone with ok=true
    reader.OnReadDone(true);

    EXPECT_TRUE(read_called);
    EXPECT_FALSE(reader.finish_called);
    EXPECT_TRUE(reader.start_read_called);
}

template <typename W>
class FakeWriteReactor {
  public:
    virtual ~FakeWriteReactor() = default;
    virtual void OnWriteDone(bool ok) = 0;
    virtual void OnDone() {}
    virtual void OnCancel() {}

    void StartWrite(const W* msg) {
        if (finish_called) {
            start_write_after_finish = true;
        }
        start_write_called = true;
        start_write_called_count++;
        last_write_msg = msg;
        written_messages.push_back(*msg);
    }

    void Finish(const grpc::Status& status) {
        finish_called_count++;
        finish_called = true;
        finish_status = status;
    }

    bool start_write_called = false;
    int start_write_called_count = 0;
    bool start_write_after_finish = false;
    bool finish_called = false;
    int finish_called_count = 0;
    grpc::Status finish_status = grpc::Status::OK;
    const W* last_write_msg = nullptr;
    std::vector<W> written_messages;
};

TEST(SimpleAsyncGrpcTest, QueueSizeTracksPendingWrites) {
    struct MyMessage {
        int value;
    };

    WithSimpleQueueWriter<FakeWriteReactor<MyMessage>, 0, 4> writer;
    const auto& const_writer = writer;
    EXPECT_EQ(const_writer.QueueSize(), 0);
    EXPECT_EQ(const_writer.RecycleQueueSize(), 0);

    writer.Write(MyMessage{1});
    EXPECT_EQ(const_writer.QueueSize(), 1);

    writer.Write(MyMessage{2});
    EXPECT_EQ(const_writer.QueueSize(), 2);

    writer.OnWriteDone(true);
    EXPECT_EQ(const_writer.QueueSize(), 1);
    EXPECT_EQ(const_writer.RecycleQueueSize(), 1);

    writer.OnWriteDone(true);
    EXPECT_EQ(const_writer.QueueSize(), 0);
    EXPECT_EQ(const_writer.RecycleQueueSize(), 2);
}

TEST(SimpleAsyncGrpcTest, RecyclingDisabledByDefault) {
    struct BufferMessage {
        std::string payload;
    };

    WithSimpleQueueWriter<FakeWriteReactor<BufferMessage>> writer;
    EXPECT_EQ(writer.RecycleQueueSize(), 0);

    BufferMessage msg;
    msg.payload.resize(1024, 'a');
    writer.Write(std::move(msg));

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.RecycleQueueSize(), 0);

    auto acquired = writer.AcquireMessage();
    EXPECT_TRUE(acquired.payload.empty());
}

TEST(SimpleAsyncGrpcTest, RecycleReusesBufferCapacity) {
    struct BufferMessage {
        std::string payload;
    };

    WithSimpleQueueWriter<FakeWriteReactor<BufferMessage>, /*max_queue_size=*/2,
                          /*recycle_size=*/2>
            writer;
    EXPECT_EQ(writer.RecycleQueueSize(), 0);

    BufferMessage msg;
    msg.payload.reserve(4096);
    msg.payload = "hello";
    writer.Write(std::move(msg));

    // First write is in flight
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.RecycleQueueSize(), 0);

    // OnWriteDone moves it to recycled_queue_
    writer.OnWriteDone(true);
    EXPECT_EQ(writer.QueueSize(), 0);
    EXPECT_EQ(writer.RecycleQueueSize(), 1);

    // AcquireMessage retrieves the recycled message with preserved capacity
    BufferMessage recycled = writer.AcquireMessage();
    EXPECT_EQ(writer.RecycleQueueSize(), 0);
    EXPECT_GE(recycled.payload.capacity(), 4096);
}

TEST(SimpleAsyncGrpcTest, RecycledObjectRetainsStaleDataUntilRepopulated) {
    struct BufferMessage {
        std::string payload;
        int count{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<BufferMessage>, /*max_queue_size=*/2,
                          /*recycle_size=*/2>
            writer;

    BufferMessage msg;
    msg.payload = "stale_data_from_previous_send";
    msg.count = 42;
    writer.Write(std::move(msg));

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.RecycleQueueSize(), 1);

    // AcquireMessage returns the object with stale data intact, enabling buffer reuse
    BufferMessage recycled = writer.AcquireMessage();
    EXPECT_EQ(recycled.payload, "stale_data_from_previous_send");
    EXPECT_EQ(recycled.count, 42);

    // Caller / populate_fn is responsible for properly re-initializing it
    recycled.payload = "fresh_data";
    recycled.count = 1;
    writer.Write(std::move(recycled));
    EXPECT_EQ(writer.last_write_msg->payload, "fresh_data");
    EXPECT_EQ(writer.last_write_msg->count, 1);
}

TEST(SimpleAsyncGrpcTest, RecycleQueueBoundedByRecycleSize) {
    struct BufferMessage {
        int id{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<BufferMessage>, /*max_queue_size=*/5,
                          /*recycle_size=*/2>
            writer;

    writer.Write(BufferMessage{1});
    writer.Write(BufferMessage{2});
    writer.Write(BufferMessage{3});

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.RecycleQueueSize(), 1);

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.RecycleQueueSize(), 2);
}

TEST(SimpleAsyncGrpcTest, MaxQueueSizeReplacesPendingUnwrittenMessage) {
    struct Frame {
        int frame_id{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<Frame>, /*max_queue_size=*/2> writer;

    writer.Write(Frame{1});  // In flight
    EXPECT_EQ(writer.QueueSize(), 1);
    ASSERT_NE(writer.last_write_msg, nullptr);
    EXPECT_EQ(writer.last_write_msg->frame_id, 1);

    writer.Write(Frame{2});  // 1st pending in queue
    EXPECT_EQ(writer.QueueSize(), 2);

    writer.Write(Frame{3});  // 2nd pending in queue (total size 3: 1 in-flight + 2 pending)
    EXPECT_EQ(writer.QueueSize(), 3);

    writer.Write(Frame{4});  // Drops Frame 3 (latest pending), replaces with Frame 4
    EXPECT_EQ(writer.QueueSize(), 3);

    writer.OnWriteDone(true);  // Frame 1 completes -> Frame 2 is dispatched
    EXPECT_EQ(writer.QueueSize(), 2);
    EXPECT_EQ(writer.last_write_msg->frame_id, 2);

    writer.OnWriteDone(true);  // Frame 2 completes -> Frame 4 is dispatched
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.last_write_msg->frame_id, 4);

    writer.OnWriteDone(true);  // Frame 4 completes -> Queue empty
    EXPECT_EQ(writer.QueueSize(), 0);
}

TEST(SimpleAsyncGrpcTest, MaxQueueSizeOneReplacesPendingUnwrittenMessage) {
    struct Frame {
        int frame_id{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<Frame>, /*max_queue_size=*/1> writer;

    writer.Write(Frame{1});  // In flight
    EXPECT_EQ(writer.QueueSize(), 1);
    ASSERT_NE(writer.last_write_msg, nullptr);
    EXPECT_EQ(writer.last_write_msg->frame_id, 1);

    writer.Write(Frame{2});  // 1st pending in queue
    EXPECT_EQ(writer.QueueSize(), 2);

    writer.Write(Frame{3});  // Drops Frame 2, replaces with Frame 3
    EXPECT_EQ(writer.QueueSize(), 2);

    writer.OnWriteDone(true);  // Frame 1 completes -> Frame 3 is dispatched
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.last_write_msg->frame_id, 3);

    writer.OnWriteDone(true);  // Frame 3 completes -> Queue empty
    EXPECT_EQ(writer.QueueSize(), 0);
}

TEST(SimpleAsyncGrpcTest, EvictedMessageIsMovedToRecycleQueue) {
    struct BufferMessage {
        std::string payload;
    };

    WithSimpleQueueWriter<FakeWriteReactor<BufferMessage>, /*max_queue_size=*/1,
                          /*recycle_size=*/2>
            writer;

    BufferMessage msg1;
    msg1.payload = "first_in_flight";
    writer.Write(std::move(msg1));

    BufferMessage msg2;
    msg2.payload.reserve(2048);
    msg2.payload = "second_pending";
    writer.Write(std::move(msg2));
    EXPECT_EQ(writer.RecycleQueueSize(), 0);

    BufferMessage msg3;
    msg3.payload = "third_replaces_second";
    writer.Write(std::move(msg3));

    // The evicted msg2 should now be in recycled_queue_ with preserved capacity
    EXPECT_EQ(writer.RecycleQueueSize(), 1);
    auto recycled = writer.AcquireMessage();
    EXPECT_GE(recycled.payload.capacity(), 2048);
    EXPECT_EQ(writer.RecycleQueueSize(), 0);
}

TEST(SimpleAsyncGrpcTest, LvalueAndRvalueWriteProduceEquivalentState) {
    struct MyMessage {
        int val{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<MyMessage>> writer;

    const MyMessage msg1{10};
    writer.Write(msg1);  // lvalue overload
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.last_write_msg->val, 10);

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.QueueSize(), 0);

    MyMessage msg2{20};
    writer.Write(std::move(msg2));  // rvalue overload
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.last_write_msg->val, 20);
}

TEST(SimpleAsyncGrpcTest, ErrorServerWriterInstantiatesAndFinishes) {
    struct TestMsg {
        int val{0};
    };
    auto* error_writer = new ::ErrorServerWriter<TestMsg>(
            ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "Not found"));
    ASSERT_NE(error_writer, nullptr);
    error_writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, StateStreamWriterImmediateSnapshotAndFilter) {
    struct StateMsg {
        int value{0};
    };

    android::base::eventing::CallbackEventSource<int> source;
    int current_state = 100;
    int populate_invocations = 0;

    auto* writer = new StateStreamWriter<StateMsg, int>(
            &source,
            [&](StateMsg* msg) {
                populate_invocations++;
                msg->value = current_state;
            },
            [](int ev) { return ev > 0; });

    // 1. Initial snapshot must be taken immediately on construction
    EXPECT_EQ(populate_invocations, 1);

    // 2. Events filtered out by predicate (ev <= 0) must not trigger populate
    source.FireEvent(-5);
    EXPECT_EQ(populate_invocations, 1);

    // 3. Events accepted by predicate (ev > 0) must trigger populate
    current_state = 200;
    source.FireEvent(10);
    EXPECT_EQ(populate_invocations, 2);

    // 4. Stream completion unregisters callback from event source
    writer->OnDone();
    source.FireEvent(30);
    EXPECT_EQ(populate_invocations, 2);
}

TEST(SimpleAsyncGrpcTest, MoveOnlyLambdaSupport) {
    struct MyMessage {
        int value{0};
    };

    auto tracker = std::make_unique<int>(42);
    bool read_invoked = false;
    bool done_invoked = false;

    // absl::AnyInvocable supports move-only lambdas (e.g. capturing std::unique_ptr)
    auto* reader = new SimpleServerLambdaReader<MyMessage, FakeReactor<MyMessage>>(
            [t = std::move(tracker), &read_invoked](const MyMessage* msg) mutable {
                read_invoked = true;
                return *t == 42 ? grpc::Status::OK : grpc::Status::CANCELLED;
            },
            [done_tracker = std::make_unique<std::string>("done"), &done_invoked]() mutable {
                if (*done_tracker == "done") {
                    done_invoked = true;
                }
            });

    reader->OnReadDone(true);
    EXPECT_TRUE(read_invoked);

    reader->OnDone();
    EXPECT_TRUE(done_invoked);
}

TEST(SimpleAsyncGrpcTest, StateStreamWriterMoveOnlyLambda) {
    struct StateMsg {
        int value{0};
    };

    android::base::eventing::CallbackEventSource<int> source;
    auto captured_ptr = std::make_unique<int>(99);
    int populate_calls = 0;

    auto* writer = new StateStreamWriter<StateMsg, int>(
            &source,
            [ptr = std::move(captured_ptr), &populate_calls](StateMsg* msg) {
                populate_calls++;
                msg->value = *ptr;
            },
            [](int ev) { return true; });

    EXPECT_EQ(populate_calls, 1);
    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, UniqueEventStreamWriterDeduplicatesDirectEvents) {
    android::base::eventing::CallbackEventSource<ClipData> source;
    int populate_calls = 0;

    auto* writer =
            new UniqueEventStreamWriter<ClipData,
                                        android::base::eventing::CallbackEventSource<ClipData>, 0,
                                        0, FakeWriteReactor<ClipData>>(&source, [&](ClipData* msg) {
                populate_calls++;
                msg->set_text("initial");
            });

    // 1. Initial state is written immediately on construction.
    EXPECT_EQ(populate_calls, 1);
    ASSERT_EQ(writer->written_messages.size(), 1U);
    EXPECT_EQ(writer->written_messages[0].text(), "initial");
    writer->OnWriteDone(true);

    // 2. Duplicate event matching the initial state is suppressed.
    ClipData duplicate;
    duplicate.set_text("initial");
    source.FireEvent(duplicate);
    EXPECT_EQ(populate_calls, 1);
    EXPECT_EQ(writer->written_messages.size(), 1U);

    // 3. Distinct event is written directly without calling populate_fn again.
    ClipData updated;
    updated.set_text("updated");
    source.FireEvent(updated);
    EXPECT_EQ(populate_calls, 1);
    ASSERT_EQ(writer->written_messages.size(), 2U);
    EXPECT_EQ(writer->written_messages[1].text(), "updated");
    writer->OnWriteDone(true);

    // 4. Consecutive duplicate event is suppressed.
    source.FireEvent(updated);
    EXPECT_EQ(writer->written_messages.size(), 2U);

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, UniqueEventStreamWriterFiltersIncomingEvents) {
    android::base::eventing::CallbackEventSource<ClipData> source;

    auto* writer =
            new UniqueEventStreamWriter<ClipData,
                                        android::base::eventing::CallbackEventSource<ClipData>, 0,
                                        0, FakeWriteReactor<ClipData>>(
                    &source, [](ClipData* msg) { msg->set_text("v1"); },
                    [](const ClipData& ev) { return !ev.text().empty(); });

    ASSERT_EQ(writer->written_messages.size(), 1U);
    EXPECT_EQ(writer->written_messages[0].text(), "v1");
    writer->OnWriteDone(true);

    // Filtered-out event (empty text) is ignored and does not overwrite last_state_.
    ClipData empty_clip;
    source.FireEvent(empty_clip);
    EXPECT_EQ(writer->written_messages.size(), 1U);

    // Subsequent duplicate of "v1" is still suppressed because last_state_ remained "v1".
    ClipData v1_clip;
    v1_clip.set_text("v1");
    source.FireEvent(v1_clip);
    EXPECT_EQ(writer->written_messages.size(), 1U);

    // Accepted event with new text is written.
    ClipData v2_clip;
    v2_clip.set_text("v2");
    source.FireEvent(v2_clip);
    ASSERT_EQ(writer->written_messages.size(), 2U);
    EXPECT_EQ(writer->written_messages[1].text(), "v2");
    writer->OnWriteDone(true);

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, UniqueStateStreamWriterDeduplicatesPopulatedSnapshots) {
    android::base::eventing::CallbackEventSource<int> source;
    std::string current_text = "snapshot_v1";
    int populate_calls = 0;

    auto* writer = new UniqueStateStreamWriter<ClipData, int,
                                               android::base::eventing::CallbackEventSource<int>, 1,
                                               2, FakeWriteReactor<ClipData>>(
            &source,
            [&](ClipData* msg) {
                populate_calls++;
                msg->Clear();
                msg->set_text(current_text);
            },
            [](int display_id) { return display_id < 0 || display_id == 1; });

    // Initial snapshot emitted on construction; completing the write recycles the message buffer.
    EXPECT_EQ(populate_calls, 1);
    ASSERT_EQ(writer->written_messages.size(), 1U);
    EXPECT_EQ(writer->written_messages[0].text(), "snapshot_v1");
    writer->OnWriteDone(true);
    EXPECT_EQ(writer->RecycleQueueSize(), 1U);

    // Filtered-out event (display_id == 2) does not call populate_fn or write.
    source.FireEvent(2);
    EXPECT_EQ(populate_calls, 1);
    EXPECT_EQ(writer->written_messages.size(), 1U);
    EXPECT_EQ(writer->RecycleQueueSize(), 1U);

    // Accepted event where populated state is unchanged suppresses duplicate write and returns
    // the acquired message back to the recycle pool.
    source.FireEvent(1);
    EXPECT_EQ(populate_calls, 2);
    EXPECT_EQ(writer->written_messages.size(), 1U);
    EXPECT_EQ(writer->RecycleQueueSize(), 1U);

    // Accepted event where populated state changed (including Unicode payload) emits new snapshot.
    current_text = "snapshot_v2_测试_🚀_é";
    source.FireEvent(1);
    EXPECT_EQ(populate_calls, 3);
    ASSERT_EQ(writer->written_messages.size(), 2U);
    EXPECT_EQ(writer->written_messages[1].text(), "snapshot_v2_测试_🚀_é");
    writer->OnWriteDone(true);

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, UniqueStateStreamWriterConcurrentUpdatesPreserveUniquenessAndConverge) {
    for (int iter = 0; iter < 50; ++iter) {
        android::base::eventing::CallbackEventSource<int> source;
        absl::Mutex state_mutex;
        std::string current_text = "v0";

        auto* writer = new UniqueStateStreamWriter<
                ClipData, int, android::base::eventing::CallbackEventSource<int>,
                /*max_queue_size=*/0, /*recycle_size=*/2, FakeWriteReactor<ClipData>>(
                &source, [&](ClipData* msg) {
                    const absl::MutexLock state_lock(state_mutex);
                    msg->Clear();
                    msg->set_text(current_text);
                });

        std::atomic<bool> start_signal{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            const std::string thread_value = (t % 2 == 0) ? "state_a" : "state_b";
            producers.emplace_back([&, thread_value]() {
                while (!start_signal.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 40; ++i) {
                    {
                        const absl::MutexLock state_lock(state_mutex);
                        current_text = thread_value;
                    }
                    source.FireEvent(1);
                }
            });
        }

        start_signal.store(true, std::memory_order_release);
        for (auto& p : producers) {
            p.join();
        }

        // Settle on "state_b" and fire one final quiescent notification.
        {
            const absl::MutexLock state_lock(state_mutex);
            current_text = "state_b";
        }
        source.FireEvent(1);

        while (writer->QueueSize() > 0) {
            writer->OnWriteDone(true);
        }

        ASSERT_FALSE(writer->written_messages.empty());
        EXPECT_EQ(writer->written_messages.back().text(), "state_b");
        for (size_t i = 1; i < writer->written_messages.size(); ++i) {
            EXPECT_NE(writer->written_messages[i - 1].text(), writer->written_messages[i].text())
                    << "Consecutive duplicate emitted at index " << i;
        }

        writer->OnDone();
    }
}

TEST(SimpleAsyncGrpcTest, UniqueStateStreamWriterConcurrentEventsAndCancel) {
    for (int iter = 0; iter < 50; ++iter) {
        android::base::eventing::CallbackEventSource<int> source;
        std::atomic<int> revision{0};

        auto* writer = new UniqueStateStreamWriter<
                ClipData, int, android::base::eventing::CallbackEventSource<int>,
                /*max_queue_size=*/2, /*recycle_size=*/2, FakeWriteReactor<ClipData>>(
                &source, [&](ClipData* msg) {
                    msg->Clear();
                    msg->set_text("rev_" +
                                  std::to_string(revision.load(std::memory_order_relaxed)));
                });

        std::atomic<bool> start_signal{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&, t]() {
                while (!start_signal.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 40; ++i) {
                    revision.store(t * 100 + i, std::memory_order_relaxed);
                    source.FireEvent(i);
                }
            });
        }

        std::thread canceller([&]() {
            while (!start_signal.load(std::memory_order_acquire)) {
            }
            writer->OnCancel();
        });

        start_signal.store(true, std::memory_order_release);
        for (auto& p : producers) {
            p.join();
        }
        canceller.join();

        if (writer->start_write_called) {
            writer->OnWriteDone(true);
        }

        EXPECT_TRUE(writer->IsCancelled());
        EXPECT_TRUE(writer->IsFinished());
        EXPECT_FALSE(writer->start_write_after_finish);
        EXPECT_EQ(writer->finish_called_count, 1);

        writer->OnDone();
        EXPECT_EQ(source.CallbackCount(), 0);
    }
}

TEST(SimpleAsyncGrpcTest, GenericEventStreamWriterRaiiDestructionUnsubscribes) {
    android::base::eventing::CallbackEventSource<ClipData> source;
    EXPECT_EQ(source.CallbackCount(), 0);

    {
        GenericEventStreamWriter<ClipData> writer(&source);
        EXPECT_EQ(source.CallbackCount(), 1);
        // Writer goes out of scope without OnDone() or OnCancel() ever being invoked.
    }

    // Must automatically unsubscribe via ~BaseEventStreamWriter().
    EXPECT_EQ(source.CallbackCount(), 0);

    // Firing an event must not dispatch to the destroyed instance.
    ClipData clip;
    clip.set_text("post_destruction_event");
    source.FireEvent(clip);
}

namespace {
bool IsLinuxSanitizerBuild() {
#if defined(__linux__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return true;
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#endif
#endif
    if (std::getenv("RUN_REAL_GRPC_SERVER_TESTS") != nullptr) {
        return true;
    }
    return false;
}
}  // namespace

class RealGrpcServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        if (!IsLinuxSanitizerBuild()) {
            GTEST_SKIP() << "Real gRPC server tests only execute in Linux TSAN/ASAN builds.";
        }
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        server_address_ = "localhost:" + std::to_string(port);
        channel_ = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
        stub_ = EmulatorController::NewStub(channel_);
    }

    void TearDown() override {
        if (server_) {
            auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(200);
            server_->Shutdown(deadline);
            server_->Wait();
        }
        service_.read_fn = nullptr;
        service_.reader_done_fn = nullptr;
        service_.stream_clipboard_fn = nullptr;
    }

    class TestService : public EmulatorController::CallbackService {
      public:
        std::function<grpc::Status(const InputEvent*)> read_fn;
        std::function<void()> reader_done_fn;

        CallbackEventSource<ClipData> clipboard_source;

        grpc::ServerReadReactor<InputEvent>* streamInputEvent(
                grpc::CallbackServerContext* context, google::protobuf::Empty* response) override {
            return new SimpleServerLambdaReader<InputEvent>(
                    [this](const InputEvent* ev) {
                        if (read_fn) return read_fn(ev);
                        return grpc::Status::OK;
                    },
                    [this]() {
                        if (reader_done_fn) reader_done_fn();
                    });
        }

        std::function<grpc::ServerWriteReactor<ClipData>*(grpc::CallbackServerContext*,
                                                          const google::protobuf::Empty*)>
                stream_clipboard_fn;

        grpc::ServerWriteReactor<ClipData>* streamClipboard(
                grpc::CallbackServerContext* context,
                const google::protobuf::Empty* request) override {
            if (stream_clipboard_fn) {
                return stream_clipboard_fn(context, request);
            }
            return new GenericEventStreamWriter<ClipData>(&clipboard_source);
        }
    };

    TestService service_;
    std::string server_address_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<EmulatorController::Stub> stub_;
};

TEST_F(RealGrpcServerTest, SimpleServerLambdaReaderErrorTriggersUaf) {
    std::atomic<bool> done_called{false};
    service_.read_fn = [](const InputEvent* ev) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid input");
    };
    service_.reader_done_fn = [&]() { done_called.store(true); };

    grpc::ClientContext context;
    google::protobuf::Empty response;
    auto stream = stub_->streamInputEvent(&context, &response);
    InputEvent ev;
    stream->Write(ev);
    auto status = stream->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    // Give gRPC thread pool time to process OnDone and self-delete
    for (int i = 0; i < 50 && !done_called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(done_called.load());
}

TEST_F(RealGrpcServerTest, WithSimpleReaderClientDoneTriggersUaf) {
    std::atomic<bool> done_called{false};
    service_.read_fn = [](const InputEvent* ev) { return grpc::Status::OK; };
    service_.reader_done_fn = [&]() { done_called.store(true); };

    grpc::ClientContext context;
    google::protobuf::Empty response;
    auto stream = stub_->streamInputEvent(&context, &response);
    InputEvent ev;
    stream->Write(ev);
    stream->WritesDone();
    auto status = stream->Finish();
    EXPECT_TRUE(status.ok());

    for (int i = 0; i < 50 && !done_called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(done_called.load());
}

TEST_F(RealGrpcServerTest, SimpleServerLambdaReaderConcurrentErrors) {
    service_.read_fn = [](const InputEvent* ev) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid input");
    };

    std::vector<std::thread> clients;
    for (int t = 0; t < 20; ++t) {
        clients.emplace_back([&]() {
            for (int i = 0; i < 5; ++i) {
                grpc::ClientContext context;
                google::protobuf::Empty response;
                auto stream = stub_->streamInputEvent(&context, &response);
                InputEvent ev;
                stream->Write(ev);
                auto status = stream->Finish();
                EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
            }
        });
    }
    for (auto& c : clients) {
        c.join();
    }
}

TEST_F(RealGrpcServerTest, GenericEventStreamWriterConcurrentEventsDuringCancel) {
    for (int iter = 0; iter < 10; ++iter) {
        while (service_.clipboard_source.CallbackCount() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        grpc::ClientContext context;
        google::protobuf::Empty request;
        auto stream = stub_->streamClipboard(&context, request);

        while (service_.clipboard_source.CallbackCount() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::atomic<bool> stop{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&, t]() {
                int count = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    ClipData clip;
                    clip.set_text("msg_" + std::to_string(t) + "_" + std::to_string(count++));
                    service_.clipboard_source.FireEvent(clip);
                }
            });
        }

        ClipData received;
        EXPECT_TRUE(stream->Read(&received));

        // Cancel the stream while producers are actively firing events to stress-test
        // concurrent Unsubscribe() and RemoveCallback() deadlock safety.
        context.TryCancel();

        auto status = stream->Finish();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);

        // Stop and join producers now that the stream has finished and unsubscribed.
        stop.store(true, std::memory_order_relaxed);
        for (auto& p : producers) {
            p.join();
        }
    }
}

TEST_F(RealGrpcServerTest, GenericEventStreamWriterConcurrentEventsDuringServerFinish) {
    for (int iter = 0; iter < 10; ++iter) {
        while (service_.clipboard_source.CallbackCount() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::atomic<GenericEventStreamWriter<ClipData>*> writer_ptr{nullptr};
        service_.stream_clipboard_fn = [&](grpc::CallbackServerContext*,
                                           const google::protobuf::Empty*) {
            auto* writer = new GenericEventStreamWriter<ClipData>(&service_.clipboard_source);
            writer_ptr.store(writer, std::memory_order_release);
            return writer;
        };

        grpc::ClientContext context;
        google::protobuf::Empty request;
        auto stream = stub_->streamClipboard(&context, request);

        while (service_.clipboard_source.CallbackCount() == 0 ||
               writer_ptr.load(std::memory_order_acquire) == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::atomic<bool> stop{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&, t]() {
                int count = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    ClipData clip;
                    clip.set_text("msg_" + std::to_string(t) + "_" + std::to_string(count++));
                    service_.clipboard_source.FireEvent(clip);
                }
            });
        }

        ClipData received;
        EXPECT_TRUE(stream->Read(&received));

        // Finish the stream from the server side without client cancellation.
        // This exercises OnDone() -> delete this while producers are actively firing events,
        // without OnCancel() being invoked.
        writer_ptr.load(std::memory_order_acquire)->Finish(grpc::Status::OK);

        while (stream->Read(&received)) {
        }
        auto status = stream->Finish();
        EXPECT_TRUE(status.ok());

        // Stop and join producers now that the stream has finished and unsubscribed.
        stop.store(true, std::memory_order_relaxed);
        for (auto& p : producers) {
            p.join();
        }

        service_.stream_clipboard_fn = nullptr;
    }
}

TEST_F(RealGrpcServerTest, WithSimpleQueueWriterQueuedWritesDuringCancel) {
    for (int iter = 0; iter < 10; ++iter) {
        while (service_.clipboard_source.CallbackCount() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        grpc::ClientContext context;
        google::protobuf::Empty request;
        auto stream = stub_->streamClipboard(&context, request);

        while (service_.clipboard_source.CallbackCount() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Fire multiple events in rapid succession so that messages 2..20 are queued
        // in write_queue_ while message 1 is in-flight on the wire.
        for (int i = 0; i < 20; ++i) {
            ClipData clip;
            clip.set_text("queued_msg_" + std::to_string(i));
            service_.clipboard_source.FireEvent(clip);
        }

        // Immediately cancel the stream while writes are still queued
        context.TryCancel();

        auto status = stream->Finish();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
    }
}

template <typename R>
class SelfDeletingFakeReactor {
  public:
    virtual ~SelfDeletingFakeReactor() = default;
    virtual void OnReadDone(bool ok) = 0;
    virtual void OnDone() {}

    void StartRead(R* msg) {}
    void Finish(grpc::Status status) {
        // Simulates gRPC dispatching OnDone immediately upon Finish()
        this->OnDone();
    }
};

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaReaderSelfDeletingUafSafety) {
    struct MyMessage {
        int value;
    };
    bool done_called = false;
    auto* reader = new SimpleServerLambdaReader<MyMessage, SelfDeletingFakeReactor<MyMessage>>(
            [](const MyMessage* msg) { return grpc::Status(grpc::StatusCode::CANCELLED, "stop"); },
            [&]() { done_called = true; });

    // OnReadDone triggers Read(), which returns non-OK status and invokes Base::Finish().
    // Base::Finish immediately triggers OnDone(), which executes 'delete this'.
    // If reactor_lock_ was held across Base::Finish, unlocking after delete this triggers a UAF.
    reader->OnReadDone(true);
    EXPECT_TRUE(done_called);
}

template <typename R>
class SelfDeletingFakeClientReactor {
  public:
    virtual ~SelfDeletingFakeClientReactor() = default;
    virtual void OnReadDone(bool ok) = 0;
    virtual void OnDone(const grpc::Status& status) {}

    void StartRead(R* msg) {}
    void Finish(grpc::Status status) {
        // Simulates gRPC dispatching OnDone immediately upon Finish()
        this->OnDone(status);
    }
};

TEST(SimpleAsyncGrpcTest, SimpleClientLambdaReaderSelfDeletingUafSafety) {
    struct MyMessage {
        int value;
    };
    bool done_called = false;
    auto context = std::make_shared<grpc::ClientContext>();
    auto* reader =
            new SimpleClientLambdaReader<MyMessage, SelfDeletingFakeClientReactor<MyMessage>>(
                    context,
                    [](const MyMessage* msg) {
                        return grpc::Status(grpc::StatusCode::CANCELLED, "stop");
                    },
                    [&](grpc::Status status) { done_called = true; });

    // OnReadDone triggers Read(), which returns non-OK status and invokes Base::Finish().
    // Base::Finish immediately triggers OnDone(), which executes 'delete this'.
    // If reactor_lock_ was held across Base::Finish, unlocking after delete this triggers a UAF.
    reader->OnReadDone(true);
    EXPECT_TRUE(done_called);
}

TEST(SimpleAsyncGrpcTest, WithSimpleQueueWriterWritesIgnoredAfterFinish) {
    struct TestMsg {
        int value{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<TestMsg>> writer;

    // Write message 1: starts writing immediately
    writer.Write(TestMsg{1});
    EXPECT_TRUE(writer.start_write_called);
    EXPECT_EQ(writer.start_write_called_count, 1);
    EXPECT_EQ(writer.QueueSize(), 1);

    // Write message 2: enqueued as pending write
    writer.Write(TestMsg{2});
    EXPECT_EQ(writer.QueueSize(), 2);
    EXPECT_EQ(writer.start_write_called_count, 1);

    // Calling Finish() marks stream as finished and delegates to FakeWriteReactor::Finish
    writer.Finish(grpc::Status::OK);
    EXPECT_TRUE(writer.finish_called);
    EXPECT_EQ(writer.finish_called_count, 1);
    EXPECT_TRUE(writer.IsFinished());
    EXPECT_FALSE(writer.IsCancelled());
    // In-flight message 1 and pending message 2 remain in queue
    EXPECT_EQ(writer.QueueSize(), 2);

    // Subsequent Finish() is idempotent and does not delegate again
    writer.Finish(grpc::Status::CANCELLED);
    EXPECT_EQ(writer.finish_called_count, 1);
    EXPECT_EQ(writer.finish_status.error_code(), grpc::StatusCode::OK);

    // New writes after finish are safely ignored (both rvalue and const lvalue)
    writer.Write(TestMsg{3});
    const TestMsg lvalue_msg{4};
    writer.Write(lvalue_msg);
    EXPECT_EQ(writer.QueueSize(), 2);

    // When the in-flight wire write completes, OnWriteDone pops message 1 and does NOT start write
    // 2 or 3
    writer.OnWriteDone(true);
    // Message 1 is popped; pending message 2 remains unwritten in queue until destruction
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.start_write_called_count, 1);
    EXPECT_FALSE(writer.start_write_after_finish);
}

TEST(SimpleAsyncGrpcTest, WithSimpleQueueWriterConstWriteAfterFinishDoesNotDepleteRecyclePool) {
    struct TestMsg {
        int value{0};
    };

    WithSimpleQueueWriter<FakeWriteReactor<TestMsg>, 0, 5> writer;

    // Send a message and complete it to populate the recycle pool
    writer.Write(TestMsg{1});
    writer.OnWriteDone(true);
    EXPECT_EQ(writer.RecycleQueueSize(), 1);

    // Finish the writer
    writer.Finish(grpc::Status::OK);
    EXPECT_TRUE(writer.IsFinished());

    // Calling Write with const lvalue after finish should NOT acquire from or modify recycle pool
    const TestMsg msg{2};
    writer.Write(msg);
    EXPECT_EQ(writer.RecycleQueueSize(), 1);
    EXPECT_EQ(writer.QueueSize(), 0);
}

TEST(SimpleAsyncGrpcTest, WithSimpleQueueWriterWritesIgnoredAfterCancel) {
    struct TestMsg {
        int value{0};
    };

    struct TestCancelWriter : public WithSimpleQueueWriter<FakeWriteReactor<TestMsg>> {
        using WithSimpleQueueWriter<FakeWriteReactor<TestMsg>>::SetCancelled;
    };

    TestCancelWriter writer;

    writer.Write(TestMsg{10});
    writer.Write(TestMsg{20});
    EXPECT_EQ(writer.QueueSize(), 2);
    EXPECT_EQ(writer.start_write_called_count, 1);

    EXPECT_TRUE(writer.SetCancelled());
    EXPECT_TRUE(writer.IsCancelled());
    EXPECT_FALSE(writer.IsFinished());
    EXPECT_EQ(writer.QueueSize(), 2);

    // Subsequent writes ignored (both rvalue and const lvalue)
    writer.Write(TestMsg{30});
    const TestMsg lvalue_cancel{40};
    writer.Write(lvalue_cancel);
    EXPECT_EQ(writer.QueueSize(), 2);

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.QueueSize(), 1);
    EXPECT_EQ(writer.start_write_called_count, 1);
    EXPECT_FALSE(writer.start_write_after_finish);

    // Subsequent SetCancelled is idempotent
    EXPECT_FALSE(writer.SetCancelled());
}

TEST(SimpleAsyncGrpcTest, WithSimpleQueueWriterConcurrentWritesAndFinish) {
    struct TestMsg {
        int value{0};
    };

    for (int iter = 0; iter < 100; ++iter) {
        WithSimpleQueueWriter<FakeWriteReactor<TestMsg>> writer;

        std::atomic<bool> start_signal{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&, t]() {
                while (!start_signal.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 50; ++i) {
                    writer.Write(TestMsg{t * 1000 + i});
                }
            });
        }

        std::thread finisher([&]() {
            while (!start_signal.load(std::memory_order_acquire)) {
            }
            writer.Finish(grpc::Status::OK);
        });

        start_signal.store(true, std::memory_order_release);

        for (auto& p : producers) {
            p.join();
        }
        finisher.join();

        // Complete the in-flight wire write if one was started
        if (writer.start_write_called) {
            writer.OnWriteDone(true);
        }

        EXPECT_TRUE(writer.IsFinished());
        EXPECT_FALSE(writer.start_write_after_finish);
    }
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterStartAndWrite) {
    struct TestMsg {
        int value{0};
    };

    bool start_called = false;
    bool done_called = false;
    SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>* writer_ptr = nullptr;

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>(
            [&](auto* w) {
                start_called = true;
                writer_ptr = w;
                w->Write(TestMsg{42});
            },
            []() {}, [&]() { done_called = true; });

    EXPECT_TRUE(start_called);
    EXPECT_EQ(writer_ptr, writer);
    EXPECT_FALSE(writer->finish_called);
    EXPECT_TRUE(writer->start_write_called);
    ASSERT_EQ(writer->written_messages.size(), 1);
    EXPECT_EQ(writer->written_messages[0].value, 42);
    EXPECT_FALSE(writer->IsFinished());
    EXPECT_FALSE(writer->IsCancelled());

    writer->Finish(::grpc::Status::OK);
    EXPECT_TRUE(writer->finish_called);
    EXPECT_EQ(writer->finish_called_count, 1);
    EXPECT_TRUE(writer->finish_status.ok());
    EXPECT_TRUE(writer->IsFinished());
    EXPECT_FALSE(writer->IsCancelled());

    // Subsequent Finish calls should be idempotent no-ops
    writer->Finish(::grpc::Status::CANCELLED);
    EXPECT_EQ(writer->finish_called_count, 1);
    EXPECT_TRUE(writer->finish_status.ok());

    writer->OnDone();
    EXPECT_TRUE(done_called);
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterCancellation) {
    struct TestMsg {
        int value{0};
    };

    bool cancel_called = false;
    bool done_called = false;

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>(
            [](auto* w) {}, [&]() { cancel_called = true; }, [&]() { done_called = true; });

    EXPECT_FALSE(writer->IsCancelled());
    EXPECT_FALSE(writer->IsFinished());

    writer->OnCancel();
    EXPECT_TRUE(cancel_called);
    EXPECT_TRUE(writer->IsCancelled());
    EXPECT_TRUE(writer->IsFinished());
    EXPECT_TRUE(writer->finish_called);
    EXPECT_EQ(writer->finish_called_count, 1);
    EXPECT_EQ(writer->finish_status.error_code(), ::grpc::StatusCode::CANCELLED);

    // Subsequent Finish calls after cancellation should be idempotent no-ops
    writer->Finish(::grpc::Status::OK);
    EXPECT_EQ(writer->finish_called_count, 1);
    EXPECT_EQ(writer->finish_status.error_code(), ::grpc::StatusCode::CANCELLED);

    writer->OnDone();
    EXPECT_TRUE(done_called);
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterMultipleOnCancelIsIdempotent) {
    struct TestMsg {
        int value{0};
    };

    int cancel_count = 0;
    int done_count = 0;

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>(
            [](auto* w) {}, [&]() { cancel_count++; }, [&]() { done_count++; });

    writer->OnCancel();
    EXPECT_EQ(cancel_count, 1);
    EXPECT_EQ(writer->finish_called_count, 1);

    // Multiple calls to OnCancel() must be safe and idempotent no-ops
    writer->OnCancel();
    writer->OnCancel();
    EXPECT_EQ(cancel_count, 1);
    EXPECT_EQ(writer->finish_called_count, 1);

    writer->OnDone();
    EXPECT_EQ(done_count, 1);
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterCancelCallbackCallsFinish) {
    struct TestMsg {
        int value{0};
    };

    bool cancel_called = false;
    SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>* writer = nullptr;
    writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>(
            [](auto* w) {},
            [&]() {
                cancel_called = true;
                // If cancel callback itself invokes Finish(), Base::Finish must only be called
                // once.
                writer->Finish(::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "quota"));
            });

    writer->OnCancel();
    EXPECT_TRUE(cancel_called);
    EXPECT_TRUE(writer->IsCancelled());
    EXPECT_TRUE(writer->IsFinished());
    EXPECT_TRUE(writer->finish_called);
    EXPECT_EQ(writer->finish_called_count, 1);
    EXPECT_EQ(writer->finish_status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED);

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterFinishImmediatelyMarksFinished) {
    struct TestMsg {
        int value{0};
    };

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>([](auto* w) {});

    EXPECT_FALSE(writer->IsFinished());
    EXPECT_FALSE(writer->IsCancelled());

    // Producer calling Finish immediately causes IsFinished() to return true,
    // while IsCancelled() remains false for clean completion.
    writer->Finish(::grpc::Status::OK);
    EXPECT_TRUE(writer->IsFinished());
    EXPECT_FALSE(writer->IsCancelled());

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterMoveOnlyLambda) {
    struct TestMsg {
        int value{0};
    };

    auto start_ptr = std::make_unique<int>(123);
    auto cancel_ptr = std::make_unique<int>(456);
    auto done_ptr = std::make_unique<int>(789);

    int cancel_val = 0;
    int done_val = 0;

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>(
            [captured = std::move(start_ptr)](auto* w) { w->Write(TestMsg{*captured}); },
            [captured = std::move(cancel_ptr), &cancel_val]() mutable { cancel_val = *captured; },
            [captured = std::move(done_ptr), &done_val]() mutable { done_val = *captured; });

    ASSERT_EQ(writer->written_messages.size(), 1);
    EXPECT_EQ(writer->written_messages[0].value, 123);

    writer->OnCancel();
    EXPECT_EQ(cancel_val, 456);

    writer->OnDone();
    EXPECT_EQ(done_val, 789);
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterWritesIgnoredAfterFinish) {
    struct TestMsg {
        int value{0};
    };

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>([](auto* w) {});

    // Write message 1: starts writing immediately
    writer->Write(TestMsg{1});
    EXPECT_TRUE(writer->start_write_called);
    EXPECT_EQ(writer->start_write_called_count, 1);
    EXPECT_EQ(writer->QueueSize(), 1);

    // Write message 2: enqueued as pending write
    writer->Write(TestMsg{2});
    EXPECT_EQ(writer->QueueSize(), 2);
    EXPECT_EQ(writer->start_write_called_count, 1);

    // Calling Finish() marks stream as finished
    writer->Finish(::grpc::Status::OK);
    EXPECT_TRUE(writer->IsFinished());
    EXPECT_FALSE(writer->IsCancelled());
    EXPECT_EQ(writer->finish_called_count, 1);
    // In-flight message 1 and pending message 2 remain in queue
    EXPECT_EQ(writer->QueueSize(), 2);

    // New writes after Finish() are safely ignored
    writer->Write(TestMsg{3});
    EXPECT_EQ(writer->QueueSize(), 2);

    // When the in-flight wire write completes, OnWriteDone pops message 1 and does NOT start write
    // 2 or 3
    writer->OnWriteDone(true);
    EXPECT_EQ(writer->QueueSize(), 1);
    EXPECT_EQ(writer->start_write_called_count, 1);
    EXPECT_FALSE(writer->start_write_after_finish);

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterWritesIgnoredAfterCancel) {
    struct TestMsg {
        int value{0};
    };

    auto* writer = new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>([](auto* w) {});

    writer->Write(TestMsg{10});
    writer->Write(TestMsg{20});
    EXPECT_EQ(writer->QueueSize(), 2);
    EXPECT_EQ(writer->start_write_called_count, 1);

    writer->OnCancel();
    EXPECT_TRUE(writer->IsCancelled());
    EXPECT_TRUE(writer->IsFinished());
    EXPECT_EQ(writer->finish_called_count, 1);
    EXPECT_EQ(writer->QueueSize(), 2);

    // Subsequent writes ignored
    writer->Write(TestMsg{30});
    EXPECT_EQ(writer->QueueSize(), 2);

    writer->OnWriteDone(true);
    EXPECT_EQ(writer->QueueSize(), 1);
    EXPECT_EQ(writer->start_write_called_count, 1);
    EXPECT_FALSE(writer->start_write_after_finish);

    writer->OnDone();
}

TEST(SimpleAsyncGrpcTest, SimpleServerLambdaWriterConcurrentWritesAndFinish) {
    struct TestMsg {
        int value{0};
    };

    for (int iter = 0; iter < 100; ++iter) {
        auto* writer =
                new SimpleServerLambdaWriter<TestMsg, FakeWriteReactor<TestMsg>>([](auto* w) {});

        std::atomic<bool> start_signal{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&, t]() {
                while (!start_signal.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 50; ++i) {
                    writer->Write(TestMsg{t * 1000 + i});
                }
            });
        }

        std::thread finisher([&]() {
            while (!start_signal.load(std::memory_order_acquire)) {
            }
            writer->Finish(::grpc::Status::OK);
        });

        start_signal.store(true, std::memory_order_release);

        for (auto& p : producers) {
            p.join();
        }
        finisher.join();

        // Complete the in-flight wire write if one was started
        if (writer->start_write_called) {
            writer->OnWriteDone(true);
        }

        EXPECT_TRUE(writer->IsFinished());
        EXPECT_FALSE(writer->start_write_after_finish);
        EXPECT_EQ(writer->finish_called_count, 1);

        writer->OnDone();
    }
}

}  // namespace android::emulation::control
