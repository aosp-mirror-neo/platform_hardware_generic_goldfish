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
#include "goldfish/display/frame_producer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "goldfish/async/event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/display/display_frames.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/frame_source.h"

namespace goldfish::display {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::goldfish::async::testing::TestEventLoop;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using Start = IFrameSource::Start;

// A 1x1 slot whose pixel reads as `tag`.
class TagSlot final : public Slot {
  public:
    explicit TagSlot(uint8_t tag, std::function<void()> on_destroy = nullptr)
            : tag_(tag), on_destroy_(std::move(on_destroy)) {}
    ~TagSlot() override {
        if (on_destroy_) on_destroy_();
    }

    Dimensions size() const override { return {1, 1}; }

  protected:
    absl::Status DoReadInto(const ReadRequest& /*read*/, const ReadTarget& dst) const override {
        const Plane& plane = std::holds_alternative<Rgba8888Target>(dst)
                                     ? std::get<Rgba8888Target>(dst).plane
                                     : std::get<Rgb888Target>(dst).plane;
        std::fill(plane.data.begin(), plane.data.end(), tag_);
        return absl::OkStatus();
    }

  private:
    const uint8_t tag_;
    const std::function<void()> on_destroy_;
};

SlotLease Lease(uint8_t tag = 0, std::function<void()> on_destroy = nullptr) {
    return SlotLease(std::make_unique<TagSlot>(tag, std::move(on_destroy)));
}

absl::StatusOr<uint8_t> ReadTag(const SlotLease& lease) {
    uint8_t pixel[4] = {};
    absl::Status status = lease.ReadInto({}, Rgba8888Target{Plane{absl::MakeSpan(pixel)}});
    if (!status.ok()) return status;
    return pixel[0];
}

absl::StatusOr<uint8_t> ReadTag(const SurfaceFrame& frame) {
    uint8_t pixel[4] = {};
    absl::Status status = frame.ReadInto({}, Rgba8888Target{Plane{absl::MakeSpan(pixel)}});
    if (!status.ok()) return status;
    return pixel[0];
}

// Records sink calls and keeps the latest frame, dropping the one it
// replaces inside PublishFrame like DisplayFrames. The hooks run inside the
// sink call, after it is recorded.
class RecordingSink final : public IFrameSink {
  public:
    enum class Call { kFrame, kReset, kNoContent };

    void PublishFrame(SlotLease lease, absl::Time capture_time) override {
        std::optional<SlotLease> replaced;
        absl::MutexLock sink_lock(&sink_mutex_);
        calls_.push_back(Call::kFrame);
        last_capture_time_ = capture_time;
        replaced = std::exchange(latest_, std::move(lease));
    }
    void OnProducerReset() override {
        {
            absl::MutexLock sink_lock(&sink_mutex_);
            calls_.push_back(Call::kReset);
        }
        if (on_reset) on_reset();
    }
    void PublishNoContent() override {
        {
            absl::MutexLock sink_lock(&sink_mutex_);
            calls_.push_back(Call::kNoContent);
        }
        if (on_no_content) on_no_content();
    }

    std::vector<Call> Calls() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return calls_;
    }
    absl::Time LastCaptureTime() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return last_capture_time_;
    }
    std::optional<SlotLease> TakeLatest() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return std::exchange(latest_, std::nullopt);
    }

    std::function<void()> on_reset;
    std::function<void()> on_no_content;

  private:
    absl::Mutex sink_mutex_;
    std::vector<Call> calls_ ABSL_GUARDED_BY(sink_mutex_);
    absl::Time last_capture_time_ ABSL_GUARDED_BY(sink_mutex_) = absl::InfinitePast();
    std::optional<SlotLease> latest_ ABSL_GUARDED_BY(sink_mutex_);
};

using Call = RecordingSink::Call;

// Runs `body` on another thread and aborts if it does not finish in time, so
// a self-deadlock fails the test instead of hanging it.
void RunWithDeadlockGuard(std::function<void()> body) {
    absl::Notification done;
    std::thread runner([&] {
        body();
        done.Notify();
    });
    if (!done.WaitForNotificationWithTimeout(absl::Seconds(10))) LOG(FATAL) << "Deadlocked";
    runner.join();
}

// Records what the producer asks of it. By default a capture starts and
// completes inline with a frame tagged with its capture count.
class FakeSource final : public IFrameSource {
  public:
    void OnDemandChanged(bool warm) override {
        demand_changes.push_back(warm);
        if (on_demand_changed) on_demand_changed(warm);
    }

    void Refresh() override {
        EXPECT_EQ(capturing, 0) << "Refresh inside Capture";
        ++refreshes;
        ++refreshing;
        if (on_refresh) on_refresh();
        --refreshing;
    }

    Start Capture(CaptureCompletion done) override {
        EXPECT_EQ(refreshing, 0) << "Capture inside Refresh";
        EXPECT_EQ(capturing, 0) << "Capture inside Capture";
        ++captures;
        ++capturing;
        Start start = Start::kStarted;
        if (on_capture) {
            start = on_capture(done);
        } else {
            std::move(done)(Lease(static_cast<uint8_t>(captures)));
        }
        --capturing;
        return start;
    }

    std::function<void(bool warm)> on_demand_changed;
    std::function<void()> on_refresh;
    // May keep, complete or drop `done`.
    std::function<Start(CaptureCompletion& done)> on_capture;
    std::vector<bool> demand_changes;
    int refreshes = 0;
    int captures = 0;
    int refreshing = 0;
    int capturing = 0;
};

class FrameProducerTest : public ::testing::Test {
  protected:
    void SetUp() override { producer_->SetSource(source_); }

    // Warms the producer up and runs the demand task (one refresh and capture).
    void Warm() {
        producer_->SetDemand(true);
        loop_->RunAll();
        ASSERT_EQ(source_->captures, 1);
    }

    // Starts captures that stay in flight; `pending_` completes them.
    void KeepCapturesInFlight() {
        source_->on_capture = [this](CaptureCompletion& done) {
            pending_ = std::move(done);
            return Start::kStarted;
        };
    }

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create("executor");
    std::shared_ptr<FakeSource> source_ = std::make_shared<FakeSource>();
    std::shared_ptr<RecordingSink> sink_ = std::make_shared<RecordingSink>();
    std::shared_ptr<FrameProducer> producer_ = std::make_shared<FrameProducer>(loop_.get(), sink_);
    CaptureCompletion pending_;
};

TEST_F(FrameProducerTest, CaptureCompletionDroppedUninvokedDeliversCancelledOnce) {
    int calls = 0;
    std::optional<absl::StatusOr<SlotLease>> delivered;
    {
        CaptureCompletion done([&](absl::StatusOr<SlotLease> frame) {
            ++calls;
            delivered = std::move(frame);
        });
        CaptureCompletion moved = std::move(done);
    }
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(delivered.has_value());
    EXPECT_THAT(*delivered, StatusIs(absl::StatusCode::kCancelled));
}

