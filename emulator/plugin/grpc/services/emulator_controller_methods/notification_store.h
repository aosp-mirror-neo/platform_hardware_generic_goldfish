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

#pragma once

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/avd_universe/grpc/grpc_notification_channel.h"

namespace android::emulation::control {

using ::goldfish::avd_universe::grpc::GrpcNotification;
using ::goldfish::avd_universe::grpc::GrpcNotificationEventSource;

/**
 * @brief The NotificationStore class is responsible for storing and replaying
 * GrpcNotification messages.
 *
 * It listens to a GrpcNotificationEventSource and keeps track of the latest
 * notification of each type. This is useful for "sticky" notifications that
 * should be sent immediately to new stream callers.
 */
class NotificationStore {
  public:
    NotificationStore(GrpcNotificationEventSource* source) : source_(source) {
        callback_id_ = source_->AddCallback(
                [this](const GrpcNotification& notification) { OnNotification(notification); });
    }

    ~NotificationStore() { source_->RemoveCallback(callback_id_); }

    /**
     * @brief Called when a new notification arrives.
     *
     * @param notification The arrived notification.
     */
    void OnNotification(const GrpcNotification& notification) {
        if (!isSticky(notification)) {
            return;
        }

        absl::MutexLock lock(mutex_);
        latest_notifications_[notification.type_case()] = notification;
    }

    /**
     * @brief Returns a list of the latest notifications that have been stored.
     *
     * @return std::vector<GrpcNotification> A vector containing the latest
     * notifications.
     */
    std::vector<GrpcNotification> GetLatest() const {
        absl::MutexLock lock(mutex_);
        std::vector<GrpcNotification> latest;
        for (const auto& [type, notification] : latest_notifications_) {
            latest.push_back(notification);
        }
        return latest;
    }

  private:
    static bool isSticky(const GrpcNotification& notification) {
        switch (notification.type_case()) {
        case GrpcNotification::TypeCase::kBooted:
        case GrpcNotification::TypeCase::kXrOptions:
        case GrpcNotification::TypeCase::kCameraNotification:
        case GrpcNotification::TypeCase::kPosture:
        case GrpcNotification::TypeCase::kMicrophoneState:
            return true;
        default:
            return false;
        }
    }

    GrpcNotificationEventSource* source_;
    GrpcNotificationEventSource::CallbackId callback_id_;
    mutable absl::Mutex mutex_;
    absl::flat_hash_map<GrpcNotification::TypeCase, GrpcNotification> latest_notifications_
            ABSL_GUARDED_BY(mutex_);
};

}  // namespace android::emulation::control
