#include "goldfish/async/event_loop.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <vector>

#include "absl/base/config.h"
#include "absl/base/log_severity.h"
#include "absl/log/log.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"

#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/qemu_event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "test/fake_qemu_callbacks.h"
#include "uv.h"

extern "C" {
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"

#ifdef shutdown
#undef shutdown
#endif
#ifdef close
#undef close
#endif
// IWYU pragma: end_keep
// clang-format on
}

using namespace std::chrono_literals;

namespace goldfish::async {

// Test fixture parameterized by the event loop type ("libuv" or "qemu").
class EventLoopTest : public ::testing::TestWithParam<std::string> {
  protected:
    void SetUp() override {
        mLoopType = GetParam();
        if (mLoopType == "libuv") {
            mLibuvLoop = LibuvEventLoop::Create();
            loop = mLibuvLoop.get();
        } else if (mLoopType == "qemu") {
            mLibuvLoop = QemuEventLoop::Create();
            loop = mLibuvLoop.get();
        }
    }

    void TearDown() override {
        shutdown();
        if (mLoopType == "qemu") {
            fake_qemu_reset();
        }
    }

    void shutdown() {
        if (loop) {
            if (loop->GetState() == LooperStatusEvent::State::kRunning ||
                loop->GetState() == LooperStatusEvent::State::kPaused ||
                (mLoopType == "qemu" &&
                 loop->GetState() == LooperStatusEvent::State::kNotStarted)) {
                auto f = loop->Shutdown();
                if (mLoopType == "qemu") {
                    // Need to run Qemu "thread" for shutdown to complete
                    fake_qemu_advance_ms(150);
                }
                ASSERT_THAT(f.get(), absl_testing::IsOk());
            }
            loop = nullptr;
        }
        if (mLoopType == "libuv") {
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
        }
        mLibuvLoop.reset();
    }

    // Starts the libuv event loop in a background thread. No-op for qemu.
    void runInThread() {
        if (mLoopType == "libuv") {
            loop_thread =
                    std::thread([this]() { (void)dynamic_cast<LibuvEventLoop*>(loop)->Run(); });
            // Wait for it to actually start.
            while (loop->GetState() != LooperStatusEvent::State::kRunning) {
                std::this_thread::sleep_for(10ms);
            }
        }
    }

    // Processes events until a future is fulfilled.
    template <typename T>
    void runUntil(std::future<T>& future) {
        if (mLoopType == "libuv") {
            ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        } else if (mLoopType == "qemu") {
            while (future.wait_for(0ms) != std::future_status::ready) {
                fake_qemu_advance_ms(1);
            }
        }
    }

    void runUntil(std::future<void>& future) {
        if (mLoopType == "libuv") {
            ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        } else if (mLoopType == "qemu") {
            while (future.wait_for(0ms) != std::future_status::ready) {
                fake_qemu_advance_ms(1);
            }
        }
    }

    void runUntil(absl::Notification& done) {
        if (mLoopType == "libuv") {
            ASSERT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(2)));
        } else if (mLoopType == "qemu") {
            while (!done.HasBeenNotified()) {
                fake_qemu_advance_ms(1);
            }
        }
    }

    // Change this if you find that the timing assertions are flaky due to machine load.
    const absl::Duration tolerance = absl::Milliseconds(100);
    std::string mLoopType;
    std::unique_ptr<EventLoop> mLibuvLoop;
    EventLoop* loop = nullptr;
    std::thread loop_thread;
};

INSTANTIATE_TEST_SUITE_P(EventLoopImplementations, EventLoopTest,
                         ::testing::Values("libuv", "qemu"),
                         [](const ::testing::TestParamInfo<EventLoopTest::ParamType>& info) {
                             std::string name = info.param;
                             std::transform(name.begin(), name.end(), name.begin(),
                                            [](unsigned char c) { return std::toupper(c); });
                             return name;
                         });

// =================================================================
//                      TEST CASES
// =================================================================

TEST_P(EventLoopTest, ScheduleAndExecuteSingleTask) {
    std::promise<bool> task_executed_promise;
    auto future = task_executed_promise.get_future();

    (void)loop->Post([&]() { task_executed_promise.set_value(true); });

    runInThread();
    runUntil(future);

    EXPECT_TRUE(future.get());
}

TEST_P(EventLoopTest, ScheduleAndExecuteSingleTaskWithinATask) {
    std::promise<bool> task_executed_promise;
    auto future = task_executed_promise.get_future();

    (void)loop->Post([&]() { (void)loop->Post([&]() { task_executed_promise.set_value(true); }); });

    runInThread();
    runUntil(future);

    EXPECT_TRUE(future.get());
}

// From https://github.com/google/googletest/blob/main/docs/advanced.md#death-tests-and-threads
// Due to well-known problems with forking in the presence of threads, death tests should be run in
// a single-threaded context. Sometimes, however, it isn't feasible to arrange that kind of
// environment. For example, statically-initialized modules may start threads before main is ever
// reached. Once threads have been created, it may be difficult or impossible to clean them up.
//
// For us this translates into all sorts of strange behavior.
TEST_P(EventLoopTest, DISABLED_PostAndWaitFromLoopThreadFails) {
    if (mLoopType == "libuv") {
        std::promise<void> status_promise;
        auto status_future = status_promise.get_future();
        runInThread();
        (void)loop->Post([this, &status_promise]() {
            EXPECT_DEATH(this->loop->PostAndWait([]() { return false; }).IgnoreError(),
                         "postAndWait cannot be called from the event loop.");
            status_promise.set_value();
        });
        runUntil(status_future);
    } else {  // qemu
        // For qemu, the main test thread is the loop thread.
        EXPECT_DEATH(this->loop->PostAndWait([]() { return false; }).IgnoreError(),
                     "postAndWait cannot be called from the event loop.");
    }
}

TEST_P(EventLoopTest, PostAndWaitBool) {
    runInThread();
    std::promise<absl::StatusOr<bool>> status_promise;
    // QEMU needs a separate thread
    std::thread t([&status_promise, this] {
        status_promise.set_value(loop->PostAndWait([]() { return false; }));
    });
    auto status_future = status_promise.get_future();
    runUntil(status_future);
    t.join();
    EXPECT_THAT(status_future.get(), absl_testing::IsOkAndHolds(false));
}

TEST_P(EventLoopTest, PostAndWaitVoid) {
    runInThread();
    std::promise<absl::Status> status_promise;
    // QEMU needs a separate thread
    std::thread t([&status_promise, this] {
        status_promise.set_value(loop->PostAndWait([]() { return; }));
    });
    auto status_future = status_promise.get_future();
    runUntil(status_future);
    t.join();
    EXPECT_THAT(status_future.get(), absl_testing::IsOk());
}

