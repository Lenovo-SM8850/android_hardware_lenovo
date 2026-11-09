/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.lenovo"

#include "TouchPolicy.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <string>

#ifndef TOUCH_REPORT_THRESHOLD_TEXT
#define TOUCH_REPORT_THRESHOLD_TEXT 0
#endif
#ifndef TOUCH_GAME_EDGE_TEXT
#define TOUCH_GAME_EDGE_TEXT 0
#endif
#ifndef TOUCH_SUPPORT_PEN_CONNECTED
#define TOUCH_SUPPORT_PEN_CONNECTED 1
#endif
#ifndef TOUCH_SUPPORT_PEN_LINKING
#define TOUCH_SUPPORT_PEN_LINKING -1
#endif
#ifndef TOUCH_SUPPORT_PEN_SESSION_OFF
#define TOUCH_SUPPORT_PEN_SESSION_OFF 0
#endif

namespace {

constexpr const char* kReportThreshold = "/proc/report_threshold";
constexpr const char* kGameMode = "/proc/game_mode";
constexpr const char* kGameEdge = "/proc/game_edge";
constexpr const char* kSupportPen = "/proc/support_pen";
constexpr int32_t kPenDisconnected = 0;
constexpr int32_t kPenConnected = 1;
constexpr int32_t kPenLinking = 2;

bool parseValue(std::string value, bool textFormat, int32_t* out) {
    while (!value.empty() && value.back() == '\0') value.pop_back();
    value = android::base::Trim(value);
    if (value.empty()) return false;
    if (!textFormat) {
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str()) return false;
        *out = static_cast<int32_t>(parsed);
        return true;
    }
    // These nodes print "Name state <n>!"; take the last decimal run.
    size_t end = value.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(value[end - 1]))) --end;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(value[begin - 1]))) --begin;
    if (begin == end) return false;
    *out = std::stoi(value.substr(begin, end - begin));
    return true;
}

bool writeNode(const char* path, int32_t value, bool textFormat) {
    if (!android::base::WriteStringToFile(std::to_string(value), path, true)) {
        PLOG(ERROR) << "Cannot write " << path;
        return false;
    }
    std::string content;
    if (!android::base::ReadFileToString(path, &content)) {
        PLOG(ERROR) << "Cannot read " << path;
        return false;
    }
    int32_t readback = 0;
    if (!parseValue(content, textFormat, &readback)) {
        LOG(ERROR) << "Unexpected value from " << path << ": " << content;
        return false;
    }
    if (readback != value) {
        LOG(ERROR) << "Wrote " << value << " to " << path << " but read " << readback;
        return false;
    }
    return true;
}

}  // namespace

namespace aidl::vendor::lenovo::hardware::touchpolicy {

TouchPolicy::TouchPolicy() {
    // Start from the idle state; the controller re-sends a session when one is live.
    endSession();
}

ndk::ScopedAStatus TouchPolicy::setSession(const TouchSessionConfig& config) {
    std::lock_guard<std::mutex> lock(mLock);
    // Keep the requested state even when a write fails, so endSession can restore.
    mSessionActive = true;
    mTapSensitivity = config.tapSensitivity;
    mMoveSensitivity = config.moveSensitivity;
    mEdgeRejection = config.edgeRejection;
    const bool tapOk =
            writeNode(kReportThreshold, mTapSensitivity, TOUCH_REPORT_THRESHOLD_TEXT);
    const bool moveOk = writeNode(kGameMode, mMoveSensitivity, false);
    const bool edgeOk = writeNode(kGameEdge, mEdgeRejection, TOUCH_GAME_EDGE_TEXT);
    const bool penOk = applySupportPenLocked();
    if (!penOk) return ndk::ScopedAStatus::fromServiceSpecificError(EIO);
    if (!tapOk || !moveOk || !edgeOk) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchPolicy::endSession() {
    std::lock_guard<std::mutex> lock(mLock);
    // Always write the idle values, including after a HAL restart.
    // Try every node, then keep the session active unless all restores succeeded.
    const bool tapOk = writeNode(kReportThreshold, 0, TOUCH_REPORT_THRESHOLD_TEXT);
    const bool moveOk = writeNode(kGameMode, 0, false);
    const bool edgeOk = writeNode(kGameEdge, 0, TOUCH_GAME_EDGE_TEXT);
    const bool wasActive = mSessionActive;
    mSessionActive = false;
    const bool penOk = applySupportPenLocked();
    if (!tapOk || !moveOk || !edgeOk || !penOk) {
        mSessionActive = wasActive;
        if (!penOk) return ndk::ScopedAStatus::fromServiceSpecificError(EIO);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    mSessionActive = false;
    mTapSensitivity = 0;
    mMoveSensitivity = 0;
    mEdgeRejection = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchPolicy::setSessionPenInput(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    const bool previous = mSessionPenInput;
    mSessionPenInput = enabled;
    if (!applySupportPenLocked()) {
        mSessionPenInput = previous;
        return ndk::ScopedAStatus::fromServiceSpecificError(EIO);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchPolicy::setPenState(int32_t state) {
    if (state < kPenDisconnected || state > kPenLinking) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(mLock);
    const int32_t previous = mPenState;
    mPenState = state;
    if (!applySupportPenLocked()) {
        mPenState = previous;
        return ndk::ScopedAStatus::fromServiceSpecificError(EIO);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchPolicy::getState(TouchPolicyState* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    _aidl_return->sessionActive = mSessionActive;
    _aidl_return->tapSensitivity = mTapSensitivity;
    _aidl_return->moveSensitivity = mMoveSensitivity;
    _aidl_return->edgeRejection = mEdgeRejection;
    _aidl_return->sessionPenInput = mSessionPenInput;
    _aidl_return->penState = mPenState;
    _aidl_return->supportPen = mSupportPen;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchPolicy::registerCallback(
        const std::shared_ptr<ITouchPolicyCallback>& callback) {
    std::lock_guard<std::mutex> lock(mLock);
    mCallback = callback;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchPolicy::unregisterCallback(
        const std::shared_ptr<ITouchPolicyCallback>& callback) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mCallback != nullptr && callback != nullptr &&
        mCallback->asBinder() == callback->asBinder()) {
        mCallback.reset();
    }
    return ndk::ScopedAStatus::ok();
}

bool TouchPolicy::applySupportPenLocked() {
    const int32_t value = resolveSupportPenLocked();
    if (value < 0) return true;
    const bool changed = value != mSupportPen;
    if (!writeNode(kSupportPen, value, false)) return false;
    mSupportPen = value;
    if (changed && mCallback != nullptr) {
        const ndk::ScopedAStatus status = mCallback->onPenStateChanged(mPenState);
        if (!status.isOk()) LOG(WARNING) << "Cannot notify the pen state";
    }
    return true;
}

int32_t TouchPolicy::resolveSupportPenLocked() const {
    // A disabled session pen input wins over the connection state.
    if (mSessionActive && !mSessionPenInput) return TOUCH_SUPPORT_PEN_SESSION_OFF;
    switch (mPenState) {
        case kPenConnected:
            return TOUCH_SUPPORT_PEN_CONNECTED;
        case kPenLinking:
            // Some controllers have no separate linking state.
            return TOUCH_SUPPORT_PEN_LINKING;
        default:
            return 0;
    }
}

}  // namespace aidl::vendor::lenovo::hardware::touchpolicy
