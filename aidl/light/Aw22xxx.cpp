/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Aw22xxx.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace aidl::android::hardware::light {
namespace {
constexpr char kRegPath[] = "/sys/class/leds/aw22xxx_led/reg";
constexpr char kEffectPath[] = "/sys/class/leds/aw22xxx_led/effect";

bool isFlashing(const HwLightState& state) {
    return state.flashMode == FlashMode::TIMED || state.flashMode == FlashMode::HARDWARE;
}

void appendSegment(std::vector<uint8_t>& payload, int32_t duration, uint8_t start, uint8_t end,
                   uint32_t color) {
    const auto ms = std::clamp(duration, 1, 65535);
    payload.insert(payload.end(), {static_cast<uint8_t>(ms >> 8), static_cast<uint8_t>(ms),
                                   start, end, static_cast<uint8_t>(color >> 16),
                                   static_cast<uint8_t>(color >> 8), static_cast<uint8_t>(color)});
}
}  // namespace

bool Aw22xxx::isActive(const HwLightState& state) {
    return (state.color & 0x00ffffff) != 0 && (!isFlashing(state) || state.flashOnMs > 0);
}

int Aw22xxx::writeRegister(uint8_t reg, uint8_t value) {
    char pair[16];
    const int length = snprintf(pair, sizeof(pair), "%x %x\n", reg, value);
    const ssize_t written = TEMP_FAILURE_RETRY(write(mRegFd.get(), pair, length));
    if (written != length) {
        const int error = written < 0 ? errno : EIO;
        LOG(ERROR) << "AW22xxx register " << static_cast<int>(reg) << ": " << strerror(error);
        return error;
    }
    return 0;
}

int Aw22xxx::initialize() {
    if (!::android::base::WriteStringToFile("0\n", kEffectPath)) {
        const int error = errno;
        PLOG(ERROR) << "Cannot clear AW22xxx effect";
        return error;
    }
    mRegFd.reset(TEMP_FAILURE_RETRY(open(kRegPath, O_WRONLY | O_CLOEXEC)));
    if (mRegFd.get() < 0) {
        const int error = errno;
        PLOG(ERROR) << "Cannot open AW22xxx registers";
        return error;
    }
    return apply(HwLightState{});
}

int Aw22xxx::apply(const HwLightState& state) {
    std::vector<uint8_t> payload;
    if (isActive(state)) {
        const bool blink = isFlashing(state) && state.flashOffMs > 0;
        payload = {0x00, 0x80, 0x01, 0x01, 0x07, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
                   0x01, 0x09, static_cast<uint8_t>(blink ? 2 : 1), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        const uint32_t color = state.color & 0x00ffffff;
        appendSegment(payload, blink ? state.flashOnMs : 1000, 0xcc, 0xcc, color);
        if (blink) appendSegment(payload, state.flashOffMs, 0x00, 0x00, color);
    }
    return applyPayload(std::move(payload));
}

int Aw22xxx::applyPayload(std::vector<uint8_t> payload) {
    if (mApplied && payload == mLastPayload) return 0;
    mApplied = false;

    int error;
    if ((error = writeRegister(0xff, 0x00))) return error;
    if (payload.empty()) {
        if ((error = writeRegister(0x0c, 0x00))) return error;
        if ((error = writeRegister(0x05, 0x01))) return error;
        if ((error = writeRegister(0x02, 0x00))) return error;
    } else {
        if ((error = writeRegister(0x02, 0x01))) return error;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        if ((error = writeRegister(0x0b, 0x01))) return error;
        if ((error = writeRegister(0x0c, 0x00))) return error;
        if ((error = writeRegister(0x05, 0x01))) return error;
        if ((error = writeRegister(0x04, 0x01))) return error;
        if ((error = writeRegister(0x09, 0x01))) return error;
        if ((error = writeRegister(0x04, 0x03))) return error;
        for (uint8_t byte : payload) {
            if ((error = writeRegister(0x06, byte))) return error;
            if ((error = writeRegister(0x04, 0x07))) return error;
        }
        if ((error = writeRegister(0x05, 0x80))) return error;
    }
    mLastPayload = std::move(payload);
    mApplied = true;
    return 0;
}

}  // namespace aidl::android::hardware::light
