// SPDX-License-Identifier: Apache-2.0
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef MCFG_RESTART_TEST
#define LOG_TAG "LenovoMcfgRestart"
#include <log/log.h>
#endif

namespace mcfg::restart {
namespace fs = std::filesystem;
using Sleep = std::function<void(std::chrono::milliseconds)>;
using Write = std::function<void(const fs::path&, const std::string&)>;

std::string read(const fs::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!input || !std::getline(input, value) || value.size() > 256)
        throw std::runtime_error("Cannot read remoteproc attribute: " + path.string());
    return value;
}

fs::path findModem(const fs::path& sysfs) {
    const auto parent = fs::canonical(sysfs / "devices/platform/soc/4080000.remoteproc-mss/remoteproc");
    fs::path modem;
    for (const auto& entry : fs::directory_iterator(sysfs / "class/remoteproc")) {
        const auto name = entry.path().filename().string();
        if (name.rfind("remoteproc", 0) != 0 || name.size() == 10 ||
            name.find_first_not_of("0123456789", 10) != std::string::npos)
            continue;
        if (read(entry.path() / "firmware") != "modem.mdt") continue;
        if (!modem.empty()) throw std::runtime_error("Multiple modem remoteprocs");
        const auto device = fs::canonical(entry.path());
        const auto relative = device.lexically_relative(parent);
        if (relative.empty() || relative == "." || *relative.begin() == "..")
            throw std::runtime_error("Modem remoteproc is outside the MSS device");
        modem = device;
    }
    if (modem.empty()) throw std::runtime_error("Modem remoteproc not found");
    return modem;
}

void writeState(const fs::path& path, const std::string& value) {
    // Never follow a replacement symlink or create a missing state node.
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("Cannot open modem state for writing");
    const auto count = write(fd, value.data(), value.size());
    const int closed = close(fd);
    if (count != static_cast<ssize_t>(value.size()) || closed != 0)
        throw std::runtime_error("Cannot write modem state");
}

void waitState(const fs::path& state, const char* wanted, const Sleep& sleep) {
    for (int poll = 0; poll <= 300; ++poll) {
        if (read(state) == wanted) return;
        if (poll != 300) sleep(std::chrono::milliseconds(100));
    }
    throw std::runtime_error(std::string("Timed out waiting for modem ") + wanted);
}

void run(const fs::path& sysfs, const Write& write, const Sleep& sleep) {
    const auto state = findModem(sysfs) / "state";
    if (read(state) != "running") throw std::runtime_error("Modem is not running");
    write(state, "stop");
    waitState(state, "offline", sleep);
    sleep(std::chrono::seconds(1));
    write(state, "start");
    waitState(state, "running", sleep);
}
}  // namespace mcfg::restart

#ifndef MCFG_RESTART_TEST
int main(int argc, char**) {
    if (argc != 1) {
        ALOGE("No arguments are supported");
        return 1;
    }
    try {
        ALOGI("Requesting MSS modem stop/start");
        mcfg::restart::run("/sys", mcfg::restart::writeState,
                           [](auto duration) { std::this_thread::sleep_for(duration); });
        ALOGI("Modem is running after restart");
        return 0;
    } catch (const std::exception& error) {
        ALOGE("Modem restart failed: %s", error.what());
        return 1;
    }
}
#endif
