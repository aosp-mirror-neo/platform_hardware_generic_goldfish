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

#include "display_service.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "android/goldfish/fake_hardware_config.h"
#include "emulator_controller.grpc.pb.h"
#include "goldfish/async/libuv_event_loop.h"
#include "goldfish/async/threaded_event_loop.h"
#include "goldfish/avd/avd_info.h"
#include "goldfish/avd/fake_avd_universe.h"
#include "goldfish/display/cpu_read_pipeline.h"
#include "goldfish/display/test/fake_multi_display.h"
#include "goldfish/display/test/fake_pixman_display.h"
#include "goldfish/memory/shared_memory.h"
#include "test/grpc_service_test.h"

namespace android::emulation::control {

using ::goldfish::display::IMultiDisplay;
using ::goldfish::display::PixelFormat;
using ::goldfish::display::test::FakeMultiDisplay;
using ::goldfish::memory::SharedMemory;
using ::goldfish::parsing::ResizableDisplayConfig;
using ::goldfish::sensors::AndroidSensor;
using ::goldfish::sensors::PhysicalModel;
using ::grpc::ServerContext;
using ::grpc::Status;
using ::grpc::StatusCode;

using namespace std::chrono_literals;

namespace {
const std::vector<ResizableDisplayConfig> kNoResizableConfigs;
}  // namespace

class DisplayServiceTest : public GrcpServiceTest {
  protected:
    ::goldfish::avd::FakeAvdUniverse fake_avd_;

    void SetUp() override {
        mHw = android::goldfish::FakeHardwareConfig::GetHwConfig();
        mPhysicalModel = std::make_unique<PhysicalModel>(mHw, fake_avd_.virtual_clock());
        mLoop = ::goldfish::async::ThreadedEventLoop::Create(
                ::goldfish::async::LibuvEventLoop::Create());
        mQemuLoop = ::goldfish::async::ThreadedEventLoop::Create(
                ::goldfish::async::LibuvEventLoop::Create());
        mMultiDisplay = std::make_unique<FakeMultiDisplay>(mLoop.get());
        mDisplayService = std::make_unique<DisplayServiceImpl>(
                mMultiDisplay.get(), mPhysicalModel.get(), kNoResizableConfigs);
        auto createResult = mMultiDisplay->CreateDisplay(1, 100, 50, 320, 1);
        ASSERT_TRUE(createResult.ok());

        GrcpServiceTest::SetUp();
    }

    void TearDown() override {
        using ::goldfish::display::test::ActiveFakePixmanDisplay;
        if (mMultiDisplay) {
            for (auto display_weak : mMultiDisplay->Displays()) {
                if (auto display = display_weak.lock()) {
                    static_cast<ActiveFakePixmanDisplay*>(display.get())->Stop();
                }
            }
        }
        GrcpServiceTest::TearDown();
    }

    // This starts the generation of fake display images on the given display id.
    void startFrames(int display_id) {
        using ::goldfish::display::test::ActiveFakePixmanDisplay;

        auto screen = mMultiDisplay->GetDisplay(display_id);
        ASSERT_TRUE(screen.ok());

        auto display = screen->lock();
        ASSERT_TRUE(display);
        reinterpret_cast<ActiveFakePixmanDisplay*>(display.get())->Start();
    }

    static std::string GetTempSharedMemoryName(std::string_view prefix) {
        auto ts = absl::ToUnixMillis(absl::Now());
        std::stringstream ss;
        ss << std::this_thread::get_id();
#ifdef _WIN32
        auto pid = GetCurrentProcessId();
#else
        auto pid = getpid();
#endif
        std::error_code ec;
        auto path = (std::filesystem::temp_directory_path(ec) /
                     absl::StrFormat("%s_%d_%s_%ld", prefix, pid, ss.str(), ts))
                            .string();
        if (ec) {
            LOG(ERROR) << "Failed to get temp directory path: " << ec.message()
                       << " expect mayhem and failures.";
        }
        return path;
    }

    EmulatorController::Service* getService() override { return mDisplayService.get(); }

  protected:
    android::goldfish::HardwareConfig mHw;
    std::unique_ptr<PhysicalModel> mPhysicalModel;
    std::unique_ptr<::goldfish::async::EventLoop> mLoop;
    std::unique_ptr<::goldfish::async::EventLoop> mQemuLoop;
    std::unique_ptr<FakeMultiDisplay> mMultiDisplay;
    std::unique_ptr<DisplayServiceImpl> mDisplayService;

    // Maps rotation -> gravity vector.
    absl::flat_hash_map<Rotation_SkinRotation, std::array<float, 3>> mRotationMap = {
        {Rotation::PORTRAIT, {0.0, -1.0, 0.0}},
        {Rotation::LANDSCAPE, {-1.0, 0.0, 0.0}},
        {Rotation::REVERSE_PORTRAIT, {0.0, 1.0, 0.0}},
        {Rotation::REVERSE_LANDSCAPE, {1.0, 0.0, 0.0}}};
};

TEST_F(DisplayServiceTest, GetScreenshotRGBA8888) {
    // Get a screenshot
    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);

    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    // Check the format
    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
    EXPECT_EQ(reply.format().format(), ImageFormat::RGBA8888);
    EXPECT_EQ(reply.format().display(), 1);

