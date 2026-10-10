// SPDX-License-Identifier: Apache-2.0
#include "Recovery.h"
#include "Log.h"

namespace mcfg {
int runRecoveryCli(const std::string& mode, uid_t caller, ProfileStore& store,
                   const std::string& directory,
                   const std::function<std::unique_ptr<EfsTransport>(bool)>& createEfs,
                   const std::function<bool()>& writePolicy, std::ostream& output,
                   std::ostream& errors) {
    try {
        require(caller == 0 || caller == 1001, "recovery CLI caller must be root or radio");
        require(mode == "--dump-readonly", "unsupported recovery mode");
        // Acquire the service's lock before opening a second DIAG session.
        auto lock = std::make_shared<TransactionLock>(directory);
        auto efs = createEfs(true);
        require(efs != nullptr, "recovery EFS transport unavailable");
        Transaction transaction(store, *efs, directory, [] { return ""; }, writePolicy,
                                [] { return false; }, lock);
        return transaction.dumpReadOnly(output) ? 0 : 1;
    } catch (const std::exception& error) {
        MCFG_ERROR("CLI %s failed: %s", mode.c_str(), error.what());
        errors << "mcfg-loader: " << error.what() << '\n';
        output << "DUMP_FAIL\n";
        return 1;
    }
}
}  // namespace mcfg
