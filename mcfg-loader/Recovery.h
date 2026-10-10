// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <ostream>
#include <sys/types.h>
#include "Transaction.h"

namespace mcfg {
int runRecoveryCli(const std::string& mode, uid_t caller, ProfileStore& store,
                   const std::string& directory,
                   const std::function<std::unique_ptr<EfsTransport>(bool)>& createEfs,
                   const std::function<bool()>& writePolicy, std::ostream& output,
                   std::ostream& errors);
}  // namespace mcfg