TEST_P(EventLoopTest, PostAndWaitStatus) {
    runInThread();
    std::promise<absl::Status> status_promise;
    std::thread t([&status_promise, this] {
        status_promise.set_value(loop->PostAndWait(
                []() -> absl::Status { return absl::InvalidArgumentError("invalid input"); }));
    });
    auto status_future = status_promise.get_future();
    runUntil(status_future);
    t.join();
    EXPECT_THAT(status_future.get(), absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(EventLoopTest, PostAndWaitStatusOr) {
    runInThread();
    std::promise<absl::StatusOr<int>> status_promise;
    std::thread t([&status_promise, this] {
        status_promise.set_value(loop->PostAndWait([]() -> absl::StatusOr<int> { return 42; }));
    });
    auto status_future = status_promise.get_future();
    runUntil(status_future);
    t.join();
    EXPECT_THAT(status_future.get(), absl_testing::IsOkAndHolds(42));
}

TEST_P(EventLoopTest, PostAndWaitStatusOrError) {
    runInThread();
    std::promise<absl::StatusOr<int>> status_promise;
    std::thread t([&status_promise, this] {
        status_promise.set_value(loop->PostAndWait(
                []() -> absl::StatusOr<int> { return absl::NotFoundError("not found"); }));
    });
    auto status_future = status_promise.get_future();
    runUntil(status_future);
    t.join();
    EXPECT_THAT(status_future.get(), absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_P(EventLoopTest, ScheduleAndExecuteSingleTaskOnRunningLoop) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "ThreadedEventLoop is not compatible with the singleton QemuEventLoop.";
    }
    std::promise<bool> task_executed_promise;
    auto future = task_executed_promise.get_future();
    auto threaded_loop = ThreadedEventLoop::Create(LibuvEventLoop::Create());

    (void)threaded_loop->Post([&]() { task_executed_promise.set_value(true); });

    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(future.get());
}

TEST_P(EventLoopTest, TasksExecuteInScheduledOrder) {
    std::promise<std::vector<int>> results_promise;
    auto future = results_promise.get_future();

    std::vector<int> results;
    const int task_count = 100;

    for (int i = 0; i < task_count; ++i) {
        (void)loop->Post([&, i]() { results.push_back(i); });
    }
    (void)loop->Post([&]() { results_promise.set_value(results); });

    runInThread();
    runUntil(future);

    std::vector<int> final_results = future.get();
    std::vector<int> expected_results(task_count);
    std::iota(expected_results.begin(), expected_results.end(), 0);

    EXPECT_EQ(final_results, expected_results);
}

TEST_P(EventLoopTest, IsOnLoopThreadIsCorrect) {
    std::promise<bool> on_loop_thread_promise;
    auto future = on_loop_thread_promise.get_future();

    if (mLoopType == "libuv") {
        EXPECT_FALSE(loop->IsOnLoopThread());
        runInThread();
    } else {  // qemu now has a separate thread runner.
        EXPECT_FALSE(loop->IsOnLoopThread());
    }

    (void)loop->Post([&]() { on_loop_thread_promise.set_value(loop->IsOnLoopThread()); });
    runUntil(future);
    EXPECT_TRUE(future.get());

    // Check from another thread
    std::promise<bool> other_thread_promise;
    auto other_future = other_thread_promise.get_future();
    std::thread other_thread([&]() { other_thread_promise.set_value(loop->IsOnLoopThread()); });
    other_thread.join();
    EXPECT_FALSE(other_future.get());
}

TEST_P(EventLoopTest, CanScheduleTaskFromAnotherThread) {
    std::promise<bool> task_ran_promise;
    auto future = task_ran_promise.get_future();

    runInThread();

    std::thread worker_thread(
            [&]() { (void)loop->Post([&]() { task_ran_promise.set_value(true); }); });

    runUntil(future);
    EXPECT_TRUE(future.get());

    worker_thread.join();
}

TEST_P(EventLoopTest, CanScheduleTaskFromWithinAnotherTask) {
    std::promise<int> final_task_promise;
    auto future = final_task_promise.get_future();

    (void)loop->Post([&]() { (void)loop->Post([&]() { final_task_promise.set_value(42); }); });

    runInThread();
    runUntil(future);
    EXPECT_EQ(future.get(), 42);
}

TEST_P(EventLoopTest, MassConcurrencyPost) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "QemuEventLoop stubs do not support this scenario.";
    }
    const int num_threads = 8;
    const int tasks_per_thread = 1000;
    const int total_tasks = num_threads * tasks_per_thread;
    absl::BlockingCounter counter(total_tasks);

    // A shared counter to track completed tasks on the event loop thread.
    std::atomic<int> completed_tasks_count = 0;

    // Start the event loop in its own thread.
    std::thread loop_thread([this]() { (void)dynamic_cast<LibuvEventLoop*>(loop)->Run(); });

    // Create multiple threads, each posting tasks.
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back([&]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                (void)loop->Post([&]() {
                    completed_tasks_count++;
                    counter.DecrementCount();
                });
            }
        });
    }

    // Wait for all tasks to be executed.
    counter.Wait();

    // Verify that the final count is correct.
    EXPECT_EQ(completed_tasks_count, total_tasks);

    // Clean up.
    EXPECT_THAT(loop->ShutdownAndWait(), absl_testing::IsOk());
    for (auto& t : worker_threads) {
        t.join();
    }
    loop_thread.join();
}

TEST_P(EventLoopTest, MassConcurrencyPostWithHeavyWorkload) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "QemuEventLoop stubs do not support this scenario.";
    }
    const int num_threads = 8;
    const int tasks_per_thread = 100;
    const int total_tasks = num_threads * tasks_per_thread;
    absl::BlockingCounter counter(total_tasks);

    std::atomic<int64_t> total_sum = 0;

    // Start the event loop in its own thread.
    std::thread loop_thread([this]() { (void)dynamic_cast<LibuvEventLoop*>(loop)->Run(); });

    constexpr int sum_up_to = 1000;
    // Create worker threads to post tasks with a small workload.
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back([&]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                (void)loop->Post([&]() {
                    // Perform a small amount of work to simulate a real task.
                    int64_t local_sum = 0;
                    for (int k = 0; k <= sum_up_to; ++k) {
                        local_sum += k;
                    }
                    total_sum += local_sum;
                    counter.DecrementCount();
                });
            }
        });
    }

    counter.Wait();

    // The expected sum is the sum of numbers from 0 to 999, repeated
    // for each of the total tasks. sum(i, n) = n/2 + (n + 1)
    int64_t expected_local_sum = (sum_up_to / 2) * (sum_up_to + 1);
    int64_t expected_total_sum = expected_local_sum * total_tasks;

    EXPECT_EQ(total_sum.load(), expected_total_sum);

    EXPECT_THAT(loop->ShutdownAndWait(), absl_testing::IsOk());
    for (auto& t : worker_threads) {
        t.join();
    }
    loop_thread.join();
}

TEST_P(EventLoopTest, MassConcurrencyPostWithDataIntegrity) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "QemuEventLoop stubs do not support this scenario.";
    }
    const int num_threads = 4;
    const int tasks_per_thread = 500;
    const int total_tasks = num_threads * tasks_per_thread;
    absl::BlockingCounter counter(total_tasks);

    // A vector that is only ever modified from the event loop thread.
    std::vector<int> ordered_sequence;

    // Start the event loop in its own thread.
    std::thread loop_thread([this]() { (void)dynamic_cast<LibuvEventLoop*>(loop)->Run(); });

    // Create worker threads. Each thread posts a task with a unique ID.
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back([&, thread_id = i]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                (void)loop->Post([&, val = (thread_id * tasks_per_thread) + j]() {
                    ordered_sequence.push_back(val);
                    counter.DecrementCount();
                });
            }
        });
    }

    // Wait for all tasks to be executed.
    counter.Wait();

    // The order of execution is not guaranteed, but the number of elements should be correct.
    ASSERT_EQ(ordered_sequence.size(), total_tasks);

    // Sort the vector to check for integrity.
    std::sort(ordered_sequence.begin(), ordered_sequence.end());

    // Create the expected sequence of numbers.
    std::vector<int> expected_sequence(total_tasks);
    std::iota(expected_sequence.begin(), expected_sequence.end(), 0);

    // Verify that all numbers are present and no duplicates or missing values exist.
    EXPECT_EQ(ordered_sequence, expected_sequence);

    EXPECT_THAT(loop->ShutdownAndWait(), absl_testing::IsOk());
    for (auto& t : worker_threads) {
        t.join();
    }
    loop_thread.join();
}

