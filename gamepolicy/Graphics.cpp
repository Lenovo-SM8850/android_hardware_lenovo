// SPDX-License-Identifier: Apache-2.0
#include "Graphics.h"
#include "Tuning.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace gamepolicy {
namespace {
constexpr size_t kMaxRecords = 900;
bool member(const std::string& value, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(), values.end(), [&](const char* v) { return value == v; });
}
std::string varint(size_t value) {
    std::string out;
    while (value >= 128) { out.push_back(static_cast<char>((value & 127) | 128)); value >>= 7; }
    out.push_back(static_cast<char>(value));
    return out;
}
std::string bytes(unsigned field, const std::string& value) {
    return varint((field << 3) | 2) + varint(value.size()) + value;
}
const char* attr(const tinyxml2::XMLElement* e, const char* name) {
    const char* value = e->Attribute(name);
    return value ? value : "";
}
} // namespace

bool GraphicsOptions::isDefault() const {
    return textureFiltering.empty() && anisotropicFiltering.empty() && mipmapLodBias.empty() && msaaLimit.empty();
}
bool validGraphicsOptions(const GraphicsOptions& o) {
    return member(o.textureFiltering, {"", "Balanced", "Fastest"}) &&
           member(o.anisotropicFiltering, {"", "1.0", "2.0", "4.0", "8.0", "16.0"}) &&
           member(o.mipmapLodBias, {"", "-4.0", "-2.0", "-1.0", "1.0", "2.0", "4.0"}) &&
           member(o.msaaLimit, {"", "1", "2", "4"});
}
std::string encodeGraphicsProfile(const GraphicsOptions& o) {
    if (!validGraphicsOptions(o) || o.isDefault()) return {};
    // Only public setting fields are emitted. API 0 is GLES; API 1 is Vulkan.
    static const char* keys[2][4] = {
        {"TextureFilteringQuality", "TextureMaxAniso", "MipLodBias", "MaxSamples"},
        {"ro.vendor.qcom.adreno.qgl.TextureFilteringQuality", "ro.vendor.qcom.adreno.qgl.MaxTextureAnisotropy",
         "ro.vendor.qcom.adreno.qgl.MipLodBias", "ro.vendor.qcom.adreno.qgl.MaxSamples"}
    };
    const std::string values[] = {o.textureFiltering, o.anisotropicFiltering, o.mipmapLodBias, o.msaaLimit};
    std::string out;
    for (unsigned api = 0; api < 2; ++api) {
        std::string profile;
        for (unsigned k = 0; k < 4; ++k)
            if (!values[k].empty()) profile += bytes(2, bytes(1, keys[api][k]) + bytes(2, values[k]));
        profile += varint(3 << 3) + varint(api);
        out += bytes(1, profile);
    }
    return out;
}

