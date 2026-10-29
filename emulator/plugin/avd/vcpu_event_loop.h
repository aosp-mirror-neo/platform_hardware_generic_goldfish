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

#pragma once

#include <future>
#include <string>

#include "absl/strings/str_cat.h"

#include "emulator/plugin/avd/qemu_cpu_wrapper.h"
#include "goldfish/async/event_loop.h"

namespace goldfish::avd {

using ::android::crashreport::FlowId;

class VCpuEventLoop : public ::goldfish::async::EventLoop {
  public:
    static int cpus_count() { return aemu_cpus_count(); }

    class VCpuTimer : public Timer {
      public:
        VCpuTimer(int cpuIndex, RepeatingTask task) : mCpuIndex(cpuIndex), mTask(std::move(task)) {
            mCallback.f = onTimer;
            mCallback.opaque = this;
        }

        void Cancel() override {
            // We can't actually cancel - but we shouldn't allow rescheduling.
        }

        void Schedule(absl::Duration new_delay, absl::Duration new_interval) override {
            if (new_delay != absl::ZeroDuration()) {
                LOG(ERROR) << "VCpuTimer does not support delayed scheduling";
                return;
            }
            if (new_interval != absl::ZeroDuration()) {
                LOG(ERROR) << "VCpuTimer does not support interval scheduling";
                return;
            }

            if (!aemu_cpus_run_async(mCpuIndex, &mCallback)) {
                LOG(ERROR) << "Failed to schedule on vcpu: " << mCpuIndex;
            }
        }

      private:
        static void onTimer(void* opaque) {
            auto* self = static_cast<VCpuTimer*>(opaque);
            self->mTask();
        }

        int mCpuIndex;
        RepeatingTask mTask;

        cpus_callback mCallback{};
    };

    VCpuEventLoop(int cpuIndex)
            : ::goldfish::async::EventLoop(absl::StrCat("VCpuLoop:", cpuIndex))
            , mCpuIndex(cpuIndex) {
        SetState(::goldfish::async::LooperStatusEvent::State::kRunning);
    }

    ~VCpuEventLoop() = default;
    VCpuEventLoop(VCpuEventLoop&& other)
            : ::goldfish::async::EventLoop(other.GetName()), mCpuIndex(other.mCpuIndex) {
        SetState(other.GetState());
    }

    int getCpuIndex() const { return mCpuIndex; }

    void ShutdownTimers() override { /* do nothing */ }
    size_t WaitUntilIdle() override { return 0; }
    std::future<absl::Status> Shutdown() override {
        std::promise<absl::Status> p;
        p.set_value(absl::OkStatus());
        return p.get_future();
    }
    bool IsOnLoopThread() const override { return false; }
    absl::Status PostImmediately(Task task, FlowId) override {
        return absl::UnimplementedError("");
    }
    absl::Status PostDelayed(Task task, absl::Duration delay, FlowId) override {
        return absl::UnimplementedError("");
    }

    std::shared_ptr<Timer> CreateTimer(RepeatingTask task) override {
        return std::make_shared<VCpuTimer>(mCpuIndex, std::move(task));
    }

  private:
    int mCpuIndex;
};

}  // namespace goldfish::avd