TEST(CaptureCompletionTest, InvokedExactlyOnce) {
    std::vector<absl::StatusCode> calls[3];
    auto counting = [&calls](int i) {
        return CaptureCompletion([&calls, i](absl::StatusOr<SlotLease> frame) {
            calls[i].push_back(frame.status().code());
        });
    };
    {
        CaptureCompletion moved_from = counting(0);
        CaptureCompletion moved_to(std::move(moved_from));
        std::move(moved_from)(Lease());  // NOLINT(bugprone-use-after-move): empty, a no-op.
        EXPECT_THAT(calls[0], IsEmpty());
        std::move(moved_to)(Lease());
        std::move(moved_to)(Lease());  // NOLINT(bugprone-use-after-move): already invoked.
        EXPECT_THAT(calls[0], ElementsAre(absl::StatusCode::kOk));

        CaptureCompletion live = counting(1);
        CaptureCompletion replacement = counting(2);
        live = std::move(replacement);  // Cancels the callback it replaces.
        EXPECT_THAT(calls[1], ElementsAre(absl::StatusCode::kCancelled));
        CaptureCompletion& self = live;
        live = std::move(self);  // Keeps its callback.
        EXPECT_THAT(calls[2], IsEmpty());
    }  // Destroying `live` cancels it; the moved-from ones do nothing.
    EXPECT_THAT(calls[0], ElementsAre(absl::StatusCode::kOk));
    EXPECT_THAT(calls[1], ElementsAre(absl::StatusCode::kCancelled));
    EXPECT_THAT(calls[2], ElementsAre(absl::StatusCode::kCancelled));
}

// A demand change holds the pump, so a source that calls back into the
// producer from OnDemandChanged must only mark or post, never wait.
TEST_F(FrameProducerTest, ADemandChangeMayCallBackIntoTheProducer) {
    source_->on_demand_changed = [this](bool /*warm*/) {
        producer_->Invalidate();
        producer_->RequestFrame();
    };
    RunWithDeadlockGuard([this] {
        producer_->SetDemand(true);
        loop_->RunAll();
    });
    EXPECT_EQ(source_->captures, 1);  // The demand's frame covers both.
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

TEST_F(FrameProducerTest, ADemandChangeThatResetsStillPumpsTheImpliedFrame) {
    source_->on_demand_changed = [this](bool warm) {
        if (warm) producer_->Reset();
    };
    producer_->SetDemand(true);
    loop_->RunAll();
    EXPECT_EQ(source_->refreshes, 1);
    EXPECT_EQ(source_->captures, 1);
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

TEST_F(FrameProducerTest, DemandRefreshesAndCapturesOnTheExecutor) {
    producer_->SetDemand(true);
    EXPECT_TRUE(producer_->warm());
    EXPECT_EQ(source_->captures, 0);
    EXPECT_EQ(loop_->TaskCount(), 1U);

    loop_->RunAll();
    EXPECT_EQ(source_->demand_changes, std::vector<bool>{true});
    EXPECT_EQ(source_->refreshes, 1);
    EXPECT_EQ(source_->captures, 1);
}

// QemuDisplay::OnFlush checks warm() before anything else, so it must follow
// SetDemand at once, not when the demand task runs.
TEST_F(FrameProducerTest, WarmFollowsSetDemandImmediately) {
    EXPECT_FALSE(producer_->warm());
    producer_->SetDemand(true);
    EXPECT_TRUE(producer_->warm());
    producer_->SetDemand(false);
    EXPECT_FALSE(producer_->warm());
    loop_->RunAll();
    // Both demand tasks ran with the demand at their time: the stale warm-up
    // did nothing, and the source, never warm, wasn't told it is cold.
    EXPECT_THAT(source_->demand_changes, IsEmpty());
    EXPECT_EQ(source_->captures, 0);
}

// The source sees demand changes strictly alternate, so it may pair them
// (QemuDisplay registers and unregisters its listener). A consumer that warms
// up again still gets a frame for it.
TEST_F(FrameProducerTest, DemandTrueFalseTrueBeforeExecutorRuns) {
    producer_->SetDemand(true);
    producer_->SetDemand(false);
    producer_->SetDemand(true);
    loop_->RunAll();
    EXPECT_EQ(source_->demand_changes, std::vector<bool>{true});
    EXPECT_GE(source_->captures, 1);

    const int captures = source_->captures;
    producer_->SetDemand(false);
    producer_->SetDemand(true);
    loop_->RunAll();
    EXPECT_EQ(source_->demand_changes, std::vector<bool>{true});
    EXPECT_EQ(source_->captures, captures + 1);

    producer_->SetDemand(false);
    producer_->SetDemand(true);
    producer_->SetDemand(false);
    loop_->RunAll();
    EXPECT_EQ(source_->demand_changes, (std::vector<bool>{true, false}));
    EXPECT_EQ(source_->captures, captures + 1);
}

TEST_F(FrameProducerTest, ManyRequestsPostOnePump) {
    Warm();
    for (int i = 0; i < 5; ++i) producer_->RequestFrame();
    EXPECT_EQ(loop_->TaskCount(), 1U);
    EXPECT_EQ(source_->captures, 1);  // Never inline.

    loop_->RunAll();
    EXPECT_EQ(source_->refreshes, 2);
    EXPECT_EQ(source_->captures, 2);
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

TEST_F(FrameProducerTest, RequestWhileColdNeitherPostsNorCaptures) {
    producer_->RequestFrame();
    EXPECT_EQ(loop_->TaskCount(), 0U);
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 0);
}

TEST_F(FrameProducerTest, RequestWhileACaptureIsInFlightPostsWhenItEnds) {
    Warm();
    KeepCapturesInFlight();
    producer_->Invalidate();
    ASSERT_EQ(source_->captures, 2);

    producer_->RequestFrame();
    producer_->RequestFrame();
    EXPECT_EQ(loop_->TaskCount(), 0U);

    std::move(pending_)(Lease());
    EXPECT_EQ(loop_->TaskCount(), 1U);
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 3);
    EXPECT_EQ(source_->refreshes, 2);
}

TEST_F(FrameProducerTest, InvalidateCapturesInlineWithoutRefresh) {
    producer_->Invalidate();  // Not gated on demand: new content is captured.
    EXPECT_EQ(source_->captures, 1);
    EXPECT_EQ(source_->refreshes, 0);
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

TEST_F(FrameProducerTest, InvalidateWhileInFlightCapturesOnceWhenItEnds) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    producer_->Invalidate();
    producer_->Invalidate();
    EXPECT_EQ(source_->captures, 1);  // At most one capture in flight.

    std::move(pending_)(Lease());
    EXPECT_EQ(loop_->TaskCount(), 1U);
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 2);  // Latest wins: one more, not three.
    EXPECT_EQ(source_->refreshes, 0);
}