    // Check if the screenshot has the correct data (at least one pixel)
    const uint32_t* pixelData = reinterpret_cast<const uint32_t*>(reply.mutable_image()->data());
    ASSERT_NE(pixelData[0] | pixelData[1] | pixelData[2] | pixelData[3], 0);
}

TEST_F(DisplayServiceTest, GetScreenshotRGB888) {
    // Get a screenshot
    Image reply;
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGB888);

    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    // Check the format
    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
    EXPECT_EQ(reply.format().format(), ImageFormat::RGB888);
    EXPECT_EQ(reply.format().display(), 1);

    // Check if the screenshot has the correct data (at least one pixel)
    const uint8_t* pixelData = reinterpret_cast<const uint8_t*>(reply.mutable_image()->data());
    ASSERT_NE(pixelData[0] | pixelData[1] | pixelData[2], 0);
}

TEST_F(DisplayServiceTest, GetScreenshotInvalidDisplay) {
    // Get a screenshot from an invalid display
    Image reply;
    ImageFormat request;
    request.set_display(99);
    request.set_format(ImageFormat::RGBA8888);

    auto context = getContextWithTimeout();
    Status status = mStub->getScreenshot(context.get(), request, &reply);
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::NOT_FOUND);
}

TEST_F(DisplayServiceTest, GetScreenshotScaling) {
    // Create a display
    auto create_result = mMultiDisplay->CreateDisplay(2, 400, 200, 320, 1);
    ASSERT_TRUE(create_result.ok());

    // Get a screenshot with scaling (re-enabled in Step 2.g)
    Image reply;
    ImageFormat request;
    request.set_display(2);
    request.set_format(ImageFormat::RGBA8888);
    request.set_width(200);
    request.set_height(100);

    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    // Check the format. We should get the scaled size back.
    EXPECT_EQ(reply.format().width(), 200);
    EXPECT_EQ(reply.format().height(), 100);
    EXPECT_EQ(reply.format().display(), 2);
}

TEST_F(DisplayServiceTest, GetDisplayConfigurations) {
    // Create a few displays
    auto createResult2 = mMultiDisplay->CreateDisplay(2, 200, 100, 320, 1);
    ASSERT_TRUE(createResult2.ok());

    // Get the display configurations
    Empty request;
    DisplayConfigurations reply;
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getDisplayConfigurations(context.get(), request, &reply));

    // Check the number of displays
    EXPECT_EQ(reply.displays_size(), 3);

    // Check the properties of the displays
    bool foundDefault = false;
    bool found1 = false;
    bool found2 = false;
    for (const auto& display : reply.displays()) {
        if (display.display() == 0) {
            foundDefault = true;
            EXPECT_EQ(display.width(), 640);
            EXPECT_EQ(display.height(), 480);
        } else if (display.display() == 1) {
            found1 = true;
            EXPECT_EQ(display.width(), 100);
            EXPECT_EQ(display.height(), 50);
        } else if (display.display() == 2) {
            found2 = true;
            EXPECT_EQ(display.width(), 200);
            EXPECT_EQ(display.height(), 100);
        }
    }
    ASSERT_TRUE(foundDefault);
    ASSERT_TRUE(found1);
    ASSERT_TRUE(found2);
}

TEST_F(DisplayServiceTest, GetScreenshotNoUpscaling) {
    // Get a screenshot with scaling that is larger than the display
    Image reply;
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.set_width(2000);
    request.set_height(1000);
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    // Check the format. We should never scale above the device size.
    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
    EXPECT_EQ(reply.format().display(), 1);
}

TEST_F(DisplayServiceTest, GetScreenshotNoSize) {
    // Get a screenshot with scaling that is larger than the display
    Image reply;
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.set_width(0);
    request.set_height(0);
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    // Check the format. We should never scale above the device size.
    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
    EXPECT_EQ(reply.format().display(), 1);

    ASSERT_NE(reply.image().size(), 0);
}

// b/448934377
TEST_F(DisplayServiceTest, GetScreenshotHasCorrectRotation) {
    // Get a screenshot
    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);

    for (const auto& [rotation, _] : mRotationMap) {
        auto [x, y, z] = mRotationMap[rotation];
        mPhysicalModel->SetGravity(x, y, z);

        auto context = getContextWithTimeout();
        ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

        switch (rotation) {
        case Rotation::PORTRAIT:
            EXPECT_EQ(reply.format().rotation().rotation(), Rotation::PORTRAIT);
            break;
        case Rotation::LANDSCAPE:
            EXPECT_EQ(reply.format().rotation().rotation(), Rotation::LANDSCAPE);
            break;
        case Rotation::REVERSE_PORTRAIT:
            EXPECT_EQ(reply.format().rotation().rotation(), Rotation::REVERSE_PORTRAIT);
            break;
        case Rotation::REVERSE_LANDSCAPE:
            EXPECT_EQ(reply.format().rotation().rotation(), Rotation::REVERSE_LANDSCAPE);
            break;
        default:
            FAIL() << "Unexpected rotation value";
        }
    }
}

