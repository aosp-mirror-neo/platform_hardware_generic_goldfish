// Copyright (C) 2025 The Android Open Source Project
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

#include "goldfish/async/threaded_event_loop.h"

#include <future>
#include <memory>
#include <thread>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

#ifdef _WIN32
// clang-format off
// IWYU pragma: begin_keep
#include <windows.h>
#include <processthreadsapi.h>
#include "android/base/win32_unicode_string.h"
// IWYU pragma: end_keep
// clang-format on
#else
#include <pthread.h>
#endif

#include "goldfish/async/event_loop.h"
#include "goldfish/async/libuv_event_loop.h"

namespace goldfish::async {

class ThreadedEventLoopImpl : public ThreadedEventLoop {
  public:
    /**
     * @brief Constructs a ThreadedEventLoopImpl.
     *
     * The constructor will spin up a new thread and run the EventLoop.
     * it will block and wait until the EventLoop has marked itself as started.
     *
     * @param loop A unique_ptr to the underlying EventLoop implementation that
     * this class will manage and run.
     */
    explicit ThreadedEventLoopImpl(std::unique_ptr<LibuvEventLoop> loop);
    ~ThreadedEventLoopImpl() override;

    // --- Prevent Copying ---
    ThreadedEventLoopImpl(const ThreadedEventLoopImpl&) = delete;
    ThreadedEventLoopImpl& operator=(const ThreadedEventLoopImpl&) = delete;

    // --- Prevent Moving ---
    ThreadedEventLoopImpl(ThreadedEventLoopImpl&& other) noexcept = delete;
    ThreadedEventLoopImpl& operator=(ThreadedEventLoopImpl&& other) noexcept = delete;

    void ShutdownTimers() override { loop_->ShutdownTimers(); }
    size_t WaitUntilIdle() override { return loop_->WaitUntilIdle(); }
    std::future<absl::Status> Shutdown() override { return loop_->Shutdown(); }

    /**
     * @brief Checks if the caller is on the background event loop thread.
     * @return Delegates the call to the underlying EventLoop.
     */
    bool IsOnLoopThread() const override { return loop_->IsOnLoopThread(); }

    ScopedTimer CreateTimer(RepeatingTask task) override {
        return loop_->CreateTimer(std::move(task));
    }

    void* GetRawLoop() override { return loop_->GetRawLoop(); }

    EventLoop* Loop() { return loop_.get(); }

    std::thread::id GetId() const override { return runner_.get_id(); }

    LooperStatusEvent::State GetState() const override { return loop_->GetState(); }

    absl::Status Start() override;

  private:
    absl::Status PostImmediately(Task task, FlowId flow_id) override {
        return loop_->PostImmediately(std::move(task), flow_id);
    }

    absl::Status PostDelayed(Task task, absl::Duration delay, FlowId flow_id) override {
        return loop_->PostDelayed(std::move(task), delay, flow_id);
    }

    std::thread runner_;
    std::unique_ptr<LibuvEventLoop> loop_;
    std::unique_ptr<android::base::eventing::ScopedEventCallback<LibuvEventLoop, LooperStatusEvent>>
            subscription_;
};

ThreadedEventLoopImpl::ThreadedEventLoopImpl(std::unique_ptr<LibuvEventLoop> loop)
        : ThreadedEventLoop(loop->GetName()), loop_(std::move(loop)) {
    subscription_ = android::base::eventing::MakeScopedCallback(
            *loop_, [this](const LooperStatusEvent& event) { this->FireEvent(event); });
}

ThreadedEventLoopImpl::~ThreadedEventLoopImpl() {
    VLOG(1) << "~ThreadedEventLoopImpl";
    auto future = Shutdown();
    auto wait = future.wait_for(absl::ToChronoNanoseconds(GetTimeout()));
    if (wait == std::future_status::ready) {
        auto status = future.get();
        if (!status.ok()) {
            LOG(ERROR) << "Failed to shutdown event loop: " << status;
        }
    } else {
        // There is likely a hung task blocking the loop.
        // Join will hang if the loop has not shutdown. All we can do is crash with an error.
        LOG(FATAL) << "ThreadedEventLoop did not complete shutdown within: " << GetTimeout();
    }

    if (runner_.joinable()) {
        runner_.join();
    }
}

absl::Status ThreadedEventLoopImpl::Start() {
    if (GetState() != LooperStatusEvent::State::kNotStarted) {
        return absl::FailedPreconditionError(
                "The event loop is automatically run, and has already started.");
    }
    runner_ = std::thread([this] {
        auto status = loop_->Run();
        if (!status.ok()) {
            LOG(WARNING) << "Event loop exited with: " << status;
        }
    });
    return absl::OkStatus();
}

std::unique_ptr<ThreadedEventLoop> ThreadedEventLoop::Create(
        std::unique_ptr<LibuvEventLoop> to_run) {
    if (!to_run) {
        LOG(WARNING) << "No looper present";
        return nullptr;
    }

    auto loop = std::make_unique<ThreadedEventLoopImpl>(std::move(to_run));
    if (auto status = loop->Start(); !status.ok()) {
        LOG(WARNING) << "Failed to start inner loop due to: " << status;
        return nullptr;
    }

    VLOG(1) << "Waiting until the thread is truly running";
    if (auto s = loop->PostAndWait([]() {}); !s.ok()) {
        LOG(WARNING) << "Eventloop state did not transition to running: " << s;
        return nullptr;
    }

    return loop;
}

}  // namespace goldfish::async