// A source that drops a capture's completion must not leave the producer
// believing it is still in flight.
TEST_F(FrameProducerTest, DroppedCompletionDoesNotStallThePump) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    producer_->Invalidate();  // Dirty while in flight.
    ASSERT_EQ(source_->captures, 1);

    pending_ = CaptureCompletion();  // Dropped uninvoked.
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 2);

    source_->on_capture = [](CaptureCompletion& done) {
        CaptureCompletion dropped = std::move(done);
        return Start::kStarted;
    };
    std::move(pending_)(Lease());
    producer_->Invalidate();
    producer_->Invalidate();
    EXPECT_EQ(source_->captures, 4);
}

TEST_F(FrameProducerTest, CaptureThatDoesNotStartLeavesNothingInFlight) {
    for (Start start : {Start::kNoSlot, Start::kNotReady, Start::kNoContent}) {
        source_->on_capture = [start](CaptureCompletion& /*done*/) { return start; };
        const int before = source_->captures;
        producer_->Invalidate();
        producer_->Invalidate();
        EXPECT_EQ(source_->captures, before + 2);
        EXPECT_EQ(loop_->TaskCount(), 0U);  // Dirty content isn't retried by the pump.
    }
}

// Refresh may re-enter Invalidate (graphic_hw_update -> dpy_gfx_update). That
// only marks the content dirty, and the capture that follows the refresh covers
// it.
TEST_F(FrameProducerTest, InvalidateDuringRefreshOnlyMarksDirty) {
    Warm();
    source_->on_refresh = [this] { producer_->Invalidate(); };
    producer_->RequestFrame();
    loop_->RunAll();
    EXPECT_EQ(source_->refreshes, 2);
    EXPECT_EQ(source_->captures, 2);
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

// A CPU capture completes inline inside Capture, and new content may arrive
// meanwhile. Neither deadlocks nor recurses; the new content is captured by one
// posted pump.
TEST_F(FrameProducerTest, InvalidateDuringCaptureWithInlineCompletionPostsOnePump) {
    int invalidations = 1;
    source_->on_capture = [&](CaptureCompletion& done) {
        if (invalidations-- > 0) producer_->Invalidate();
        std::move(done)(Lease());
        return Start::kStarted;
    };
    producer_->Invalidate();
    EXPECT_EQ(source_->captures, 1);
    EXPECT_EQ(loop_->TaskCount(), 1U);
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 2);
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

TEST_F(FrameProducerTest, SetDemandFalseForgetsDirtyContentAndRequests) {
    Warm();
    KeepCapturesInFlight();
    producer_->Invalidate();
    ASSERT_EQ(source_->captures, 2);
    producer_->Invalidate();
    producer_->RequestFrame();

    producer_->SetDemand(false);
    std::move(pending_)(Lease());
    loop_->RunAll();  // The demand task only.
    EXPECT_EQ(source_->demand_changes, (std::vector<bool>{true, false}));
    EXPECT_EQ(source_->captures, 2);
    EXPECT_EQ(loop_->TaskCount(), 0U);
}

// After a reset (snapshot load), a request captures the restored content at
// once, even though the capture from before the reset never completed; that
// capture's completion changes nothing.
TEST_F(FrameProducerTest, RequestAfterResetWhileWarmCapturesAgain) {
    Warm();
    KeepCapturesInFlight();
    producer_->Invalidate();
    ASSERT_EQ(source_->captures, 2);
    CaptureCompletion stale = std::move(pending_);

    producer_->Reset();
    producer_->RequestFrame();
    loop_->RunAll();
    EXPECT_EQ(source_->refreshes, 2);
    EXPECT_EQ(source_->captures, 3);

    producer_->Invalidate();  // Dirty while the new capture is in flight.
    std::move(stale)(Lease());
    // The stale end neither publishes nor ends the new capture, so nothing
    // else is captured while that one is in flight.
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 3);
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame, Call::kReset));
    std::move(pending_)(Lease());
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 4);
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame, Call::kReset, Call::kFrame));
}

TEST_F(FrameProducerTest, ResetForgetsDirtyContent) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    producer_->Invalidate();
    producer_->Reset();
    std::move(pending_)(Lease());
    EXPECT_EQ(loop_->TaskCount(), 0U);
    EXPECT_EQ(source_->captures, 1);
}

// A capture that a Reset made stale publishes nothing, but its completion may
// still touch source resources, so Quiesce waits for it.
TEST_F(FrameProducerTest, QuiesceWaitsForACaptureAResetMadeStale) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    EXPECT_FALSE(producer_->Quiesce(absl::ZeroDuration()));

    producer_->Reset();
    EXPECT_FALSE(producer_->Quiesce(absl::Milliseconds(10)));
    std::move(pending_)(Lease());
    EXPECT_TRUE(producer_->Quiesce(absl::ZeroDuration()));
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kReset));
}

TEST_F(FrameProducerTest, QuiesceCountsFailedAndDroppedCompletionsAsEnded) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    std::move(pending_)(absl::InternalError("copy failed"));
    EXPECT_TRUE(producer_->Quiesce(absl::ZeroDuration()));

    producer_->Invalidate();
    pending_ = CaptureCompletion();  // Dropped uninvoked.
    EXPECT_TRUE(producer_->Quiesce(absl::ZeroDuration()));

    for (Start start : {Start::kNoSlot, Start::kNotReady, Start::kNoContent}) {
        source_->on_capture = [start](CaptureCompletion& /*done*/) { return start; };
        producer_->Invalidate();
        EXPECT_TRUE(producer_->Quiesce(absl::ZeroDuration()));
    }
}

// A pump inside Refresh has no capture outstanding yet, but is about to start
// one that may touch source resources.
TEST_F(FrameProducerTest, QuiesceWaitsForAPumpInsideRefresh) {
    Warm();
    absl::Notification refreshing;
    absl::Notification release;
    source_->on_refresh = [&] {
        refreshing.Notify();
        release.WaitForNotification();
    };
    producer_->RequestFrame();
    std::thread executor([this] { loop_->RunAll(); });
    refreshing.WaitForNotification();
    EXPECT_FALSE(producer_->Quiesce(absl::Milliseconds(20)));
    release.Notify();
    EXPECT_TRUE(producer_->Quiesce(absl::Seconds(10)));
    executor.join();
    EXPECT_EQ(source_->captures, 2);
}

