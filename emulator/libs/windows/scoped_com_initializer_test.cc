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

#include "android/base/scoped_com_initializer.h"

#include <thread>
#include <utility>

#include "gtest/gtest.h"

namespace android::base {
namespace {

TEST(ScopedComInitializerTest, InitializesAndUninitializesOnThread) {
    std::thread([]() {
        {
            ScopedComInitializer com;
            EXPECT_EQ(com.hr(), S_OK);
            EXPECT_TRUE(com.succeeded());
            EXPECT_TRUE(com.ok());
            EXPECT_TRUE(static_cast<bool>(com));
        }
        // After com goes out of scope, COM is uninitialized on this thread,
        // so initializing with a different apartment model succeeds with S_OK.
        ScopedComInitializer mta(COINIT_MULTITHREADED);
        EXPECT_EQ(mta.hr(), S_OK);
        EXPECT_TRUE(mta.succeeded());
        EXPECT_TRUE(mta.ok());
    }).join();
}

TEST(ScopedComInitializerTest, NestedSameApartmentModeReturnsSFalseAndBalancesRefCount) {
    std::thread([]() {
        {
            ScopedComInitializer outer(COINIT_APARTMENTTHREADED);
            EXPECT_EQ(outer.hr(), S_OK);
            {
                ScopedComInitializer inner(COINIT_APARTMENTTHREADED);
                EXPECT_EQ(inner.hr(), S_FALSE);
                EXPECT_TRUE(inner.succeeded());
                EXPECT_TRUE(inner.ok());
            }
        }
        ScopedComInitializer mta(COINIT_MULTITHREADED);
        EXPECT_EQ(mta.hr(), S_OK);
    }).join();
}

TEST(ScopedComInitializerTest, ChangedModeRemainsUsableWithoutUninitializingOuterCom) {
    std::thread([]() {
        {
            ScopedComInitializer outer(COINIT_MULTITHREADED);
            EXPECT_EQ(outer.hr(), S_OK);
            {
                ScopedComInitializer inner(COINIT_APARTMENTTHREADED);
                EXPECT_EQ(inner.hr(), RPC_E_CHANGED_MODE);
                EXPECT_FALSE(inner.succeeded());
                EXPECT_TRUE(inner.ok());
                EXPECT_TRUE(static_cast<bool>(inner));

                inner.reset();
                EXPECT_FALSE(inner.ok());
                EXPECT_FALSE(static_cast<bool>(inner));
            }
            // Verify inner did not call CoUninitialize(): a nested MTA call
            // must return S_FALSE (already initialized), not S_OK.
            ScopedComInitializer nested_mta(COINIT_MULTITHREADED);
            EXPECT_EQ(nested_mta.hr(), S_FALSE);
        }
        // Once outer goes out of scope, COM is fully uninitialized.
        ScopedComInitializer sta(COINIT_APARTMENTTHREADED);
        EXPECT_EQ(sta.hr(), S_OK);
    }).join();
}

TEST(ScopedComInitializerTest, MoveAndResetMaintainBalancedUninitialization) {
    std::thread([]() {
        {
            ScopedComInitializer first(COINIT_APARTMENTTHREADED);
            EXPECT_TRUE(first.succeeded());

            ScopedComInitializer second(std::move(first));
            EXPECT_FALSE(first.succeeded());
            EXPECT_FALSE(first.ok());
            EXPECT_TRUE(second.succeeded());
            EXPECT_TRUE(second.ok());

            second.reset();
            EXPECT_FALSE(second.succeeded());
            EXPECT_FALSE(second.ok());
        }
        ScopedComInitializer mta(COINIT_MULTITHREADED);
        EXPECT_EQ(mta.hr(), S_OK);
    }).join();
}

}  // namespace
}  // namespace android::base