// --- Tests for Fire-and-Forget `post` ---

TEST_P(EventLoopTest, PostDelayedExecutesAfterDelay) {
    runInThread();

    std::promise<void> task_completed;
    const auto delay = absl::Milliseconds(50);
    auto start_time = std::chrono::steady_clock::now();

    (void)loop->Post(
            [&]() {
                if (mLoopType == "libuv") {
                    auto elapsed = absl::FromChrono(std::chrono::steady_clock::now() - start_time);
                    EXPECT_LE(absl::AbsDuration(elapsed - delay), tolerance);
                }
                task_completed.set_value();
            },
            delay);

    auto future = task_completed.get_future();
    runUntil(future);
}

TEST_P(EventLoopTest, ScheduleDelayedHelperExecutesSuccessfully) {
    runInThread();

    std::promise<void> task_completed;
    const auto delay = absl::Milliseconds(50);
    auto start_time = std::chrono::steady_clock::now();

    auto handle = loop->ScheduleDelayed(
            [&]() {
                if (mLoopType == "libuv") {
                    auto elapsed = absl::FromChrono(std::chrono::steady_clock::now() - start_time);
                    EXPECT_LE(absl::AbsDuration(elapsed - delay), tolerance);
                }
                task_completed.set_value();
            },
            delay);

    auto future = task_completed.get_future();
    runUntil(future);
    ASSERT_NE(handle, nullptr);
}

TEST_P(EventLoopTest, ScheduleDelayedExecutesSuccessfully) {
    runInThread();

    std::promise<void> task_completed;
    const auto delay = absl::Milliseconds(50);
    auto start_time = std::chrono::steady_clock::now();

    auto handle = loop->CreateTimer([&]() {
        if (mLoopType == "libuv") {
            auto elapsed = absl::FromChrono(std::chrono::steady_clock::now() - start_time);
            EXPECT_LE(absl::AbsDuration(elapsed - delay), tolerance);
        }
        task_completed.set_value();
        return true;
    });
    handle->Schedule(delay);

    auto future = task_completed.get_future();

    runUntil(future);
    ASSERT_NE(handle, nullptr);
}

TEST_P(EventLoopTest, ScheduleDelayedIsReschedulableAfterFiring) {
    runInThread();

    std::atomic<bool> task_executed = false;
    auto handle = loop->CreateTimer([&]() {
        task_executed = true;
        return true;
    });
    handle->Schedule(absl::Milliseconds(100));

    // Advance time past the timer's expiration.
    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(150);
    } else {
        std::this_thread::sleep_for(150ms);
    }

    ASSERT_TRUE(task_executed.load());
    task_executed = false;

    handle->Schedule(absl::Milliseconds(100));

    // Advance time past the timer's expiration.
    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(150);
    } else {
        std::this_thread::sleep_for(150ms);
    }
    ASSERT_TRUE(task_executed.load());
}

TEST_P(EventLoopTest, ScheduleDelayedIsCancelledByHandle) {
    runInThread();

    std::atomic<bool> task_executed = false;
    auto handle = loop->CreateTimer([&]() {
        task_executed = true;
        return true;
    });
    handle->Schedule(absl::Milliseconds(100), absl::ZeroDuration());

    if (mLoopType == "qemu") {
        const auto pin = std::async([]() {
            std::this_thread::sleep_for(250ms);

            for (unsigned n = 150; n; --n) {
                fake_qemu_advance_ms(1);
            }
        });

        handle.reset();
    } else {
        handle.reset();
        std::this_thread::sleep_for(150ms);
    }

    ASSERT_FALSE(task_executed.load());
}

TEST_P(EventLoopTest, ScheduleDelayedIsCancelledByRAII) {
    runInThread();
    std::atomic<bool> task_executed = false;

    std::future<void> pin;
    {
        auto handle = loop->CreateTimer([&]() {
            task_executed = true;
            return true;
        });

        handle->Schedule(absl::Milliseconds(100), absl::ZeroDuration());

        if (mLoopType == "qemu") {
            pin = std::async([]() {
                std::this_thread::sleep_for(250ms);

                for (unsigned n = 150; n; --n) {
                    fake_qemu_advance_ms(1);
                }
            });
        }
    }  // handle is destroyed here, cancelling the timer.

    if (mLoopType != "qemu") {
        std::this_thread::sleep_for(150ms);
    }

    ASSERT_FALSE(task_executed.load());
}

TEST_P(EventLoopTest, ScheduleRepeatingHelperExecutesMultipleTimes) {
    runInThread();
    std::promise<void> promise;
    std::atomic<int> counter = 0;
    const int target_count = 3;

    auto handle = loop->ScheduleRepeating(
            [&]() {
                if (++counter == target_count) {
                    promise.set_value();
                }
                return true;
            },
            absl::Milliseconds(10),   // Initial delay
            absl::Milliseconds(50));  // Interval

    auto future = promise.get_future();
    runUntil(future);
    handle.reset();
    ASSERT_EQ(counter.load(), target_count);
}

TEST_P(EventLoopTest, ScheduleRepeatingExecutesMultipleTimes) {
    runInThread();
    std::promise<void> promise;
    std::atomic<int> counter = 0;
    const int target_count = 3;

    auto handle = loop->CreateTimer([&]() {
        if (++counter == target_count) {
            promise.set_value();
        }
        return true;
    });
    handle->Schedule(absl::Milliseconds(10),   // Initial delay
                     absl::Milliseconds(50));  // Interval

    auto future = promise.get_future();
    runUntil(future);
    handle.reset();
    ASSERT_EQ(counter.load(), target_count);
}