// b/448934377
TEST_F(DisplayServiceTest, GetScreenshotConcurrent) {
    // Test to make sure we do not lock when having multiple threads.
    // Number of concurrent threads
    const int numThreads = 100;
    std::vector<std::thread> threads;
    std::vector<Status> statuses(numThreads);
    std::vector<Image> replies(numThreads);

    // Lambda function to be executed by each thread
    auto threadFunc = [&](int threadId) {
        // Get a screenshot
        ImageFormat request;
        request.set_display(1);
        request.set_format(ImageFormat::RGBA8888);

        auto context = getContextWithTimeout();
        statuses[threadId] = mStub->getScreenshot(context.get(), request, &replies[threadId]);
    };

    // Create and start threads
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(threadFunc, i);
    }

    // Join threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Check the results
    for (int i = 0; i < numThreads; ++i) {
        ASSERT_TRUE(statuses[i].ok())
                << "Thread " << i << " failed with error: " << statuses[i].error_message();
        EXPECT_EQ(replies[i].format().width(), 100);
        EXPECT_EQ(replies[i].format().height(), 50);
    }
}

TEST_F(DisplayServiceTest, StreamScreenshotSequenceIncreases) {
    // Get a screenshot with scaling that is larger than the display
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    startFrames(1);

    // Create a context with a timeout, so we don't hang forever if there are
    // issues.
    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;
    int seq = 0;
    int count = 0;
    int64_t timestampus = 0;

    // Check that we have montonically increasing sequence numbers and timestamps
    while (reader->Read(&image) && count < 5) {
        EXPECT_GE(image.seq(), seq);
        EXPECT_GE(image.timestampus(), timestampus);

        timestampus = image.timestampus();
        seq = image.seq();
        count++;
    }

    ASSERT_EQ(count, 5);
}

TEST_F(DisplayServiceTest, StreamScreenshotImmediatelyGetsAFrame) {
    // Make sure we immediately get a frame.
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);

    // Create a context with a timeout, so we don't hang forever if there are
    // issues.
    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;

    // We are not producing frames, but still should read one that is immediately
    // available
    EXPECT_TRUE(reader->Read(&image));
    EXPECT_GE(image.seq(), 0);
    EXPECT_GE(image.timestampus(), 1);
    EXPECT_EQ(image.format().width(), 100);
    EXPECT_EQ(image.format().height(), 50);
    EXPECT_EQ(image.format().display(), 1);
    ASSERT_NE(image.image().size(), 0);
}

