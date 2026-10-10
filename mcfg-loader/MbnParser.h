// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ProfileStore.h"

namespace mcfg {
// Software MBN file-item overlays only. Numeric NV, RF hardware configuration,
// unknown items and paths outside the immutable device allowlist are excluded.
Snapshot parseMbn(const Bytes& mbn, const std::set<std::string>& allow);
}  // namespace mcfg