TEST_P(EventLoopTest, MultiThreadedCreationAndCancellation) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "QemuEventLoop stubs do not support this scenario.";
    }
    // Use a shared_ptr to ensure the EventLoop is not destroyed
    // while the worker threads are still using it.
    runInThread();
    // The shared queue for timer handles.
    std::queue<EventLoop::ScopedTimer> timer_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    const int num_tasks_per_creator = 10;
    const int num_creators = 100;
    const int total_tasks_to_process = num_creators * num_tasks_per_creator;

    // Atomic counters for verification
    std::atomic<int> created_task_count = 0;
    std::atomic<int> canceled_task_count = 0;

    // A blocking counter to wait for all creator threads to finish.
    std::atomic_int creators_done = num_creators;
    // A blocking counter to wait for all cancellations to occur.
    std::atomic_int cancellations_done = total_tasks_to_process;

    // Thread function for creating tasks.
    auto creator_thread_func = [&, loop_ptr = loop](int creator_id) {
        for (int i = 0; i < num_tasks_per_creator; ++i) {
            // Schedule a repeating timer.
            auto handle = loop_ptr->CreateTimer([&]() { /* Task body not critical for this test */
                                                        return true;
            });
            handle->Schedule(absl::Milliseconds(10),  // Initial delay
                             absl::Seconds(10)        // Long interval to avoid accidental ticks
            );

            // Push the handle into the shared queue.
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                timer_queue.push(std::move(handle));
                created_task_count++;
            }
            queue_cv.notify_one();
        }
        --creators_done;
    };

    // Thread function for canceling tasks.
    auto canceller_thread_func = [&]() {
        // Continue while there are still tasks to cancel.
        while (cancellations_done > 0) {
            EventLoop::ScopedTimer handle;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                // Wait for a task or for all creators to finish.
                queue_cv.wait(lock, [&] {
                    return !timer_queue.empty() || (creators_done == 0 && timer_queue.empty());
                });

                if (timer_queue.empty() && cancellations_done > 0) {
                    // This could happen if creators are still working but the queue is temporarily
                    // empty. A final wait and exit check is needed to ensure we don't prematurely
                    // exit.
                    continue;
                }

                if (timer_queue.empty() && creators_done == 0) {
                    break;
                }

                handle = std::move(timer_queue.front());

                timer_queue.pop();
            }

            if (handle) {
                handle.reset();
                canceled_task_count++;
                cancellations_done--;
            }
        }
    };

    // Start creator threads.
    std::vector<std::thread> creator_threads;
    for (int i = 0; i < num_creators; ++i) {
        creator_threads.emplace_back(creator_thread_func, i);
    }

    // Start the canceller thread.
    LOG(INFO) << "Starting cancel";
    std::thread canceller_thread(canceller_thread_func);

    // Wait for all creator threads to finish.
    for (auto& t : creator_threads) {
        t.join();
    }

    // Notify the canceller one last time in case it's waiting on an empty queue.
    queue_cv.notify_one();

    // Wait for the canceller thread to finish.
    canceller_thread.join();

    // Assertions to verify the test's success.
    ASSERT_EQ(created_task_count, total_tasks_to_process);
    ASSERT_EQ(canceled_task_count, total_tasks_to_process);
}

TEST_P(EventLoopTest, ScheduleRepeatingIsCancelledMidway) {
    runInThread();
    std::atomic<int> counter = 0;

    auto handle = loop->CreateTimer([&]() {
        counter++;
        return true;
    });
    handle->Schedule(absl::Milliseconds(10), absl::Milliseconds(40));

    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(100);
    } else {
        std::this_thread::sleep_for(100ms);
    }

    // Under heavy load, it's possible that the timer's callback was invoked a few times.
    // However, once we cancel, we should not see any more invocations.
    int count_before_cancel = counter.load();
    handle.reset();

    // Note: we can be in the middle of executing the task! (which may increase the counter, and yes
    // this does happen!)
    int count_after_cancel = counter.load();

    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(100);
    } else {
        std::this_thread::sleep_for(100ms);
    }

    int final_count = counter.load();
    EXPECT_TRUE(final_count == count_after_cancel || final_count == count_after_cancel + 1)
            << "Counter increased by more than 1 after cancellation. "
            << "count before cancel: " << count_before_cancel
            << ", count after cancel: " << count_after_cancel << ", final count: " << final_count;

    EXPECT_LE(count_after_cancel - count_before_cancel, 2)
            << "Too many invocations after cancellation, indicating the timer was not properly "
               "cancelled, count before cancel: "
            << count_before_cancel << ", count after cancel: " << count_after_cancel;
}

TEST_P(EventLoopTest, ThreadedEventLoopWaitsAtMostTimeout) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "ThreadedEventLoop is not compatible with the singleton QemuEventLoop.";
    }
    std::promise<void> task_completed;
    auto start_time = std::chrono::steady_clock::now();
    EventLoop::ScopedTimer task;
    {
        auto threaded_loop = ThreadedEventLoop::Create(LibuvEventLoop::Create());
        (void)threaded_loop->Post([&]() { task_completed.set_value(); }, absl::Seconds(10));
        start_time = std::chrono::steady_clock::now();
    }
    auto elapsed = absl::FromChrono(std::chrono::steady_clock::now() - start_time);
    EXPECT_LT(elapsed, ThreadedEventLoop::GetTimeout() + tolerance);
}

TEST_P(EventLoopTest, ShutdownRaceConditionStressTest) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "QemuEventLoop stubs do not support this scenario.";
    }
    // --- Test Setup ---
    const int kCreatorThreads = 4;
    const int kTimersPerThread = 250;
    const int kTotalTimers = kCreatorThreads * kTimersPerThread;

    // A shared vector to hold all timer handles, protected by a mutex.
    absl::Mutex vec_mutex;
    std::vector<EventLoop::ScopedTimer> all_timers;
    all_timers.reserve(kTotalTimers);

    // Start the event loop in the background.
    runInThread();

    // --- Phase 1: Create many timers from multiple threads ---
    std::vector<std::thread> creators;
    for (int i = 0; i < kCreatorThreads; ++i) {
        creators.emplace_back([&]() {
            for (int j = 0; j < kTimersPerThread; ++j) {
                // Create long-running timers so they don't fire during the test.
                auto handle = loop->CreateTimer([]() { /* no-op */ return true; });
                handle->Schedule(absl::Hours(1), absl::Hours(1));

                absl::MutexLock lock(&vec_mutex);
                all_timers.push_back(std::move(handle));
            }
        });
    }
    for (auto& t : creators) {
        t.join();
    }
    ASSERT_EQ(all_timers.size(), kTotalTimers);

    // --- Phase 2: Engineer the Race Condition ---

    // 1. Post a "slow" task to the event loop. This creates a delay,
    //    giving other threads a window to act before the shutdown task runs.
    (void)loop->Post([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });

    // 2. Call shutdown IMMEDIATELY. Its logic is now queued *behind* the slow task.
    //    We will let TearDown() call the actual shutdown, but we need to start
    //    the destroyers now.
    absl::Notification shutdown_future_available;
    absl::StatusOr<std::future<absl::Status>> shutdown_future;
    auto shutdown = std::thread([lp = loop, &shutdown_future_available, &shutdown_future] {
        shutdown_future = lp->Shutdown();
        shutdown_future_available.Notify();
    });

    // 3. Start destroyer threads. These threads will race against the shutdown
    //    task. They will destroy timers while the shutdown task is waiting in the queue.
    std::vector<std::thread> destroyers;
    const int kDestroyerThreads = 4;
    for (int i = 0; i < kDestroyerThreads; ++i) {
        destroyers.emplace_back([&]() {
            while (true) {
                EventLoop::ScopedTimer handle;
                {
                    absl::MutexLock lock(&vec_mutex);
                    if (all_timers.empty()) {
                        break;  // No more timers to destroy
                    }
                    handle = std::move(all_timers.back());
                    all_timers.pop_back();
                }
                // Resetting the ScopedTimer triggers cancellation on THIS thread.
                handle.reset();
            }
        });
    }

    // Wait for all destroyers to finish.
    for (auto& t : destroyers) {
        t.join();
    }
    shutdown_future_available.WaitForNotification();
    auto shutdown_status = shutdown_future.ok() ? shutdown_future->get() : shutdown_future.status();
    EXPECT_TRUE(shutdown_status.ok()) << "Shutdown failure: " << shutdown_status;
    shutdown.join();

    // The test's main assertion is that it doesn't crash.
    // The real result comes from the ASan/TSan output.
    // The fixture's TearDown will now perform the shutdown and cleanup.
}

