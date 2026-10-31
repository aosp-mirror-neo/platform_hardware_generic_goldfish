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

#include "goldfish_sync.h"

#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"

#include "goldfish/legacy/common/adapters.h"

namespace goldfish::legacy {

GoldfishSyncDevice::GoldfishSyncDevice(std::unique_ptr<IGuestMemory> memory, SetIrqCallback set_irq,
                                       IGoldfishGpu& gpu)
        : memory_(std::move(memory))
        , callback_state_(std::make_shared<CallbackState>())
        , set_irq_(std::move(set_irq))
        , gpu_(gpu) {
    DCHECK(memory_ != nullptr);
    DCHECK(set_irq_ != nullptr);
    {
        absl::MutexLock lock(&callback_state_->mu);
        callback_state_->dev = this;
    }
    std::weak_ptr<CallbackState> weak_state = callback_state_;
    gpu_.SetTimelineIncCallback([weak_state](uint64_t guest_timeline, uint32_t how_much) {
        if (auto state = weak_state.lock()) {
            absl::MutexLock lock(&state->mu);
            if (state->dev != nullptr) {
                state->dev->QueueTimelineInc(guest_timeline, how_much);
            }
        }
    });
}

GoldfishSyncDevice::~GoldfishSyncDevice() {
    {
        absl::MutexLock lock(&callback_state_->mu);
        callback_state_->dev = nullptr;
    }
    gpu_.SetTimelineIncCallback(nullptr);
}

void GoldfishSyncDevice::QueueTimelineInc(uint64_t guest_timeline, uint32_t how_much)
        ABSL_LOCKS_EXCLUDED(mu_) {
    bool should_assert_irq = false;
    {
        absl::MutexLock lock(&mu_);
        if (!pending_.empty() && pending_.back().cmd == kSyncCmdSyncTimelineInc &&
            pending_.back().handle == guest_timeline) {
            pending_.back().time_arg += how_much;
        } else {
            GoldfishSyncCommand cmd{};
            cmd.cmd = kSyncCmdSyncTimelineInc;
            cmd.handle = guest_timeline;
            cmd.time_arg = how_much;
            pending_.push_back(cmd);
        }
        if (!irq_level_) {
            irq_level_ = true;
            should_assert_irq = true;
        }
    }
    if (should_assert_irq) {
        set_irq_(true);
    }
}

void GoldfishSyncDevice::QueueHostCommand(const GoldfishSyncCommand& cmd) ABSL_LOCKS_EXCLUDED(mu_) {
    bool should_assert_irq = false;
    {
        absl::MutexLock lock(&mu_);
        pending_.push_back(cmd);
        if (!irq_level_) {
            irq_level_ = true;
            should_assert_irq = true;
        }
    }
    if (should_assert_irq) {
        set_irq_(true);
    }
}

uint32_t GoldfishSyncDevice::Read(uint32_t offset) ABSL_LOCKS_EXCLUDED(mu_) {
    switch (offset) {
    case kSyncRegBatchCommand: {
        bool lower_irq = false;
        uint32_t cmd_code = kSyncCmdReady;
        {
            absl::MutexLock lock(&mu_);
            if (pending_.empty()) {
                if (irq_level_) {
                    irq_level_ = false;
                    lower_irq = true;
                }
                if (batch_cmd_addr_ != 0) {
                    GoldfishSyncCommand ready_cmd{};
                    ready_cmd.cmd = kSyncCmdReady;
                    memory_->Write(batch_cmd_addr_, &ready_cmd, sizeof(ready_cmd));
                }
            } else if (batch_cmd_addr_ == 0) {
                LOG(WARNING) << "goldfish_sync: read BATCH_COMMAND before "
                                "BATCH_COMMAND_ADDR is programmed";
                cmd_code = kSyncCmdReady;
            } else {
                const GoldfishSyncCommand front = pending_.front();
                if (!memory_->Write(batch_cmd_addr_, &front, sizeof(front))) {
                    LOG(WARNING) << "goldfish_sync: failed to write batch command at 0x" << std::hex
                                 << batch_cmd_addr_;
                    cmd_code = kSyncCmdReady;
                } else {
                    pending_.pop_front();
                    cmd_code = front.cmd;
                }
            }
        }
        if (lower_irq) {
            set_irq_(false);
        }
        return cmd_code;
    }
    case kSyncRegBatchCommandAddr: {
        absl::MutexLock lock(&mu_);
        return static_cast<uint32_t>(batch_cmd_addr_);
    }
    case kSyncRegBatchCommandAddrHigh: {
        absl::MutexLock lock(&mu_);
        return static_cast<uint32_t>(batch_cmd_addr_ >> 32);
    }
    case kSyncRegBatchGuestCommandAddr: {
        absl::MutexLock lock(&mu_);
        return static_cast<uint32_t>(batch_guestcmd_addr_);
    }
    case kSyncRegBatchGuestCommandAddrHigh: {
        absl::MutexLock lock(&mu_);
        return static_cast<uint32_t>(batch_guestcmd_addr_ >> 32);
    }
    default:
        LOG(WARNING) << "goldfish_sync: unexpected read at offset 0x" << std::hex << offset;
        return 0;
    }
}

void GoldfishSyncDevice::Write(uint32_t offset, uint32_t value) ABSL_LOCKS_EXCLUDED(mu_) {
    switch (offset) {
    case kSyncRegInit: {
        bool lower_irq = false;
        {
            absl::MutexLock lock(&mu_);
            pending_.clear();
            if (irq_level_) {
                irq_level_ = false;
                lower_irq = true;
            }
        }
        if (lower_irq) {
            set_irq_(false);
        }
        break;
    }
    case kSyncRegBatchCommand: {
        GoldfishSyncCommand cmd{};
        absl::MutexLock lock(&mu_);
        if (batch_cmd_addr_ == 0) {
            break;
        }
        if (!memory_->Read(batch_cmd_addr_, &cmd, sizeof(cmd))) {
            LOG(WARNING) << "goldfish_sync: failed to read batch command at 0x" << std::hex
                         << batch_cmd_addr_;
        }
        break;
    }
    case kSyncRegBatchGuestCommand: {
        GoldfishSyncGuestCommand cmd{};
        {
            absl::MutexLock lock(&mu_);
            if (batch_guestcmd_addr_ == 0) {
                break;
            }
            if (!memory_->Read(batch_guestcmd_addr_, &cmd, sizeof(cmd))) {
                LOG(WARNING) << "goldfish_sync: failed to read guest command at 0x" << std::hex
                             << batch_guestcmd_addr_;
                break;
            }
        }
        if (cmd.host_command == kSyncCmdTriggerHostWait) {
            gpu_.TriggerHostWait(cmd.glsync_handle, cmd.thread_handle, cmd.guest_timeline_handle);
        } else {
            LOG(WARNING) << "goldfish_sync: unhandled guest command " << cmd.host_command;
        }
        break;
    }
    case kSyncRegBatchCommandAddr: {
        absl::MutexLock lock(&mu_);
        batch_cmd_addr_ = (batch_cmd_addr_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    }
    case kSyncRegBatchCommandAddrHigh: {
        absl::MutexLock lock(&mu_);
        batch_cmd_addr_ =
                (batch_cmd_addr_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    }
    case kSyncRegBatchGuestCommandAddr: {
        absl::MutexLock lock(&mu_);
        batch_guestcmd_addr_ =
                (batch_guestcmd_addr_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(value);
        break;
    }
    case kSyncRegBatchGuestCommandAddrHigh: {
        absl::MutexLock lock(&mu_);
        batch_guestcmd_addr_ = (batch_guestcmd_addr_ & 0x00000000FFFFFFFFULL) |
                               (static_cast<uint64_t>(value) << 32);
        break;
    }
    default:
        LOG(WARNING) << "goldfish_sync: unexpected write at offset 0x" << std::hex << offset;
        break;
    }
}

void GoldfishSyncDevice::Reset() ABSL_LOCKS_EXCLUDED(mu_) {
    {
        absl::MutexLock lock(&mu_);
        batch_cmd_addr_ = 0;
        batch_guestcmd_addr_ = 0;
        pending_.clear();
        irq_level_ = false;
    }
    set_irq_(false);
}

}  // namespace goldfish::legacy

extern "C" GoldfishMmioCore GoldfishSyncCoreCreate(const GoldfishDeviceEnv* env) {
    DCHECK(env != nullptr);
    auto dev = std::make_unique<goldfish::legacy::GoldfishSyncDevice>(
            goldfish::legacy::WrapGuestMemory(env->memory), goldfish::legacy::WrapIrq(env->irq),
            goldfish::legacy::GetGoldfishGpu());
    return goldfish::legacy::ToMmioCore(std::move(dev));
}
