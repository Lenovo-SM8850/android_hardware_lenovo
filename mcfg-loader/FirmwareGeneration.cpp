// SPDX-License-Identifier: Apache-2.0
#include "FirmwareGeneration.h"
#include "Log.h"
#include <atomic>

namespace mcfg {
std::string parseFirmwareGeneration(const Bytes& bytes) {
    if (bytes.empty() || bytes.size() > kFirmwareInfoLimit) return "";
    try {
        const auto info = parseJson(bytes);
        // The modem build ID can stay the same across firmware releases; the meta build time
        // changes with every release.
        if (!info.isObject() || !info["Metabuild_Info"].isObject()) return "";
        const auto& stamp = info["Metabuild_Info"]["Time_Stamp"];
        if (!stamp.isString()) return "";
        const auto value = stamp.asString();
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
        MCFG_ERROR("Firmware build time unavailable; firmware application deferred");
    return value_;
}
}  // namespace mcfg