TEST_P(EventLoopTest, CancelTimerFromTaskCallback) {
    runInThread();

    int count = 0;

    {
        std::promise<void> reached_limit;
        auto reached_limit_future = reached_limit.get_future();

        auto timer = loop->ScheduleRepeating(
                [&]() {
                    ++count;
                    if (count == 3) {
                        reached_limit.set_value();
                    }
                    return count < 3;
                },
                absl::ZeroDuration(), absl::Milliseconds(10));

        runUntil(reached_limit_future);

        // Wait a bit longer to ensure the timer *would* have fired if not stopped.
        if (mLoopType == "qemu") {
            fake_qemu_advance_ms(50);
        } else {
            std::this_thread::sleep_for(50ms);
        }
    }

    EXPECT_EQ(count, 3);
}

TEST_P(EventLoopTest, NoTsanFailuresOnLaunch) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "This test is specific to the LibuvEventLoop lifecycle.";
    }

    // This test validates a race-free shutdown of the event loop immediately after its creation.
    //
    // The problem this test addresses is a potential race condition between a newly created
    // ThreadedEventLoop and its shutdown sequence. In the old implementation, the event loop's
    // state might have been prematurely marked as "RUNNING" or its shutdown process initiated
    // before the worker thread had fully entered the `uv_run` loop. This could lead to:
    //
    // 1. **Data Race:** A race between the main thread calling `uv_stop` during shutdown and the
    //    worker thread reading the loop's state inside `uv_run`. This is a classic read/write
    //    race on shared state, which ThreadSanitizer (TSan) would correctly flag.
    //
    // 2. **Premature Shutdown:** The main thread's `shutdown` calls could execute
    //    before the `uv_run` loop was fully initialized, causing the teardown to operate on
    //    an incomplete or invalid state.
    //
    // The current implementation in `ThreadedEventLoop` correctly addresses this by posting
    // a task to the event loop that updates the state to "RUNNING" and by ensuring the constructor
    // will finish after the event loop is marked as running.
    //
    // The test confirms this by creating a `ThreadedEventLoop` and immediately shutting it
    // down, verifying that no TSan failures or crashes occur.
    auto threaded_loop = ThreadedEventLoop::Create(LibuvEventLoop::Create());
    auto s = threaded_loop->ShutdownAndWait(absl::Seconds(1));
    ASSERT_THAT(s, absl_testing::IsOk())
            << "Shutdown failed, the thread host run is likely not active.";
}

TEST_P(EventLoopTest, RescheduleRepeatingTimer) {
    runInThread();
    std::atomic<int> counter = 0;
    std::promise<void> fired1_promise, fired2_promise, fired3_promise;
    auto fired1_future = fired1_promise.get_future();
    auto fired2_future = fired2_promise.get_future();
    auto fired3_future = fired3_promise.get_future();

    // Shared pointers to hold timestamps to be checked inside the callback
    auto schedule_time = std::make_shared<std::atomic<std::chrono::steady_clock::time_point>>();
    auto reschedule_time = std::make_shared<std::atomic<std::chrono::steady_clock::time_point>>();
    auto last_fire_time = std::make_shared<std::atomic<std::chrono::steady_clock::time_point>>();

    auto handle = loop->CreateTimer([&, schedule_time, reschedule_time, last_fire_time]() {
        auto now = std::chrono::steady_clock::now();
        int c = ++counter;

        if (mLoopType == "libuv") {
            if (c == 1) {
                auto elapsed = absl::FromChrono(now - schedule_time->load());
                EXPECT_LE(absl::AbsDuration(elapsed - absl::Milliseconds(100)), tolerance);
            } else if (c == 2) {
                auto elapsed = absl::FromChrono(now - reschedule_time->load());
                EXPECT_LE(absl::AbsDuration(elapsed - absl::Milliseconds(200)), tolerance);
            } else if (c == 3) {
                auto elapsed = absl::FromChrono(now - last_fire_time->load());
                EXPECT_LE(absl::AbsDuration(elapsed - absl::Milliseconds(200)), tolerance);
            }
        }
        *last_fire_time = now;

        if (c == 1) fired1_promise.set_value();
        if (c == 2) fired2_promise.set_value();
        if (c == 3) fired3_promise.set_value();

        return true;
    });

    // Set interval to 1h  to prevent race condition
    // where the old timer fires before the main thread can reschedule it.
    handle->Schedule(absl::Milliseconds(100), absl::Hours(1));

    *schedule_time = std::chrono::steady_clock::now();

    runUntil(fired1_future);
    ASSERT_EQ(counter.load(), 1);

    *reschedule_time = std::chrono::steady_clock::now();

    // Apply the new schedule. The loop will discard the 1h wait and switch to 200ms.
    handle->Schedule(absl::Milliseconds(200), absl::Milliseconds(200));

    runUntil(fired2_future);
    ASSERT_EQ(counter.load(), 2);

    runUntil(fired3_future);
    ASSERT_GE(counter.load(), 3);
}

TEST_P(EventLoopTest, TimerCreatedAfterLoopShutdown) {
    runInThread();

    auto f = loop->Shutdown();
    if (mLoopType == "qemu") {
        // Need to run Qemu "thread" for shutdown to complete
        fake_qemu_advance_ms(150);
    }
    f.wait();

    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kError, ::testing::_, ::testing::_)).Times(0);
    log.StartCapturingLogs();

    auto timer = loop->CreateTimer([]() { return true; });

    timer->Schedule(absl::Milliseconds(100), absl::Milliseconds(100));

    timer.reset();
    log.StopCapturingLogs();

    loop = nullptr;
}

TEST_P(EventLoopTest, TimerCreatedDuringLoopShutdown) {
    runInThread();

    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kError, ::testing::_, ::testing::_)).Times(0);
    log.StartCapturingLogs();

    auto f = loop->Shutdown();
    auto timer = loop->CreateTimer([]() { return true; });
    if (mLoopType == "qemu") {
        // Need to run Qemu "thread" for shutdown to complete
        fake_qemu_advance_ms(150);
    }
    f.wait();

    timer->Schedule(absl::Milliseconds(100), absl::Milliseconds(100));

    timer.reset();
    log.StopCapturingLogs();

    loop = nullptr;
}

TEST_P(EventLoopTest, TimerReturningFalseCanBeRescheduled) {
    runInThread();
    std::atomic<int> count = 0;
    absl::Notification stopped_first_batch;
    absl::Notification stopped_second_batch;

    auto timer = loop->ScheduleRepeating(
            [&]() {
                const int c = ++count;
                if (c == 2) {
                    stopped_first_batch.Notify();
                    return false;
                }
                if (c == 4) {
                    stopped_second_batch.Notify();
                    return false;
                }
                return true;
            },
            absl::Milliseconds(10), absl::Milliseconds(10));

    runUntil(stopped_first_batch);
    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(50);
    } else {
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_EQ(count.load(), 2);

    // Re-arm the disarmed ScopedTimer and verify it fires again.
    timer->Schedule(absl::Milliseconds(10), absl::Milliseconds(10));
    runUntil(stopped_second_batch);
    EXPECT_EQ(count.load(), 4);
}

