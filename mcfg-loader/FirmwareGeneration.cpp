// SPDX-License-Identifier: Apache-2.0
#include "FirmwareGeneration.h"
#include "Log.h"
#include <atomic>

namespace mcfg {
std::string parseFirmwareGeneration(const Bytes& bytes) {
    if (bytes.empty() || bytes.size() > kFirmwareInfoLimit) return "";
    try {
        const auto info = parseJson(bytes);
        if (!info.isObject() || !info["Image_Build_IDs"].isObject()) return "";
        const auto& modem = info["Image_Build_IDs"]["modem"];
        if (!modem.isString()) return "";
        const auto value = modem.asString();
        if (value.empty() || value.size() > 256 ||
            value.find_first_of("\r\n\t") != std::string::npos ||
            value.find('\0') != std::string::npos) return "";
        return value;
    } catch (const std::exception&) {
        return "";
    }
}
const std::string& FirmwareGeneration::get() {
    if (!value_.empty()) return value_;
    try {
        value_ = parseFirmwareGeneration(readFile(path_, kFirmwareInfoLimit));
    } catch (const std::exception&) {}
    static std::atomic<bool> failureLogged{false};
    if (value_.empty() && !failureLogged.exchange(true))
        MCFG_ERROR("Modem build ID unavailable; firmware application deferred");
    return value_;
}
}  // namespace mcfg