TEST_F(DisplayServiceTest, StreamScreenshotSimulateEmbeddedInteraction) {
    // This simulates a resize operation as performed by the Android Studio
    // embedded emulator. When the user resizes the emulator window in Android
    // Studio, a sequence of `streamScreenshot` requests, each with potentially
    // different dimensions, is initiated in rapid succession.

    // The sequence of events typically unfolds as follows:

    // 1. **Initial Request (call1):** Android Studio makes a `streamScreenshot`
    // request (let's call it `call1`) to the emulator.
    //    This request specifies the desired width (w) and height (h) for the
    //    screenshot stream, based on the current emulator window size. `call1`
    //    establishes a continuous stream of screenshot data.

    // 2. **Resize Initiated:** The user begins resizing the emulator window.

    // 3. **New Request (call2):** As the window resize is in progress, Android
    // Studio immediately makes a second `streamScreenshot` request
    //    (let's call it `call2`). This new request often specifies new width (w1)
    //    and height (h2) values that differ from the original (w and h),
    //    reflecting the changed window dimensions. For example `w1 > w` or `h2 <
    //    h`.  This is because the resize is immediate and there is no delay.

    // 4. **Overlapping Requests:**  Crucially, at this point, both `call1` and
    // `call2` are active concurrently for a short duration.
    //    This overlap is because Android Studio doesn't wait for the first stream
    //    (`call1`) to finish before starting the second
    //    (`call2`).  This results in overlapping calls, each with a different
    //    desired window size.

    // 5. **Cancellation of Old Request (call1):** Very soon after initiating
    // `call2`, Android Studio will cancel the initial
    //    `streamScreenshot` request (`call1`). This is done because the data from
    //    `call1` is no longer needed, as the user has initiated a resize. Only
    //    the data from `call2` (the new window dimensions) is relevant.
    //
    // 6. **New Resize (call3, call4, ...):** If the user continues to resize the
    // window, this pattern repeats.
    //   `call3` is started with a new width/height, `call2` is cancelled, etc...

    // **Purpose of this test**

    // This test aims to simulate this real-world scenario. It verifies that the
    // emulator's `streamScreenshot` implementation can gracefully handle these
    // concurrent requests, overlapping streams, and cancellations without
    // deadlocks or data corruption. It also verifies that the client(android
    // studio) gets the correct frame.

    // It tests this by:
    // 1. Launching many threads, each creating a stream
    // 2. The threads simulate the cancellation of an old stream.
    // 3. The test will complete when all of the threads are completed.
    // 4. Asserts that the streams where working correctly, even when being
    // cancelled.

    using namespace std::chrono_literals;
    const int num_threads = 100;
    std::vector<std::thread> threads;
    // Sized up front: threads read earlier entries while later ones are set.
    std::vector<std::unique_ptr<grpc::ClientContext>> contexts(num_threads);
    startFrames(1);

    // Lambda function to be executed by each thread
    auto threadFunc = [&](int threadId) {
        int startingWidth = 100;
        int startingHeight = 50;

        ImageFormat request;
        request.set_display(1);
        request.set_format(ImageFormat::RGBA8888);
        request.set_width(startingWidth + threadId);
        request.set_height(startingHeight + threadId);

        std::unique_ptr<grpc::ClientReader<Image>> reader;

        int seq = 0;
        int64_t timestampus = 0;

        if (threadId > 0) {
            contexts[threadId - 1].get()->TryCancel();
        }

        auto context = contexts[threadId].get();
        reader = mStub->streamScreenshot(context, request);
        Image image;
        while (reader->Read(&image)) {
            EXPECT_GE(image.seq(), seq);
            EXPECT_GE(image.timestampus(), timestampus);
            timestampus = image.timestampus();
            seq = image.seq();
        }
    };

    // Create and start threads
    for (int i = 0; i < num_threads; ++i) {
        contexts[i] = getContextWithTimeout(500ms);
        threads.emplace_back(threadFunc, i);
        std::this_thread::sleep_for(5ms);
    }

    // Join threads
    for (auto& thread : threads) {
        thread.join();
    }
}

// b/448934377
TEST_F(DisplayServiceTest, StreamScreenshotRotationProducesAFrame) {
    // Get a screenshot with scaling that is larger than the display
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);

    // Create a context with a timeout, so we don't hang forever if there are
    // issues.
    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;
    int seq = 0;
    int64_t timestampus = 0;

    // Check that we have montonically increasing sequence numbers and timestamps
    auto status = reader->Read(&image);
    if (!status) {
        // Obtain failure detail from calling Finish
        ASSERT_GRPC_STATUS(reader->Finish());
    }

    EXPECT_GE(image.seq(), seq);
    EXPECT_GE(image.timestampus(), timestampus);
    EXPECT_EQ(image.format().format(), ImageFormat::RGBA8888);
    EXPECT_EQ(image.format().rotation().rotation(), Rotation::PORTRAIT);

    seq = image.seq();
    timestampus = image.timestampus();

    // We are now going to trigger a rotation event, which should result in a new
    // frame. Note that if this doesn't work we will timeout with our context
    // deadline and fail the test.
    auto [x, y, z] = mRotationMap[Rotation::LANDSCAPE];
    mPhysicalModel->SetGravity(x, y, z);

    status = reader->Read(&image);
    if (!status) {
        // Obtain failure detail from calling Finish
        ASSERT_GRPC_STATUS(reader->Finish());
    }

    EXPECT_GE(image.seq(), seq);
    EXPECT_GE(image.timestampus(), timestampus);
    EXPECT_EQ(image.format().format(), ImageFormat::RGBA8888);
    EXPECT_EQ(image.format().rotation().rotation(), Rotation::LANDSCAPE);
}

