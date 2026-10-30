// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ProfileStore.h"

namespace mcfg {
constexpr size_t kFirmwareInfoLimit = 8 * 1024;
// Empty means unavailable. Never substitute a property or a different format.
std::string parseFirmwareGeneration(const Bytes& bytes);
class FirmwareGeneration {
  public:
    explicit FirmwareGeneration(std::string path =
            "/vendor/firmware_mnt/verinfo/ver_info.txt") : path_(std::move(path)) {}
    const std::string& get();

  private:
    std::string path_, value_;
};
}  // namespace mcfg
