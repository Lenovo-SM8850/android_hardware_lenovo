// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdexcept>
#include <string>
#include <chrono>
#include <map>
#include <mutex>
#include "Log.h"

namespace mcfg {
inline void logEfsError(const std::string& message) {
    static std::mutex mutex;
    static std::map<std::string, std::chrono::steady_clock::time_point> logged;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    const auto previous = logged.find(message);
    if (previous != logged.end() && now - previous->second < std::chrono::seconds(30)) return;
    if (logged.size() >= 64) logged.clear();
    logged[message] = now;
    MCFG_ERROR("%s", message.c_str());
}
[[noreturn]] inline void efsFailure(const std::string& path, const char* step,
                                  const std::exception& error) {
    std::string message = "EFS step=" + std::string(step) + " path=" + path + " " + error.what();
    // Transport and validation failures may have no modem errno reply.
    if (message.find("errno=") == std::string::npos) message += " errno=unknown";
    logEfsError(message);
    throw std::runtime_error(message);
}

template <typename F>
decltype(auto) efsStep(const std::string& path, const char* step, F&& operation) {
    try {
        return operation();
    } catch (const std::exception& error) {
        efsFailure(path, step, error);
    }
}
}  // namespace mcfg
