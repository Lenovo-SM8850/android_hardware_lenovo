// SPDX-License-Identifier: Apache-2.0
#include "FirmwareGeneration.h"
#include "Transaction.h"
#include <grp.h>
#include <unistd.h>
#include <iostream>

namespace {
// Device acceptance fixture: exercise production journaling while the transport
// fails before its first write. No DIAG connection or EFS mutation is possible.
class BeforeWriteFailure final : public mcfg::EfsTransport {
  public:
    int attemptedWrites = 0;
    std::optional<mcfg::Bytes> read(const std::string&) override { return mcfg::Bytes{0}; }
    void checkParent(const std::string&) override {
        throw std::runtime_error("injected pre-write parent failure; no EFS mutation");
    }
    void write(const std::string& path, const mcfg::Bytes&,
               const std::function<void()>& checkpoint) override {
        if (checkpoint) checkpoint();
        ++attemptedWrites;
        checkParent(path);
    }
    void remove(const std::string&, const std::function<void()>&) override {
        throw std::runtime_error("fixture must never remove EFS files");
    }
};
}
int main(int argc, char** argv) {
    using namespace mcfg;
    try {
        require(argc == 2 && std::string(argv[1]).size() >= 5 &&
                        std::string(argv[1]).size() <= 6 &&
                        std::string(argv[1]).find_first_not_of("0123456789") == std::string::npos,
                "usage: lenovo_mcfg_firmware_failure_test home-plmn");
        require(getuid() == 0, "root-only device acceptance fixture");
        const gid_t groups[] = {1001, 1000};
        require(setgroups(2, groups) == 0 && setgid(1001) == 0 && setuid(1001) == 0,
                "fixture radio credentials");
        ProfileStore store("/vendor/etc/mcfg-loader", "/data/vendor/mcfg-loader");
        FirmwareGeneration firmware;
        require(!firmware.get().empty(), "current modem build unavailable");
        BeforeWriteFailure efs;
        Transaction txn(store, efs, "/data/vendor/mcfg-loader", [] { return "fixture"; },
                        [] { return true; }, {}, {}, [&] { return firmware.get(); });
        const std::vector<std::string> home{argv[1]};
        require(txn.status().enabled && txn.status().state == "committed" &&
                        txn.status().appliedFirmware == firmware.get(),
                "fixture requires a current committed ON journal");
        const auto profile = txn.status().profile;
        txn.enable(true, home);
        txn.evaluate(home);
        try {
            txn.apply(home, std::string(32, 'f'));
        } catch (const std::exception&) {}
        require(efs.attemptedWrites == 1 && txn.status().state == "firmware_repair_blocked" &&
                        txn.status().profile == profile && txn.status().token.empty() &&
                        txn.status().appliedFirmware == firmware.get(),
                "pre-write failure did not preserve the current key/stamp as blocked");
        std::cout << "PASS: production Transaction pre-write failure persisted firmware_repair_blocked; "
                     "DIAG sessions=0 EFS mutations=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
