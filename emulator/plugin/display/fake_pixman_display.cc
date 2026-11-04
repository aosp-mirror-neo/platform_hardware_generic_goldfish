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
// limitations under the License.

#include "goldfish/display/test/fake_pixman_display.h"

#include <vector>

#include "absl/log/log.h"

#include "standard-headers/linux/input-event-codes.h"

namespace goldfish::display::test {

FakePixmanDisplay::FakePixmanDisplay(EventLoop* loop, int id, ::pixman_image_t* image, uint32_t dpi,
                                     uint32_t flags)
        : PixmanDisplay(loop, id, image, dpi, flags) {
    if (SourceImage()) {
        const auto d = GetDimensions();
        UpdateSurface(0, 0, static_cast<int>(d.width), static_cast<int>(d.height));
    }
}

FakePixmanDisplay::FakePixmanDisplay(EventLoop* loop, int id, const PixmanImagePtr& image,
                                     uint32_t dpi, uint32_t flags)
        : PixmanDisplay(loop, id, image, dpi, flags) {
    if (SourceImage()) {
        const auto d = GetDimensions();
        UpdateSurface(0, 0, static_cast<int>(d.width), static_cast<int>(d.height));
    }
}

FakePixmanDisplay::~FakePixmanDisplay() {
    ShutdownProducer();
}

void FakePixmanDisplay::UpdateSourceImage(::pixman_image_t* image) {
    PixmanDisplay::UpdateSourceImage(image);
    const Dimensions dims = GetDimensions();
    UpdateSurface(0, 0, static_cast<int>(dims.width), static_cast<int>(dims.height));
}

void FakePixmanDisplay::RunDuringNextResize(std::function<void()> hook) {
    const absl::MutexLock lock(&hook_mutex_);
    resize_hook_ = std::move(hook);
}

std::pair<int, int> FakePixmanDisplay::ResizeKeepAspectRatio(int desired_width,
                                                             int desired_height) {
    std::function<void()> hook;
    {
        const absl::MutexLock lock(&hook_mutex_);
        hook = std::exchange(resize_hook_, nullptr);
    }
    if (hook) hook();
    return PixmanDisplay::ResizeKeepAspectRatio(desired_width, desired_height);
}

void FakePixmanDisplay::SendEvDevEvent(uint16_t type, uint16_t code, uint32_t value) {
    evdevs.push_back({type, code, value});
}

void FakePixmanDisplay::SendEvDevReport(EvDevEvents events) {
    FakePixmanDisplay::WriteEvDevReportOnLoop(events);
}

void FakePixmanDisplay::WriteEvDevReportOnLoop(absl::Span<const EvDevEvent> events) {
    std::vector<FakeEvDevEvent>& report = evdev_reports.emplace_back();
    for (const EvDevEvent& ev : events) {
        report.push_back({ev.type, ev.code, ev.value});
    }
    report.push_back({EV_SYN, SYN_REPORT, 0});
    evdevs.insert(evdevs.end(), report.begin(), report.end());
}

ActiveFakePixmanDisplay::~ActiveFakePixmanDisplay() {
    generator_->Stop();  // Its thread calls UpdateSourceImage.
    ShutdownProducer();
}

void ActiveFakePixmanDisplay::EventArrived(const PixmanImagePtr& image) {
    // Update the FakePixmanDisplay with the new image
    VLOG(1) << "Image arrived";
    UpdateSourceImage(image.get());
}

void ActiveFakePixmanDisplay::Start() {
    generator_->Start();
}

void ActiveFakePixmanDisplay::Stop() {
    generator_->Stop();
}

void ActiveFakePixmanDisplay::Resize(int w, int h) {
    generator_->Resize(w, h);
}

bool ActiveFakePixmanDisplay::WaitForFramesWithTimeout(int n, absl::Duration timeout) {
    return generator_->WaitForFramesWithTimeout(n, timeout);
}

std::shared_ptr<ActiveFakePixmanDisplay> ActiveFakePixmanDisplay::CreateShared(
        EventLoop* loop, int id, int fps, int w, int h, uint32_t dpi, uint32_t flags) {
    auto generator = std::make_unique<PixmanImageGenerator>(fps, w, h);
    auto fake = std::shared_ptr<ActiveFakePixmanDisplay>(
            new ActiveFakePixmanDisplay(loop, id, std::move(generator), dpi, flags));
    fake->generator_->AddListener(fake);
    return fake;
}

ActiveFakePixmanDisplay::ActiveFakePixmanDisplay(EventLoop* loop, int id,
                                                 std::unique_ptr<PixmanImageGenerator> generator,
                                                 uint32_t dpi, uint32_t flags)
        : FakePixmanDisplay(loop, id, generator->GenerateImage(Color::kBlue), dpi, flags)
        , generator_(std::move(generator)) {}

}  // namespace goldfish::display::test
