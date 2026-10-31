// SPDX-License-Identifier: Apache-2.0
#include "Recorder.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gamepolicy {
namespace {
constexpr char kMagic[] = "game-policy-record 2";
constexpr char kMagicV1[] = "game-policy-record 1";
constexpr char kSamplesLine[] = "samples";
constexpr char kColumns[] = "t,fps,fpsMin,jank,frames,gold,prime,gpu,goldCap,primeCap,gpuCap,skin,cpu,gpu,power,band,case,"
                            "flags,mode,gen,scene,maxFt,utilGold,utilPrime,gpuBusy,battMilliC,battPct,reqFps,throttleFps";
constexpr size_t kOldColumns = 20, kV1Columns = 27, kColumnCount = 29;
constexpr char kRecSuffix[] = ".rec";
constexpr char kPartialSuffix[] = ".rec.partial";

bool endsWith(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}
std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(separator, from);
        if (at == std::string::npos) { parts.push_back(text.substr(from)); return parts; }
        parts.push_back(text.substr(from, at - from));
        from = at + 1;
    }
}
std::string join(const std::vector<std::string>& parts, char separator) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += separator; out += parts[i]; }
    return out;
}
bool parseInt(const std::string& text, long long* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (errno || *end) return false;
    *value = v;
    return true;
}
bool parseInt32(const std::string& text, int* value) {
    long long v;
    if (!parseInt(text, &v) || v < INT32_MIN || v > INT32_MAX) return false;
    *value = static_cast<int>(v);
    return true;
}
bool parseFloat(const std::string& text, float* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(text.c_str(), &end);
    if (errno || *end || !std::isfinite(v)) return false;
    *value = v;
    return true;
}
bool parseDouble(const std::string& text, double* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text.c_str(), &end);
    if (errno || *end || !std::isfinite(v)) return false;
    *value = v;
    return true;
}
std::string num(long long v) { return std::to_string(v); }
std::string fixed1(float v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v)); return b; }
std::string exact(float v) { char b[32]; std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v)); return b; }
std::string temperature(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    std::string t = b;
    if (endsWith(t, ".0")) t.resize(t.size() - 2);
    return t;
}

// Record ids are [0-9]+_[A-Za-z0-9._]+.
bool validId(const std::string& id) {
    const size_t split = id.find('_');
    if (split == std::string::npos || split == 0 || split + 1 >= id.size()) return false;
    for (size_t i = 0; i < split; ++i) if (id[i] < '0' || id[i] > '9') return false;
    for (size_t i = split + 1; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (!(std::isalnum(c) || c == '.' || c == '_')) return false;
    }
    return true;
}
std::string userDir(const std::string& root, int user) { return root + "/u" + std::to_string(user); }

bool mkdirs(const std::string& path) {
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty() && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
        if (i < path.size()) current += path[i];
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

bool writeAll(int fd, const char* data, size_t size) {
    while (size) {
        const ssize_t n = ::write(fd, data, size);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        data += n; size -= static_cast<size_t>(n);
    }
    return true;
}
bool writeAtomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = writeAll(fd, text.data(), text.size()) && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}
bool readFile(const std::string& path, std::string* text) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    text->clear();
    char buffer[16384];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) { if (errno == EINTR) continue; ::close(fd); return false; }
        if (n == 0) break;
        text->append(buffer, static_cast<size_t>(n));
        if (text->size() > (64u << 20)) { ::close(fd); return false; }
    }
    ::close(fd);
    return true;
}
std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return names;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}
// Parses "u<N>"; -1 when the name is something else.
int userOf(const std::string& name) {
    if (name.size() < 2 || name[0] != 'u') return -1;
    int user;
    return parseInt32(name.substr(1), &user) && user >= 0 && name.find_first_not_of("0123456789", 1) == std::string::npos ? user : -1;
}

