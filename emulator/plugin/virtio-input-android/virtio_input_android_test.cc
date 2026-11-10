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

#include "goldfish/input/virtio_input_android.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <thread>
#include <utility>
#include <vector>

#include "absl/types/span.h"

#include "standard-headers/linux/input-event-codes.h"
#include "standard-headers/linux/input.h"
#include "virtio_input_android_config.h"

struct VirtIOInputHID {
    int placeholder = 0;
};

namespace goldfish::input {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

TEST(VirtioInputAndroidEventsTest, ScopedCppListenerReceivesEventsUntilReset) {
    VirtIOInputHID dev_a{};
    VirtIOInputHID dev_b{};
    std::vector<std::pair<VirtIOInputHID*, uint64_t>> observed;

    auto handle = ::android::base::eventing::MakeScopedCallback(
            VirtioInputActiveEvents(), [&observed](const VirtioInputActiveEvent& event) {
                observed.emplace_back(event.vhid, event.active_generation);
            });

    virtio_input_android_notify_active_changed(&dev_a, 1);
    virtio_input_android_notify_active_changed(&dev_b, 7);
    EXPECT_THAT(observed, ElementsAre(Pair(&dev_a, 1), Pair(&dev_b, 7)));

    handle.reset();
    virtio_input_android_notify_active_changed(&dev_a, 2);
    EXPECT_THAT(observed, ElementsAre(Pair(&dev_a, 1), Pair(&dev_b, 7)));
}

TEST(VirtioInputAndroidEventsTest, CListenerRegistrationRemovalAndNullSafety) {
    EXPECT_EQ(virtio_input_android_add_active_listener(nullptr, nullptr), 0U);
    virtio_input_android_remove_active_listener(0U);

    VirtIOInputHID dev{};
    std::vector<std::pair<VirtIOInputHID*, uint64_t>> observed;
    const size_t id = virtio_input_android_add_active_listener(
            +[](VirtIOInputHID* vhid, uint64_t generation, void* opaque) {
                auto* out = static_cast<std::vector<std::pair<VirtIOInputHID*, uint64_t>>*>(opaque);
                out->emplace_back(vhid, generation);
            },
            &observed);
    ASSERT_NE(id, 0U);

    virtio_input_android_notify_active_changed(nullptr, 99);
    EXPECT_THAT(observed, IsEmpty());

    virtio_input_android_notify_active_changed(&dev, 3);
    EXPECT_THAT(observed, ElementsAre(Pair(&dev, 3)));

    virtio_input_android_remove_active_listener(id);
    virtio_input_android_notify_active_changed(&dev, 4);
    EXPECT_THAT(observed, ElementsAre(Pair(&dev, 3)));
}

TEST(VirtioInputAndroidEventsTest, ConcurrentNotifiesAndUnsubscriptionsAreRaceFree) {
    VirtIOInputHID dev{};
    constexpr int kSubscribers = 4;
    constexpr uint64_t kNotifyIterations = 500;
    std::latch start_gate(kSubscribers + 1);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> delivered{0};

    std::vector<std::thread> subscribers;
    subscribers.reserve(kSubscribers);
    for (int i = 0; i < kSubscribers; ++i) {
        subscribers.emplace_back([&start_gate, &done, &delivered, &dev]() {
            start_gate.arrive_and_wait();
            while (!done.load(std::memory_order_acquire)) {
                auto sub = ::android::base::eventing::MakeScopedCallback(
                        VirtioInputActiveEvents(),
                        [&delivered, &dev](const VirtioInputActiveEvent& event) {
                            if (event.vhid == &dev) {
                                delivered.fetch_add(1, std::memory_order_relaxed);
                            }
                        });
                std::this_thread::yield();
            }
        });
    }

    std::thread notifier([&start_gate, &done, &delivered, &dev]() {
        start_gate.arrive_and_wait();
        for (uint64_t g = 1;
             g <= kNotifyIterations || delivered.load(std::memory_order_relaxed) == 0; ++g) {
            virtio_input_android_notify_active_changed(&dev, g);
        }
        done.store(true, std::memory_order_release);
    });

    notifier.join();
    for (auto& t : subscribers) {
        t.join();
    }
    EXPECT_GT(delivered.load(std::memory_order_relaxed), 0U);
}

TEST(VirtioInputAndroidEventsTest, CallbackCanRegisterAndRemovePeerListenerWithoutDeadlock) {
    VirtIOInputHID dev{};
    std::vector<uint64_t> peer_observed;
    VirtioInputActiveEventSource::ScopedCallbackHandle peer_handle;

    auto primary_handle = ::android::base::eventing::MakeScopedCallback(
            VirtioInputActiveEvents(),
            [&peer_handle, &peer_observed, &dev](const VirtioInputActiveEvent& event) {
                if (event.vhid != &dev) {
                    return;
                }
                if (event.active_generation == 10) {
                    peer_handle = ::android::base::eventing::MakeScopedCallback(
                            VirtioInputActiveEvents(),
                            [&peer_observed, &dev](const VirtioInputActiveEvent& peer_event) {
                                if (peer_event.vhid == &dev) {
                                    peer_observed.push_back(peer_event.active_generation);
                                }
                            });
                } else if (event.active_generation == 12) {
                    peer_handle.reset();
                }
            });

    virtio_input_android_notify_active_changed(&dev, 10);
    EXPECT_THAT(peer_observed, IsEmpty());

    virtio_input_android_notify_active_changed(&dev, 11);
    EXPECT_THAT(peer_observed, ElementsAre(11));

    virtio_input_android_notify_active_changed(&dev, 12);
    EXPECT_THAT(peer_observed, ElementsAre(11));
}

TEST(VirtioInputAndroidConfigTest, AdvertisesStylusRubberKeysAndSignedDegreeRanges) {
    size_t key_count = 0;
    const unsigned short* keys = virtio_input_android_ev_key_codes(&key_count);
    EXPECT_THAT(absl::MakeConstSpan(keys, key_count),
                ::testing::UnorderedElementsAre(BTN_TOOL_RUBBER, BTN_STYLUS));

    size_t abs_count = 0;
    const unsigned short* abs_codes = virtio_input_android_ev_abs_codes(&abs_count);
    EXPECT_THAT(absl::MakeConstSpan(abs_codes, abs_count),
                ::testing::AllOf(::testing::Contains(ABS_MT_ORIENTATION),
                                 ::testing::Contains(ABS_TILT_X), ::testing::Contains(ABS_TILT_Y),
                                 ::testing::Contains(ABS_MT_TOOL_TYPE),
                                 ::testing::Contains(ABS_MT_PRESSURE)));

    int32_t min_val = 0;
    int32_t max_val = 0;
    ASSERT_TRUE(virtio_input_android_get_abs_range(ABS_MT_ORIENTATION, &min_val, &max_val));
    EXPECT_EQ(min_val, -90);
    EXPECT_EQ(max_val, 90);

    ASSERT_TRUE(virtio_input_android_get_abs_range(ABS_TILT_X, &min_val, &max_val));
    EXPECT_EQ(min_val, -90);
    EXPECT_EQ(max_val, 90);

    ASSERT_TRUE(virtio_input_android_get_abs_range(ABS_TILT_Y, &min_val, &max_val));
    EXPECT_EQ(min_val, -90);
    EXPECT_EQ(max_val, 90);

    ASSERT_TRUE(virtio_input_android_get_abs_range(ABS_MT_TOOL_TYPE, &min_val, &max_val));
    EXPECT_EQ(min_val, 0);
    EXPECT_EQ(max_val, MT_TOOL_MAX);

    ASSERT_TRUE(virtio_input_android_get_abs_range(ABS_MT_PRESSURE, &min_val, &max_val));
    EXPECT_EQ(min_val, 0);
    EXPECT_EQ(max_val, 0x400);

    ASSERT_TRUE(virtio_input_android_get_abs_range(ABS_MT_SLOT, &min_val, &max_val));
    EXPECT_EQ(min_val, 0);
    EXPECT_EQ(max_val, 10);

    EXPECT_FALSE(virtio_input_android_get_abs_range(ABS_RZ, &min_val, &max_val));

    constexpr uint8_t kExpectedSwBitmap =
            (1U << SW_LID) | (1U << SW_HEADPHONE_INSERT) | (1U << SW_MICROPHONE_INSERT);
    EXPECT_EQ(virtio_input_android_ev_sw_bitmap(), kExpectedSwBitmap);
}

}  // namespace
}  // namespace goldfish::input