// b/448934377
TEST_F(DisplayServiceTest, StreamScreenshotHasCorrectRotation) {
    // Note: if StreamScreenshotRotationProducesAFrame fails, then this will
    // fail as well.
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);

    // Create a context with a timeout, so we don't hang forever if there are
    // issues.
    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;
    int seq = 0;
    int64_t timestampus = 0;

    // Check that we have montonically increasing sequence numbers and timestamps
    auto status = reader->Read(&image);
    if (!status) {
        // Obtain failure detail from calling Finish
        ASSERT_GRPC_STATUS(reader->Finish());
    }

    EXPECT_GE(image.seq(), seq);
    EXPECT_GE(image.timestampus(), timestampus);
    EXPECT_EQ(image.format().rotation().rotation(), Rotation::PORTRAIT);

    seq = image.seq();
    timestampus = image.timestampus();

    // Starts with a change: the display is idle, so only a rotation change
    // streams a new frame.
    for (const auto rotation : {Rotation::LANDSCAPE, Rotation::REVERSE_PORTRAIT,
                                Rotation::REVERSE_LANDSCAPE, Rotation::PORTRAIT}) {
        auto [x, y, z] = mRotationMap[rotation];
        mPhysicalModel->SetGravity(x, y, z);

        status = reader->Read(&image);
        if (!status) {
            // Obtain failure detail from calling Finish
            ASSERT_GRPC_STATUS(reader->Finish());
        }

        switch (rotation) {
        case Rotation::PORTRAIT:
            EXPECT_EQ(image.format().format(), ImageFormat::RGBA8888);
            EXPECT_EQ(image.format().rotation().rotation(), Rotation::PORTRAIT);
            break;
        case Rotation::LANDSCAPE:
            EXPECT_EQ(image.format().format(), ImageFormat::RGBA8888);
            EXPECT_EQ(image.format().rotation().rotation(), Rotation::LANDSCAPE);
            break;
        case Rotation::REVERSE_PORTRAIT:
            EXPECT_EQ(image.format().format(), ImageFormat::RGBA8888);
            EXPECT_EQ(image.format().rotation().rotation(), Rotation::REVERSE_PORTRAIT);
            break;
        case Rotation::REVERSE_LANDSCAPE:
            EXPECT_EQ(image.format().format(), ImageFormat::RGBA8888);
            EXPECT_EQ(image.format().rotation().rotation(), Rotation::REVERSE_LANDSCAPE);
            break;
        default:
            FAIL() << "Unexpected rotation value";
        }
    }
}

TEST_F(DisplayServiceTest, GetScreenshotRGBA8888Mmap) {
    // Create a shared memory region.
    std::string name = GetTempSharedMemoryName("test_mmap");
    size_t size = size_t{100} * 50 * 4;
    SharedMemory mem(name, size);
    ASSERT_TRUE(mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());

    // Get a screenshot via MMAP
    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.mutable_transport()->set_channel(ImageTransport::MMAP);
    request.mutable_transport()->set_handle(name);

    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    // Check the format
    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
    EXPECT_EQ(reply.format().format(), ImageFormat::RGBA8888);
    EXPECT_EQ(reply.format().display(), 1);

    // Image should be empty
    EXPECT_TRUE(reply.image().empty());

    // Shared memory should have data
    const uint32_t* pixelData = reinterpret_cast<const uint32_t*>(*mem);
    ASSERT_NE(pixelData[0] | pixelData[1] | pixelData[2] | pixelData[3], 0);
}

TEST_F(DisplayServiceTest, StreamScreenshotMmap) {
    // Create a shared memory region.
    std::string name = GetTempSharedMemoryName("test_mmap");

    size_t size = size_t{100} * 50 * 4;
    SharedMemory mem(name, size);
    ASSERT_TRUE(mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());
    startFrames(1);

    // Get a screenshot stream via MMAP
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.mutable_transport()->set_channel(ImageTransport::MMAP);
    request.mutable_transport()->set_handle(name);

    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;
    int count = 0;

    // Check that we have montonically increasing sequence numbers and timestamps
    while (reader->Read(&image) && count < 5) {
        // Image should be empty
        EXPECT_TRUE(image.image().empty());

        // Shared memory should have data
        const uint32_t* pixelData = reinterpret_cast<const uint32_t*>(*mem);
        EXPECT_NE(pixelData[0] | pixelData[1] | pixelData[2] | pixelData[3], 0);
        count++;
    }

    ASSERT_EQ(count, 5);
}

TEST_F(DisplayServiceTest, StreamScreenshotMmapResourceExhausted) {
    // Create a shared memory region that is too small for the screenshot but
    // large enough to map (e.g. 4KB).
    std::string name = GetTempSharedMemoryName("test_stream_mmap_small");
    size_t size = 4096;  // One page, but too small for 100*50*4 = 20000 bytes
    SharedMemory mem(name, size);
    ASSERT_TRUE(mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());
    startFrames(1);

    // Get a screenshot stream via MMAP
    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.mutable_transport()->set_channel(ImageTransport::MMAP);
    request.mutable_transport()->set_handle(name);

    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;

    // Should fail immediately
    EXPECT_FALSE(reader->Read(&image));
    auto status = reader->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::OUT_OF_RANGE);
}

TEST_F(DisplayServiceTest, GetScreenshotPNG) {
    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::PNG);

    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
    EXPECT_EQ(reply.format().format(), ImageFormat::PNG);
    // PNG data should start with 89 50 4E 47
    const std::string& data = reply.image();
    ASSERT_GE(data.size(), 4);
    EXPECT_EQ(static_cast<uint8_t>(data[0]), 0x89);
    EXPECT_EQ(static_cast<uint8_t>(data[1]), 'P');
    EXPECT_EQ(static_cast<uint8_t>(data[2]), 'N');
    EXPECT_EQ(static_cast<uint8_t>(data[3]), 'G');
}

