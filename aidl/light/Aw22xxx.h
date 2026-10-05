/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/light/HwLightState.h>
#include <android-base/unique_fd.h>
#include <cstdint>
#include <vector>

namespace aidl::android::hardware::light {

class Aw22xxx {
  public:
    int initialize();
    int apply(const HwLightState& state);
    int applyPayload(std::vector<uint8_t> payload);
    static bool isActive(const HwLightState& state);

  private:
    int writeRegister(uint8_t reg, uint8_t value);
    ::android::base::unique_fd mRegFd;
    std::vector<uint8_t> mLastPayload;
    bool mApplied = false;
};

}  // namespace aidl::android::hardware::light