TEST_P(EventLoopTest, ScheduleDelayedTimerCanBeRearmedAfterFiring) {
    runInThread();
    std::atomic<int> count = 0;
    absl::Notification first_fire;
    absl::Notification second_fire;

    auto timer = loop->ScheduleDelayed(
            [&]() {
                const int c = ++count;
                if (c == 1) {
                    first_fire.Notify();
                } else if (c == 2) {
                    second_fire.Notify();
                }
            },
            absl::Milliseconds(10));

    runUntil(first_fire);
    ASSERT_EQ(count.load(), 1);

    timer->Schedule(absl::Milliseconds(10));
    runUntil(second_fire);
    EXPECT_EQ(count.load(), 2);
}
TEST_P(EventLoopTest, LibuvEventStateChanges) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "This test is specific to the LibuvEventLoop lifecycle.";
    }

    std::vector<LooperStatusEvent::State> states;
    absl::Notification finished;
    absl::Notification running;

    auto subscription =
            android::base::eventing::MakeScopedCallback(*loop, [&](const LooperStatusEvent& event) {
                states.push_back(event.state);
                LOG(ERROR) << event;
                if (event.state == LooperStatusEvent::State::kRunning) {
                    running.Notify();
                }
                if (event.state == LooperStatusEvent::State::kFinished) {
                    finished.Notify();
                }
            });

    runInThread();
    running.WaitForNotification();
    auto status = loop->ShutdownAndWait();
    ASSERT_TRUE(status.ok());
    finished.WaitForNotification();

    ASSERT_THAT(states, ::testing::ElementsAre(LooperStatusEvent::State::kRunning,
                                               LooperStatusEvent::State::kShuttingDown,
                                               LooperStatusEvent::State::kFinished));
}

TEST_P(EventLoopTest, ThreadedEventStateChanges) {
    if (mLoopType == "qemu") {
        GTEST_SKIP() << "ThreadedEventLoop is not compatible with the singleton QemuEventLoop.";
    }

    auto threaded_loop = ThreadedEventLoop::Create(LibuvEventLoop::Create());
    std::vector<LooperStatusEvent::State> states;
    absl::Notification finished;

    auto subscription = android::base::eventing::MakeScopedCallback(
            *threaded_loop, [&](const LooperStatusEvent& event) {
                states.push_back(event.state);
                LOG(ERROR) << "state: " << event;
                if (event.state == LooperStatusEvent::State::kFinished) {
                    finished.Notify();
                }
            });

    auto status = threaded_loop->ShutdownAndWait();
    ASSERT_TRUE(status.ok());
    finished.WaitForNotification();

    ASSERT_THAT(states, ::testing::ElementsAre(LooperStatusEvent::State::kShuttingDown,
                                               LooperStatusEvent::State::kFinished));
}

TEST_P(EventLoopTest, PostVoidExecutesOnLoopThread) {
    runInThread();
    absl::Notification done;
    bool executed = false;

    // Post with void lambda auto-derives return type as absl::Status.
    absl::Status status = loop->Post([&]() {
        executed = true;
        done.Notify();
    });

    ASSERT_TRUE(status.ok());
    runUntil(done);
    EXPECT_TRUE(executed);
}

TEST_P(EventLoopTest, PostVoidDelayedExecutesAfterDelay) {
    runInThread();
    absl::Notification done;
    auto start = std::chrono::steady_clock::now();
    const auto delay = absl::Milliseconds(50);

    absl::Status status = loop->Post(
            [&]() {
                if (mLoopType == "libuv") {
                    auto elapsed = absl::FromChrono(std::chrono::steady_clock::now() - start);
                    EXPECT_LE(absl::AbsDuration(elapsed - delay), tolerance);
                }
                done.Notify();
            },
            delay);

    ASSERT_TRUE(status.ok());
    runUntil(done);
}

TEST_P(EventLoopTest, PostVoidWithContextAndMoveOnlyCapture) {
    runInThread();
    absl::Notification done;
    auto ptr = std::make_unique<int>(42);
    int captured_val = 0;

    absl::Status status = loop->Post(
            [p = std::move(ptr), &captured_val, &done]() {
                captured_val = *p;
                done.Notify();
            },
            "test_context");

    ASSERT_TRUE(status.ok());
    runUntil(done);
    EXPECT_EQ(captured_val, 42);
}

TEST_P(EventLoopTest, CancelTimerBeforeLoopRunsDoesNotHang) {
    std::atomic<bool> timer1_executed = false;
    std::atomic<bool> timer2_executed = false;

    auto timer1 = loop->CreateTimer([&]() {
        timer1_executed = true;
        return true;
    });
    timer1->Schedule(absl::Milliseconds(10), absl::ZeroDuration());
    timer1.reset();

    auto timer2 = loop->CreateTimer([&]() {
        timer2_executed = true;
        return true;
    });
    timer2->Schedule(absl::Milliseconds(10), absl::ZeroDuration());
    timer2.reset();

    runInThread();
    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(150);
    } else {
        std::this_thread::sleep_for(150ms);
    }

    EXPECT_FALSE(timer1_executed.load());
    EXPECT_FALSE(timer2_executed.load());
}

TEST_P(EventLoopTest, CancelTimerOnLoopThreadBeforeDrainDoesNotFire) {
    runInThread();

    std::atomic<bool> task_executed = false;
    absl::Notification done;

    (void)loop->Post([&]() {
        auto timer = loop->CreateTimer([&]() {
            task_executed = true;
            return true;
        });
        timer->Schedule(absl::Milliseconds(10), absl::ZeroDuration());
        timer.reset();
        done.Notify();
    });

    runUntil(done);

    if (mLoopType == "qemu") {
        fake_qemu_advance_ms(150);
    } else {
        std::this_thread::sleep_for(150ms);
    }

    EXPECT_FALSE(task_executed.load());
}

TEST_P(EventLoopTest, QemuSnapshotProgressPausesAndResumesRunningLoop) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    std::vector<LooperStatusEvent::State> states;
    auto subscription = android::base::eventing::MakeScopedCallback(
            *loop, [&](const LooperStatusEvent& event) { states.push_back(event.state); });

    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kNotStarted);

    // Drain the initial BH so the loop transitions to kRunning.
    fake_qemu_advance_ms(1);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);

    QemuSnapshotProgress progress{};
    progress.operation = QEMU_SNAPSHOT_OP_SAVE;
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kPaused);

    progress.phase = QEMU_SNAPSHOT_PHASE_TRANSFERRING;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kPaused);

    progress.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);

    // Failed snapshots must also resume the loop back to kRunning.
    progress.operation = QEMU_SNAPSHOT_OP_LOAD;
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kPaused);

    progress.phase = QEMU_SNAPSHOT_PHASE_FAILED;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);

    EXPECT_THAT(states, ::testing::ElementsAre(LooperStatusEvent::State::kRunning,
                                               LooperStatusEvent::State::kPaused,
                                               LooperStatusEvent::State::kRunning,
                                               LooperStatusEvent::State::kPaused,
                                               LooperStatusEvent::State::kRunning));
}