TEST_F(FrameProducerTest, QuiesceWaitsForACompletionOnAnotherThread) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    absl::Notification quiescing;
    std::thread worker([&] {
        quiescing.WaitForNotification();
        absl::SleepFor(absl::Milliseconds(20));  // Lets Quiesce block first.
        std::move(pending_)(Lease(7));
    });
    quiescing.Notify();
    EXPECT_TRUE(producer_->Quiesce(absl::Seconds(10)));
    // Quiesce returned only once the completion had run.
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame));
    worker.join();
}

// The sink may outlive the producer, so a capture completing after Shutdown
// must not publish after its final reset, and still frees its slot.
TEST_F(FrameProducerTest, ShutdownWithCaptureInFlightNeverPublishesAfterFinalReset) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    producer_->Shutdown();
    std::atomic<bool> freed{false};
    std::thread worker([&] { std::move(pending_)(Lease(1, [&freed] { freed = true; })); });
    worker.join();
    EXPECT_TRUE(freed);
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kReset));
    EXPECT_TRUE(producer_->Quiesce(absl::ZeroDuration()));
}

// Posted tasks and completions hold the producer weakly; once it is gone
// they do nothing (ASAN checks they don't touch it).
TEST_F(FrameProducerTest, CompletionAfterProducerDestroyedIsANoOp) {
    Warm();
    producer_->RequestFrame();   // A posted pump,
    producer_->SetDemand(true);  // a posted demand change,
    KeepCapturesInFlight();
    producer_->Invalidate();  // and a capture in flight.
    ASSERT_EQ(source_->captures, 2);
    ASSERT_EQ(loop_->TaskCount(), 2U);

    producer_.reset();
    bool freed = false;
    std::move(pending_)(Lease(9, [&freed] { freed = true; }));
    loop_->RunAll();
    EXPECT_TRUE(freed);
    EXPECT_EQ(source_->captures, 2);
    EXPECT_EQ(source_->demand_changes, std::vector<bool>{true});
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame, Call::kReset));
}

// A Reset re-entered from Refresh (a device reset during graphic_hw_update)
// comes before the capture reads its generation, so the frame is current.
TEST_F(FrameProducerTest, ResetDuringRefreshCapturesWithTheNewGeneration) {
    Warm();
    source_->on_refresh = [this] { producer_->Reset(); };
    producer_->RequestFrame();
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 2);
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame, Call::kReset, Call::kFrame));
    std::optional<SlotLease> latest = sink_->TakeLatest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_THAT(ReadTag(*latest), IsOkAndHolds(2));
}

TEST_F(FrameProducerTest, NothingReachesTheSourceAfterShutdown) {
    Warm();
    producer_->RequestFrame();
    producer_->Shutdown();
    producer_->Shutdown();  // Idempotent.
    producer_->SetDemand(true);
    producer_->RequestFrame();
    producer_->Invalidate();
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 1);
    EXPECT_EQ(source_->demand_changes, std::vector<bool>{true});
}

// The source may use its display's state until Shutdown, which the display's
// destructor calls. Its source calls null that state as soon as Shutdown
// returns, so a call that races past Shutdown dereferences null.
TEST(FrameProducerShutdownTest, WaitsForASourceCallOnAnotherThreadAndAllowsNoneAfter) {
    class StateSource final : public IFrameSource {
      public:
        Start Capture(CaptureCompletion done) override {
            ++*state;
            absl::SleepFor(absl::Microseconds(20));  // Widens the race with Shutdown.
            ++*state;
            std::move(done)(Lease());
            return Start::kStarted;
        }
        std::unique_ptr<int> state = std::make_unique<int>(0);
    };

    for (int i = 0; i < 100; ++i) {
        auto producer =
                std::make_shared<FrameProducer>(/*executor=*/nullptr, std::weak_ptr<IFrameSink>());
        auto source = std::make_shared<StateSource>();
        producer->SetSource(source);
        std::atomic<bool> stop{false};
        absl::Notification calling;
        std::thread invalidator([&] {
            while (!stop.load()) {
                producer->Invalidate();
                if (!calling.HasBeenNotified()) calling.Notify();
            }
        });
        calling.WaitForNotification();
        producer->Shutdown();
        source->state.reset();
        absl::SleepFor(absl::Microseconds(50));  // Give a stray call time to crash.
        stop.store(true);
        invalidator.join();
    }
}

TEST_F(FrameProducerTest, PublishesOnlyTheFramesOfCompletedCaptures) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    EXPECT_THAT(sink_->Calls(), IsEmpty());
    std::move(pending_)(Lease(7));
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame));
    std::optional<SlotLease> latest = sink_->TakeLatest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_THAT(ReadTag(*latest), IsOkAndHolds(7));

    producer_->Invalidate();
    std::move(pending_)(absl::InternalError("copy failed"));
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame));
}

TEST_F(FrameProducerTest, ResetResetsTheSinkAndDropsTheFrameInFlight) {
    KeepCapturesInFlight();
    producer_->Invalidate();
    CaptureCompletion stale = std::move(pending_);
    producer_->Reset();
    std::move(stale)(Lease(1));
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kReset));
}

// Consumers measure latency from the moment the capture started, not from
// its completion.
TEST_F(FrameProducerTest, AFrameCarriesTheTimeItsCaptureStarted) {
    KeepCapturesInFlight();
    const absl::Time before = absl::Now();
    producer_->Invalidate();
    const absl::Time started = absl::Now();
    std::move(pending_)(Lease());
    EXPECT_GE(sink_->LastCaptureTime(), before);
    EXPECT_LE(sink_->LastCaptureTime(), started);
}

TEST_F(FrameProducerTest, AnExpiredSinkDropsTheFrameAndFreesItsSlot) {
    bool freed = false;
    source_->on_capture = [&freed](CaptureCompletion& done) {
        std::move(done)(Lease(1, [&freed] { freed = true; }));
        return Start::kStarted;
    };
    sink_.reset();
    producer_->Invalidate();
    EXPECT_TRUE(freed);
    EXPECT_TRUE(producer_->Quiesce(absl::ZeroDuration()));
}

// A Reset between the pump reading the generation and the source capturing
// makes the frame stale, though it holds post-reset content. While warm,
// dropping it asks for the frame again whether the capture completes inline or
// asynchronously.
TEST_F(FrameProducerTest, AFrameMadeStaleByAResetDuringItsCaptureIsCapturedAgain) {
    Warm();
    bool reset = false;
    source_->on_capture = [&](CaptureCompletion& done) {
        if (!std::exchange(reset, true)) producer_->Reset();
        std::move(done)(Lease(7));
        return Start::kStarted;
    };
    producer_->Invalidate();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame, Call::kReset));
    EXPECT_EQ(loop_->TaskCount(), 1U);
    loop_->RunAll();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kFrame, Call::kReset, Call::kFrame));

    // Deferred completion: Reset cleared in_flight_, so OnCaptureDone posts the
    // follow-up pump itself when the stale frame arrives later.
    reset = false;
    source_->on_capture = [&](CaptureCompletion& done) {
        if (!std::exchange(reset, true)) {
            producer_->Reset();
            pending_ = std::move(done);
            return Start::kStarted;
        }
        std::move(done)(Lease(8));
        return Start::kStarted;
    };
    producer_->Invalidate();
    EXPECT_THAT(sink_->Calls(),
                ElementsAre(Call::kFrame, Call::kReset, Call::kFrame, Call::kReset));
    EXPECT_EQ(loop_->TaskCount(), 0U);
    std::move(pending_)(Lease(7));
    EXPECT_EQ(loop_->TaskCount(), 1U);
    loop_->RunAll();
    EXPECT_THAT(sink_->Calls(),
                ElementsAre(Call::kFrame, Call::kReset, Call::kFrame, Call::kReset, Call::kFrame));
}