// ---- Text format -----------------------------------------------------------------------------------
std::string sanitized(std::string text) {
    for (auto& c : text) if (c == ',' || c == '\n' || c == '\r' || c == ';') c = '_';
    return text;
}
std::string formatRow(const Sample& s) {
    std::string row;
    auto add = [&](const std::string& v) { if (!row.empty()) row += ','; row += v; };
    add(num(s.timeMs)); add(fixed1(s.fps)); add(fixed1(s.fpsMin)); add(num(s.jankFrames)); add(num(s.frames));
    for (int v : s.cur) add(num(v));
    for (int v : s.cap) add(num(v));
    add(num(s.skinMilliC)); add(num(s.cpuMilliC)); add(num(s.gpuMilliC));
    add(num(s.powerMw)); add(num(s.band)); add(num(s.thermalCase)); add(num(s.flags)); add(num(s.mode));
    add(num(s.profileGeneration)); add(num(s.sceneId)); add(num(s.maxFrameTimeUs));
    add(num(s.cpuUtilPermille[0])); add(num(s.cpuUtilPermille[1]));
    add(num(s.gpuBusyPercent)); add(num(s.batteryMilliC)); add(num(s.batteryPercent));
    add(exact(s.requestedFps)); add(exact(s.throttleFps));
    row += '\n';
    return row;
}
// Version 1 rows have 20 or 27 columns, version 2 rows 29.
bool parseRow(const std::string& line, int version, Sample* out) {
    const auto f = split(line, ',');
    if (version == 1 ? f.size() != kOldColumns && f.size() != kV1Columns : f.size() != kColumnCount) return false;
    Sample s;
    int* ints[] = {&s.timeMs, nullptr, nullptr, &s.jankFrames, &s.frames, &s.cur[0], &s.cur[1], &s.cur[2],
                   &s.cap[0], &s.cap[1], &s.cap[2], &s.skinMilliC, &s.cpuMilliC, &s.gpuMilliC, &s.powerMw, &s.band,
                   &s.thermalCase, &s.flags, &s.mode, &s.profileGeneration, &s.sceneId, &s.maxFrameTimeUs,
                   &s.cpuUtilPermille[0], &s.cpuUtilPermille[1], &s.gpuBusyPercent, &s.batteryMilliC, &s.batteryPercent};
    for (size_t i = 0; i < f.size(); ++i) {
        if (i == 1) { if (!parseFloat(f[i], &s.fps)) return false; }
        else if (i == 2) { if (!parseFloat(f[i], &s.fpsMin)) return false; }
        else if (i == 27) { if (!parseFloat(f[i], &s.requestedFps)) return false; }
        else if (i == 28) { if (!parseFloat(f[i], &s.throttleFps)) return false; }
        else if (!parseInt32(f[i], ints[i])) return false;
    }
    *out = s;
    return true;
}

std::string formatProfile(const ProfileView& p) {
    auto list = [](const std::vector<int>& v) {
        std::vector<std::string> parts;
        for (int x : v) parts.push_back(num(x));
        return join(parts, ';');
    };
    return sanitized(p.profileKey) + "," + num(p.baseMode) + "," + (p.tuned ? "1" : "0") + "," + num(p.thermalCase) + "," +
           num(p.uclampMin) + "," + num(p.gpuDefaultHz) + "," + (p.keepCoresOnline ? "1" : "0") + "," +
           list(p.frameBoostSteps) + "," + list(p.jankBoostSteps) + "," + num(p.startDelayMs) + "," +
           num(p.threadRules) + "," + num(p.threadRuleCount) + "," + num(p.sceneCount);
}
bool parseList(const std::string& text, std::vector<int>* out) {
    out->clear();
    if (text.empty()) return true;
    for (const auto& part : split(text, ';')) {
        int v;
        if (!parseInt32(part, &v)) return false;
        out->push_back(v);
    }
    return true;
}
bool parseProfile(const std::string& text, ProfileView* p) {
    const auto f = split(text, ',');
    if (f.size() != 13) return false;
    int tuned, keep;
    if (f[0].empty() || !parseInt32(f[1], &p->baseMode) || !parseInt32(f[2], &tuned) || !parseInt32(f[3], &p->thermalCase) ||
        !parseInt32(f[4], &p->uclampMin) || !parseInt32(f[5], &p->gpuDefaultHz) || !parseInt32(f[6], &keep) ||
        !parseList(f[7], &p->frameBoostSteps) || !parseList(f[8], &p->jankBoostSteps) || !parseInt32(f[9], &p->startDelayMs) ||
        !parseInt32(f[10], &p->threadRules) || !parseInt32(f[11], &p->threadRuleCount) || !parseInt32(f[12], &p->sceneCount))
        return false;
    p->profileKey = f[0];
    p->tuned = tuned != 0;
    p->keepCoresOnline = keep != 0;
    return true;
}
std::string formatStep(const Step& s) {
    std::string line = temperature(s.temperature);
    for (const auto& u : s.units) line += "," + num(u.floor) + "," + num(u.ceiling);
    return line;
}
bool parseStep(const std::string& text, Step* s) {
    const auto f = split(text, ',');
    if (f.size() != 7 || !parseDouble(f[0], &s->temperature)) return false;
    for (int r = 0; r < 3; ++r)
        if (!parseInt32(f[1 + 2 * r], &s->units[r].floor) || !parseInt32(f[2 + 2 * r], &s->units[r].ceiling)) return false;
    return true;
}