TEST_P(EventLoopTest, QemuSnapshotProgressDoesNotOverwriteShutdown) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    using State = LooperStatusEvent::State;
    using ::testing::AnyOf;
    using ::testing::Each;

    fake_qemu_advance_ms(1);
    ASSERT_EQ(loop->GetState(), State::kRunning);

    QemuSnapshotProgress progress{};
    progress.operation = QEMU_SNAPSHOT_OP_SAVE;
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);
    ASSERT_EQ(loop->GetState(), State::kPaused);

    // Shut down from another thread mid-snapshot. Shutdown() sets kShuttingDown before it
    // returns and queues the rest of the shutdown on the loop thread (this thread).
    std::future<absl::Status> shutdown_future;
    std::thread([&]() { shutdown_future = loop->Shutdown(); }).join();
    ASSERT_EQ(loop->GetState(), State::kShuttingDown);

    std::vector<State> states;
    auto subscription = android::base::eventing::MakeScopedCallback(
            *loop, [&](const LooperStatusEvent& event) { states.push_back(event.state); });

    // Neither the end of this snapshot nor the start of the next one may resurrect the loop.
    progress.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), State::kShuttingDown);
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), State::kShuttingDown);

    runUntil(shutdown_future);
    EXPECT_THAT(shutdown_future.get(), absl_testing::IsOk());

    progress.phase = QEMU_SNAPSHOT_PHASE_FAILED;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_THAT(loop->GetState(), AnyOf(State::kShuttingDown, State::kFinished));
    EXPECT_THAT(states, Each(AnyOf(State::kShuttingDown, State::kFinished)));
}

TEST_P(EventLoopTest, QemuInitialDrainDoesNotOverwriteShutdown) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    using State = LooperStatusEvent::State;
    using ::testing::AnyOf;
    using ::testing::Each;

    ASSERT_EQ(loop->GetState(), State::kNotStarted);

    // With a snapshot in progress, the initial drain would move the loop to kPaused.
    QemuSnapshotProgress progress{};
    progress.operation = QEMU_SNAPSHOT_OP_LOAD;
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);

    // Shut down from another thread before the loop has drained anything.
    std::future<absl::Status> shutdown_future;
    std::thread([&]() { shutdown_future = loop->Shutdown(); }).join();
    ASSERT_EQ(loop->GetState(), State::kShuttingDown);

    std::vector<State> states;
    auto subscription = android::base::eventing::MakeScopedCallback(
            *loop, [&](const LooperStatusEvent& event) { states.push_back(event.state); });

    // The initial drain runs before the queued shutdown task and must keep kShuttingDown.
    runUntil(shutdown_future);
    EXPECT_THAT(shutdown_future.get(), absl_testing::IsOk());
    EXPECT_THAT(loop->GetState(), AnyOf(State::kShuttingDown, State::kFinished));
    EXPECT_THAT(states, Each(AnyOf(State::kShuttingDown, State::kFinished)));
}

TEST_P(EventLoopTest, QemuSnapshotCompletionRacingShutdownKeepsShutdown) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    // Unlike the tests above, this lets the snapshot callback (this thread) and Shutdown()
    // (another thread) race freely, so it can hit the read-to-write window of a
    // check-then-set transition.
    constexpr int kIterations = 2000;
    constexpr int kMaxStartDelaySpins = 400;
    fake_qemu_advance_ms(1);
    QemuSnapshotProgress progress{};
    progress.operation = QEMU_SNAPSHOT_OP_SAVE;
    for (int i = 0; i < kIterations; ++i) {
        auto racing_loop = QemuEventLoop::Create("RacingLoop");
        fake_qemu_advance_ms(1);
        progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
        fake_qemu_fire_snapshot_progress(&progress);
        ASSERT_EQ(racing_loop->GetState(), LooperStatusEvent::State::kPaused);

        std::atomic<int> arrived = 0;
        std::future<absl::Status> shutdown_future;
        std::thread shutdown_thread([&, delay_spins = i % kMaxStartDelaySpins]() {
            arrived.fetch_add(1);
            while (arrived.load() < 2) {
            }
            // Vary the start offset so that many points of the race window get sampled.
            for (int spin = 0; spin < delay_spins; ++spin) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            shutdown_future = racing_loop->Shutdown();
        });
        arrived.fetch_add(1);
        while (arrived.load() < 2) {
        }
        progress.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
        fake_qemu_fire_snapshot_progress(&progress);
        shutdown_thread.join();

        // The shutdown task is still queued, so the loop must be exactly kShuttingDown.
        ASSERT_EQ(racing_loop->GetState(), LooperStatusEvent::State::kShuttingDown)
                << "iteration " << i;
        runUntil(shutdown_future);
        ASSERT_THAT(shutdown_future.get(), absl_testing::IsOk());
    }
}

TEST_P(EventLoopTest, QemuSnapshotBeforeInitialDrainPreservesNotStartedAndPausedStates) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kNotStarted);

    // A startup -loadvm that starts and finishes before the main loop ever drains BHs
    // must keep the loop in kNotStarted until the first BH actually drains.
    QemuSnapshotProgress progress{};
    progress.operation = QEMU_SNAPSHOT_OP_LOAD;
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kNotStarted);

    progress.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kNotStarted);

    // If another snapshot begins before the first BH drain and a BH drains mid-snapshot,
    // the initial drain transitions to kPaused (not kRunning) until the snapshot finishes.
    progress.phase = QEMU_SNAPSHOT_PHASE_PREPARING;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kNotStarted);

    fake_qemu_advance_ms(1);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kPaused);

    // Cancelling a timer while in kPaused must not deadlock.
    std::atomic<bool> timer_executed = false;
    auto timer = loop->CreateTimer([&]() {
        timer_executed = true;
        return true;
    });
    timer->Schedule(absl::Milliseconds(10), absl::ZeroDuration());
    timer.reset();

    progress.phase = QEMU_SNAPSHOT_PHASE_COMPLETED;
    fake_qemu_fire_snapshot_progress(&progress);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);
    EXPECT_FALSE(timer_executed.load());
}