TEST_F(FrameProducerTest, NoContentPublishesNoContent) {
    source_->on_capture = [](CaptureCompletion& /*done*/) { return Start::kNoContent; };
    producer_->Invalidate();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kNoContent));
    producer_->NoContent();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kNoContent, Call::kNoContent));
}

// A pump publishes its blank before it ends. New content arriving meanwhile
// only marks the producer dirty: capturing it at once could publish its frame
// first, and the late blank would then hide it.
TEST_F(FrameProducerTest, NoContentNeverOverwritesANewerFrame) {
    std::atomic<int> captures{0};
    source_->on_capture = [&captures](CaptureCompletion& done) {
        if (captures.fetch_add(1) == 0) return Start::kNoContent;
        std::move(done)(Lease(2));
        return Start::kStarted;
    };
    absl::Notification publishing_blank;
    absl::Notification release_blank;
    sink_->on_no_content = [&] {
        publishing_blank.Notify();
        release_blank.WaitForNotification();
    };
    std::thread blank([this] { producer_->Invalidate(); });
    publishing_blank.WaitForNotification();

    absl::Notification invalidated;
    std::thread guest([&] {
        producer_->Invalidate();
        invalidated.Notify();
    });
    EXPECT_TRUE(invalidated.WaitForNotificationWithTimeout(absl::Seconds(5)));
    EXPECT_EQ(captures.load(), 1);
    release_blank.Notify();
    blank.join();
    guest.join();

    loop_->RunAll();  // The follow-up pump captures the new content.
    EXPECT_EQ(captures.load(), 2);
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kNoContent, Call::kFrame));
    std::optional<SlotLease> latest = sink_->TakeLatest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_THAT(ReadTag(*latest), IsOkAndHolds(2));
}

// Like a frame, a blank from a capture that a Reset overtook is dropped; while
// warm the content, possibly post-reset, is captured again.
TEST_F(FrameProducerTest, NoContentAfterResetIsDropped) {
    source_->on_capture = [this](CaptureCompletion& /*done*/) {
        producer_->Reset();
        return Start::kNoContent;
    };
    producer_->Invalidate();
    loop_->RunAll();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kReset));
    EXPECT_EQ(source_->captures, 1);  // Cold: nothing owed.

    source_->on_capture = nullptr;
    producer_->SetDemand(true);
    loop_->RunAll();
    ASSERT_EQ(source_->captures, 2);
    bool reset = false;
    source_->on_capture = [&](CaptureCompletion& done) {
        if (std::exchange(reset, true)) {
            std::move(done)(Lease(3));
            return Start::kStarted;
        }
        producer_->Reset();
        return Start::kNoContent;
    };
    producer_->Invalidate();
    loop_->RunAll();
    EXPECT_THAT(sink_->Calls(),
                ElementsAre(Call::kReset, Call::kFrame, Call::kReset, Call::kFrame));
}

// The sink may outlive the producer, so shutdown triggers a final reset.
TEST_F(FrameProducerTest, ShutdownIsTheLastReset) {
    producer_->Shutdown();
    producer_->Shutdown();
    producer_->Reset();
    producer_->NoContent();
    producer_.reset();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kReset));
}

TEST_F(FrameProducerTest, DestructionIsAReset) {
    producer_.reset();
    EXPECT_THAT(sink_->Calls(), ElementsAre(Call::kReset));
}

// A lease dropped inside PublishFrame (the sink replacing its frame) may ask
// for a re-capture when recovering from slot starvation. That runs under the
// publish mutex, so it must only post.
TEST_F(FrameProducerTest, RequestFromALeaseDroppedInsidePublishFramePostsAfterIt) {
    Warm();
    // The sink's latest frame asks for a re-capture when it is dropped.
    ASSERT_TRUE(sink_->TakeLatest().has_value());
    sink_->PublishFrame(Lease(1, [this] { producer_->RequestFrame(); }), absl::Now());
    KeepCapturesInFlight();
    producer_->Invalidate();
    // Its replacement completes outside any pump, like an asynchronous capture.
    RunWithDeadlockGuard([this] { std::move(pending_)(Lease(2)); });
    EXPECT_EQ(source_->captures, 2);
    EXPECT_EQ(loop_->TaskCount(), 1U);  // Posted, not captured inline.

    source_->on_capture = nullptr;
    loop_->RunAll();
    EXPECT_EQ(source_->captures, 3);
    EXPECT_EQ(source_->refreshes, 2);
}

// A sink drops its frame on reset. Nothing is in flight then, so a request
// from that lease that pumped inline would capture, complete inline and
// publish under the publish mutex Reset holds: it must only post.
TEST_F(FrameProducerTest, RequestFrameFromALeaseDroppedInOnProducerResetOnlyPosts) {
    Warm();
    ASSERT_TRUE(sink_->TakeLatest().has_value());
    sink_->PublishFrame(Lease(1, [this] { producer_->RequestFrame(); }), absl::Now());
    sink_->on_reset = [this] { sink_->TakeLatest(); };
    RunWithDeadlockGuard([this] { producer_->Reset(); });
    EXPECT_EQ(source_->captures, 1);

    loop_->RunAll();
    EXPECT_EQ(source_->refreshes, 2);
    EXPECT_EQ(source_->captures, 2);
    EXPECT_THAT(sink_->Calls(),
                ElementsAre(Call::kFrame, Call::kFrame, Call::kReset, Call::kFrame));
}

// A DisplayFrames sink makes old frames unreadable on reset, so both a reset
// and the producer's shutdown must reach it.
class FrameProducerDisplayFramesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        producer_->SetSource(source_);
        producer_->Invalidate();
        frame_ = frames_->AcquireLatestFrame().frame;
        ASSERT_NE(frame_, nullptr);
        ASSERT_THAT(ReadTag(*frame_), IsOkAndHolds(1));
    }
    void TearDown() override { loop_->RunAll(); }

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    std::shared_ptr<DisplayFrames> frames_ = DisplayFrames::Create(loop_.get(), 0, {1, 1});
    std::shared_ptr<FakeSource> source_ = std::make_shared<FakeSource>();
    std::shared_ptr<FrameProducer> producer_ =
            std::make_shared<FrameProducer>(/*executor=*/nullptr, frames_);
    FramePtr frame_;
};