TEST_F(DisplayServiceTest, GetScreenshotResponseHasCorrectFormat) {
    ImageFormat_ImgFormat formats[] = {ImageFormat::RGBA8888, ImageFormat::RGB888,
                                       ImageFormat::PNG};

    for (auto format : formats) {
        ImageFormat request;
        Image reply;
        request.set_display(1);
        request.set_format(format);

        auto context = getContextWithTimeout();
        ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));
        EXPECT_EQ(reply.format().format(), format) << "Failed for format " << format;
    }
}

TEST_F(DisplayServiceTest, GetScreenshotPNGMmap) {
    std::string name = GetTempSharedMemoryName("test_png_mmap");
    size_t size = size_t{100} * 50 * 4;
    SharedMemory mem(name, size);
    ASSERT_TRUE(mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());

    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::PNG);
    request.mutable_transport()->set_channel(ImageTransport::MMAP);
    request.mutable_transport()->set_handle(name);

    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    EXPECT_EQ(reply.format().format(), ImageFormat::PNG);
    EXPECT_TRUE(reply.image().empty());

    const uint8_t* pngData = reinterpret_cast<const uint8_t*>(*mem);
    EXPECT_EQ(pngData[0], 0x89);
    EXPECT_EQ(pngData[1], 'P');
    EXPECT_EQ(pngData[2], 'N');
    EXPECT_EQ(pngData[3], 'G');
}

TEST_F(DisplayServiceTest, GetScreenshotMmapInvalidHandle) {
    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.mutable_transport()->set_channel(ImageTransport::MMAP);
    request.mutable_transport()->set_handle("invalid_handle_path_that_does_not_exist");

    auto context = getContextWithTimeout();
    Status status = mStub->getScreenshot(context.get(), request, &reply);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(DisplayServiceTest, GetScreenshotMmapTooSmall) {
    std::string name = GetTempSharedMemoryName("test_mmap_too_small");
    size_t size = 10;
    SharedMemory mem(name, size);
    ASSERT_TRUE(mem.Create(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
                        .ok());

    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.mutable_transport()->set_channel(ImageTransport::MMAP);
    request.mutable_transport()->set_handle(name);

    auto context = getContextWithTimeout();
    Status status = mStub->getScreenshot(context.get(), request, &reply);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::OUT_OF_RANGE);
}

TEST_F(DisplayServiceTest, SetDisplayConfigurationsAddsAndUpdates) {
    // 1. ADD a new display (ID 2)
    DisplayConfigurations request;
    auto* disp = request.add_displays();
    disp->set_display(2);
    disp->set_width(1280);
    disp->set_height(720);
    disp->set_dpi(320);
    disp->set_flags(1);

    DisplayConfigurations reply;
    auto context1 = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->setDisplayConfigurations(context1.get(), request, &reply));

    // Verify it was added with exact width, height, dpi, flags
    bool found2 = false;
    for (const auto& d : reply.displays()) {
        if (d.display() == 2) {
            found2 = true;
            EXPECT_EQ(d.width(), 1280);
            EXPECT_EQ(d.height(), 720);
            EXPECT_EQ(d.dpi(), 320);
            EXPECT_EQ(d.flags(), 1);
        }
    }
    EXPECT_TRUE(found2);

    // 2. UPDATE the existing display (ID 2) with new resolution and DPI
    DisplayConfigurations request_update;
    auto* disp_update = request_update.add_displays();
    disp_update->set_display(2);
    disp_update->set_width(1920);
    disp_update->set_height(1080);
    disp_update->set_dpi(480);
    disp_update->set_flags(2);

    DisplayConfigurations reply_update;
    auto context2 = getContextWithTimeout();
    ASSERT_GRPC_STATUS(
            mStub->setDisplayConfigurations(context2.get(), request_update, &reply_update));

    // Verify it was successfully updated
    bool found2_updated = false;
    for (const auto& d : reply_update.displays()) {
        if (d.display() == 2) {
            found2_updated = true;
            EXPECT_EQ(d.width(), 1920);
            EXPECT_EQ(d.height(), 1080);
            EXPECT_EQ(d.dpi(), 480);
            EXPECT_EQ(d.flags(), 2);
        }
    }
    EXPECT_TRUE(found2_updated);
}