std::string formatSummary(const Summary& s) {
    std::vector<std::string> f;
    f.push_back(num(s.validSamples)); f.push_back(exact(s.avgFps)); f.push_back(exact(s.medianFps));
    f.push_back(exact(s.lowFps1)); f.push_back(exact(s.lowFps5)); f.push_back(num(s.targetFps));
    f.push_back(exact(s.hitRate)); f.push_back(exact(s.stability)); f.push_back(exact(s.jankPercent));
    f.push_back(exact(s.avgSkinC)); f.push_back(exact(s.maxSkinC)); f.push_back(exact(s.maxCpuC)); f.push_back(exact(s.maxGpuC));
    f.push_back(num(s.avgPowerMw));
    for (int v : s.avgCur) f.push_back(num(v));
    for (float v : s.pegged) f.push_back(exact(v));
    f.push_back(exact(s.throttled)); f.push_back(s.mixed ? "1" : "0");
    f.push_back(exact(s.floor5Fps)); f.push_back(exact(s.tail95)); f.push_back(exact(s.jankPer1000));
    f.push_back(exact(s.floorRatio));
    std::vector<std::string> scenes;
    for (int id : s.scenesSeen) scenes.push_back(num(id));
    f.push_back(join(scenes, ';'));
    return join(f, ',');
}
bool parseSummary(const std::string& text, Summary* out) {
    const auto f = split(text, ',');
    if (f.size() != 22 && f.size() != 26 && f.size() != 27) return false;
    Summary s;
    int mixed = 0;
    float* floats[] = {nullptr, &s.avgFps, &s.medianFps, &s.lowFps1, &s.lowFps5, nullptr, &s.hitRate, &s.stability,
                       &s.jankPercent, &s.avgSkinC, &s.maxSkinC, &s.maxCpuC, &s.maxGpuC};
    for (size_t i = 0; i < 13; ++i) {
        if (floats[i]) { if (!parseFloat(f[i], floats[i])) return false; }
    }
    if (!parseInt32(f[0], &s.validSamples) || !parseInt32(f[5], &s.targetFps) || !parseInt32(f[13], &s.avgPowerMw) ||
        !parseInt32(f[14], &s.avgCur[0]) || !parseInt32(f[15], &s.avgCur[1]) || !parseInt32(f[16], &s.avgCur[2]) ||
        !parseFloat(f[17], &s.pegged[0]) || !parseFloat(f[18], &s.pegged[1]) || !parseFloat(f[19], &s.pegged[2]) ||
        !parseFloat(f[20], &s.throttled) || !parseInt32(f[21], &mixed)) return false;
    s.mixed = mixed != 0;
    if (f.size() >= 26) {
        // Version 2 adds floorRatio before the scene list; older summaries leave it unknown (-1).
        const size_t scenes = f.size() == 27 ? 26 : 25;
        if (!parseFloat(f[22], &s.floor5Fps) || !parseFloat(f[23], &s.tail95) || !parseFloat(f[24], &s.jankPer1000) ||
            (f.size() == 27 && !parseFloat(f[25], &s.floorRatio)) || !parseList(f[scenes], &s.scenesSeen)) return false;
    } else {
        s.floor5Fps = s.lowFps5;
    }
    *out = s;
    return true;
}

