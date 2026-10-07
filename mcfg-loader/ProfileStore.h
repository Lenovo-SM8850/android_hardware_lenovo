// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <json/json.h>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mcfg {
using Bytes = std::vector<uint8_t>;
using Snapshot = std::map<std::string, std::optional<Bytes>>;
inline constexpr char kControl[] = "/nv/item_files/mcfg/mcfg_autoselect_by_uim";
std::string sha256(const Bytes& data);
Bytes readFile(const std::string& path, size_t limit);
Json::Value parseJson(const Bytes& data);
Bytes jsonBytes(const Json::Value& value);
std::string hex(const Bytes& data);
Bytes unhex(const std::string& text);
void require(bool condition, const char* error);
void durableWrite(const std::string& path, const Json::Value& value);
Json::Value encodeSnapshot(const Snapshot& snapshot);
Snapshot decodeSnapshot(const Json::Value& value, const std::set<std::string>& allow);

struct Selection {
    std::string state;
    std::string profile;
};
class ProfileStore {
  public:
    explicit ProfileStore(std::string root, std::string userDirectory = "");
    Selection select(const std::vector<std::string>& homePlmns) const;
    Snapshot load(const std::string& id) const;
    const std::set<std::string>& paths() const { return allow_; }
    Json::Value profiles() const;
    Json::Value metadata(const std::string& id) const;
    std::string importMbn(const Bytes& mbn, const std::string& filename,
                          const std::string& homePlmn);
    void deleteImported(const std::string& id);

  private:
    std::string root_;
    Snapshot validate(const Json::Value& profile, const std::string& id, unsigned count) const;
    void saveImports(const Json::Value& profiles);
    std::string importsPath_;
    Json::Value index_, imports_{Json::arrayValue};
    std::set<std::string> allow_;
    Bytes pack_;
};
}  // namespace mcfg