TEST_F(DisplayServiceTest, FiresInitialPostureOnConstruction) {
    // Reconfigure hardware for a foldable device
    mHw.hw_sensor_hinge = true;
    mHw.hw_sensor_hinge_count = 2;
    mHw.hw_sensor_hinge_type = 0;
    mHw.hw_sensor_hinge_sub_type = 1;
    mHw.hw_sensor_hinge_ranges = (char*)"0- 360, 0-180";
    mHw.hw_sensor_hinge_defaults = (char*)"180,90";
    mHw.hw_sensor_hinge_areas = (char*)"25-10, 50-10";
    mHw.hw_sensor_posture_list = (char*)"1, 2, 3, 4";
    mHw.hw_sensor_hinge_angles_posture_definitions =
            (char*)"0-30&0-15, 30-150 & 15-75, 150-330&75-165, 330-360&165-180";

    // Reset the old service first to prevent it from holding a dangling pointer to the old model
    mDisplayService.reset();

    mPhysicalModel = std::make_unique<PhysicalModel>(mHw, fake_avd_.virtual_clock());
    mPhysicalModel->SetCurrentTime(1000000000L);  // Initialize physical model state

    bool received_posture = false;
    Posture::PostureValue received_value = Posture::POSTURE_UNKNOWN;

    auto callback_id = ::goldfish::avd::GetAvd().GetGrpcNotificationChannel().AddCallback(
            [&](const ::android::emulation::control::Notification& notification) {
                if (notification.has_posture()) {
                    received_posture = true;
                    received_value = notification.posture().value();
                }
            });

    auto test_service = std::make_unique<DisplayServiceImpl>(
            mMultiDisplay.get(), mPhysicalModel.get(), kNoResizableConfigs);

    EXPECT_TRUE(received_posture);
    EXPECT_EQ(received_value,
              DisplayServiceImpl::ToProtoPosture(mPhysicalModel->GetFoldablePosture()));

    ::goldfish::avd::GetAvd().GetGrpcNotificationChannel().RemoveCallback(callback_id);
}

TEST(DisplayServiceTest_ToProtoPosture, ConvertsPostures) {
    using ::goldfish::sensors::FoldablePostures;
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(FoldablePostures::kClosed),
              Posture::POSTURE_CLOSED);
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(FoldablePostures::kHalfOpened),
              Posture::POSTURE_HALF_OPENED);
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(FoldablePostures::kOpened),
              Posture::POSTURE_OPENED);
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(FoldablePostures::kFlipped),
              Posture::POSTURE_FLIPPED);
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(FoldablePostures::kTent), Posture::POSTURE_TENT);
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(FoldablePostures::kPostureMax),
              Posture::POSTURE_MAX);
    EXPECT_EQ(DisplayServiceImpl::ToProtoPosture(static_cast<FoldablePostures>(99)),
              Posture::POSTURE_UNKNOWN);
}

TEST_F(DisplayServiceTest, StreamScreenshotEndsWithNotFoundOnDisplayRemoval) {
    ASSERT_TRUE(mMultiDisplay->CreateDisplay(2, 100, 50, 320, 1).ok());

    ImageFormat request;
    request.set_display(2);
    request.set_format(ImageFormat::RGBA8888);

    auto context = getContextWithTimeout(2s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;
    ASSERT_TRUE(reader->Read(&image));

    ASSERT_TRUE(mMultiDisplay->EraseDisplay(2).ok());
    while (reader->Read(&image)) {
    }
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::NOT_FOUND);
}

// B10: the display grew but has not produced a frame at its new size yet. A
// scaled read sized from the display would upscale the frame and fail.
TEST_F(DisplayServiceTest, GetScreenshotAfterDisplayGrowsBeforeNewFrame) {
    auto display = mMultiDisplay->GetDisplay(1)->lock();
    ASSERT_NE(display, nullptr);
    display->SetDimensions(200, 100);  // The source image stays 100x50.

    ImageFormat request;
    Image reply;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.set_width(150);
    request.set_height(75);
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 50);
}

// B10: a quarter-turned display returns the scaled size in rotated order.
TEST_F(DisplayServiceTest, GetScreenshotRotatedAndScaledSwapsDims) {
    auto created = mMultiDisplay->CreateDisplay(2, 400, 200, 320, 1);
    ASSERT_TRUE(created.ok());
    auto display = created->lock();
    ASSERT_NE(display, nullptr);
    display->SetRotation(::goldfish::display::ImageRotation::kRotation90);

    ImageFormat request;
    Image reply;
    request.set_display(2);
    request.set_format(ImageFormat::RGBA8888);
    request.set_width(200);
    request.set_height(100);
    auto context = getContextWithTimeout();
    ASSERT_GRPC_STATUS(mStub->getScreenshot(context.get(), request, &reply));

    EXPECT_EQ(reply.format().width(), 100);
    EXPECT_EQ(reply.format().height(), 200);
    EXPECT_EQ(reply.image().size(), 100U * 200U * 4U);
}

// B11: a device reset fences the frame a screenshot already acquired. The
// stream retries with the next frame instead of ending with UNAVAILABLE.
TEST_F(DisplayServiceTest, StreamSurvivesProducerReset) {
    using ::goldfish::display::test::ActiveFakePixmanDisplay;
    auto display = mMultiDisplay->GetDisplay<ActiveFakePixmanDisplay>(mMultiDisplay->GetDisplay(1));
    ASSERT_NE(display, nullptr);
    startFrames(1);

    ImageFormat request;
    request.set_display(1);
    request.set_format(ImageFormat::RGBA8888);
    request.set_width(100);  // Scaled reads call ResizeKeepAspectRatio.
    request.set_height(50);
    auto context = getContextWithTimeout(5s);
    std::unique_ptr<grpc::ClientReader<Image>> reader(
            mStub->streamScreenshot(context.get(), request));
    Image image;
    ASSERT_TRUE(reader->Read(&image));

    // Runs inside the display's own call, so the raw pointer is live.
    auto reset = std::make_shared<absl::Notification>();
    display->RunDuringNextResize([raw = display.get(), reset] {
        raw->ResetProducer();
        reset->Notify();
    });
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(reader->Read(&image)) << reader->Finish().error_message();
    }
    EXPECT_TRUE(reset->HasBeenNotified());
    context->TryCancel();
    while (reader->Read(&image)) {
    }
}

