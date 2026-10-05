/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Lights.h"
#include "RingEffectEncoder.h"

namespace aidl::android::hardware::light {
namespace {
constexpr std::array<LightType, 3> kTypes = {LightType::BATTERY, LightType::NOTIFICATIONS,
                                           LightType::ATTENTION};
constexpr std::array<size_t, 3> kPriority = {1, 2, 0};
}  // namespace

int Lights::initialize() {
    std::lock_guard lock(mMutex);
    return mBackend.initialize();
}

int Lights::turnOff() {
    std::lock_guard lock(mMutex);
    mStopping = true;
    return mBackend.apply(HwLightState{});
}

ndk::ScopedAStatus Lights::getLights(std::vector<HwLight>* lights) {
    lights->clear();
    for (LightType type : kTypes) {
        HwLight light;
        light.id = static_cast<int32_t>(type);
        light.type = type;
        light.ordinal = 0;
        lights->push_back(light);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Lights::setLightState(int32_t id, const HwLightState& state) {
    std::lock_guard lock(mMutex);
    size_t index = 0;
    while (index < kTypes.size() && id != static_cast<int32_t>(kTypes[index])) ++index;
    if (index == kTypes.size()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (mStopping) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    mStates[index] = state;
    return applyLocked();
}

ndk::ScopedAStatus Lights::setEffect(const ::aidl::vendor::lenovo::hardware::lightring::Effect& effect) {
    if (effect.type < 0 || effect.type > 3 || effect.colors.empty() || effect.colors.size() > 9 ||
        (effect.type < 2 && effect.colors.size() != 1) || effect.speed < 0 || effect.speed > 2 ||
        effect.brightness < 0 || effect.brightness > 255) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    for (int32_t color : effect.colors) {
        if (color < 0 || color > 0xffffff)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mMutex);
    if (mStopping) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    mEffect = effect;
    return applyLocked();
}

ndk::ScopedAStatus Lights::clearEffect() {
    std::lock_guard lock(mMutex);
    if (mStopping) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    mEffect.reset();
    return applyLocked();
}

ndk::ScopedAStatus Lights::applyLocked() {
    int error = 0;
    bool higherActive = false;
    for (size_t candidate : kPriority) {
        if (Aw22xxx::isActive(mStates[candidate])) {
            error = mBackend.apply(mStates[candidate]);
            higherActive = true;
            break;
        }
    }
    if (!higherActive) {
        error = mEffect ? mBackend.applyPayload(::lenovo::lightring::encodeEffect(
                mEffect->type, mEffect->colors, mEffect->speed, mEffect->brightness)) :
                mBackend.apply(HwLightState{});
    }
    if (error) return ndk::ScopedAStatus::fromServiceSpecificError(error);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::light
