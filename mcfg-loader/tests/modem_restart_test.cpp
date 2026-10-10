// SPDX-License-Identifier: Apache-2.0
#define MCFG_RESTART_TEST
#include "../ModemRestart.cpp"
#include <iostream>
#include <vector>

namespace {
int checks;
void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Restart check " + std::to_string(checks));
}
template <typename F> void rejects(F action) {
    try {
        action();
    } catch (const std::exception&) {
        check(true);
        return;
    }
    check(false);
}
void put(const std::filesystem::path& path, const std::string& value) {
    std::ofstream(path) << value << '\n';
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2);
        namespace fs = std::filesystem;
        using namespace mcfg::restart;
        const fs::path root = argv[1];
        const auto classes = root / "class/remoteproc";
        const auto mss = root / "devices/platform/soc/4080000.remoteproc-mss/remoteproc";
        const auto modem = mss / "remoteproc17";
        const auto adsp = root / "devices/platform/soc/adsp/remoteproc/remoteproc3";
        fs::create_directories(classes);
        fs::create_directories(modem);
        fs::create_directories(adsp);
        put(modem / "firmware", "modem.mdt");
        put(modem / "state", "running");
        put(adsp / "firmware", "adsp.mdt");
        put(adsp / "state", "running");
        fs::create_directory_symlink(modem, classes / "remoteproc17");
        fs::create_directory_symlink(adsp, classes / "remoteproc3");
        check(findModem(root) == modem);
        std::vector<std::string> writes;
        long waited = 0;
        const Sleep sleep = [&](auto duration) { waited += duration.count(); };
        const Write writer = [&](const auto& path, const auto& command) {
            check(path == modem / "state");
            writes.push_back(command);
            put(path, command == "stop" ? "offline" : "running");
        };
        run(root, writer, sleep);
        check(writes == std::vector<std::string>({"stop", "start"}));
        check(waited == 1000);
        check(read(adsp / "state") == "running");
        put(modem / "state", "offline");
        rejects([&] { run(root, writer, sleep); });
        check(writes.size() == 2);
        put(modem / "state", "running");
        waited = 0;
        rejects([&] { run(root, [](const auto&, const auto&) {}, sleep); });
        check(waited == 30000);
        waited = 0;
        writes.clear();
        rejects([&] {
            run(root, [&](const auto& path, const auto& command) {
                writes.push_back(command);
                put(path, "offline");
            }, sleep);
        });
        check(writes == std::vector<std::string>({"stop", "start"}));
        check(waited == 31000);
        put(adsp / "firmware", "modem.mdt");
        rejects([&] { findModem(root); });
        put(modem / "firmware", "other.mdt");
        rejects([&] { findModem(root); });
        put(adsp / "firmware", "adsp.mdt");
        rejects([&] { findModem(root); });
        put(modem / "firmware", "modem.mdt");
        fs::rename(modem, mss / "remoteproc9");
        fs::remove(classes / "remoteproc17");
        fs::create_directory_symlink(mss / "remoteproc9", classes / "remoteproc9");
        check(findModem(root) == mss / "remoteproc9");
        const auto state = mss / "remoteproc9/state";
        fs::remove(state);
        fs::create_symlink(adsp / "state", state);
        rejects([&] { writeState(state, "stop"); });
        check(read(adsp / "state") == "running");
        fs::remove(state);
        rejects([&] { run(root, writer, sleep); });
        std::cout << "PASS: " << checks << " modem restart path/state checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