struct Meta {
    int user = 0; std::string package; int mode = 1; int64_t startEpochMs = 0; const ProfileView* profile = nullptr;
};
std::string headText(const Meta& m, int endReason) {
    std::string t = std::string(kMagic) + "\nuser=" + num(m.user) + "\npackage=" + m.package + "\nmode=" + num(m.mode) +
                    "\nstart=" + num(m.startEpochMs) + "\nprofile=" + formatProfile(*m.profile) + "\n";
    for (const auto& step : m.profile->steps) t += "step=" + formatStep(step) + "\n";
    const ProfileView& p = *m.profile;
    for (const auto& scene : p.scenes)
        t += "scene=" + num(scene.sceneId) + "," + num(scene.enabled) + "," +
             formatStep(Step{static_cast<double>(scene.holdMs), scene.units}) + "\n";
    for (const auto& thread : p.threads)
        t += "thread=" + std::to_string(thread.mask) + "," + num(thread.rtPriority) + "," + thread.name + "\n";
    t += "dropWindow=" + num(p.dropLowerPermille) + "," + num(p.dropUpperPermille) + "\n";
    t += "expert=" + num(p.boostLengthMs) + "," + num(p.dropSensitivity) + "," + num(p.hysteresisDeciC) + "," +
         num(p.skinTargetMilliC) + "," + num(p.powerTargetMw) + "," + num(p.sceneMode) + "\n";
    std::string id = p.variantId, name = p.variantName;
    for (auto* text : {&id, &name}) text->erase(std::remove_if(text->begin(), text->end(),
        [](char c) { return c == '\n' || c == '\r'; }), text->end());
    t += "variant=" + id + "\nvariantName=" + name + "\n";
    if (endReason) t += "end=" + num(endReason) + "\n";
    t += std::string(kSamplesLine) + "\n" + kColumns + "\n";
    return t;
}

