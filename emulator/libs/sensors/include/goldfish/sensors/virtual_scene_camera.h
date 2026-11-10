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

#pragma once

#include <glm/vec3.hpp>

#include <mutex>

#include "goldfish/eventing/observable_value.h"
#include "goldfish/system/virtual_clock.h"

namespace goldfish::sensors {

class PhysicalModel;

/**
 * @brief Manages virtual scene camera control state and translates camera rotation/velocity
 *        inputs into physical model target state updates.
 */
class VirtualSceneCamera {
  public:
    static constexpr float kAmbientMotionExtentMeters = 0.005F;
    static constexpr float kMinVerticalRotationDegrees = -80.0F;
    static constexpr float kMaxVerticalRotationDegrees = 80.0F;

    using ObservableActive =
            eventing::ObservableValue<bool, eventing::ObservableValueTriggerOnUpdate>;

    VirtualSceneCamera(PhysicalModel* physical_model, bool enable_ambient_motion,
                       const ::goldfish::system::VirtualClock& virtual_clock);

    VirtualSceneCamera(const VirtualSceneCamera&) = delete;
    VirtualSceneCamera(VirtualSceneCamera&&) = delete;
    VirtualSceneCamera& operator=(const VirtualSceneCamera&) = delete;
    VirtualSceneCamera& operator=(VirtualSceneCamera&&) = delete;

    void AddSceneUser();
    void RemoveSceneUser();

    bool IsConnected() const { return is_active_.GetValue(); }
    ObservableActive& GetActiveListener() { return is_active_; }
    const ObservableActive& GetActiveListener() const { return is_active_; }

    void Rotate(glm::vec3 delta_radians);
    void SetVelocity(glm::vec3 velocity);

  private:
    void AdvanceTime();
    void OnEnable();
    void OnDisable();
    void UpdateVelocity();

    PhysicalModel* const physical_model_;
    const bool enable_ambient_motion_;
    const ::goldfish::system::VirtualClock& virtual_clock_;

    mutable std::mutex mutex_;
    int num_users_{0};
    glm::vec3 euler_rotation_radians_{0.0F};
    glm::vec3 base_velocity_{0.0F};
    ObservableActive is_active_{false};
};

}  // namespace goldfish::sensors