TEST_F(FrameProducerDisplayFramesTest, FramesReadUnavailableAfterAReset) {
    producer_->Reset();
    EXPECT_THAT(ReadTag(*frame_), StatusIs(absl::StatusCode::kUnavailable));
}

TEST_F(FrameProducerDisplayFramesTest, FramesReadUnavailableAfterShutdown) {
    producer_->Shutdown();
    EXPECT_THAT(ReadTag(*frame_), StatusIs(absl::StatusCode::kUnavailable));
}

// Checks that no frame captured before an OnProducerReset is published after
// it. The source tags each frame with the number of resets begun before its
// capture, and a resetter bumps that tag before calling Reset, so a published
// frame's tag is never below the resets seen. Like DisplayFrames, it drops the
// frame it replaces inside PublishFrame; it also drops its frame on reset.
// Calls after Close() are counted as late.
class GenerationCheckingSink final : public IFrameSink {
  public:
    void PublishFrame(SlotLease lease, absl::Time /*capture_time*/) override {
        const absl::StatusOr<uint8_t> tag = ReadTag(lease);
        std::optional<SlotLease> replaced;  // Dropped inside PublishFrame, after unlocking.
        absl::MutexLock sink_lock(&sink_mutex_);
        ++frames_;
        // Modulo 256: a tag bumped for a reset not yet seen reads as ahead.
        const uint8_t behind = tag.ok() ? static_cast<uint8_t>(resets_ - *tag) : 1;
        if (behind != 0 && behind < 128) ++stale_frames_;
        if (closed_) ++late_calls_;
        replaced = std::exchange(latest_, std::move(lease));
    }
    void OnProducerReset() override {
        std::optional<SlotLease> dropped;
        absl::MutexLock sink_lock(&sink_mutex_);
        ++resets_;
        if (closed_) ++late_calls_;
        dropped = std::exchange(latest_, std::nullopt);
    }
    void PublishNoContent() override {
        absl::MutexLock sink_lock(&sink_mutex_);
        if (closed_) ++late_calls_;
    }

    std::optional<SlotLease> TakeLatest() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return std::exchange(latest_, std::nullopt);
    }
    void Close() {
        absl::MutexLock sink_lock(&sink_mutex_);
        closed_ = true;
    }
    int Frames() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return frames_;
    }
    int StaleFrames() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return stale_frames_;
    }
    int LateCalls() {
        absl::MutexLock sink_lock(&sink_mutex_);
        return late_calls_;
    }

  private:
    absl::Mutex sink_mutex_;
    int resets_ ABSL_GUARDED_BY(sink_mutex_) = 0;
    int frames_ ABSL_GUARDED_BY(sink_mutex_) = 0;
    int stale_frames_ ABSL_GUARDED_BY(sink_mutex_) = 0;
    int late_calls_ ABSL_GUARDED_BY(sink_mutex_) = 0;
    bool closed_ ABSL_GUARDED_BY(sink_mutex_) = false;
    std::optional<SlotLease> latest_ ABSL_GUARDED_BY(sink_mutex_);
};

// Keeps captures pending until CompleteNext, on any thread.
class AsyncSource final : public IFrameSource {
  public:
    Start Capture(CaptureCompletion done) override {
        absl::MutexLock pending_lock(&pending_mutex_);
        pending_.emplace_back(std::move(done), tag.load());
        return Start::kStarted;
    }

    // Completes the oldest pending capture with a frame of its tag.
    bool CompleteNext() {
        std::pair<CaptureCompletion, uint8_t> next;
        {
            absl::MutexLock pending_lock(&pending_mutex_);
            if (pending_.empty()) return false;
            next = std::move(pending_.front());
            pending_.pop_front();
        }
        std::move(next.first)(Lease(next.second));
        return true;
    }

    std::atomic<uint8_t> tag{0};  // Of the frames captured from now on.

  private:
    absl::Mutex pending_mutex_;
    std::deque<std::pair<CaptureCompletion, uint8_t>> pending_ ABSL_GUARDED_BY(pending_mutex_);
};

// Completions run on a worker thread (like an asynchronous storage backend) while
// the producer's thread captures and resets. No frame captured before a reset
// may reach the sink after that reset's OnProducerReset.
TEST(FrameProducerRaceTest, CompletionRacingResetNeverPublishesAfterTheReset) {
    auto sink = std::make_shared<GenerationCheckingSink>();
    auto source = std::make_shared<AsyncSource>();
    auto producer = std::make_shared<FrameProducer>(/*executor=*/nullptr, sink);
    producer->SetSource(source);
    std::atomic<bool> done{false};
    std::thread worker([&] {
        while (!done.load()) {
            if (!source->CompleteNext()) std::this_thread::yield();
        }
        while (source->CompleteNext()) {
        }
    });
    uint8_t resets = 0;
    for (int i = 0; i < 20000; ++i) {
        if (i % 7 == 0) {
            producer->Reset();
            source->tag = ++resets;
        } else {
            producer->Invalidate();
        }
    }
    // Every capture above may have gone stale (the loop ends with a reset, and
    // the worker can be starved), so end with one capture of the current
    // generation: it must be published.
    EXPECT_TRUE(producer->Quiesce(absl::Seconds(10)));
    producer->Invalidate();
    done = true;
    worker.join();
    EXPECT_GT(sink->Frames(), 0);
    EXPECT_EQ(sink->StaleFrames(), 0);
}

// OnDemandChanged runs on the executor, but Invalidate may pump on another
// thread (PixmanDisplay: display loop vs QEMU thread). The source is still
// called one method at a time.
TEST(FrameProducerThreadTest, DemandChangeWaitsForACaptureOnAnotherThread) {
    class OverlapCheckingSource final : public IFrameSource {
      public:
        void OnDemandChanged(bool /*warm*/) override {
            Enter();
            demand_changes.fetch_add(1);
            Leave();
        }
        Start Capture(CaptureCompletion done) override {
            Enter();
            if (!capture_entered.HasBeenNotified()) capture_entered.Notify();
            release_capture.WaitForNotification();
            Leave();
            std::move(done)(Lease());
            return Start::kStarted;
        }
        std::atomic<int> demand_changes{0};
        std::atomic<int> overlaps{0};
        absl::Notification capture_entered;
        absl::Notification release_capture;

      private:
        void Enter() {
            if (calls_.fetch_add(1) != 0) overlaps.fetch_add(1);
        }
        void Leave() { calls_.fetch_sub(1); }
        std::atomic<int> calls_{0};
    };
    auto loop = TestEventLoop::Create("executor");
    auto source = std::make_shared<OverlapCheckingSource>();
    auto sink = std::make_shared<RecordingSink>();
    auto producer = std::make_shared<FrameProducer>(loop.get(), sink);
    producer->SetSource(source);

    std::thread guest([&] { producer->Invalidate(); });
    source->capture_entered.WaitForNotification();
    producer->SetDemand(true);
    std::thread executor([&] { loop->RunAll(); });
    absl::SleepFor(absl::Milliseconds(50));  // Room for an overlapping call.
    EXPECT_EQ(source->demand_changes.load(), 0);
    source->release_capture.Notify();
    guest.join();
    executor.join();
    EXPECT_EQ(source->demand_changes.load(), 1);
    EXPECT_EQ(source->overlaps.load(), 0);
}