struct Parsed {
    RecordHeader h;
    bool hasEnd = false, hasSummary = false;
    int version = 2;
    std::vector<Sample> rows;
};
bool parseRecord(const std::string& text, bool partial, Parsed* out) {
    std::vector<std::string> lines = split(text, '\n');
    // A torn final line of a partial file is dropped; a complete file ends with a newline.
    if (!lines.empty() && (lines.back().empty() || partial)) lines.pop_back();
    if (lines.empty() || (lines[0] != kMagic && lines[0] != kMagicV1)) return false;
    Parsed p;
    p.version = lines[0] == kMagicV1 ? 1 : 2;
    size_t i = 1;
    bool sawSamples = false, hasPackage = false, hasStart = false, hasProfile = false;
    for (; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line == kSamplesLine) { sawSamples = true; ++i; break; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "user") { if (!parseInt32(value, &p.h.userId)) return false; }
        else if (key == "package") { p.h.package = value; hasPackage = !value.empty(); }
        else if (key == "mode") { if (!parseInt32(value, &p.h.mode)) return false; }
        else if (key == "start") { long long v; if (!parseInt(value, &v) || v < 0) return false; p.h.startEpochMs = v; hasStart = true; }
        else if (key == "profile") { if (!parseProfile(value, &p.h.profile)) return false; hasProfile = true; }
        else if (key == "step") { Step s; if (!parseStep(value, &s)) return false; p.h.profile.steps.push_back(s); }
        else if (key == "scene") {
            const size_t a = value.find(','), b = value.find(',', a == std::string::npos ? a : a + 1);
            SceneRowRec scene; Step step; int enabled;
            if (a == std::string::npos || b == std::string::npos || !parseInt32(value.substr(0, a), &scene.sceneId) ||
                !parseInt32(value.substr(a + 1, b - a - 1), &enabled) || !parseStep(value.substr(b + 1), &step) ||
                step.temperature < 0 || step.temperature > INT32_MAX) return false;
            scene.enabled = enabled != 0; scene.holdMs = static_cast<int>(step.temperature); scene.units = step.units;
            p.h.profile.scenes.push_back(scene);
        }
        else if (key == "thread") {
            const size_t a = value.find(','), b = value.find(',', a == std::string::npos ? a : a + 1);
            ThreadRuleRec thread;
            if (a == std::string::npos || b == std::string::npos ||
                !parseInt32(value.substr(a + 1, b - a - 1), &thread.rtPriority)) return false;
            const std::string mask = value.substr(0, a);
            char* end = nullptr; errno = 0;
            thread.mask = std::strtoull(mask.c_str(), &end, 10);
            if (mask.empty() || mask[0] == '-' || errno || *end) return false;
            thread.name = value.substr(b + 1);
            p.h.profile.threads.push_back(std::move(thread));
        }
        else if (key == "end") { if (!parseInt32(value, &p.h.endReason)) return false; p.hasEnd = true; }
        else if (key == "dropWindow") {
            const auto f = split(value, ',');
            if (f.size() != 2 || !parseInt32(f[0], &p.h.profile.dropLowerPermille) ||
                !parseInt32(f[1], &p.h.profile.dropUpperPermille)) return false;
            if (!validDropWindow(p.h.profile.dropLowerPermille, p.h.profile.dropUpperPermille))
                p.h.profile.dropLowerPermille = p.h.profile.dropUpperPermille = -1;
        }
        else if (key == "expert") {
            const auto f = split(value, ',');
            ProfileView& pv = p.h.profile;
            if (f.size() != 6 || !parseInt32(f[0], &pv.boostLengthMs) || !parseInt32(f[1], &pv.dropSensitivity) ||
                !parseInt32(f[2], &pv.hysteresisDeciC) || !parseInt32(f[3], &pv.skinTargetMilliC) ||
                !parseInt32(f[4], &pv.powerTargetMw) || !parseInt32(f[5], &pv.sceneMode)) return false;
        }
        else if (key == "variant") p.h.profile.variantId = value;
        else if (key == "variantName") p.h.profile.variantName = value;
    }
    if (!sawSamples || !hasPackage || !hasStart || !hasProfile || i >= lines.size()) return false;
    ++i;   // Column names.
    for (; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.compare(0, 8, "summary=") == 0) {
            if (!parseSummary(line.substr(8), &p.h.summary)) return false;
            p.hasSummary = true;
            if (i + 1 != lines.size()) return false;
            break;
        }
        Sample s;
        if (!parseRow(line, p.version, &s)) {
            if (partial && i + 1 == lines.size()) break;
            return false;
        }
        p.rows.push_back(s);
    }
    p.h.sampleCount = static_cast<int>(p.rows.size());
    p.h.durationMs = p.rows.empty() ? 0 : p.rows.back().timeMs;
    *out = std::move(p);
    return true;
}
// Header and summary of a finished file without parsing its sample rows: only the row count and the last row
// (for the duration) are taken from them.
bool parseHeaderOnly(const std::string& text, Parsed* out) {
    const size_t summary = text.rfind("\nsummary=");
    const std::string marker = std::string("\n") + kSamplesLine + "\n";
    const size_t samples = text.find(marker);
    if (summary == std::string::npos || samples == std::string::npos || samples >= summary) return false;
    const size_t columnsEnd = text.find('\n', samples + marker.size());
    if (columnsEnd == std::string::npos || columnsEnd > summary) return false;
    const size_t count = static_cast<size_t>(std::count(text.begin() + static_cast<std::ptrdiff_t>(columnsEnd) + 1,
                                                        text.begin() + static_cast<std::ptrdiff_t>(summary) + 1, '\n'));
    // Head, columns line and summary line only; the rows are skipped.
    Parsed parsed;
    if (!parseRecord(text.substr(0, columnsEnd + 1) + text.substr(summary + 1), false, &parsed)) return false;
    int duration = 0;
    if (count > 0) {
        const size_t lastStart = text.rfind('\n', summary - 1) + 1;
        Sample last;
        if (!parseRow(text.substr(lastStart, summary - lastStart), parsed.version, &last)) return false;
        duration = last.timeMs;
    }
    parsed.h.sampleCount = static_cast<int>(count);
    parsed.h.durationMs = duration;
    *out = std::move(parsed);
    return true;
}
} // namespace

