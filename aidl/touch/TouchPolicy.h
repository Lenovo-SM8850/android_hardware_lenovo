/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lenovo/hardware/touchpolicy/BnTouchPolicy.h>
#include <aidl/vendor/lenovo/hardware/touchpolicy/ITouchPolicyCallback.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace aidl::vendor::lenovo::hardware::touchpolicy {

class TouchPolicy : public BnTouchPolicy {
  public:
    TouchPolicy();
    ndk::ScopedAStatus setSession(const TouchSessionConfig& config) override;
    ndk::ScopedAStatus endSession() override;
    ndk::ScopedAStatus setSessionPenInput(bool enabled) override;
    ndk::ScopedAStatus setPenState(int32_t state) override;
    ndk::ScopedAStatus getState(TouchPolicyState* _aidl_return) override;
    ndk::ScopedAStatus registerCallback(
            const std::shared_ptr<ITouchPolicyCallback>& callback) override;
    ndk::ScopedAStatus unregisterCallback(
            const std::shared_ptr<ITouchPolicyCallback>& callback) override;

  private:
    bool applySupportPenLocked();
    int32_t resolveSupportPenLocked() const;

    std::mutex mLock;
    bool mSessionActive = false;
    bool mSessionPenInput = true;
    int32_t mPenState = 0;
    int32_t mTapSensitivity = 0;
    int32_t mMoveSensitivity = 0;
    int32_t mEdgeRejection = 0;
    int32_t mSupportPen = 0;
    std::shared_ptr<ITouchPolicyCallback> mCallback;
};

}  // namespace aidl::vendor::lenovo::hardware::touchpolicy
