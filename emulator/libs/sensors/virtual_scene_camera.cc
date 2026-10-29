/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "goldfish/sensors/virtual_scene_camera.h"

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/time.h"

#include "goldfish/physics/glm_helpers.h"
#include "goldfish/physics/physics.h"
#include "goldfish/sensors/physical_model.h"

namespace goldfish::sensors {

VirtualSceneCamera::VirtualSceneCamera(PhysicalModel* physical_model, bool enable_ambient_motion,
                                       const ::goldfish::system::VirtualClock& virtual_clock)
        : physical_model_(physical_model)
        , enable_ambient_motion_(enable_ambient_motion)
        , virtual_clock_(virtual_clock) {
    CHECK(physical_model_ != nullptr);
}

void VirtualSceneCamera::AddSceneUser() {
    bool enabled = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (++num_users_ == 1) {
            OnEnable();
            enabled = true;
        }
    }
    if (enabled) {
        is_active_.SetValue(true);
    }
}

void VirtualSceneCamera::RemoveSceneUser() {
    bool disabled = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        --num_users_;
        if (num_users_ == 0) {
            OnDisable();
            disabled = true;
        } else if (num_users_ < 0) {
            LOG(ERROR) << "Removed more virtual scene users than were added";
            num_users_ = 0;
        }
    }
    if (disabled) {
        is_active_.SetValue(false);
    }
}

void VirtualSceneCamera::Rotate(glm::vec3 delta_radians) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (num_users_ <= 0) {
        return;
    }

    AdvanceTime();
    euler_rotation_radians_ += delta_radians;
    euler_rotation_radians_.x =
            glm::clamp(euler_rotation_radians_.x, glm::radians(kMinVerticalRotationDegrees),
                       glm::radians(kMaxVerticalRotationDegrees));

    const glm::vec3 rotation_degrees =
            glm::degrees(ToEulerAnglesXyz(FromEulerAnglesYxz(euler_rotation_radians_)));
    physical_model_->SetTargetRotation(rotation_degrees, PhysicalInterpolation::kSmooth);
    UpdateVelocity();
}

void VirtualSceneCamera::SetVelocity(glm::vec3 velocity) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (num_users_ <= 0) {
        return;
    }

    AdvanceTime();
    base_velocity_ = velocity;
    UpdateVelocity();
}

void VirtualSceneCamera::AdvanceTime() {
    physical_model_->SetCurrentTime(absl::ToInt64Nanoseconds(virtual_clock_.Now()));
}

void VirtualSceneCamera::OnEnable() {
    AdvanceTime();

    const glm::vec3 xyz_rotation_degrees =
            physical_model_->GetParameterRotation(ParameterValueType::kTarget);
    euler_rotation_radians_ =
            ToEulerAnglesYxz(FromEulerAnglesXyz(glm::radians(xyz_rotation_degrees)));

    if (enable_ambient_motion_) {
        physical_model_->SetTargetAmbientMotion(kAmbientMotionExtentMeters,
                                                PhysicalInterpolation::kSmooth);
    }
}

void VirtualSceneCamera::OnDisable() {
    AdvanceTime();

    base_velocity_ = glm::vec3(0.0F);
    UpdateVelocity();
    physical_model_->SetTargetAmbientMotion(0.0F, PhysicalInterpolation::kSmooth);
}

void VirtualSceneCamera::UpdateVelocity() {
    const glm::vec3 look_velocity =
            glm::angleAxis(euler_rotation_radians_.y, glm::vec3(0.0F, 1.0F, 0.0F)) * base_velocity_;
    physical_model_->SetTargetVelocity(look_velocity, PhysicalInterpolation::kSmooth);
}

}  // namespace goldfish::sensors