Recorder::Recorder(std::string root) : mRoot(std::move(root)) {
    while (mRoot.size() > 1 && mRoot.back() == '/') mRoot.pop_back();
}

void Recorder::retain(int user) {
    const std::string dir = userDir(mRoot, user);
    struct Entry { long long start; std::string name; };
    std::vector<Entry> records;
    for (const auto& name : listDir(dir)) {
        if (!endsWith(name, kRecSuffix)) continue;
        const std::string id = name.substr(0, name.size() - 4);
        if (!validId(id)) continue;
        long long start;
        if (parseInt(id.substr(0, id.find('_')), &start)) records.push_back({start, name});
    }
    std::sort(records.begin(), records.end(), [](const Entry& a, const Entry& b) {
        return a.start != b.start ? a.start < b.start : a.name < b.name;
    });
    for (size_t i = 0; records.size() - i > kMaxRecords; ++i) ::unlink((dir + "/" + records[i].name).c_str());
}

void Recorder::recover(const Ladders& ladders) {
    try {
        for (const auto& entry : listDir(mRoot)) {
            const int user = userOf(entry);
            if (user < 0) continue;
            const std::string dir = mRoot + "/" + entry;
            for (const auto& name : listDir(dir)) {
                const std::string path = dir + "/" + name;
                if (endsWith(name, ".tmp")) { ::unlink(path.c_str()); continue; }
                if (!endsWith(name, kPartialSuffix)) continue;
                const std::string id = name.substr(0, name.size() - std::strlen(kPartialSuffix));
                const std::string finalPath = dir + "/" + id + kRecSuffix;
                std::string text;
                Parsed parsed;
                if (!validId(id) || !readFile(path, &text) || !parseRecord(text, true, &parsed) ||
                    parsed.rows.empty() || parsed.rows.back().timeMs < kMinDurationMs) {
                    ::unlink(path.c_str());
                    continue;
                }
                if (exists(finalPath)) { ::unlink(path.c_str()); continue; }
                Meta meta;
                meta.user = user; meta.package = parsed.h.package; meta.mode = parsed.h.mode;
                meta.startEpochMs = parsed.h.startEpochMs; meta.profile = &parsed.h.profile;
                std::string out = headText(meta, kEndInterrupted);
                for (const auto& row : parsed.rows) out += formatRow(row);
                out += "summary=" + formatSummary(summarize(parsed.rows, ladders)) + "\n";
                if (writeAtomic(finalPath, out)) ::unlink(path.c_str());
            }
            retain(user);
        }
    } catch (...) {
    }
}

void Recorder::start(int user, const std::string& pkg, int pid, int64_t startTicks, int mode,
                     const ProfileView& profile, int64_t nowMs, int64_t epochMs) {
    try {
        if (mSession) flush(*mSession);   // The caller should have finished it; the partial file is recovered later.
        mSession.reset();
        if (user < 0 || !validPackage(pkg) || epochMs < 0) return;
        const std::string dir = userDir(mRoot, user);
        if (!mkdirs(dir)) return;
        Session s;
        s.user = user; s.package = pkg; s.pid = pid; s.startTicks = startTicks; s.mode = mode; s.profile = profile;
        s.startNowMs = nowMs;
        for (int tries = 0; tries < 1000; ++tries, ++epochMs) {
            s.id = std::to_string(epochMs) + "_" + pkg;
            if (!exists(dir + "/" + s.id + kRecSuffix) && !exists(dir + "/" + s.id + kPartialSuffix)) break;
        }
        s.startEpochMs = epochMs;
        s.nextDue = nowMs + kPeriodMs;
        s.samples.reserve(512);
        Meta meta{user, pkg, mode, epochMs, &s.profile};
        writeAtomic(dir + "/" + s.id + kPartialSuffix, headText(meta, 0));
        mSession = std::move(s);
    } catch (...) {
        mSession.reset();
    }
}

