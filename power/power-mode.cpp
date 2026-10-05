/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aidl/android/hardware/power/Mode.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

using aidl::android::hardware::power::Mode;

namespace aidl::android::hardware::power::impl {

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
#ifdef LENOVO_DOUBLE_TAP_TO_WAKE
    if (type != Mode::DOUBLE_TAP_TO_WAKE && type != Mode::GAME) {
#else
    if (type != Mode::GAME) {
#endif
        return false;
    }
    *_aidl_return = true;
    return true;
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    if (type == Mode::GAME) {
        ::android::base::SetProperty("vendor.lenovo.game_mode", enabled ? "1" : "0");
        return true;
    }
#ifdef LENOVO_DOUBLE_TAP_TO_WAKE
    if (type != Mode::DOUBLE_TAP_TO_WAKE) {
        return false;
    }
    if (!::android::base::WriteStringToFile(enabled ? GESTURE_ENABLE : GESTURE_DISABLE, GESTURE_NODE, true)) {
        PLOG(ERROR) << "Cannot set NVT double tap to wake to " << enabled;
    }
    // QTI's bool extension result means handled, not I/O success.
    return true;
#else
    return false;
#endif
}

}  // namespace aidl::android::hardware::power::impl