// Forwards posts to a TestEventLoop, but fails the next one when asked, as a
// loop that is shutting down does.
class FlakyLoop final : public async::EventLoop {
  public:
    FlakyLoop() : EventLoop("flaky") {}
    std::future<absl::Status> Shutdown() override { return inner_->Shutdown(); }
    void ShutdownTimers() override {}
    size_t WaitUntilIdle() override { return 0; }
    bool IsOnLoopThread() const override { return false; }
    ScopedTimer CreateTimer(RepeatingTask /*task*/) override { return nullptr; }

    TestEventLoop& Inner() { return *inner_; }
    bool fail_next = false;

  protected:
    absl::Status PostImmediately(Task task, async::FlowId /*flow_id*/) override {
        if (std::exchange(fail_next, false)) return absl::UnavailableError("Shutting down");
        return inner_->Post(std::move(task));
    }
    absl::Status PostDelayed(Task /*task*/, absl::Duration /*delay*/,
                             async::FlowId /*flow_id*/) override {
        return absl::UnimplementedError("Not used");
    }

  private:
    std::unique_ptr<TestEventLoop> inner_ = TestEventLoop::Create("flaky");
};

// A pump task that could not be posted must not leave the producer believing
// one is queued, or no request would ever post again.
TEST(FrameProducerPostTest, AFailedPostDoesNotWedgeLaterRequests) {
    FlakyLoop loop;
    auto source = std::make_shared<FakeSource>();
    auto sink = std::make_shared<RecordingSink>();
    auto producer = std::make_shared<FrameProducer>(&loop, sink);
    producer->SetSource(source);
    producer->SetDemand(true);
    loop.Inner().RunAll();
    ASSERT_EQ(source->captures, 1);

    loop.fail_next = true;
    producer->RequestFrame();
    EXPECT_EQ(loop.Inner().TaskCount(), 0U);

    producer->RequestFrame();
    EXPECT_EQ(loop.Inner().TaskCount(), 1U);
    loop.Inner().RunAll();
    EXPECT_EQ(source->captures, 2);
}

TEST(FrameProducerPostTest, AFailedDemandPostIsAppliedByTheNextChange) {
    FlakyLoop loop;
    auto source = std::make_shared<FakeSource>();
    auto sink = std::make_shared<RecordingSink>();
    auto producer = std::make_shared<FrameProducer>(&loop, sink);
    producer->SetSource(source);
    loop.fail_next = true;
    producer->SetDemand(true);
    EXPECT_TRUE(producer->warm());
    loop.Inner().RunAll();
    EXPECT_THAT(source->demand_changes, IsEmpty());
    EXPECT_EQ(source->captures, 0);

    producer->SetDemand(true);
    loop.Inner().RunAll();
    EXPECT_EQ(source->demand_changes, std::vector<bool>{true});
    EXPECT_EQ(source->captures, 1);
}

TEST(FrameProducerPostTest, AFailedPostWhenACaptureEndsDoesNotWedgeLaterRequests) {
    FlakyLoop loop;
    auto source = std::make_shared<FakeSource>();
    auto sink = std::make_shared<RecordingSink>();
    auto producer = std::make_shared<FrameProducer>(&loop, sink);
    CaptureCompletion pending;
    producer->SetSource(source);
    producer->SetDemand(true);
    loop.Inner().RunAll();
    ASSERT_EQ(source->captures, 1);

    source->on_capture = [&pending](CaptureCompletion& done) {
        pending = std::move(done);
        return Start::kStarted;
    };
    producer->Invalidate();
    producer->RequestFrame();  // Posted when the capture ends...
    loop.fail_next = true;
    std::move(pending)(Lease());  // ...which fails.
    loop.Inner().RunAll();
    EXPECT_EQ(source->captures, 2);

    source->on_capture = nullptr;
    producer->RequestFrame();
    loop.Inner().RunAll();
    EXPECT_EQ(source->captures, 3);
}

// Thread-safe source for the stress tests. Counts overlapping calls, calls
// after `closed`, and demand changes that don't alternate. A capture
// completes inline, stays pending for CompleteOne, or doesn't start. Its
// frame is tagged with `tag` and, while `starving`, sometimes asks for a new
// frame when dropped, as a starved slot pool does.
class StressSource final : public IFrameSource {
  public:
    void OnDemandChanged(bool warm) override {
        Enter();
        if (last_demand_.exchange(warm) == warm) repeated_demands.fetch_add(1);
        Leave();
    }
    void Refresh() override {
        Enter();
        if (rng_() % 4 == 0) {  // graphic_hw_update flushing the guest surface.
            if (std::shared_ptr<FrameProducer> self = producer.lock()) self->Invalidate();
        }
        Leave();
    }
    Start Capture(CaptureCompletion done) override {
        Enter();
        SlotLease frame = MakeLease(tag.load(), /*starved=*/rng_() % 4 == 0);
        Start start = Start::kStarted;
        switch (rng_() % 8) {
        case 0:
            start = Start::kNoContent;
            break;
        case 1:
            start = Start::kNotReady;
            break;
        case 2:
        case 3:
        case 4:
            std::move(done)(std::move(frame));
            break;
        default: {
            absl::MutexLock pending_lock(&pending_mutex_);
            pending_.push_back({std::move(done), std::move(frame)});
        }
        }
        Leave();
        return start;
    }

    // Completes the oldest pending capture, or fails or drops it, per `roll`.
    bool CompleteOne(uint32_t roll) {
        std::optional<Pending> next;
        {
            absl::MutexLock pending_lock(&pending_mutex_);
            if (pending_.empty()) return false;
            next.emplace(std::move(pending_.front()));
            pending_.pop_front();
        }
        switch (roll % 8) {
        case 0:
            break;  // Dropped: cancelled.
        case 1:
            std::move(next->done)(absl::InternalError("copy failed"));
            break;
        default:
            std::move(next->done)(std::move(next->frame));
        }
        return true;
    }

    bool LastDemand() const { return last_demand_.load(); }

