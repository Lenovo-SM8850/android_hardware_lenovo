// SPDX-License-Identifier: Apache-2.0
#include "ProfileStore.h"
#include <fcntl.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include "MbnParser.h"

namespace mcfg {
void require(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}
std::string hex(const Bytes& data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto byte : data) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}
Bytes unhex(const std::string& text) {
    require(text.size() % 2 == 0 && text.size() <= 131072, "hex bounds");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("noncanonical hex");
    };
    Bytes out;
    for (size_t i = 0; i < text.size(); i += 2)
        out.push_back((nibble(text[i]) << 4) | nibble(text[i + 1]));
    return out;
}
std::string sha256(const Bytes& data) {
    Bytes digest(SHA256_DIGEST_LENGTH);
    SHA256(data.data(), data.size(), digest.data());
    return hex(digest);
}
Bytes readFile(const std::string& path, size_t limit) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(fd >= 0, "file open");
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || uint64_t(st.st_size) > limit) {
        close(fd);
        throw std::runtime_error("file bounds/type");
    }
    Bytes out(st.st_size);
    size_t offset = 0;
    while (offset < out.size()) {
        ssize_t n = read(fd, out.data() + offset, out.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("file short read");
        }
        offset += n;
    }
    close(fd);
    return out;
}
Json::Value parseJson(const Bytes& data) {
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["allowComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string error;
    const char* begin = reinterpret_cast<const char*>(data.data());
    require(!data.empty() && reader->parse(begin, begin + data.size(), &value, &error),
            "invalid JSON");
    return value;
}
Bytes jsonBytes(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string out = Json::writeString(builder, value);
    return Bytes(out.begin(), out.end());
}
void durableWrite(const std::string& path, const Json::Value& value) {
    const auto bytes = jsonBytes(value);
    const std::string temp = path + ".new";
    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(fd >= 0, "journal open");
    size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("journal write");
        }
        offset += n;
    }
    int synced = fsync(fd);
    int closed = close(fd);
    require(synced == 0 && closed == 0, "journal fsync");
    require(rename(temp.c_str(), path.c_str()) == 0, "journal rename");
    const auto parent = path.substr(0, path.find_last_of('/'));
    fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(fd >= 0, "journal directory");
    synced = fsync(fd);
    close(fd);
    require(synced == 0, "journal directory fsync");
}
Json::Value encodeSnapshot(const Snapshot& snapshot) {
    Json::Value out(Json::objectValue);
    for (const auto& [path, bytes] : snapshot)
        out[path] = bytes ? Json::Value(hex(*bytes)) : Json::Value();
    return out;
}
Snapshot decodeSnapshot(const Json::Value& value, const std::set<std::string>& allow) {
    require(value.isObject() && value.size() <= 86, "snapshot bounds");
    Snapshot out;
    for (const auto& path : value.getMemberNames()) {
        require(allow.count(path) || path == kControl, "snapshot path");
        require(value[path].isNull() || value[path].isString(), "snapshot value");
        out[path] = value[path].isNull() ? std::optional<Bytes>{} : unhex(value[path].asString());
    }
    return out;
}
ProfileStore::ProfileStore(std::string root, std::string userDirectory) : root_(std::move(root)) {
    index_ = parseJson(readFile(root_ + "/profiles.json", 2 * 1024 * 1024));
    require(index_["schema_version"].asInt() == 1 && index_["profiles"].isArray() &&
                    index_["profiles"].size() == 430,
            "index schema/count");
    pack_ = readFile(root_ + "/profiles.pack", 8 * 1024 * 1024);
    const Bytes magic{'M', 'C', 'F', 'G', 'P', 'K', '1', 0};
    require(pack_.size() >= magic.size() && std::equal(magic.begin(), magic.end(), pack_.begin()) &&
                    pack_.size() == index_["pack_size"].asUInt64() &&
                    sha256(pack_) == index_["pack_sha256"].asString(),
            "pack integrity");
    for (const auto& path : index_["allowlisted_paths"]) {
        std::string p = path.asString();
        require(!p.empty() && p[0] == '/' && p.find("..") == std::string::npos &&
                        p.find("//") == std::string::npos && p.find('\0') == std::string::npos &&
                        p != kControl && p.back() != '/' && allow_.insert(p).second,
                "allowlist path");
    }
    require(allow_.size() <= 85 && !allow_.empty(), "allowlist bounds");
    std::set<std::string> ids, archives;
    uint64_t end = magic.size();
    for (const auto& p : index_["profiles"]) {
        std::string id = p["id"].asString(), name = p["archive"].asString();
        Bytes idBytes(id.begin(), id.end());
        require(!id.empty() && ids.insert(id).second && archives.insert(name).second &&
                        name == sha256(idBytes) + ".z" && p["home_plmns"].isArray(),
                "index identity");
        auto offset = p["offset"].asUInt64(), size = p["compressed_size"].asUInt64();
        require(offset == end && size > 0 && size <= 2 * 1024 * 1024 && offset <= pack_.size() &&
                        size <= pack_.size() - offset,
                "pack index overlap/gap/bounds");
        end += size;
        for (const auto& match : p["home_plmns"]) {
            require(match.isString(), "home PLMN type");
            const auto home = match.asString();
            require((home.size() == 5 || home.size() == 6) &&
                            std::all_of(home.begin(), home.end(),
                                        [](char c) { return c >= '0' && c <= '9'; }),
                    "home PLMN format");
        }
    }
    require(end == pack_.size(), "pack unindexed tail");
    if (!userDirectory.empty()) {
        importsPath_ = userDirectory + "/imports.json";
        if (access(importsPath_.c_str(), F_OK) == 0) {
            auto saved = parseJson(readFile(importsPath_, 40 * 1024 * 1024));
            const auto digest = saved["sha256"].asString();
            saved.removeMember("sha256");
            require(sha256(jsonBytes(saved)) == digest && saved["schema_version"].asInt() == 1 &&
                            saved["profiles"].isArray() && saved["profiles"].size() <= 16,
                    "imports integrity");
            imports_ = saved["profiles"];
            std::set<std::string> homes;
            for (const auto& p : imports_) {
                const auto id = p["id"].asString();
                require(id.rfind("imported/", 0) == 0 && p["imported"].asBool() &&
                                p["source_name"].isString() &&
                                p["source_sha256"].asString().size() == 64 &&
                                p["home_plmns"].isArray() && !p["home_plmns"].empty(),
                        "import identity");
                for (const auto& home : p["home_plmns"]) {
                    const auto text = home.asString();
                    require((text.size() == 5 || text.size() == 6) &&
                                    text.find_first_not_of("0123456789") == std::string::npos &&
                                    homes.insert(text).second,
                            "import home identity");
                }
                validate(p, id, p["entries"].size());
            }
        } else
            require(errno == ENOENT, "imports access");
    }
}
Selection ProfileStore::select(const std::vector<std::string>& homePlmns) const {
    if (homePlmns.empty()) return {"waiting_for_identity", ""};
    if (homePlmns.size() > 2) return {"conflict", ""};
    std::string chosen;
    for (const auto& plmn : homePlmns) {
        if ((plmn.size() != 5 && plmn.size() != 6) ||
            !std::all_of(plmn.begin(), plmn.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return {"waiting_for_identity", ""};
        std::set<std::string> matches;
        for (const auto& p : imports_)
            for (const auto& m : p["home_plmns"])
                if (m.asString() == plmn) matches.insert(p["id"].asString());
        if (matches.empty())
            for (const auto& p : index_["profiles"])
                for (const auto& m : p["home_plmns"])
                    if (m.asString() == plmn) matches.insert(p["id"].asString());
        if (matches.size() != 1)
            return {homePlmns.size() == 2 || matches.size() > 1 ? "conflict" : "unsupported", ""};
        if (!chosen.empty() && chosen != *matches.begin()) return {"conflict", ""};
        chosen = *matches.begin();
    }
    return {"checking", chosen};
}
Snapshot ProfileStore::load(const std::string& id) const {
    for (const auto& p : imports_)
        if (p["id"].asString() == id) return validate(p, id, p["entries"].size());
    const Json::Value* metadata = nullptr;
    for (const auto& p : index_["profiles"])
        if (p["id"].asString() == id) metadata = &p;
    require(metadata, "unknown profile");
    const auto offset = (*metadata)["offset"].asUInt64(),
               size = (*metadata)["compressed_size"].asUInt64();
    const Bytes compressed(pack_.begin() + offset, pack_.begin() + offset + size);
    require(sha256(compressed) == (*metadata)["archive_sha256"].asString(), "archive digest");
    auto expandedSize = (*metadata)["expanded_size"].asUInt();
    require(expandedSize > 0 && expandedSize <= 2 * 1024 * 1024, "expanded bounds");
    Bytes expanded(expandedSize);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(compressed.data());
    stream.avail_in = compressed.size();
    stream.next_out = expanded.data();
    stream.avail_out = expanded.size();
    require(inflateInit(&stream) == Z_OK, "inflate init");
    int result = inflate(&stream, Z_FINISH);
    bool ok = result == Z_STREAM_END && stream.total_out == expanded.size() && stream.avail_in == 0;
    inflateEnd(&stream);
    require(ok && sha256(expanded) == (*metadata)["expanded_sha256"].asString(),
            "expanded digest/stream");
    return validate(parseJson(expanded), id, (*metadata)["entries"].asUInt());
}
Snapshot ProfileStore::validate(const Json::Value& profile, const std::string& id,
                                unsigned count) const {
    require(profile["schema_version"].asInt() == 1 && profile["id"].asString() == id &&
                    profile["entries"].isArray() && profile["entries"].size() == count &&
                    profile["entries"].size() > 0 && profile["entries"].size() <= 85,
            "profile schema");
    Snapshot snapshot;
    for (const auto& item : profile["entries"]) {
        auto path = item["path"].asString();
        auto bytes = unhex(item["hex"].asString());
        require(allow_.count(path) && !snapshot.count(path) &&
                        sha256(bytes) == item["sha256"].asString(),
                "entry integrity/path");
        snapshot[path] = std::move(bytes);
    }
    return snapshot;
}
Json::Value ProfileStore::profiles() const {
    auto result = index_["profiles"];
    for (const auto& imported : imports_) result.append(imported);
    return result;
}
Json::Value ProfileStore::metadata(const std::string& id) const {
    for (const auto& p : imports_)
        if (p["id"].asString() == id) return p;
    for (const auto& p : index_["profiles"])
        if (p["id"].asString() == id) {
            auto result = p;
            result["source_name"] = id + "/mcfg_sw.mbn";
            result["source_sha256"] = p["source_mbn_sha256"];
            return result;
        }
    return Json::Value();
}
void ProfileStore::saveImports(const Json::Value& profiles) {
    require(!importsPath_.empty() && profiles.size() <= 16, "import storage unavailable/full");
    Json::Value saved;
    saved["schema_version"] = 1;
    saved["profiles"] = profiles;
    saved["sha256"] = sha256(jsonBytes(saved));
    require(jsonBytes(saved).size() <= 40 * 1024 * 1024, "imports size");
    durableWrite(importsPath_, saved);
    imports_ = profiles;
}
std::string ProfileStore::importMbn(const Bytes& mbn, const std::string& filename,
                                    const std::string& home) {
    require((home.size() == 5 || home.size() == 6) &&
                    home.find_first_not_of("0123456789") == std::string::npos,
            "import home identity");
    require(!filename.empty() && filename.size() <= 128 &&
                    filename.find_first_of("/\\") == std::string::npos &&
                    std::all_of(filename.begin(), filename.end(),
                                [](unsigned char c) { return c >= 32 && c != 127; }),
            "import filename");
    const auto snapshot = parseMbn(mbn, allow_);
    Json::Value homes(Json::arrayValue);
    homes.append(home);
    const auto selected = select({home});
    if (!selected.profile.empty()) homes = metadata(selected.profile)["home_plmns"];
    const std::string id = "imported/" + sha256(jsonBytes(homes));
    Json::Value profile;
    profile["schema_version"] = 1;
    profile["id"] = id;
    profile["imported"] = true;
    profile["home_plmns"] = homes;
    profile["source_name"] = filename;
    profile["source_sha256"] = sha256(mbn);
    profile["entries"] = Json::Value(Json::arrayValue);
    for (const auto& [path, bytes] : snapshot) {
        Json::Value entry;
        entry["path"] = path;
        entry["hex"] = hex(*bytes);
        entry["sha256"] = sha256(*bytes);
        profile["entries"].append(entry);
    }
    require(jsonBytes(profile).size() <= 2 * 1024 * 1024, "import expanded bounds");
    validate(profile, id, snapshot.size());
    Json::Value next(Json::arrayValue);
    for (const auto& p : imports_)
        if (p["id"].asString() != id) next.append(p);
    next.append(profile);
    saveImports(next);
    return id;
}
void ProfileStore::deleteImported(const std::string& id) {
    require(id.rfind("imported/", 0) == 0, "bundled profile cannot be deleted");
    Json::Value next(Json::arrayValue);
    bool found = false;
    for (const auto& p : imports_) {
        if (p["id"].asString() == id)
            found = true;
        else
            next.append(p);
    }
    require(found, "unknown imported profile");
    saveImports(next);
}
}  // namespace mcfg
