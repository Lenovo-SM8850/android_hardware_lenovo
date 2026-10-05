/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.lenovo"

#include "HighTouchPollingRate.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

namespace {
constexpr const char* kHighReportRate = HIGH_REPORT_RATE_NODE;

bool readEnabled(bool* enabled) {
    std::string value;
    if (!android::base::ReadFileToString(kHighReportRate, &value)) {
        PLOG(ERROR) << "Cannot read " << kHighReportRate;
        return false;
    }
    // NVT returns text; THP caches a NUL-terminated number that may be empty.
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    value = android::base::Trim(value);
    if (value == "High Report Rate state 0!" || value == HIGH_REPORT_RATE_DISABLE) {
        *enabled = false;
    } else if (value == "High Report Rate state 1!" || value == HIGH_REPORT_RATE_ENABLE) {
        *enabled = true;
    } else {
        LOG(ERROR) << "Unexpected HighReportRate state: " << value;
        return false;
    }
    return true;
}
}  // namespace

namespace aidl::vendor::lineage::touch {

ndk::ScopedAStatus HighTouchPollingRate::getEnabled(bool* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    *_aidl_return = false;
    if (!readEnabled(_aidl_return)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HighTouchPollingRate::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    // The device selects the values accepted by its controller.
    if (!android::base::WriteStringToFile(enabled ? HIGH_REPORT_RATE_ENABLE : HIGH_REPORT_RATE_DISABLE, kHighReportRate, true)) {
        PLOG(ERROR) << "Cannot write " << kHighReportRate;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    bool current = false;
    if (!readEnabled(&current) || current != enabled) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::vendor::lineage::touch