    std::weak_ptr<FrameProducer> producer;
    const std::shared_ptr<std::atomic<bool>> starving = std::make_shared<std::atomic<bool>>(true);
    std::atomic<uint8_t> tag{0};
    std::atomic<bool> closed{false};
    std::atomic<int> overlaps{0};
    std::atomic<int> late_calls{0};
    std::atomic<int> repeated_demands{0};

  private:
    struct Pending {
        CaptureCompletion done;
        SlotLease frame;
    };

    SlotLease MakeLease(uint8_t frame_tag, bool starved) {
        if (!starved) return Lease(frame_tag);
        return Lease(frame_tag, [weak = producer, starving = starving] {
            if (!starving->load()) return;
            if (std::shared_ptr<FrameProducer> self = weak.lock()) self->RequestFrame();
        });
    }
    void Enter() {
        if (calls_.fetch_add(1) != 0) overlaps.fetch_add(1);
        if (closed.load()) late_calls.fetch_add(1);
    }
    void Leave() { calls_.fetch_sub(1); }

    std::atomic<int> calls_{0};
    std::atomic<bool> last_demand_{false};
    std::minstd_rand rng_;  // Unsynchronized: the producer serializes source calls.
    absl::Mutex pending_mutex_;
    std::deque<Pending> pending_ ABSL_GUARDED_BY(pending_mutex_);
};

// Runs the executor's tasks and completes pending captures until neither
// has anything left to do.
void Drain(TestEventLoop& loop, StressSource& source) {
    for (bool busy = true; busy;) {
        busy = loop.TaskCount() > 0;
        loop.RunAll();
        while (source.CompleteOne(/*roll=*/2)) busy = true;
    }
}

// Every role that drives a producer in production, each on its own thread:
// guest flushes, the executor, a storage backend completing, failing or
// dropping captures, snapshot loads resetting, consumers toggling demand, and
// consumers dropping frames that ask for new ones.
TEST(FrameProducerStressTest, StressAllRolesNeverPublishesStaleOrOverlapsSourceCalls) {
    for (const bool final_warm : {false, true}) {
        SCOPED_TRACE(final_warm ? "Ending warm" : "Ending cold");
        auto loop = TestEventLoop::Create("executor");
        auto sink = std::make_shared<GenerationCheckingSink>();
        auto source = std::make_shared<StressSource>();
        auto producer = std::make_shared<FrameProducer>(loop.get(), sink);
        source->producer = producer;
        producer->SetSource(source);

        absl::Notification start;
        std::atomic<bool> stop_roles{false};
        std::atomic<bool> stop_workers{false};
        std::vector<std::thread> roles;
        roles.emplace_back([&] {  // Guest.
            start.WaitForNotification();
            while (!stop_roles.load()) {
                producer->Invalidate();
                std::this_thread::yield();
            }
        });
        roles.emplace_back([&] {  // Snapshot loads.
            start.WaitForNotification();
            while (!stop_roles.load()) {
                source->tag.fetch_add(1);  // Captures from now on hold post-reset content.
                producer->Reset();
                absl::SleepFor(absl::Microseconds(200));
            }
        });
        roles.emplace_back([&] {  // Consumers subscribing and unsubscribing.
            start.WaitForNotification();
            std::minstd_rand rng(1);
            while (!stop_roles.load()) {
                producer->SetDemand(rng() % 2 == 0);
                absl::SleepFor(absl::Microseconds(100));
            }
            producer->SetDemand(final_warm);
        });
        roles.emplace_back([&] {  // Consumers releasing frames.
            start.WaitForNotification();
            while (!stop_roles.load()) {
                sink->TakeLatest();
                std::this_thread::yield();
            }
        });
        std::thread executor([&] {
            start.WaitForNotification();
            while (!stop_workers.load()) {
                loop->RunAll();
                std::this_thread::yield();
            }
        });
        std::thread completer([&] {
            start.WaitForNotification();
            std::minstd_rand rng(2);
            while (!stop_workers.load()) {
                if (!source->CompleteOne(rng())) std::this_thread::yield();
            }
        });
        start.Notify();
        absl::SleepFor(absl::Milliseconds(200));
        stop_roles = true;
        for (std::thread& role : roles) role.join();
        source->starving->store(false);  // Nothing asks for frames from here on.
        stop_workers = true;
        executor.join();
        completer.join();
        Drain(*loop, *source);

        EXPECT_TRUE(producer->Quiesce(absl::Seconds(5)));
        EXPECT_EQ(producer->warm(), final_warm);
        EXPECT_EQ(source->LastDemand(), final_warm);
        EXPECT_EQ(source->repeated_demands.load(), 0);
        EXPECT_EQ(source->overlaps.load(), 0);
        EXPECT_GT(sink->Frames(), 0);
        EXPECT_EQ(sink->StaleFrames(), 0);
        producer->Shutdown();
    }
}

// Shutdown while a guest pumps on one thread, the executor on another, and a
// storage backend completes captures on a third: nothing reaches the sink
// after Shutdown's final reset, and no source call starts after it returns.
TEST(FrameProducerStressTest, ShutdownRacingAsyncCompletions) {
    for (int i = 0; i < 30; ++i) {
        auto loop = TestEventLoop::Create("executor");
        auto sink = std::make_shared<GenerationCheckingSink>();
        auto source = std::make_shared<StressSource>();
        auto producer = std::make_shared<FrameProducer>(loop.get(), sink);
        source->producer = producer;
        producer->SetSource(source);
        producer->SetDemand(true);

        absl::Notification start;
        std::atomic<bool> stop{false};
        std::thread guest([&] {
            start.WaitForNotification();
            while (!stop.load()) {
                producer->Invalidate();
                std::this_thread::yield();
            }
        });
        std::thread executor([&] {
            start.WaitForNotification();
            while (!stop.load()) {
                loop->RunAll();
                std::this_thread::yield();
            }
        });
        std::thread completer([&] {
            start.WaitForNotification();
            std::minstd_rand rng(i);
            while (!stop.load()) {
                if (!source->CompleteOne(rng())) std::this_thread::yield();
            }
        });
        start.Notify();
        absl::SleepFor(absl::Milliseconds(5));
        producer->Shutdown();
        sink->Close();
        source->closed = true;
        absl::SleepFor(absl::Milliseconds(2));  // Stragglers race on.
        stop = true;
        guest.join();
        executor.join();
        completer.join();
        source->starving->store(false);
        Drain(*loop, *source);

        EXPECT_TRUE(producer->Quiesce(absl::Seconds(5)));
        EXPECT_EQ(sink->LateCalls(), 0);
        EXPECT_EQ(sink->StaleFrames(), 0);
        EXPECT_EQ(source->late_calls.load(), 0);
        EXPECT_EQ(source->overlaps.load(), 0);
    }
}

}  // namespace
}  // namespace goldfish::display
