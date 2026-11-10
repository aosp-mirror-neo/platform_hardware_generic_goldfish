// Copyright 2025 The Android Open Source Project
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
// limitations under the License.D
#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/display/pixman_display.h"
#include "goldfish/display/test/pixman_image_generator.h"
#include "goldfish/eventing/event_sources.h"

extern "C" {
// clang-format off
  // IWYU pragma: begin_keep
  #include "qemu/osdep.h"
  #include "pixman.h"
  // IWYU pragma: end_keep
// clang-format on
}

namespace goldfish::display::test {

/**
 * @brief Represents a fake evdev event.
 *
 * This struct is used to store information about a simulated evdev event.
 */
struct FakeEvDevEvent {
    /**
     * @brief The type of the evdev event.
     */
    uint16_t type;
    /**
     * @brief The code of the evdev event.
     */
    uint16_t code;
    /**
     * @brief The value of the evdev event.
     */
    uint32_t value;

    /**
     * @brief Checks if two FakeEvDevEvent objects are equal.
     *
     * @param other The other FakeEvDevEvent object to compare with.
     * @return True if the two objects are equal, false otherwise.
     */
    bool operator==(const FakeEvDevEvent& other) const {
        return type == other.type && code == other.code && value == other.value;
    }
};

/**
 * @brief A fake implementation of the PixmanDisplay class.
 *
 * This class is used for testing purposes and simulates a PixmanDisplay.
 */
struct FakePixmanDisplay : public PixmanDisplay {
  public:
    /**
     * @brief Constructs a FakePixmanDisplay.
     *
     * @param id The ID of the display.
     * @param image The initial pixman image to display.
     */
    FakePixmanDisplay(EventLoop* loop, int id, ::pixman_image_t* image, uint32_t dpi = 0,
                      uint32_t flags = 0);
    FakePixmanDisplay(EventLoop* loop, int id, const PixmanImagePtr& image, uint32_t dpi = 0,
                      uint32_t flags = 0);

    /**
     * @brief Destroys the FakePixmanDisplay, shutting its producer down first.
     */
    ~FakePixmanDisplay() override;

    /**
     * @brief Sends a simulated evdev event.
     *
     * @param type The type of the evdev event.
     * @param code The code of the evdev event.
     * @param value The value of the evdev event.
     */
    void SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) override;
    /**
     * @brief Records a simulated evdev report in `evdev_reports`, and its
     * events followed by EV_SYN/SYN_REPORT in `evdevs`.
     *
     * @param events The report's events, without the trailing SYN_REPORT.
     */
    void SendEvDevReport(EvDevEvents events) override;
    /**
     * @brief Records a simulated evdev report exactly as SendEvDevReport does.
     *
     * @param events The report's events, without the trailing SYN_REPORT.
     */
    void WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) override;
    /**
     * @brief Returns `input_active`.
     */
    bool IsInputActive() const override { return input_active; }
    /**
     * @brief Returns `input_active_generation`.
     */
    uint64_t InputActiveGeneration() const override { return input_active_generation; }
    /**
     * @brief Advances `input_active_generation` and notifies active-generation
     * listeners, simulating a guest input driver reset or re-probe.
     */
    uint64_t AdvanceInputActiveGeneration() {
        const uint64_t generation = ++input_active_generation;
        NotifyInputActiveChanged(generation);
        return generation;
    }

    /**
     * @brief Updates the source image of the display, triggering a new frame event
     *
     * @param image The new pixman image to display.
     */
    void UpdateSourceImage(::pixman_image_t* image) override;

    /**
     * @brief Gets the current source image of the display.
     *
     * @return A pointer to the current pixman image.
     */
    PixmanImagePtr Image() { return SourceImage(); }

    /**
     * @brief Resets the producer as a guest device reset or snapshot load
     * does: frames published before it can no longer be read.
     */
    void ResetProducer() { frame_producer_->Reset(); }

    /**
     * @brief Runs `hook` inside the next ResizeKeepAspectRatio call, which a
     * scaled screenshot makes after it acquires its frame and before it reads it.
     */
    void RunDuringNextResize(std::function<void()> hook) ABSL_LOCKS_EXCLUDED(hook_mutex_);

    std::pair<int, int> ResizeKeepAspectRatio(int desired_width, int desired_height) override
            ABSL_LOCKS_EXCLUDED(hook_mutex_);

    std::vector<FakeEvDevEvent> evdevs;  ///< A list of simulated evdev events.
    /// Each simulated evdev report, ending with SYN_REPORT.
    std::vector<std::vector<FakeEvDevEvent>> evdev_reports;
    /// Whether the simulated guest input driver is active.
    bool input_active = true;
    /// Bump to simulate a reset or re-probe of the guest input driver.
    uint64_t input_active_generation = 0;

  private:
    absl::Mutex hook_mutex_;
    std::function<void()> resize_hook_ ABSL_GUARDED_BY(hook_mutex_);
};

/**
 * @brief A FakePixmanDisplay that actively generates images using a PixmanImageGenerator.
 *
 * This class combines a FakePixmanDisplay with a PixmanImageGenerator to create a display that
 * continuously updates its image.
 */
class ActiveFakePixmanDisplay : public FakePixmanDisplay,
                                public android::base::eventing::EventListener<PixmanImagePtr> {
  public:
    /**
     * @brief Destroys the ActiveFakePixmanDisplay, stopping the generator
     * thread and then the producer before any member is destroyed.
     */
    ~ActiveFakePixmanDisplay() override;

    /**
     * @brief Called when a new image is generated by the PixmanImageGenerator.
     *
     * @param image The newly generated pixman image.
     */
    void EventArrived(const PixmanImagePtr& image) override;

    /**
     * @brief Starts the image generation process.
     */
    void Start();

    /**
     * @brief Stops the image generation process.
     */
    void Stop();

    /**
     * @brief Resizes the generated images.
     *
     * @param w The new width.
     * @param h The new height.
     */
    void Resize(int w, int h);

    /**
     * @brief Waits for a specific number of frames to be generated with a timeout.
     *
     * @param n The number of frames to wait for.
     * @param timeout The maximum time to wait.
     * @return True if the desired number of frames were generated within the timeout, false
     * otherwise.
     */
    bool WaitForFramesWithTimeout(int n, absl::Duration timeout);

    /**
     * @brief Creates a shared_ptr of ActiveFakePixmanDisplay.
     *
     * @param id The ID of the display.
     * @param fps The frames per second at which to generate images.
     * @param w The width of the generated images.
     * @param h The height of the generated images.
     * @return An ActiveFakePixmanDisplay object.
     */
    static std::shared_ptr<ActiveFakePixmanDisplay> CreateShared(EventLoop* loop, int id, int fps,
                                                                 int w, int h, uint32_t dpi = 0,
                                                                 uint32_t flags = 0);

  private:
    /**
     * @brief Constructs an ActiveFakePixmanDisplay.
     *
     * @param id The ID of the display.
     * @param generator A unique pointer to the PixmanImageGenerator.
     */
    ActiveFakePixmanDisplay(EventLoop* loop, int id,
                            std::unique_ptr<PixmanImageGenerator> generator, uint32_t dpi = 0,
                            uint32_t flags = 0);

    std::unique_ptr<PixmanImageGenerator> generator_;
};
}  // namespace goldfish::display::test