bool Recorder::resume(int user, const std::string& pkg, int pid, int64_t startTicks, int64_t nowMs) {
    if (!mSession || !mSession->paused) return false;
    Session& s = *mSession;
    if (s.endReason == kEndLimit || nowMs - s.pausedAt >= kPauseMs) return false;
    if (s.user != user || s.package != pkg || s.pid != pid || s.startTicks != startTicks) return false;
    s.paused = false;
    s.endReason = 0;
    s.nextDue = nowMs + kPeriodMs;
    resetSecond(s);
    return true;
}

void Recorder::pause(int64_t nowMs, int reason) {
    try {
        if (!mSession || mSession->paused) return;
        Session& s = *mSession;
        s.paused = true;
        s.pausedAt = nowMs;
        s.endReason = reason;
        resetSecond(s);
        flush(s);
    } catch (...) {
    }
}

void Recorder::finish(const Ladders& ladders) {
    try {
        if (!mSession) return;
        Session s = std::move(*mSession);
        mSession.reset();
        const std::string dir = userDir(mRoot, s.user);
        const std::string partial = dir + "/" + s.id + kPartialSuffix;
        if (s.samples.empty() || s.samples.back().timeMs < kMinDurationMs) {
            ::unlink(partial.c_str());
            return;
        }
        Meta meta{s.user, s.package, s.mode, s.startEpochMs, &s.profile};
        std::string text = headText(meta, s.endReason ? s.endReason : kEndStopped);
        for (const auto& row : s.samples) text += formatRow(row);
        text += "summary=" + formatSummary(summarize(s.samples, ladders)) + "\n";
        mkdirs(dir);
        if (writeAtomic(dir + "/" + s.id + kRecSuffix, text)) {
            ::unlink(partial.c_str());
            retain(s.user);
        } else {
            flush(s);   // Keep everything for the next recover().
        }
    } catch (...) {
    }
}

bool Recorder::active() const { return mSession && !mSession->paused; }
bool Recorder::paused() const { return mSession && mSession->paused; }

std::optional<int64_t> Recorder::deadline() const {
    if (!mSession) return std::nullopt;
    return mSession->paused ? mSession->pausedAt + kPauseMs : mSession->nextDue;
}
bool Recorder::due(int64_t nowMs) const { return active() && nowMs >= mSession->nextDue; }
bool Recorder::expired(int64_t nowMs) const { return paused() && nowMs - mSession->pausedAt >= kPauseMs; }

void Recorder::resetSecond(Session& s) {
    s.fpsSum = 0; s.fpsCount = 0; s.fpsMin = -1; s.jank = 0; s.frames = 0; s.maxFrameTimeUs = -1;
    s.requestedFps = s.throttleFps = -1;
}

void Recorder::frame(float fps, int jankFrames, int frames, int maxFrameTimeUs, float requestedFps, float throttleFps) {
    if (!active() || !std::isfinite(fps) || fps < 0) return;
    Session& s = *mSession;
    s.fpsSum += fps;
    s.fpsMin = s.fpsCount == 0 ? fps : std::min(s.fpsMin, fps);
    ++s.fpsCount;
    if (jankFrames > 0) s.jank += std::min(jankFrames, INT32_MAX - s.jank);
    if (frames > 0) s.frames += std::min(frames, INT32_MAX - s.frames);
    if (maxFrameTimeUs >= 0) s.maxFrameTimeUs = std::max(s.maxFrameTimeUs, maxFrameTimeUs);
    s.requestedFps = std::isfinite(requestedFps) && requestedFps > 0 ? requestedFps : -1;
    s.throttleFps = std::isfinite(throttleFps) && throttleFps > 0 ? throttleFps : -1;
}