TEST_F(DisplayServiceTest, GetScreenshotPngReusesScratchPoolBufferAcrossCalls) {
    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
    constexpr size_t kRgbaBytes = size_t{kWidth} * kHeight * 4;
    constexpr int kIterations = 20;

    ASSERT_TRUE(mMultiDisplay->CreateDisplay(2, kWidth, kHeight, 320, 1).ok());

    ImageFormat request;
    request.set_display(2);
    request.set_format(ImageFormat::PNG);

    Image reply;
    for (int i = 0; i < kIterations; ++i) {
        ServerContext server_ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&server_ctx, &request, &reply));
        EXPECT_EQ(reply.format().width(), static_cast<int>(kWidth));
        EXPECT_EQ(reply.format().height(), static_cast<int>(kHeight));
        ASSERT_GE(reply.image().size(), 4U);
        EXPECT_EQ(static_cast<uint8_t>(reply.image()[0]), 0x89);
    }

    const auto stats = ::goldfish::display::internal::GetScratchPoolStatsForTesting();
    EXPECT_EQ(stats.on_loan, 0U);
    EXPECT_EQ(stats.free_count, 1U);
    EXPECT_GE(stats.free_bytes, kRgbaBytes);
}

TEST_F(DisplayServiceTest, GetScreenshotReusesReplyImageCapacityAcrossVariableSizeFrames) {
    Image reply;

    ImageFormat rgba_full;
    rgba_full.set_display(1);
    rgba_full.set_format(ImageFormat::RGBA8888);
    {
        ServerContext ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&ctx, &rgba_full, &reply));
    }
    ASSERT_EQ(reply.image().size(), 100U * 50U * 4U);
    const char* initial_data = reply.image().data();
    const size_t initial_capacity = reply.image().capacity();

    // Shrinking via scaled RGBA8888 (48x24 = 4,608 bytes) must reuse existing capacity.
    ImageFormat rgba_scaled;
    rgba_scaled.set_display(1);
    rgba_scaled.set_format(ImageFormat::RGBA8888);
    rgba_scaled.set_width(48);
    rgba_scaled.set_height(24);
    {
        ServerContext ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&ctx, &rgba_scaled, &reply));
    }
    EXPECT_EQ(reply.image().size(), 48U * 24U * 4U);
    EXPECT_EQ(reply.image().data(), initial_data);
    EXPECT_EQ(reply.image().capacity(), initial_capacity);

    // Variable-size PNG frames (unscaled and scaled) must reuse existing capacity.
    ImageFormat png_full;
    png_full.set_display(1);
    png_full.set_format(ImageFormat::PNG);
    {
        ServerContext ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&ctx, &png_full, &reply));
    }
    EXPECT_LT(reply.image().size(), initial_capacity);
    EXPECT_EQ(reply.image().data(), initial_data);
    EXPECT_EQ(reply.image().capacity(), initial_capacity);

    ImageFormat png_scaled = png_full;
    png_scaled.set_width(48);
    png_scaled.set_height(24);
    {
        ServerContext ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&ctx, &png_scaled, &reply));
    }
    EXPECT_LT(reply.image().size(), initial_capacity);
    EXPECT_EQ(reply.image().data(), initial_data);
    EXPECT_EQ(reply.image().capacity(), initial_capacity);

    // Growing back to 100x50 RGB888 (15,000 bytes) and RGBA8888 (20,000 bytes) reuses capacity.
    ImageFormat rgb_full;
    rgb_full.set_display(1);
    rgb_full.set_format(ImageFormat::RGB888);
    {
        ServerContext ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&ctx, &rgb_full, &reply));
    }
    EXPECT_EQ(reply.image().size(), 100U * 50U * 3U);
    EXPECT_EQ(reply.image().data(), initial_data);
    EXPECT_EQ(reply.image().capacity(), initial_capacity);

    {
        ServerContext ctx;
        ASSERT_GRPC_STATUS(mDisplayService->getScreenshot(&ctx, &rgba_full, &reply));
    }
    EXPECT_EQ(reply.image().size(), 100U * 50U * 4U);
    EXPECT_EQ(reply.image().data(), initial_data);
    EXPECT_EQ(reply.image().capacity(), initial_capacity);
}

}  // namespace android::emulation::control
