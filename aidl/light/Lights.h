/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "Aw22xxx.h"

#include <aidl/android/hardware/light/BnLights.h>
#include <array>
#include <mutex>
#include <optional>
#include <aidl/vendor/lenovo/hardware/lightring/BnLightRing.h>
#include <android/binder_ibinder_platform.h>
#include <string_view>

namespace aidl::android::hardware::light {

class Lights : public BnLights {
  public:
    int initialize();
    int turnOff();
    ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) override;
    ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) override;
    ndk::ScopedAStatus setEffect(const ::aidl::vendor::lenovo::hardware::lightring::Effect& effect);
    ndk::ScopedAStatus clearEffect();

  private:
    Aw22xxx mBackend;
    std::array<HwLightState, 3> mStates{};
    std::mutex mMutex;
    bool mStopping = false;
    std::optional<::aidl::vendor::lenovo::hardware::lightring::Effect> mEffect;
    ndk::ScopedAStatus applyLocked();
};

class LightRing : public ::aidl::vendor::lenovo::hardware::lightring::BnLightRing {
  public:
    explicit LightRing(std::shared_ptr<Lights> lights) : mLights(std::move(lights)) {}
    ndk::ScopedAStatus setEffect(const ::aidl::vendor::lenovo::hardware::lightring::Effect& effect) override {
        if (!isClient()) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        return mLights->setEffect(effect);
    }
    ndk::ScopedAStatus clearEffect() override {
        if (!isClient()) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        return mLights->clearEffect();
    }
  private:
    static bool isClient() {
        const char* sid = AIBinder_getCallingSid();
        if (!sid) return false;
        const std::string_view context(sid);
        const auto userEnd = context.find(':');
        if (userEnd == std::string_view::npos) return false;
        const auto roleEnd = context.find(':', userEnd + 1);
        if (roleEnd == std::string_view::npos) return false;
        const auto typeEnd = context.find(':', roleEnd + 1);
        return typeEnd != std::string_view::npos &&
                context.substr(roleEnd + 1, typeEnd - roleEnd - 1) == "legionhalo_app";
    }
    std::shared_ptr<Lights> mLights;
};

}  // namespace aidl::android::hardware::light
