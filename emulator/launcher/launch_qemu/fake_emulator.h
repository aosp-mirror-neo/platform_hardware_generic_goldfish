#include <memory>

#include "android/base/bazel_info.h"
#include "android/cmdline_definitions.h"
#include "android/goldfish/constants.h"
#include "emulator_config.h"
#include "mock_avd.h"

namespace android::goldfish {

class FakeEmulator {
  public:
    FakeEmulator(EmulatorPorts ports, AndroidOptions opts)
            : mPorts(std::move(ports))
            , mOpts(std::move(opts))
            , mEmulatorPaths{.launcher_directory = fs::path(android::base::Bazel::RunfilesPath(
                                                                    "goldfish+/emulator/launcher"))
                                                           .make_preferred(),
                                                          }
            , mMockAvd(std::make_unique<MockAvd>()) {
        ON_CALL(*mMockAvd, GetSystemImagePaths())
                .WillByDefault(testing::ReturnRef(mSystemImagePaths));
        ON_CALL(*mMockAvd, ApiLevel()).WillByDefault(testing::Return(kVirtioReadyApiLevel));
        using S = android_studio::EmulatorFeatureFlagState;
        mFeatureFlags = FeatureFlags::CreateForTesting({
            S::MINIGBM,
            S::GUEST_ANGLE,
            S::DYNAMIC_PARTITION,
            S::MAC80211HWSIM_USERSPACE_MANAGED,
            S::VULKAN_SNAPSHOTS,
            S::VULKAN_VIRTUAL_QUEUE,
            S::VULKAN_USE_DEDICATED_AHB_MEMORY_TYPE,
            S::NETSIMCLIUI,
            S::NETSIMWEBUI,
        });

        mEmulatorConfig = std::make_unique<EmulatorConfig>(
                mPorts, mChardevEndpoints, mMetricsConfig, mUserPaths, mEmulatorPaths, *mMockAvd,
                mOpts, mFeatureFlags);
    }

    explicit FakeEmulator(AndroidOptions opts) : FakeEmulator(EmulatorPorts{}, std::move(opts)) {}

    FakeEmulator() : FakeEmulator(AndroidOptions{}) {}

    MockAvd& mock_avd() { return *mMockAvd; }

    EmulatorPaths& emulator_paths() { return mEmulatorPaths; }

    UserPaths& user_paths() { return mUserPaths; }

    SystemImagePaths& system_image_paths() { return mSystemImagePaths; }

    FeatureFlags& feature_flags() { return mFeatureFlags; }

    const EmulatorConfig& config() { return *mEmulatorConfig; }

  private:
    EmulatorPorts mPorts;
    AndroidOptions mOpts;
    ChardevEndpoints mChardevEndpoints;
    MetricsConfig mMetricsConfig;
    FeatureFlags mFeatureFlags;

    EmulatorPaths mEmulatorPaths;
    UserPaths mUserPaths;
    SystemImagePaths mSystemImagePaths;
    std::unique_ptr<MockAvd> mMockAvd;

    std::unique_ptr<EmulatorConfig> mEmulatorConfig;
};

}  // namespace android::goldfish
