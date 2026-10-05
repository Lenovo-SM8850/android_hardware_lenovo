/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "Aw22xxx.h"

#include <aidl/android/hardware/light/BnLights.h>
#include <array>
#include <mutex>

namespace aidl::android::hardware::light {

class Lights : public BnLights {
  public:
    int initialize();
    int turnOff();
    ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) override;
    ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) override;

  private:
    Aw22xxx mBackend;
    std::array<HwLightState, 3> mStates{};
    std::mutex mMutex;
    bool mStopping = false;
    ndk::ScopedAStatus applyLocked();
};

}  // namespace aidl::android::hardware::light