GraphicsStore::GraphicsStore(std::string dir) : mDir(std::move(dir)) {}
GraphicsOptions GraphicsStore::get(int user, const std::string& package) const {
    const auto it = mRecords.find({user, package});
    return it == mRecords.end() ? GraphicsOptions{} : it->second;
}
bool GraphicsStore::load() {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile((mDir + "/graphics.xml").c_str()) != tinyxml2::XML_SUCCESS) {
        std::error_code error;
        const bool exists = std::filesystem::exists(mDir + "/graphics.xml", error);
        return !error && !exists;
    }
    const auto* root = doc.FirstChildElement("graphics");
    int user = -1;
    if (!root || root->QueryIntAttribute("user", &user) != tinyxml2::XML_SUCCESS || user < 0) return false;
    std::map<Key, GraphicsOptions> records;
    std::set<std::string> managed;
    for (const auto* e = root->FirstChildElement("package"); e; e = e->NextSiblingElement("package")) {
        const std::string pkg = attr(e, "name");
        if (!validPackage(pkg) || !managed.insert(pkg).second || managed.size() > kMaxRecords) return false;
    }
    for (const auto* e = root->FirstChildElement("options"); e; e = e->NextSiblingElement("options")) {
        int owner = -1;
        const std::string pkg = attr(e, "package");
        GraphicsOptions o{attr(e, "texture"), attr(e, "aniso"), attr(e, "lod"), attr(e, "msaa")};
        if (e->QueryIntAttribute("user", &owner) != tinyxml2::XML_SUCCESS || owner < 0 || !managed.count(pkg) ||
            !validGraphicsOptions(o) || o.isDefault() || !records.emplace(Key{owner, pkg}, o).second ||
            records.size() > kMaxRecords) return false;
    }
    mForegroundUser = user; mRecords = std::move(records); mManaged = std::move(managed);
    mPending = !mManaged.empty();
    return true;
}
bool GraphicsStore::save(int user, const std::map<Key, GraphicsOptions>& records, const std::set<std::string>& managed) {
    tinyxml2::XMLDocument doc;
    auto* root = doc.NewElement("graphics"); root->SetAttribute("user", user); doc.InsertEndChild(root);
    for (const auto& pkg : managed) {
        auto* e = doc.NewElement("package"); e->SetAttribute("name", pkg.c_str()); root->InsertEndChild(e);
    }
    for (const auto& [key, o] : records) {
        auto* e = doc.NewElement("options");
        e->SetAttribute("user", key.first); e->SetAttribute("package", key.second.c_str());
        e->SetAttribute("texture", o.textureFiltering.c_str()); e->SetAttribute("aniso", o.anisotropicFiltering.c_str());
        e->SetAttribute("lod", o.mipmapLodBias.c_str()); e->SetAttribute("msaa", o.msaaLimit.c_str());
        root->InsertEndChild(e);
    }
    tinyxml2::XMLPrinter printer; doc.Print(&printer);
    std::error_code error;
    std::filesystem::create_directories(mDir, error);
    if (error) return false;
    const std::string path = mDir + "/graphics.xml", tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const size_t size = printer.CStrSize() - 1;
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        const ssize_t n = write(fd, printer.CStr() + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += static_cast<size_t>(n);
    }
    ok = ok && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    const int dirFd = open(mDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) { fsync(dirFd); close(dirFd); }
    return true;
}
bool GraphicsStore::set(int user, const std::string& package, const GraphicsOptions& o, GraphicsBackend& backend) {
    if (user < 0 || !validPackage(package) || !validGraphicsOptions(o)) return false;
    auto records = mRecords;
    auto managed = mManaged;
    if (o.isDefault()) records.erase({user, package});
    else records[{user, package}] = o;
    managed.insert(package);
    if (records.size() > kMaxRecords || managed.size() > kMaxRecords || !save(mForegroundUser, records, managed)) return false;
    mRecords = std::move(records); mManaged = std::move(managed); mForegroundDirty = false;
    if (user != mForegroundUser) return true;
    mPending = true; mRetryAt = 0; mRetryDelayMs = 1000;
    return reapply(backend);
}
bool GraphicsStore::activate(int user, GraphicsBackend& backend) {
    if (user < 0) return false;
    if (user == mForegroundUser) return true;
    mForegroundUser = user;
    mForegroundDirty = !save(user, mRecords, mManaged);
    mPending = true; mRetryAt = 0; mRetryDelayMs = 1000;
    return reapply(backend);
}
bool GraphicsStore::reapply(GraphicsBackend& backend) {
    // Replacing desired bytes also repairs missing or externally changed HAL files.
    if (mForegroundDirty) mForegroundDirty = !save(mForegroundUser, mRecords, mManaged);
    bool ok = !mForegroundDirty;
    for (const auto& pkg : mManaged)
        if (!backend.apply(pkg, encodeGraphicsProfile(get(mForegroundUser, pkg)))) ok = false;
    mPending = !ok;
    if (ok) { mRetryAt = 0; mRetryDelayMs = 1000; }
    return ok;
}
bool GraphicsStore::retry(int64_t nowMs, GraphicsBackend& backend) {
    if (!mPending || nowMs < mRetryAt) return !mPending;
    if (backend.supported() && reapply(backend)) return true;
    mRetryAt = nowMs + mRetryDelayMs;
    mRetryDelayMs = std::min(mRetryDelayMs * 2, 60000);
    return false;
}
} // namespace gamepolicy
