// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif
#include <fstream>
#include <iomanip>

namespace gamepolicy {
namespace {
using namespace tinyxml2;
constexpr int kGpuDefault = 0x42800000;
constexpr int kGoldCores = 0x41000000, kPrimeCores = 0x41000200;
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}
#if !__has_include(<sys/random.h>)
// The host sysroot of the policy checks predates getrandom().
ssize_t getrandom(void* buffer, size_t size, unsigned) {
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    const auto n = read(fd, buffer, size);
    close(fd);
    return n;
}
#endif
std::string newEpoch() {
    std::array<unsigned char, 16> bytes{};
    size_t done = 0;
    while (done < bytes.size()) {
        const auto n = getrandom(bytes.data() + done, bytes.size() - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Cannot generate built-in epoch");
        done += n;
    }
    std::ostringstream s; s << std::hex << std::setfill('0');
    for (auto b : bytes) s << std::setw(2) << int(b);
    return s.str();
}
bool validEpoch(const std::string& epoch) {
    return epoch.size() == 32 && std::all_of(epoch.begin(), epoch.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string text(const XMLElement* e, const char* key) {
    const char* s = e ? e->Attribute(key) : nullptr;
    if (!s) throw std::runtime_error(std::string("Missing archive attribute ") + key);
    return s;
}
int number(const XMLElement* e, const char* key) {
    int n;
    if (!e || e->QueryIntAttribute(key, &n) != XML_SUCCESS)
        throw std::runtime_error(std::string("Invalid archive integer ") + key);
    return n;
}
uint64_t generation(const XMLElement* e) {
    uint64_t n;
    if (!e || e->QueryUnsigned64Attribute("generation", &n) != XML_SUCCESS || !n)
        throw std::runtime_error("Invalid archive generation");
    return n;
}
XMLElement* child(XMLElement* e, const char* name) {
    auto c = e->GetDocument()->NewElement(name); e->InsertEndChild(c); return c;
}
std::string xml(const XMLNode& node) {
    XMLPrinter printer(nullptr, true); node.Accept(&printer); return printer.CStr();
}
std::string contentId(const std::string& data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream out; out << std::hex << hash; return out.str();
}
XMLElement* content(XMLElement* root, const std::string& id) {
    for (auto c = root->FirstChildElement("content"); c; c = c->NextSiblingElement("content"))
        if (text(c, "id") == id) return c;
    return nullptr;
}
void writeBand(XMLElement* e, const Band& b) {
    e->SetAttribute("temperature", b.temperature);
    for (int r = 0; r < 3; ++r) {
        auto u = child(e, "unit"); u->SetAttribute("floor", b.units[r].floor);
        u->SetAttribute("ceiling", b.units[r].ceiling);
    }
}
Band readBand(const XMLElement* e) {
    Band b{};
    if (!e || e->QueryDoubleAttribute("temperature", &b.temperature) != XML_SUCCESS || !std::isfinite(b.temperature))
        throw std::runtime_error("Invalid archive temperature");
    auto u = e->FirstChildElement("unit");
    for (int r = 0; r < 3; ++r) {
        b.units[r] = {number(u, "floor"), number(u, "ceiling")};
        if (b.units[r].floor < -1 || b.units[r].ceiling < -1)
            throw std::runtime_error("Invalid archive frequency");
        u = u->NextSiblingElement("unit");
    }
    if (u) throw std::runtime_error("Extra archive frequency");
    return b;
}
// The durable and comparison representation uses physical frequencies, never GPU indices.
void writeMode(XMLElement* e, const Mode& m) {
    e->SetAttribute("thermal", m.thermalCase); e->SetAttribute("delay", m.delayMs);
    e->SetAttribute("jankTime", m.jankAdjustTime); e->SetAttribute("linear", int(m.linearBands));
    e->SetAttribute("boostLength", m.boostLengthMs); e->SetAttribute("sensitivity", m.dropSensitivity);
    e->SetAttribute("dropLower", m.dropLowerPermille); e->SetAttribute("dropUpper", m.dropUpperPermille);
    e->SetAttribute("hysteresis", m.hysteresisDeciC); e->SetAttribute("skin", m.skinTargetMilliC);
    e->SetAttribute("inheritedHysteresis", m.inheritedHysteresis);
    e->SetAttribute("power", m.powerTargetMw);
    for (auto pair : {std::make_pair("normal", &m.bands), {"fps", &m.boostedBands}, {"jank", &m.jankBands}})
        for (const auto& b : *pair.second) writeBand(child(e, pair.first), b);
    for (auto pair : {std::make_pair("fpsAdjust", &m.fpsAdjust), {"jankAdjust", &m.jankAdjust}}) if (*pair.second) {
        auto a = child(e, pair.first);
        for (int n : **pair.second) child(a, "delta")->SetAttribute("value", n);
    }
    for (const auto& h : m.perfHints) {
        auto a = child(e, "hint"); a->SetAttribute("opcode", h.opcode); a->SetAttribute("value", h.value);
    }
    for (const auto& t : m.bindCore) {
        auto a = child(e, "bind"); a->SetAttribute("name", t.name.c_str()); a->SetAttribute("mask", t.mask);
    }
    for (const auto& t : m.schedulers) {
        auto a = child(e, "scheduler"); a->SetAttribute("name", t.name.c_str());
        a->SetAttribute("priority", t.priority); a->SetAttribute("enabled", int(t.enabled));
    }
    for (const auto& [id, s] : m.scenes) {
        auto a = child(e, "scene"); a->SetAttribute("id", id); a->SetAttribute("time", s.timeMs);
        writeBand(a, s.frequencies);
    }
}
Mode readMode(const XMLElement* e) {
    Mode m;
    m.thermalCase = number(e, "thermal"); m.delayMs = number(e, "delay");
    m.jankAdjustTime = number(e, "jankTime"); m.linearBands = number(e, "linear");
    m.boostLengthMs = number(e, "boostLength"); m.dropSensitivity = number(e, "sensitivity");
    m.dropLowerPermille = e->IntAttribute("dropLower", -1); m.dropUpperPermille = e->IntAttribute("dropUpper", -1);
    m.hysteresisDeciC = number(e, "hysteresis"); m.skinTargetMilliC = number(e, "skin");
    if (e->QueryDoubleAttribute("inheritedHysteresis", &m.inheritedHysteresis) != XML_SUCCESS ||
        !std::isfinite(m.inheritedHysteresis) || m.inheritedHysteresis < -1 || m.inheritedHysteresis > 5)
        throw std::runtime_error("Invalid archive hysteresis");
    m.powerTargetMw = number(e, "power");
    for (auto pair : {std::make_pair("normal", &m.bands), {"fps", &m.boostedBands}, {"jank", &m.jankBands}})
        for (auto a = e->FirstChildElement(pair.first); a; a = a->NextSiblingElement(pair.first))
            pair.second->push_back(readBand(a));
    for (auto pair : {std::make_pair("fpsAdjust", &m.fpsAdjust), {"jankAdjust", &m.jankAdjust}}) {
        if (auto a = e->FirstChildElement(pair.first)) {
            std::array<int, 3> v{}; auto d = a->FirstChildElement("delta");
            for (int& n : v) { n = number(d, "value"); d = d->NextSiblingElement("delta"); }
            if (d) throw std::runtime_error("Extra archive boost delta");
            *pair.second = v;
        }
    }
    for (auto a = e->FirstChildElement("hint"); a; a = a->NextSiblingElement("hint"))
        m.perfHints.push_back({number(a, "opcode"), number(a, "value")});
    for (auto a = e->FirstChildElement("bind"); a; a = a->NextSiblingElement("bind")) {
        uint64_t mask;
        if (a->QueryUnsigned64Attribute("mask", &mask) != XML_SUCCESS) throw std::runtime_error("Invalid archive CPU mask");
        m.bindCore.push_back({text(a, "name"), mask});
    }
    for (auto a = e->FirstChildElement("scheduler"); a; a = a->NextSiblingElement("scheduler"))
        m.schedulers.push_back({text(a, "name"), number(a, "priority"), bool(number(a, "enabled"))});
    for (auto a = e->FirstChildElement("scene"); a; a = a->NextSiblingElement("scene"))
        if (!m.scenes.emplace(number(a, "id"), Scene{number(a, "time"), readBand(a)}).second)
            throw std::runtime_error("Duplicate archive scene");
    return m;
}
Policy physical(const Policy& policy) {
    Policy p = policy;
    p.resetArchiveRegistry();
    for (auto& [pkg, modes] : p.games) for (auto& [name, m] : modes) {
        m.inheritedHysteresis = m.hysteresisDeciC >= 0 ? m.hysteresisDeciC / 10.0
                : m.inheritedHysteresis >= 0 ? m.inheritedHysteresis : policy.overlay.hysteresis;
        m.hysteresisDeciC = -1;
        for (auto& h : m.perfHints) if (h.opcode == kGpuDefault) h.value = policy.tables()[2].at(h.value);
    }
    return p;
}
void writePolicy(XMLElement* e, const Policy& p) {
    e->SetAttribute("version", p.version.c_str());
    for (const auto& [mode, name] : p.overlay.modes) {
        auto a = child(e, "mapping"); a->SetAttribute("mode", mode); a->SetAttribute("name", name.c_str());
    }
    for (const auto& [pkg, alias] : p.overlay.aliases) {
        auto a = child(e, "alias"); a->SetAttribute("package", pkg.c_str());
        a->SetAttribute("profile", alias.profile.c_str()); a->SetAttribute("placement", int(alias.threadPlacement));
    }
    for (const auto& [pkg, modes] : p.games) {
        auto a = child(e, "profile"); a->SetAttribute("key", pkg.c_str());
        for (const auto& [name, m] : modes) {
            auto b = child(a, "mode"); b->SetAttribute("name", name.c_str()); writeMode(b, m);
        }
    }
}
Policy readPolicy(const XMLElement* e) {
    Policy p; p.version = text(e, "version");
    for (auto a = e->FirstChildElement("mapping"); a; a = a->NextSiblingElement("mapping"))
        if (!p.overlay.modes.emplace(number(a, "mode"), text(a, "name")).second)
            throw std::runtime_error("Duplicate archive mapping");
    for (auto a = e->FirstChildElement("alias"); a; a = a->NextSiblingElement("alias"))
        if (!p.overlay.aliases.emplace(text(a, "package"), Alias{text(a, "profile"), bool(number(a, "placement"))}).second)
            throw std::runtime_error("Duplicate archive alias");
    for (auto a = e->FirstChildElement("profile"); a; a = a->NextSiblingElement("profile")) {
        const auto key = text(a, "key");
        if (p.games.count(key)) throw std::runtime_error("Duplicate archive profile");
        auto& modes = p.games[key];
        for (auto b = a->FirstChildElement("mode"); b; b = b->NextSiblingElement("mode"))
            if (!modes.emplace(text(b, "name"), readMode(b)).second) throw std::runtime_error("Duplicate archive mode");
        if (!modes.count("Balance")) throw std::runtime_error("Archive has no Balance fallback");
    }
    if (!p.games.count("default")) throw std::runtime_error("Archive has no default profile");
    return p;
}
std::string fingerprint(const Selection& s) {
    XMLDocument doc; auto e = doc.NewElement("effective"); doc.InsertEndChild(e);
    // Identity matters only through scene gating. Compare all gating equivalence classes.
    Mode m = *s.mode;
    if (m.fpsAdjust || (m.jankAdjust && m.jankAdjustTime > 0))
        for (int scene : {-1, 7, 102, 103, 110, 111, 112})
            child(e, "gate")->SetAttribute("enabled", int(Policy::sceneEnabled(s.profile,
                    scene < 0 ? std::optional<int>{} : std::optional<int>{scene})));
    if (!s.threadPlacement) { m.bindCore.clear(); m.schedulers.clear(); }
    // Hints are an unordered opcode map; thread rules and frequency rows remain ordered.
    std::stable_sort(m.perfHints.begin(), m.perfHints.end(), [](const auto& a, const auto& b) { return a.opcode < b.opcode; });
    if (!m.fpsAdjust) { m.boostedBands.clear(); m.boostLengthMs = 0; m.dropSensitivity = 0; }
    else m.fpsAdjust = std::array<int, 3>{}; // Only presence and resolved boost rows affect execution.
    if (!m.jankAdjust || m.jankAdjustTime <= 0) { m.jankAdjust.reset(); m.jankBands.clear(); m.jankAdjustTime = 0; }
    else m.jankAdjust = std::array<int, 3>{};
    m.schedulers.erase(std::remove_if(m.schedulers.begin(), m.schedulers.end(), [](const auto& t) { return !t.enabled; }), m.schedulers.end());
    if (!m.linearBands && !m.bands.empty()) {
        const int positive = Policy::positiveCount(m);
        e->SetAttribute("positiveBranch", positive == 3 || positive == 4 ? positive : 0);
        // Non-3/4 positive-count policies always use row zero; the rest are unreachable.
        if (positive != 3 && positive != 4) {
            m.hysteresisDeciC = -1; m.inheritedHysteresis = 0;
            m.bands.resize(1); m.bands[0].temperature = 0;
            if (!m.boostedBands.empty()) { m.boostedBands.resize(1); m.boostedBands[0].temperature = 0; }
            if (!m.jankBands.empty()) { m.jankBands.resize(1); m.jankBands[0].temperature = 0; }
        } else {
            m.bands.resize(positive);
            if (m.boostedBands.size() > size_t(positive)) m.boostedBands.resize(positive);
            if (m.jankBands.size() > size_t(positive)) m.jankBands.resize(positive);
        }
    }
    for (auto rows : {&m.boostedBands, &m.jankBands}) for (auto& b : *rows) b.temperature = 0;
    writeMode(e, m); return xml(doc);
}
Mode safeMode(const Policy& p, Mode m) {
    if (!allowedCase(m.thermalCase)) m.thermalCase = 0;
    // The current overlay is the safety authority for inherited skin budgets.
    if (p.overlay.skinTargetMilliC >= 0 &&
        (m.skinTargetMilliC < 0 || m.skinTargetMilliC > p.overlay.skinTargetMilliC))
        m.skinTargetMilliC = p.overlay.skinTargetMilliC;
    auto clamp = [&](int r, int value) {
        if (value < 0) return -1;
        int best = -1;
        for (int f : p.ladder(r)) if (f <= value) best = std::max(best, f);
        return best;
    };
    auto encode = [&](Band& b) {
        for (int r = 0; r < 3; ++r) {
            auto& u = b.units[r];
            u.ceiling = clamp(r, u.ceiling);
            u.floor = clamp(r, u.floor < 0 ? -1 : std::min(u.floor, u.ceiling));
            auto encoded = [&](int f) {
                if (f < 0) return -1;
                if (r < 2) return f / 1000;
                const auto& table = p.tables()[2];
                return int(std::find(table.begin(), table.end(), f) - table.begin());
            };
            b.bounds[r] = {encoded(u.floor), encoded(u.ceiling)};
        }
    };
    for (auto rows : {&m.bands, &m.boostedBands, &m.jankBands}) for (auto& b : *rows) encode(b);
    for (auto& [id, s] : m.scenes) encode(s.frequencies);
    auto& hints = m.perfHints;
    hints.erase(std::remove_if(hints.begin(), hints.end(), [&](PerfHint& h) {
        if (h.opcode == kGpuDefault) {
            const int f = clamp(2, h.value);
            if (f < 0) return true;
            const auto& table = p.tables()[2]; h.value = std::find(table.begin(), table.end(), f) - table.begin();
        } else if (h.opcode == kGoldCores || h.opcode == kPrimeCores) {
            const int count = p.cores(h.opcode == kGoldCores ? 0 : 1);
            if (count <= 0 || h.value < 0) return true;
            h.value = std::min(h.value, count);
        }
        return false;
    }), hints.end());
    for (auto& t : m.bindCore) t.mask &= p.presentMask();
    m.bindCore.erase(std::remove_if(m.bindCore.begin(), m.bindCore.end(), [](const auto& t) { return !t.mask; }), m.bindCore.end());
    return m;
}
bool replace(const std::string& dir, const std::string& data) {
    for (size_t i = 1; i <= dir.size(); ++i) if (i == dir.size() || dir[i] == '/')
        if (::mkdir(dir.substr(0, i).c_str(), 0700) && errno != EEXIST) return false;
    const auto path = dir + "/builtins.xml", temp = path + ".tmp";
    const int directory = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    bool ok = fd >= 0;
    size_t done = 0;
    while (ok && done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += n;
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (fd >= 0 && ::close(fd) != 0) ok = false;
    if (ok) ok = ::rename(temp.c_str(), path.c_str()) == 0;
    if (ok) ok = ::fsync(directory) == 0;
    const int saved = errno;
    ::close(directory);
    if (!ok) ::unlink(temp.c_str());
    errno = saved; return ok;
}
}

uint64_t Policy::archiveGeneration() const {
    std::lock_guard lock(mHistory->mutex); return mHistory->generation;
}
#ifdef GAMEPOLICY_IO_FAULT_TEST
bool archiveRoundTrip(const Policy& policy) {
    XMLDocument before, after;
    auto first = before.NewElement("current"); before.InsertEndChild(first);
    auto second = after.NewElement("current"); after.InsertEndChild(second);
    writePolicy(first, physical(policy));
    writePolicy(second, readPolicy(first));
    return xml(before) == xml(after);
}
#endif
bool Policy::archiveReady() const {
    std::lock_guard lock(mHistory->mutex); return mHistory->generation && !mHistory->retryAtMs;
}
std::string Policy::archiveEpoch() const {
    std::lock_guard lock(mHistory->mutex); return mHistory->epoch;
}
std::string Policy::archiveMarker() const {
    std::lock_guard lock(mHistory->mutex);
    return mHistory->generation && !mHistory->retryAtMs
        ? "@builtin/current/" + mHistory->epoch + "/" + std::to_string(mHistory->generation) : "@builtin/pending";
}
std::shared_ptr<const Policy> Policy::archiveRuntime() const {
    std::lock_guard lock(mHistory->mutex); return mHistory->runtime;
}
std::optional<ArchivedBuiltin> Policy::archived(const std::string& key) const {
    std::lock_guard lock(mHistory->mutex);
    const auto it = mHistory->entries.find(key);
    if (it == mHistory->entries.end()) return {};
    auto entry = it->second;
    entry.mode = safeMode(mHistory->runtime ? *mHistory->runtime : *this, entry.mode);
    return entry;
}
std::optional<ArchivedBuiltin> Policy::history(const std::string& pkg, int mode, uint64_t after) const {
    std::lock_guard lock(mHistory->mutex);
    const ArchivedBuiltin* next = nullptr;
    for (const auto& [key, a] : mHistory->entries) {
        if (a.gameMode != mode || a.generation <= after || (next && next->generation <= a.generation)) continue;
        const bool included = std::find(a.packages.begin(), a.packages.end(), pkg) != a.packages.end();
        const bool fallback = std::find(a.packages.begin(), a.packages.end(), "*") != a.packages.end() &&
                std::find(a.excluded.begin(), a.excluded.end(), pkg) == a.excluded.end();
        if (included || fallback) next = &a;
    }
    if (!next) return {};
    auto entry = *next;
    entry.mode = safeMode(mHistory->runtime ? *mHistory->runtime : *this, entry.mode);
    return entry;
}
bool Policy::retryBuiltinArchive(std::string* error) {
    std::string dir;
    { std::lock_guard lock(mHistory->mutex);
      if (!mHistory->retryAtMs || nowMs() < mHistory->retryAtMs) return true;
      dir = mHistory->directory; }
    return updateBuiltinArchive(dir, error);
}
bool Policy::updateBuiltinArchive(const std::string& dir, std::string* error) {
    std::lock_guard lock(mHistory->mutex);
    const bool sameDirectory = mHistory->directory == dir;
    mHistory->directory = dir; mHistory->retryAtMs = nowMs() + 5000;
    try {
        XMLDocument doc;
        struct stat st{};
        const bool statOk = ::stat((dir + "/builtins.xml").c_str(), &st) == 0;
        const int64_t stamp = statOk ? st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : -1;
        const bool cached = statOk && sameDirectory && mHistory->document && mHistory->checkpoint &&
                mHistory->fileStamp == stamp && mHistory->fileSize == uint64_t(st.st_size);
        auto result = XML_SUCCESS;
        if (cached) mHistory->document->DeepCopy(&doc);
        else result = doc.LoadFile((dir + "/builtins.xml").c_str());
        XMLElement* root = nullptr;
        Policy previous;
        uint64_t gen = 1;
        std::string epoch;
        std::map<std::string, ArchivedBuiltin> entries;
        bool exists = result != XML_ERROR_FILE_NOT_FOUND;
        try {
        if (exists) {
            root = doc.FirstChildElement("builtins");
            if (result != XML_SUCCESS || !root || number(root, "schema") != 1)
                throw std::runtime_error("Unreadable built-in snapshot");
            gen = generation(root);
            previous = cached ? *mHistory->checkpoint : readPolicy(root->FirstChildElement("current"));
            epoch = text(root, "epoch");
            if (!validEpoch(epoch)) throw std::runtime_error("Invalid built-in epoch");
            if (cached) entries = mHistory->entries;
            for (auto e = cached ? nullptr : root->FirstChildElement("archive"); e; e = e->NextSiblingElement("archive")) {
                ArchivedBuiltin a;
                a.key = text(e, "key"); a.version = text(e, "version"); a.profile = text(e, "profile");
                a.generation = generation(e); a.gameMode = number(e, "gameMode"); a.placement = number(e, "placement");
                if (a.generation > gen || a.gameMode < 1 || a.gameMode > 3 || a.key.rfind("@builtin/" + epoch + "/", 0) != 0)
                    throw std::runtime_error("Invalid built-in transition");
                for (auto p = e->FirstChildElement("package"); p; p = p->NextSiblingElement("package"))
                    a.packages.push_back(text(p, "name"));
                for (auto p = e->FirstChildElement("exclude"); p; p = p->NextSiblingElement("exclude"))
                    a.excluded.push_back(text(p, "name"));
                auto stored = content(root, text(e, "content"));
                if (!stored) throw std::runtime_error("Missing immutable built-in content");
                a.mode = readMode(stored->FirstChildElement("mode"));
                if (!entries.emplace(a.key, a).second) throw std::runtime_error("Duplicate built-in archive");
            }
            // Publish the durable journal even when capturing the new policy subsequently fails.
            mHistory->entries = entries; mHistory->generation = gen; mHistory->epoch = epoch;
            if (!cached) {
                mHistory->checkpoint = std::make_shared<Policy>(previous);
                mHistory->document = std::make_shared<XMLDocument>(); doc.DeepCopy(mHistory->document.get());
                mHistory->fileStamp = stamp; mHistory->fileSize = st.st_size;
            }
        }
        } catch (const std::exception&) {
            const auto path = dir + "/builtins.xml";
            const auto bad = path + ".bad-" + newEpoch();
            if (::rename(path.c_str(), bad.c_str())) throw std::runtime_error("Cannot quarantine built-in snapshot");
            const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0) throw std::runtime_error("Cannot open quarantine directory");
            const int synced = ::fsync(fd); ::close(fd);
            if (synced) throw std::runtime_error("Cannot sync quarantine directory");
            doc.Clear(); root = nullptr; entries.clear(); gen = 1; epoch.clear(); exists = false;
            mHistory->document.reset(); mHistory->checkpoint.reset();
        }
        if (!exists) {
            if (mHistory->generation) { mHistory->entries.clear(); mHistory->generation = 0; mHistory->epoch.clear(); }
            epoch = newEpoch();
        }
        const Policy* runtime = this;
        std::shared_ptr<Policy> recovered = mHistory->runtime;
        if (recovered) runtime = recovered.get();
        if ((!mResolved || !mPresent || mCores[0] <= 0 || mCores[1] <= 0 ||
             ladder(0).empty() || ladder(1).empty() || ladder(2).empty()) && mUnresolved && !recovered) {
            recovered = std::make_shared<Policy>(*mUnresolved);
            recovered->resetArchiveRegistry();
            Opps actual; std::array<int, 2> cores{}; uint64_t present = 0;
            for (int r = 0; r < 3; ++r) {
                std::ifstream input(overlay.oppPaths[r]); int f;
                while (input >> f) actual[r].push_back(f);
                if (r < 2) {
                    const auto path = overlay.oppPaths[r];
                    std::ifstream related(path.substr(0, path.rfind('/')) + "/related_cpus");
                    std::string line; std::getline(related, line);
                    cores[r] = coreCount(line); present |= cpuMask(line);
                }
            }
            recovered->resolve(actual, cores, cores[0] && cores[1] ? present : 0);
            runtime = recovered.get();
        }
        if (!runtime->mResolved || !runtime->mPresent || runtime->mCores[0] <= 0 || runtime->mCores[1] <= 0)
            throw std::runtime_error("Incomplete built-in runtime topology");
        for (int r = 0; r < 3; ++r) if (runtime->ladder(r).empty()) throw std::runtime_error("Incomplete built-in runtime tables");
        mHistory->runtime = recovered;
        const Policy current = physical(*runtime);
        if (exists && previous.version != version) {
            if (gen >= uint64_t(std::numeric_limits<int64_t>::max())) throw std::runtime_error("Built-in generation overflow");
            ++gen;
            std::set<std::string> packages;
            for (const Policy* p : {static_cast<const Policy*>(&previous), &current}) {
                for (const auto& [pkg, modes] : p->games) if (pkg != "default") packages.insert(pkg);
                for (const auto& [pkg, alias] : p->overlay.aliases) packages.insert(pkg);
            }
            size_t index = 0;
            for (int mode = 1; mode <= 3; ++mode) {
                auto compare = [&](const std::string& pkg, bool fallback) {
                    auto old = previous.select(pkg, mode), next = current.select(pkg, mode);
                    if (fingerprint(old) == fingerprint(next)) return;
                    for (const auto& [key, existing] : entries) {
                        if (existing.gameMode != mode) continue;
                        const bool wildcard = std::find(existing.packages.begin(), existing.packages.end(), "*") != existing.packages.end();
                        const bool covered = fallback
                            ? wildcard && std::all_of(existing.excluded.begin(), existing.excluded.end(),
                                [&](const auto& excluded) { return packages.count(excluded); })
                            : std::find(existing.packages.begin(), existing.packages.end(), pkg) != existing.packages.end() ||
                                (wildcard && std::find(existing.excluded.begin(), existing.excluded.end(), pkg) == existing.excluded.end());
                        if (!covered) continue;
                        if (fingerprint({&existing.mode, existing.profile, existing.placement}) == fingerprint(old)) return;
                    }
                    ArchivedBuiltin a;
                    a.key = "@builtin/" + epoch + "/" + std::to_string(gen) + "/" + std::to_string(index++);
                    a.version = previous.version; a.profile = old.profile; a.placement = old.threadPlacement;
                    a.generation = gen; a.gameMode = mode; a.packages = {fallback ? "*" : pkg};
                    if (fallback) a.excluded.assign(packages.begin(), packages.end());
                    a.mode = *old.mode;
                    auto e = child(root, "archive");
                    e->SetAttribute("key", a.key.c_str()); e->SetAttribute("version", a.version.c_str());
                    e->SetAttribute("profile", a.profile.c_str()); e->SetAttribute("generation", gen);
                    e->SetAttribute("gameMode", mode); e->SetAttribute("placement", int(a.placement));
                    for (const auto& pkg : a.packages) child(e, "package")->SetAttribute("name", pkg.c_str());
                    for (const auto& pkg : a.excluded) child(e, "exclude")->SetAttribute("name", pkg.c_str());
                    XMLDocument contentDoc;
                    auto savedMode = contentDoc.NewElement("mode"); contentDoc.InsertEndChild(savedMode); writeMode(savedMode, a.mode);
                    const auto data = xml(*savedMode), hash = contentId(data);
                    std::string id = hash;
                    size_t collision = 0;
                    // Compare full content before reusing an identity, including hash collisions.
                    auto stored = content(root, id);
                    while (stored && xml(*stored->FirstChildElement("mode")) != data) {
                        id = hash + "-" + std::to_string(++collision); stored = content(root, id);
                    }
                    if (!stored) {
                        stored = child(root, "content"); stored->SetAttribute("id", id.c_str());
                        stored->InsertEndChild(savedMode->DeepClone(&doc));
                    }
                    e->SetAttribute("content", id.c_str());
                    entries.emplace(a.key, std::move(a));
                };
                for (const auto& pkg : packages) compare(pkg, false);
                compare("", true);
            }
        }
        if (!root) {
            root = doc.NewElement("builtins"); doc.InsertEndChild(root); root->SetAttribute("schema", 1);
        }
        XMLDocument snapshot; auto s = snapshot.NewElement("current"); snapshot.InsertEndChild(s); writePolicy(s, current);
        const auto old = root->FirstChildElement("current");
        if (exists && xml(*old) == xml(*s)) {
            const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0) throw std::runtime_error("Cannot open built-in directory");
            const int result = ::fsync(fd); ::close(fd);
            if (result) throw std::runtime_error("Cannot sync built-in directory");
            mHistory->retryAtMs = 0; return true;
        }
        if (old) root->DeleteChild(old);
        root->InsertFirstChild(s->DeepClone(&doc)); root->SetAttribute("generation", gen); root->SetAttribute("epoch", epoch.c_str());
        // Journal and checkpoint are one transaction: neither can advance without the other.
        if (!replace(dir, xml(doc))) throw std::runtime_error(std::string("Cannot persist built-ins: ") + std::strerror(errno));
        mHistory->entries = std::move(entries); mHistory->generation = gen; mHistory->epoch = epoch; mHistory->retryAtMs = 0;
        auto checkpoint = std::make_shared<Policy>(current); checkpoint->resetArchiveRegistry();
        mHistory->checkpoint = std::move(checkpoint);
        mHistory->document = std::make_shared<XMLDocument>(); doc.DeepCopy(mHistory->document.get());
        if (::stat((dir + "/builtins.xml").c_str(), &st) == 0) {
            mHistory->fileStamp = st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec; mHistory->fileSize = st.st_size;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}
} // namespace gamepolicy