void Recorder::sample(Sample sample, int64_t nowMs) {
    try {
        if (!active()) return;
        Session& s = *mSession;
        if (s.samples.size() >= kMaxSamples) { pause(nowMs, kEndLimit); return; }
        sample.timeMs = static_cast<int>(nowMs - s.startNowMs);
        if (s.fpsCount > 0) {
            sample.fps = std::round(static_cast<float>(s.fpsSum / s.fpsCount) * 10.f) / 10.f;
            sample.fpsMin = std::round(s.fpsMin * 10.f) / 10.f;
        } else {
            sample.fps = sample.fpsMin = -1;
        }
        sample.jankFrames = s.jank;
        sample.frames = s.frames;
        sample.maxFrameTimeUs = s.maxFrameTimeUs;
        sample.requestedFps = s.requestedFps;
        sample.throttleFps = s.throttleFps;
        resetSecond(s);
        s.samples.push_back(sample);
        // No drift: the next sample is due one period after the previous due time, unless a whole period was missed.
        s.nextDue = nowMs - s.nextDue >= kPeriodMs ? nowMs + kPeriodMs : s.nextDue + kPeriodMs;
        if (s.samples.size() - s.flushed >= static_cast<size_t>(kFlushEvery)) flush(s);
    } catch (...) {
    }
}

void Recorder::flush(Session& s) {
    try {
        if (s.flushed >= s.samples.size()) return;
        const std::string dir = userDir(mRoot, s.user);
        const std::string path = dir + "/" + s.id + kPartialSuffix;
        // Replace the entire checkpoint atomically. A short write or failed fsync leaves the previous
        // checkpoint intact, so retries and crash recovery cannot join partial rows or duplicate samples.
        if (!mkdirs(dir)) return;
        Meta meta{s.user, s.package, s.mode, s.startEpochMs, &s.profile};
        std::string text = headText(meta, 0);
        for (const auto& row : s.samples) text += formatRow(row);
        if (writeAtomic(path, text)) s.flushed = s.samples.size();
    } catch (...) {
    }
}

bool Recorder::header(int user, const std::string& id, RecordHeader* out) const {
    try {
        if (!validId(id) || user < 0) return false;
        std::string text;
        Parsed parsed;
        if (!readFile(userDir(mRoot, user) + "/" + id + kRecSuffix, &text) || !parseHeaderOnly(text, &parsed) ||
            !parsed.hasEnd || !parsed.hasSummary) return false;
        parsed.h.id = id;
        parsed.h.userId = user;
        if (out) *out = std::move(parsed.h);
        return true;
    } catch (...) {
        return false;
    }
}

bool Recorder::read(int user, const std::string& id, std::vector<Sample>* samples) const {
    try {
        if (!validId(id) || user < 0) return false;
        std::string text;
        Parsed parsed;
        if (!readFile(userDir(mRoot, user) + "/" + id + kRecSuffix, &text) || !parseRecord(text, false, &parsed) ||
            !parsed.hasEnd || !parsed.hasSummary) return false;
        if (samples) *samples = std::move(parsed.rows);
        return true;
    } catch (...) {
        return false;
    }
}

bool Recorder::remove(int user, const std::string& id) {
    try {
        if (!validId(id) || user < 0) return false;
        return ::unlink((userDir(mRoot, user) + "/" + id + kRecSuffix).c_str()) == 0;
    } catch (...) {
        return false;
    }
}

std::vector<RecordHeader> Recorder::list(int user, const std::string& pkg) const {
    std::vector<RecordHeader> out;
    try {
        if (user < 0) return out;
        for (const auto& name : listDir(userDir(mRoot, user))) {
            if (!endsWith(name, kRecSuffix)) continue;
            RecordHeader h;
            if (!header(user, name.substr(0, name.size() - 4), &h)) continue;
            if (!pkg.empty() && h.package != pkg) continue;
            out.push_back(std::move(h));
        }
        std::sort(out.begin(), out.end(), [](const RecordHeader& a, const RecordHeader& b) {
            return a.startEpochMs != b.startEpochMs ? a.startEpochMs > b.startEpochMs : a.id > b.id;
        });
    } catch (...) {
        out.clear();
    }
    return out;
}
} // namespace gamepolicy