TEST_P(EventLoopTest, QemuLoopShutdownOnLoopThreadDrainsQueuedTasksAndReachesFinished) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }

    std::vector<LooperStatusEvent::State> states;
    auto subscription = android::base::eventing::MakeScopedCallback(
            *loop, [&](const LooperStatusEvent& event) { states.push_back(event.state); });

    // Establish the QEMU loop thread and transition kNotStarted -> kRunning.
    fake_qemu_advance_ms(1);
    ASSERT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);
    ASSERT_TRUE(loop->IsOnLoopThread());

    // Queue a task from a worker thread before Shutdown() runs on the QEMU loop thread.
    absl::StatusOr<std::future<std::string>> queued_future;
    std::thread worker(
            [&]() { queued_future = loop->Post([]() -> std::string { return "测试_🚀_é"; }); });
    worker.join();
    ASSERT_THAT(queued_future, absl_testing::IsOk());

    // Calling ShutdownAndWait() on the loop thread must drain the queued task before
    // transitioning to kFinished.
    ASSERT_THAT(loop->ShutdownAndWait(), absl_testing::IsOk());
    ASSERT_EQ(queued_future->wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_EQ(queued_future->get(), "测试_🚀_é");
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kFinished);
    EXPECT_THAT(states, ::testing::ElementsAre(LooperStatusEvent::State::kRunning,
                                               LooperStatusEvent::State::kShuttingDown,
                                               LooperStatusEvent::State::kFinished));

    // Subsequent Post and PostAndWait calls from any thread fail fast without blocking.
    EXPECT_THAT(loop->Post([]() {}), absl_testing::StatusIs(absl::StatusCode::kUnavailable));
    absl::Status post_after_shutdown_status = absl::OkStatus();
    std::thread late_worker([&]() { post_after_shutdown_status = loop->PostAndWait([]() {}); });
    late_worker.join();
    EXPECT_THAT(post_after_shutdown_status,
                absl_testing::StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_P(EventLoopTest, QemuLoopConcurrentPostAndWaitDuringShutdownNeverHangs) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }

    fake_qemu_advance_ms(1);
    ASSERT_EQ(loop->GetState(), LooperStatusEvent::State::kRunning);

    constexpr int kNumWorkers = 4;
    constexpr int kCallsPerWorker = 50;
    absl::BlockingCounter ready(kNumWorkers);
    absl::Notification start;
    std::atomic<int> completed_calls = 0;

    std::vector<std::thread> workers;
    workers.reserve(kNumWorkers);
    for (int i = 0; i < kNumWorkers; ++i) {
        workers.emplace_back([&]() {
            ready.DecrementCount();
            start.WaitForNotification();
            for (int j = 0; j < kCallsPerWorker; ++j) {
                const absl::Status status = loop->PostAndWait([]() {});
                EXPECT_TRUE(status.ok() || absl::IsUnavailable(status) ||
                            absl::IsFailedPrecondition(status))
                        << status;
                completed_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    ready.Wait();
    start.Notify();
    ASSERT_THAT(loop->ShutdownAndWait(), absl_testing::IsOk());

    for (std::thread& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(completed_calls.load(), kNumWorkers * kCallsPerWorker);
    EXPECT_EQ(loop->GetState(), LooperStatusEvent::State::kFinished);
}

TEST(LibuvEventLoopShutdownTest, DrainsHandlesClosedAfterShutdownWithoutWarning) {
    using ::testing::_;
    using ::testing::HasSubstr;

    bool close_cb_ran = false;
    auto local_loop = LibuvEventLoop::Create("ShutdownDrainTest");
    std::thread runner([&]() { (void)local_loop->Run(); });

    auto* handle = new uv_async_t{};
    handle->data = &close_cb_ran;
    ASSERT_THAT(local_loop->PostAndWait([&]() {
        auto* raw_loop = static_cast<uv_loop_t*>(local_loop->GetRawLoop());
        ASSERT_EQ(uv_async_init(raw_loop, handle, [](uv_async_t* /*h*/) {}), 0);
        uv_unref(reinterpret_cast<uv_handle_t*>(handle));
    }),
                absl_testing::IsOk());

    ASSERT_THAT(local_loop->ShutdownAndWait(), absl_testing::IsOk());
    runner.join();

    // Closing a handle after Run() has already exited leaves it pending in
    // closing_handles until ~LibuvEventLoopImpl() drains the loop.
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
        *static_cast<bool*>(h->data) = true;
        delete reinterpret_cast<uv_async_t*>(h);
    });

    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _, HasSubstr("Failed to close uv_loop")))
            .Times(0);
    log.StartCapturingLogs();

    local_loop.reset();

    log.StopCapturingLogs();
    EXPECT_TRUE(close_cb_ran);
}

TEST(LibuvEventLoopShutdownTest, ClosesLingeringUnreferencedHandlesOnDestroyWithoutWarning) {
    using ::testing::_;
    using ::testing::HasSubstr;

    uv_timer_t lingering_timer{};
    auto local_loop = LibuvEventLoop::Create("LingeringHandleTest");
    std::thread runner([&]() { (void)local_loop->Run(); });

    ASSERT_THAT(local_loop->PostAndWait([&]() {
        auto* raw_loop = static_cast<uv_loop_t*>(local_loop->GetRawLoop());
        ASSERT_EQ(uv_timer_init(raw_loop, &lingering_timer), 0);
        uv_unref(reinterpret_cast<uv_handle_t*>(&lingering_timer));
    }),
                absl_testing::IsOk());

    ASSERT_THAT(local_loop->ShutdownAndWait(), absl_testing::IsOk());
    runner.join();

    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _, HasSubstr("Failed to close uv_loop")))
            .Times(0);
    log.StartCapturingLogs();

    local_loop.reset();

    log.StopCapturingLogs();
    EXPECT_NE(uv_is_closing(reinterpret_cast<uv_handle_t*>(&lingering_timer)), 0);
}

TEST_P(EventLoopTest, TimerDoesNotFireAfterCancelReturnsDuringShutdown) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    int fired = 0;
    auto timer = loop->ScheduleDelayed([&fired] { ++fired; }, absl::Milliseconds(1));
    fake_qemu_advance_ms(1);

    std::future<absl::Status> shutdown;
    std::thread grpc([&] { shutdown = loop->Shutdown(); });
    grpc.join();

    std::thread hang_detector([&] { timer.reset(); });
    hang_detector.join();

    fake_qemu_advance_ms(1);
    ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_THAT(shutdown.get(), absl_testing::IsOk());

    EXPECT_EQ(fired, 0) << "the timer callback ran after ScopedTimer::reset() returned";
}

TEST(FakeQemuTest, AdvanceIsRaceFreeWithTimersRegisteredOnOtherThreads) {
#ifndef ABSL_HAVE_THREAD_SANITIZER
    GTEST_SKIP() << "Run with --config=tsan: only ThreadSanitizer reports this race reliably.";
#else
    constexpr int kTimers = 1000;
    std::vector<QEMUTimer> timers(kTimers);
    std::atomic<bool> start = false;
    std::thread registrar([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (QEMUTimer& timer : timers) {
            timer_init_full(
                    &timer, nullptr, QEMU_CLOCK_VIRTUAL, SCALE_MS, 0, [](void*) {}, nullptr);
        }
    });
    start.store(true, std::memory_order_release);
    for (int i = 0; i < kTimers; ++i) {
        fake_qemu_advance_ms(1);
    }
    registrar.join();

    for (QEMUTimer& timer : timers) {
        timer_del(&timer);
    }
    fake_qemu_reset();
#endif
}

TEST_P(EventLoopTest, ShutdownOnQemuMainThreadBeforeFirstDrainCompletes) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    fake_qemu_set_bql_tracking(true);
    bql_lock();
    const absl::Status status = loop->ShutdownAndWait(absl::Milliseconds(200));
    bql_unlock();

    EXPECT_THAT(status, absl_testing::IsOk());
    fake_qemu_advance_ms(1);
}

TEST_P(EventLoopTest, ShutdownTimersStopsTimersBeforeReturning) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    int fired = 0;
    auto timer = loop->ScheduleDelayed([&fired] { ++fired; }, absl::Milliseconds(1));
    fake_qemu_advance_ms(1);

    loop->ShutdownTimers();
    fake_qemu_advance_ms(5);

    EXPECT_EQ(fired, 0) << "a timer fired after ShutdownTimers() returned on the loop thread";
}

TEST_P(EventLoopTest, SubMillisecondRepeatingTimerKeepsRepeating) {
    if (mLoopType != "qemu") {
        GTEST_SKIP() << "This test is specific to QemuEventLoop.";
    }
    int fired = 0;
    auto timer = loop->ScheduleRepeating(
            [&fired] {
                ++fired;
                return true;
            },
            absl::Microseconds(500), absl::Microseconds(500));

    fake_qemu_advance_ms(20);
    timer.reset();

    EXPECT_GT(fired, 1) << "a timer repeating every 500us fired " << fired
                        << " time(s) in 20ms of QEMU time";
}

}  // namespace goldfish::async
