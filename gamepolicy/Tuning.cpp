// SPDX-License-Identifier: Apache-2.0
#include "Tuning.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>

namespace gamepolicy {
namespace {
constexpr int kUclamp = 0x40CAC000;
constexpr int kGpuDefault = 0x42800000;
constexpr int kGoldCores = 0x41000000;
constexpr int kPrimeCores = 0x41000200;
const char* const kStockNames[] = {"Balance", "Savage", "PowerSave", "chaoshen"};

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
bool member(const std::vector<int>& ladder, int value) { return std::binary_search(ladder.begin(), ladder.end(), value); }
// Largest index whose value is at or below the request, 0 when none.
int indexAtOrBelow(const std::vector<int>& ladder, int value) {
    const auto it = std::upper_bound(ladder.begin(), ladder.end(), value);
    return it == ladder.begin() ? 0 : static_cast<int>(it - ladder.begin()) - 1;
}
bool tristate(int value) { return value == kKeep || value == 0 || value == 1; }
bool stepped(int value, int low, int high, int step) { return value >= low && value <= high && value % step == 0; }
bool validName(const std::string& name) {
    if (name.empty() || name.size() > 15) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
}
bool validVariantId(const std::string& id) {
    return id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}
bool validRecordId(const std::string& id) {
    const auto split = id.find('_');
    if (id.empty() || split == std::string::npos || split == 0 || split + 1 >= id.size()) return false;
    return std::all_of(id.begin(), id.begin() + split, [](unsigned char c) { return std::isdigit(c); }) &&
           validPackage(id.substr(split + 1));
}

bool checkUnits(const Policy& policy, const std::array<Bounds, 3>& units, const std::string& what, std::string* error) {
    for (int r = 0; r < 3; ++r) {
        const auto ladder = policy.ladder(r);
        if (ladder.empty()) return fail(error, "Policy has no runtime OPP table");
        for (int value : {units[r].floor, units[r].ceiling})
            if (value != -1 && !member(ladder, value)) return fail(error, what + ": Value is not an OPP step");
        if (units[r].floor >= 0 && units[r].ceiling >= 0 && units[r].floor > units[r].ceiling)
            return fail(error, what + ": Minimum above maximum");
    }
    return true;
}

// Zero offsets keep a row on its ladder (rounding down); other offsets move whole OPP steps.
std::array<Bounds, 3> shifted(const std::array<std::vector<int>, 3>& ladders, const std::array<Bounds, 3>& units,
                              const std::array<int, 3>& maxOffsets, const std::array<int, 3>& minOffsets) {
    std::array<Bounds, 3> result{};
    for (int r = 0; r < 3; ++r) {
        const auto& ladder = ladders[r];
        const int top = static_cast<int>(ladder.size()) - 1;
        const int ceiling = units[r].ceiling, floor = units[r].floor;
        const int base = ceiling < 0 ? top : indexAtOrBelow(ladder, ceiling);
        int high = maxOffsets[r] == 0 ? (ceiling < 0 ? -1 : ladder[base]) : ladder[std::clamp(base + maxOffsets[r], 0, top)];
        int low;
        if (minOffsets[r] == 0) low = floor < 0 ? -1 : ladder[indexAtOrBelow(ladder, floor)];
        else {
            const int n = std::clamp((floor < 0 ? 0 : indexAtOrBelow(ladder, floor)) + minOffsets[r], 0, top);
            low = n == 0 ? -1 : ladder[n];
        }
        if (low >= 0 && high >= 0 && low > high) low = high;
        result[r] = {low, high};
    }
    return result;
}

Band encode(const Policy& policy, double temperature, const std::array<Bounds, 3>& units) {
    Band band{temperature, {}, units};
    for (int r = 0; r < 3; ++r) {
        if (r < 2) {
            if (units[r].floor >= 0) band.bounds[r].floor = units[r].floor / 1000;
            if (units[r].ceiling >= 0) band.bounds[r].ceiling = units[r].ceiling / 1000;
            continue;
        }
        const auto& table = policy.tables()[2];
        auto index = [&](int hz) {
            const auto it = std::find(table.begin(), table.end(), hz);
            if (it == table.end()) throw std::runtime_error("GPU value is not in the runtime table");
            return static_cast<int>(it - table.begin());
        };
        if (units[r].floor >= 0) band.bounds[r].floor = index(units[r].floor);
        if (units[r].ceiling >= 0) band.bounds[r].ceiling = index(units[r].ceiling);
    }
    return band;
}

void setHint(std::vector<PerfHint>& hints, int opcode, int value) {
    for (auto& hint : hints) if (hint.opcode == opcode) { hint.value = value; return; }
    hints.push_back({opcode, value});
}
void removeHint(std::vector<PerfHint>& hints, int opcode) {
    hints.erase(std::remove_if(hints.begin(), hints.end(), [&](const PerfHint& h) { return h.opcode == opcode; }), hints.end());
}

bool allZero(const std::array<int, 3>& values) { return values[0] == 0 && values[1] == 0 && values[2] == 0; }
} // namespace

bool validPackage(const std::string& package) {
    return !package.empty() && package.size() <= 255 && std::all_of(package.begin(), package.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_';
    });
}

const char* baseModeName(int baseMode) {
    return baseMode >= kBaseBalanced && baseMode <= kBaseMaximum ? kStockNames[baseMode - 1] : nullptr;
}

int baseModeOf(const std::string& modeName) {
    for (int i = 0; i < 4; ++i) if (modeName == kStockNames[i]) return i + 1;
    return 0;
}

int gameModeToBase(const Policy& policy, int gameMode) {
    const auto it = policy.overlay.modes.find(gameMode);
    const int base = it == policy.overlay.modes.end() ? 0 : baseModeOf(it->second);
    return base ? base : kBaseBalanced;
}

std::vector<Step> stockSteps(const Mode& mode) {
    std::vector<Step> steps;
    if (mode.bands.empty()) return steps;
    const int positive = Policy::positiveCount(mode);
    const size_t rows = positive == 3 || positive == 4 ? positive : 1;
    for (size_t i = 0; i < rows; ++i) steps.push_back({i == 0 ? 0 : mode.bands[i].temperature, mode.bands[i].units});
    return steps;
}

bool normalize(const Policy& policy, TuningRecord& r, std::string* error) {
    try {
        const auto runtime = policy.archiveRuntime();
        const Policy& hardware = runtime ? *runtime : policy;
        if (!validPackage(r.package)) return fail(error, "Invalid package name");
        if (r.mode < 1 || r.mode > 3) return fail(error, "Invalid game mode");
        if (r.baseProfile.empty()) r.baseProfile = policy.select(r.package, r.mode).profile;
        const auto base = policy.games.find(r.baseProfile);
        const auto archived = policy.archived(r.baseProfile);
        const bool reserved = r.baseProfile.rfind("@builtin/", 0) == 0;
        const bool missingArchive = reserved && !archived;
        if (base == policy.games.end() && !archived && !reserved) return fail(error, "Unknown base profile");
        const char* baseName = baseModeName(r.baseMode);
        if (!baseName) return fail(error, "Invalid base mode");
        if (!reserved && r.baseMode == kBaseMaximum && !base->second.count(baseName)) return fail(error, "Base profile has no maximum mode");
        for (int i = 0; i < 3; ++i) {
            if (r.maxOffsets[i] < -10 || r.maxOffsets[i] > 10 || r.minOffsets[i] < -10 || r.minOffsets[i] > 10)
                return fail(error, "Offset out of range");
        }
        if (r.minOffsets[1] > 2) return fail(error, "Prime minimum offset above +2");
        if (r.steps.size() > 4) return fail(error, "Too many steps");
        double previous = 0;
        for (size_t i = 0; i < r.steps.size(); ++i) {
            auto& step = r.steps[i];
            if (i == 0) step.temperature = 0;
            else {
                const double t = step.temperature;
                if (!std::isfinite(t) || t < 20.0 || t > 60.0 || std::fabs(t * 2 - std::round(t * 2)) > 1e-9)
                    return fail(error, "Step temperature out of range");
                if (t <= previous) return fail(error, "Step temperatures must increase");
                previous = t;
            }
            if (!missingArchive && !checkUnits(hardware, step.units, "Step " + std::to_string(i), error)) return false;
        }
        const int c = r.thermalCase;
        if (c != kKeep && c != 100 && c != 200 && c != 300 && c != 400) return fail(error, "Invalid thermal case");
        if (r.uclampMin != kKeep && r.uclampMin != -1 && !stepped(r.uclampMin, 0, 100, 5)) return fail(error, "Invalid uclamp");
        if (r.gpuDefaultHz != kKeep && r.gpuDefaultHz != -1) {
            const auto gpu = hardware.ladder(2);
            if (!missingArchive && !member(gpu, r.gpuDefaultHz)) return fail(error, "GPU default is not an OPP step");
        }
        if (!tristate(r.keepCoresOnline) || !tristate(r.frameBoost) || !tristate(r.jankBoost))
            return fail(error, "Invalid switch value");
        for (const auto* list : {&r.frameBoostSteps, &r.jankBoostSteps}) {
            if (!list->empty() && list->size() != 3) return fail(error, "Boost steps need three values");
            for (int v : *list) if (v < 0 || v > 10) return fail(error, "Boost step out of range");
        }
        if (r.startDelayMs != kKeep && !stepped(r.startDelayMs, 0, 30000, 1000)) return fail(error, "Invalid start delay");
        if (r.threadRules < kThreadsOff || r.threadRules > kThreadsCustom) return fail(error, "Invalid thread rules");
        if (r.sceneMode != kKeep && r.sceneMode != 0 && r.sceneMode != 1) return fail(error, "Invalid scene mode");
        if (r.scenes.size() > 32) return fail(error, "Too many scene rows");
        std::set<int> ids;
        for (const auto& row : r.scenes) {
            if (row.sceneId < 0 || row.sceneId > 100000 || !ids.insert(row.sceneId).second) return fail(error, "Invalid scene id");
            if (row.holdMs != 0 && !stepped(row.holdMs, 100, 10000, 100)) return fail(error, "Invalid scene hold time");
            if (!missingArchive && !checkUnits(hardware, row.units, "Scene " + std::to_string(row.sceneId), error)) return false;
        }
        if (r.customThreads.size() > 16) return fail(error, "Too many thread rules");
        int realtime = 0;
        for (const auto& rule : r.customThreads) {
            if (!validName(rule.name)) return fail(error, "Invalid thread name");
            if (!missingArchive && (!rule.mask || !hardware.presentMask() || (rule.mask & ~hardware.presentMask())))
                return fail(error, "Invalid thread CPU mask");
            if (rule.rtPriority != -1 && (rule.rtPriority < 90 || rule.rtPriority > 99))
                return fail(error, "Invalid thread priority");
            realtime += rule.rtPriority >= 90;
        }
        if (realtime > 8) return fail(error, "Too many real-time thread rules");
        if (r.boostLengthMs != kKeep && !stepped(r.boostLengthMs, 100, 5000, 100)) return fail(error, "Invalid boost length");
        if (r.dropSensitivity != kKeep && (r.dropSensitivity < 1 || r.dropSensitivity > 3)) return fail(error, "Invalid drop sensitivity");
        if ((r.dropLowerPermille != -1 || r.dropUpperPermille != -1) &&
            !validDropWindow(r.dropLowerPermille, r.dropUpperPermille)) return fail(error, "Invalid drop window");
        if (r.hysteresisDeciC != kKeep && (r.hysteresisDeciC < 0 || r.hysteresisDeciC > 30)) return fail(error, "Invalid hysteresis");
        if (r.skinTargetMilliC != kKeep && r.skinTargetMilliC != -1 && !stepped(r.skinTargetMilliC, 35000, 48000, 500))
            return fail(error, "Invalid skin target");
        if (r.powerTargetMw != kKeep && r.powerTargetMw != -1 && !stepped(r.powerTargetMw, 2000, 12000, 500))
            return fail(error, "Invalid power target");
        if (!validVariantId(r.variantId)) return fail(error, "Invalid variant id");
        if (r.variantName.size() > 64 || std::any_of(r.variantName.begin(), r.variantName.end(),
                [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) return fail(error, "Invalid variant name");
        if (r.pendingProbe) {
            const auto& probe = *r.pendingProbe;
            if (probe.resource < -1 || probe.resource > 2 || probe.deltaSteps < -10 || probe.deltaSteps > 10 ||
                probe.armedAtEpochMs < 0 || probe.baselineRevision < 0 || probe.analysisVersion < 0 || probe.analysisTargetFps < 0 || !std::isfinite(probe.renderScale) || !std::isfinite(probe.refreshRate) ||
                (probe.renderScale != -1 && (probe.renderScale <= 0 || probe.renderScale > 1)) ||
                (probe.refreshRate != -1 && probe.refreshRate <= 0) || probe.diagnosis < 0 || probe.diagnosis > 6 || (!probe.baseline.empty() && !validRecordId(probe.baseline)))
                return fail(error, "Invalid pending probe");
        }
        return true;
    } catch (const std::exception& e) { return fail(error, e.what()); }
}

bool build(const Policy& policy, const TuningRecord& input, Mode* out, Selection* selection, std::string* error) {
    try {
        if (!input.enabled) return fail(error, "disabled");
        if (!out) return fail(error, "No output mode");
        TuningRecord r = input;
        if (!normalize(policy, r, error)) return false;
        const auto archived = policy.archived(r.baseProfile);
        const auto runtime = policy.archiveRuntime();
        const Policy& hardware = runtime ? *runtime : policy;
        if (!archived && r.baseProfile.rfind("@builtin/", 0) == 0) {
            const auto current = policy.select(r.package, r.mode);
            *out = *current.mode;
            if (selection) *selection = {out, current.profile, current.threadPlacement};
            return true;
        }
        const Selection own = archived ? Selection{&archived->mode, archived->profile, archived->placement}
                                       : policy.select(r.package, r.mode);
        const Mode& ownMode = *own.mode;
        const Mode* basePtr = &ownMode;
        if (!archived) {
            const auto& baseModes = hardware.games.at(r.baseProfile);
            auto found = baseModes.find(baseModeName(r.baseMode));
            if (found == baseModes.end()) found = baseModes.find("Balance");
            basePtr = &found->second;
        }
        const Mode& base = *basePtr;
        // An untouched archive must keep the original band algorithm, boost rows, exact
        // hints, placement and gating identity, including values absent from PolicyProfile.
        if (archived && r.steps.empty() && allZero(r.maxOffsets) && allZero(r.minOffsets) &&
            r.thermalCase == kKeep && r.uclampMin == kKeep && r.gpuDefaultHz == kKeep &&
            r.keepCoresOnline == kKeep && r.frameBoost == kKeep && r.jankBoost == kKeep &&
            r.frameBoostSteps.empty() && r.jankBoostSteps.empty() && r.startDelayMs == kKeep &&
            r.threadRules == kThreadsOwn && r.sceneMode == kKeep && r.scenes.empty() &&
            r.customThreads.empty() && r.boostLengthMs == kKeep && r.dropSensitivity == kKeep &&
            r.dropLowerPermille == -1 && r.dropUpperPermille == -1 &&
            r.hysteresisDeciC == kKeep && r.skinTargetMilliC == kKeep && r.powerTargetMw == kKeep) {
            *out = base;
            if (selection) *selection = {out, own.profile, own.threadPlacement};
            return true;
        }
        std::array<std::vector<int>, 3> ladders;
        for (int i = 0; i < 3; ++i) {
            ladders[i] = hardware.ladder(i);
            if (ladders[i].empty()) return fail(error, "Policy has no runtime OPP table");
        }

        std::vector<Step> steps = r.steps.empty() ? stockSteps(base) : r.steps;
        for (auto& step : steps) step.units = shifted(ladders, step.units, r.maxOffsets, r.minOffsets);

        Mode m = base;
        m.linearBands = true;
        m.bands.clear();
        for (const auto& step : steps) m.bands.push_back(encode(hardware, step.temperature, step.units));

        const auto chosen = [](int on, const std::vector<int>& list, const std::optional<std::array<int, 3>>& stock) -> std::optional<std::array<int, 3>> {
            if (on == kKeep) return stock;
            if (on == 0) return std::nullopt;
            if (list.size() == 3) return std::array<int, 3>{list[0], list[1], list[2]};
            return stock;
        };
        m.fpsAdjust = chosen(r.frameBoost, r.frameBoostSteps, base.fpsAdjust);
        m.jankAdjust = chosen(r.jankBoost, r.jankBoostSteps, base.jankAdjust);
        if (r.jankBoost == 0) m.jankAdjustTime = 0;

        // Without edits the stock rows are reused verbatim, which keeps parity with a runtime table
        // that differs from the declared one.
        const bool untouched = r.steps.empty() && allZero(r.maxOffsets) && allZero(r.minOffsets);
        const auto boosted = [&](const std::vector<Band>& stockRows, bool keep, const std::optional<std::array<int, 3>>& deltas) {
            std::vector<Band> rows;
            if (untouched && keep && stockRows.size() >= steps.size()) {
                rows.assign(stockRows.begin(), stockRows.begin() + steps.size());
                for (size_t i = 0; i < rows.size(); ++i) rows[i].temperature = steps[i].temperature;
                return rows;
            }
            for (const auto& step : steps) {
                auto units = step.units;
                if (deltas) for (int i = 0; i < 3; ++i) {
                    const int d = (*deltas)[i];
                    if (d <= 0) continue;
                    const auto& ladder = ladders[i];
                    const int top = static_cast<int>(ladder.size()) - 1;
                    const int high = ladder[std::min(units[i].ceiling < 0 ? top : indexAtOrBelow(ladder, units[i].ceiling) + d, top)];
                    units[i].ceiling = high;
                    if (units[i].floor >= 0) {
                        units[i].floor = ladder[std::min(indexAtOrBelow(ladder, units[i].floor) + d, top)];
                        units[i].floor = std::min(units[i].floor, high);
                    }
                }
                rows.push_back(encode(hardware, step.temperature, units));
            }
            return rows;
        };
        m.boostedBands = boosted(base.boostedBands, r.frameBoost == kKeep, m.fpsAdjust);
        m.jankBands = boosted(base.jankBands, r.jankBoost == kKeep, m.jankAdjust);

        if (r.uclampMin == -1) removeHint(m.perfHints, kUclamp);
        else if (r.uclampMin != kKeep) setHint(m.perfHints, kUclamp, r.uclampMin);
        if (r.gpuDefaultHz == -1) removeHint(m.perfHints, kGpuDefault);
        else if (r.gpuDefaultHz != kKeep) {
            const auto& table = hardware.tables()[2];
            const auto it = std::find(table.begin(), table.end(), r.gpuDefaultHz);
            if (it == table.end()) return fail(error, "GPU default is not in the runtime table");
            setHint(m.perfHints, kGpuDefault, static_cast<int>(it - table.begin()));
        }
        if (r.keepCoresOnline == 0) {
            removeHint(m.perfHints, kGoldCores);
            removeHint(m.perfHints, kPrimeCores);
        } else if (r.keepCoresOnline == 1) {
            if (hardware.cores(0) > 0) setHint(m.perfHints, kGoldCores, hardware.cores(0));
            if (hardware.cores(1) > 0) setHint(m.perfHints, kPrimeCores, hardware.cores(1));
        }

        if (r.thermalCase != kKeep) m.thermalCase = r.thermalCase;
        if (r.startDelayMs != kKeep) m.delayMs = r.startDelayMs;
        if (r.boostLengthMs != kKeep) m.boostLengthMs = r.boostLengthMs;
        m.dropSensitivity = r.dropSensitivity == kKeep ? -1 : r.dropSensitivity;
        m.dropLowerPermille = r.dropLowerPermille;
        m.dropUpperPermille = r.dropUpperPermille;
        if (m.dropLowerPermille == -1 && m.dropUpperPermille == -1 && r.dropSensitivity >= 1 && r.dropSensitivity <= 3) {
            m.dropLowerPermille = 600; m.dropUpperPermille = r.dropSensitivity == 1 ? 900 : r.dropSensitivity == 3 ? 980 : 965;
        }
        if ((r.dropLowerPermille != -1 || r.dropUpperPermille != -1) &&
            !validDropWindow(r.dropLowerPermille, r.dropUpperPermille)) return fail(error, "Invalid drop window");
        if (r.hysteresisDeciC != kKeep) m.hysteresisDeciC = r.hysteresisDeciC;
        if (r.skinTargetMilliC != kKeep) m.skinTargetMilliC = r.skinTargetMilliC;
        if (r.powerTargetMw != kKeep) m.powerTargetMw = r.powerTargetMw;

        bool placement = own.threadPlacement;
        m.bindCore.clear();
        m.schedulers.clear();
        if (r.threadRules == kThreadsOwn) {
            m.bindCore = ownMode.bindCore;
            m.schedulers = ownMode.schedulers;
        } else if (r.threadRules == kThreadsBase) {
            m.bindCore = base.bindCore;
            m.schedulers = base.schedulers;
            placement = true;
        } else if (r.threadRules == kThreadsCustom) {
            for (const auto& rule : r.customThreads) {
                m.bindCore.push_back({rule.name, rule.mask});
                if (rule.rtPriority >= 90) m.schedulers.push_back({rule.name, rule.rtPriority, true});
            }
            placement = true;
        } else placement = false;

        m.scenes.clear();
        if (r.sceneMode == 0) {
        } else if (r.sceneMode == 1 && !r.scenes.empty()) {
            const std::array<int, 3> none{};
            for (const auto& row : r.scenes) {
                if (!row.enabled || row.holdMs <= 0) continue;
                m.scenes.emplace(row.sceneId, Scene{row.holdMs, encode(hardware, 0, shifted(ladders, row.units, none, none))});
            }
        } else m.scenes = ownMode.scenes;

        *out = std::move(m);
        if (selection) *selection = {out, own.profile, placement};
        return true;
    } catch (const std::exception& e) { return fail(error, e.what()); }
}

ProfileView view(const Policy& policy, const Mode& mode, const std::string& key, int baseMode, bool tuned, int threadRules) {
    if (auto runtime = policy.archiveRuntime()) return view(*runtime, mode, key, baseMode, tuned, threadRules);
    ProfileView v;
    v.profileKey = key; v.baseMode = baseMode; v.tuned = tuned; v.threadRules = threadRules;
    v.thermalCase = mode.thermalCase;
    if (mode.linearBands) for (const auto& band : mode.bands) v.steps.push_back({band.temperature, band.units});
    else v.steps = stockSteps(mode);
    bool gold = false, prime = false;
    for (const auto& hint : mode.perfHints) {
        if (hint.opcode == kUclamp) v.uclampMin = hint.value;
        else if (hint.opcode == kGpuDefault && hint.value >= 0 && static_cast<size_t>(hint.value) < policy.tables()[2].size())
            v.gpuDefaultHz = policy.tables()[2][hint.value];
        else if (hint.opcode == kGoldCores) gold = policy.cores(0) > 0 && hint.value == policy.cores(0);
        else if (hint.opcode == kPrimeCores) prime = policy.cores(1) > 0 && hint.value == policy.cores(1);
    }
    v.keepCoresOnline = gold && prime;
    if (mode.fpsAdjust) v.frameBoostSteps.assign(mode.fpsAdjust->begin(), mode.fpsAdjust->end());
    if (mode.jankAdjust && mode.jankAdjustTime > 0) v.jankBoostSteps.assign(mode.jankAdjust->begin(), mode.jankAdjust->end());
    v.startDelayMs = mode.delayMs;
    v.threadRuleCount = static_cast<int>(mode.bindCore.size() + mode.schedulers.size());
    v.sceneCount = static_cast<int>(mode.scenes.size());
    v.sceneMode = mode.scenes.empty() ? 0 : 1;
    for (const auto& [id, scene] : mode.scenes) v.scenes.push_back({id, true, scene.timeMs, scene.frequencies.units});
    for (const auto& rule : mode.bindCore) v.threads.push_back({rule.name, rule.mask, -1});
    for (const auto& rule : mode.schedulers) if (rule.enabled) {
        auto it = std::find_if(v.threads.begin(), v.threads.end(), [&](const auto& t) { return t.name == rule.name; });
        if (it != v.threads.end()) it->rtPriority = rule.priority;
        else v.threads.push_back({rule.name, 0, rule.priority});
    }
    v.boostLengthMs = mode.boostLengthMs;
    v.dropSensitivity = -1;
    v.dropLowerPermille = mode.dropLowerPermille; v.dropUpperPermille = mode.dropUpperPermille;
    v.hysteresisDeciC = mode.hysteresisDeciC >= 0 ? mode.hysteresisDeciC
            : static_cast<int>(std::lround((mode.inheritedHysteresis >= 0
                    ? mode.inheritedHysteresis : policy.overlay.hysteresis) * 10));
    v.skinTargetMilliC = mode.skinTargetMilliC;
    v.powerTargetMw = mode.powerTargetMw;
    return v;
}

ProfileView archiveSourceProfile(const Policy& policy, const std::string& query, int gameMode) {
    const std::string prefix = "@builtin/history/";
    if (query.rfind(prefix, 0) != 0 || gameMode < 1 || gameMode > 3)
        throw std::runtime_error("Invalid archive query");
    const auto epochEnd = query.find('/', prefix.size()), split = query.rfind('/');
    if (epochEnd == std::string::npos || split <= epochEnd) throw std::runtime_error("Invalid archive query");
    const auto epoch = query.substr(prefix.size(), epochEnd - prefix.size());
    const auto pkg = query.substr(epochEnd + 1, split - epochEnd - 1), after = query.substr(split + 1);
    if (epoch != policy.archiveEpoch() || !policy.archiveReady() || !validPackage(pkg) || after.empty() || after.size() > 19 ||
        !std::all_of(after.begin(), after.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw std::runtime_error("Invalid archive query");
    const auto cursor = std::stoull(after);
    if (cursor > policy.archiveGeneration()) throw std::runtime_error("Invalid archive generation");
    ProfileView out;
    if (auto entry = policy.history(pkg, gameMode, cursor)) {
        out.profileKey = entry->key; out.baseMode = gameMode;
        out.variantId = std::to_string(entry->generation); out.variantName = entry->version;
    }
    return out;
}

namespace {
// ---- Store helpers -------------------------------------------------------------------------------
int strictInt(const std::string& text) {
    char* end = nullptr;
    errno = 0;
    const long n = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || *end || errno || n < INT32_MIN || n > INT32_MAX) throw std::runtime_error("Invalid integer: " + text);
    return static_cast<int>(n);
}
double strictDouble(const std::string& text) {
    char* end = nullptr;
    const double n = std::strtod(text.c_str(), &end);
    if (text.empty() || *end || !std::isfinite(n)) throw std::runtime_error("Invalid number: " + text);
    return n;
}
int intAttr(const tinyxml2::XMLElement* e, const char* name, int fallback) {
    const char* text = e->Attribute(name);
    return text ? strictInt(text) : fallback;
}
double doubleAttr(const tinyxml2::XMLElement* e, const char* name) {
    const char* text = e->Attribute(name);
    if (!text) throw std::runtime_error(std::string("Missing attribute ") + name);
    return strictDouble(text);
}
std::string textAttr(const tinyxml2::XMLElement* e, const char* name) {
    const char* text = e->Attribute(name);
    return text ? text : "";
}
bool boolAttr(const tinyxml2::XMLElement* e, const char* name, bool fallback) {
    const char* text = e->Attribute(name);
    if (!text) return fallback;
    const std::string s = text;
    if (s != "true" && s != "false") throw std::runtime_error("Invalid boolean: " + s);
    return s == "true";
}
std::string joinInts(const int* values, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) out += (i ? "," : "") + std::to_string(values[i]);
    return out;
}
std::vector<int> splitInts(const std::string& text) {
    std::vector<int> out;
    if (text.empty()) return out;
    size_t start = 0;
    for (;;) {
        const auto comma = text.find(',', start);
        out.push_back(strictInt(text.substr(start, comma == std::string::npos ? comma : comma - start)));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}
std::string number(double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}
void writeUnits(tinyxml2::XMLElement* e, const std::array<Bounds, 3>& u) {
    static const char* const names[3][2] = {{"goldMin", "goldMax"}, {"primeMin", "primeMax"}, {"gpuMin", "gpuMax"}};
    for (int r = 0; r < 3; ++r) {
        e->SetAttribute(names[r][0], u[r].floor);
        e->SetAttribute(names[r][1], u[r].ceiling);
    }
}
std::array<Bounds, 3> readUnits(const tinyxml2::XMLElement* e) {
    static const char* const names[3][2] = {{"goldMin", "goldMax"}, {"primeMin", "primeMax"}, {"gpuMin", "gpuMax"}};
    std::array<Bounds, 3> u{};
    for (int r = 0; r < 3; ++r) u[r] = {intAttr(e, names[r][0], -1), intAttr(e, names[r][1], -1)};
    return u;
}

void writeRecord(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent, const TuningRecord& r) {
    auto e = doc.NewElement("tuning");
    parent->InsertEndChild(e);
    e->SetAttribute("pkg", r.package.c_str());
    e->SetAttribute("mode", r.mode);
    e->SetAttribute("enabled", r.enabled);
    e->SetAttribute("base", r.baseProfile.c_str());
    e->SetAttribute("baseMode", r.baseMode);
    e->SetAttribute("threads", r.threadRules);
    e->SetAttribute("thermalCase", r.thermalCase);
    e->SetAttribute("uclamp", r.uclampMin);
    e->SetAttribute("gpuDefault", r.gpuDefaultHz);
    e->SetAttribute("cores", r.keepCoresOnline);
    e->SetAttribute("delay", r.startDelayMs);
    e->SetAttribute("frameBoost", r.frameBoost);
    e->SetAttribute("frameSteps", joinInts(r.frameBoostSteps.data(), r.frameBoostSteps.size()).c_str());
    e->SetAttribute("jankBoost", r.jankBoost);
    e->SetAttribute("jankSteps", joinInts(r.jankBoostSteps.data(), r.jankBoostSteps.size()).c_str());
    e->SetAttribute("maxOffsets", joinInts(r.maxOffsets.data(), 3).c_str());
    e->SetAttribute("minOffsets", joinInts(r.minOffsets.data(), 3).c_str());
    e->SetAttribute("sceneMode", r.sceneMode);
    e->SetAttribute("boostLength", r.boostLengthMs);
    e->SetAttribute("dropSensitivity", r.dropSensitivity);
    e->SetAttribute("dropLower", r.dropLowerPermille); e->SetAttribute("dropUpper", r.dropUpperPermille);
    e->SetAttribute("hysteresis", r.hysteresisDeciC);
    e->SetAttribute("skinTarget", r.skinTargetMilliC);
    e->SetAttribute("powerTarget", r.powerTargetMw);
    e->SetAttribute("variant", r.variantId.c_str());
    e->SetAttribute("variantName", r.variantName.c_str());
    if (r.pendingProbe) {
        e->SetAttribute("probeResource", r.pendingProbe->resource);
        e->SetAttribute("probeDelta", r.pendingProbe->deltaSteps);
        e->SetAttribute("probeBase", r.pendingProbe->baseline.c_str());
        e->SetAttribute("probeSince", r.pendingProbe->armedAtEpochMs);
        e->SetAttribute("probeRevision", r.pendingProbe->baselineRevision);
        e->SetAttribute("probeTarget", r.pendingProbe->analysisTargetFps);
        e->SetAttribute("probeDiagnosis", r.pendingProbe->diagnosis);
        e->SetAttribute("probeAnalysisVersion", r.pendingProbe->analysisVersion);
        e->SetAttribute("probeScale", r.pendingProbe->renderScale); e->SetAttribute("probeRefresh", r.pendingProbe->refreshRate);
    }
    for (const auto& step : r.steps) {
        auto s = doc.NewElement("step");
        e->InsertEndChild(s);
        char temp[32];
        std::snprintf(temp, sizeof(temp), "%.1f", step.temperature);
        s->SetAttribute("temp", temp);
        writeUnits(s, step.units);
    }
    for (const auto& row : r.scenes) {
        auto s = doc.NewElement("scene");
        e->InsertEndChild(s);
        s->SetAttribute("id", row.sceneId);
        s->SetAttribute("enabled", row.enabled);
        s->SetAttribute("hold", row.holdMs);
        writeUnits(s, row.units);
    }
    for (const auto& rule : r.customThreads) {
        auto s = doc.NewElement("thread");
        e->InsertEndChild(s);
        s->SetAttribute("name", rule.name.c_str());
        char mask[32];
        std::snprintf(mask, sizeof(mask), "0x%llx", static_cast<unsigned long long>(rule.mask));
        s->SetAttribute("mask", mask);
        s->SetAttribute("rt", rule.rtPriority);
    }
}

TuningRecord readRecord(const tinyxml2::XMLElement* e) {
    TuningRecord r;
    r.package = textAttr(e, "pkg");
    r.mode = intAttr(e, "mode", 0);
    r.enabled = boolAttr(e, "enabled", true);
    r.baseProfile = textAttr(e, "base");
    r.baseMode = intAttr(e, "baseMode", kBaseBalanced);
    r.threadRules = intAttr(e, "threads", kThreadsOwn);
    r.thermalCase = intAttr(e, "thermalCase", kKeep);
    r.uclampMin = intAttr(e, "uclamp", kKeep);
    r.gpuDefaultHz = intAttr(e, "gpuDefault", kKeep);
    r.keepCoresOnline = intAttr(e, "cores", kKeep);
    r.startDelayMs = intAttr(e, "delay", kKeep);
    r.frameBoost = intAttr(e, "frameBoost", kKeep);
    r.frameBoostSteps = splitInts(textAttr(e, "frameSteps"));
    r.jankBoost = intAttr(e, "jankBoost", kKeep);
    r.jankBoostSteps = splitInts(textAttr(e, "jankSteps"));
    const auto offsets = [&](const char* name, std::array<int, 3>& target) {
        const auto list = splitInts(textAttr(e, name));
        if (list.empty()) return;
        if (list.size() != 3) throw std::runtime_error(std::string("Invalid ") + name);
        std::copy(list.begin(), list.end(), target.begin());
    };
    offsets("maxOffsets", r.maxOffsets);
    offsets("minOffsets", r.minOffsets);
    r.sceneMode = intAttr(e, "sceneMode", kKeep);
    r.boostLengthMs = intAttr(e, "boostLength", kKeep);
    r.dropSensitivity = intAttr(e, "dropSensitivity", kKeep);
    r.dropLowerPermille = intAttr(e, "dropLower", -1); r.dropUpperPermille = intAttr(e, "dropUpper", -1);
    const bool hasWindow = e->Attribute("dropLower") || e->Attribute("dropUpper");
    if (!hasWindow && r.dropSensitivity >= 1 && r.dropSensitivity <= 3) {
        r.dropLowerPermille = 600;
        r.dropUpperPermille = r.dropSensitivity == 1 ? 900 : r.dropSensitivity == 3 ? 980 : 965;
    }
    if (!validDropWindow(r.dropLowerPermille, r.dropUpperPermille)) {
        r.dropLowerPermille = r.dropUpperPermille = -1;
        if (hasWindow) r.dropSensitivity = kKeep;
    }
    r.hysteresisDeciC = intAttr(e, "hysteresis", kKeep);
    r.skinTargetMilliC = intAttr(e, "skinTarget", kKeep);
    r.powerTargetMw = intAttr(e, "powerTarget", kKeep);
    r.variantId = textAttr(e, "variant");
    r.variantName = textAttr(e, "variantName");
    if (e->Attribute("probeResource")) {
        r.pendingProbe = ProbeRec{intAttr(e, "probeResource", -1), intAttr(e, "probeDelta", 0), textAttr(e, "probeBase")};
        e->QueryInt64Attribute("probeRevision", &r.pendingProbe->baselineRevision);
        r.pendingProbe->analysisTargetFps = intAttr(e, "probeTarget", 0);
        r.pendingProbe->diagnosis = intAttr(e, "probeDiagnosis", 0);
        r.pendingProbe->analysisVersion = intAttr(e, "probeAnalysisVersion", 0);
        r.pendingProbe->renderScale = e->FloatAttribute("probeScale", -1);
        r.pendingProbe->refreshRate = e->FloatAttribute("probeRefresh", -1);
        if (e->Attribute("probeSince") && e->QueryInt64Attribute("probeSince", &r.pendingProbe->armedAtEpochMs) != tinyxml2::XML_SUCCESS)
            throw std::runtime_error("Invalid pending probe");
    }
    for (auto s = e->FirstChildElement("step"); s; s = s->NextSiblingElement("step"))
        r.steps.push_back({doubleAttr(s, "temp"), readUnits(s)});
    for (auto s = e->FirstChildElement("scene"); s; s = s->NextSiblingElement("scene"))
        r.scenes.push_back({intAttr(s, "id", -1), boolAttr(s, "enabled", true), intAttr(s, "hold", 0), readUnits(s)});
    for (auto s = e->FirstChildElement("thread"); s; s = s->NextSiblingElement("thread"))
        r.customThreads.push_back({textAttr(s, "name"), Policy::mask(textAttr(s, "mask")), intAttr(s, "rt", -1)});
    return r;
}

bool mkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool replaceFile(const std::string& dir, const std::string& name, const char* data, size_t size, std::string* error) {
    const std::string path = dir + "/" + name, tmp = path + ".tmp";
    if (!mkdirs(dir)) return fail(error, "Cannot create " + dir + ": " + std::strerror(errno));
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return fail(error, "Cannot write " + tmp + ": " + std::strerror(errno));
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        const ssize_t n = ::write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    const int saved = errno;
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); return fail(error, "Cannot write " + tmp + ": " + std::strerror(saved)); }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int renamed = errno;
        ::unlink(tmp.c_str());
        return fail(error, "Cannot replace " + path + ": " + std::strerror(renamed));
    }
    const int dirFd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) { ::fsync(dirFd); ::close(dirFd); }
    return true;
}

// Returns the user id of "u<N>.xml" (suffix "") or "u<N>-sensitivity.xml", -1 otherwise.
int userOf(const std::string& name, const std::string& suffix) {
    const std::string tail = suffix + ".xml";
    if (name.size() <= 1 + tail.size() || name[0] != 'u' || name.compare(name.size() - tail.size(), tail.size(), tail) != 0) return -1;
    const std::string digits = name.substr(1, name.size() - 1 - tail.size());
    if (digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) return -1;
    return std::stoi(digits);
}
} // namespace

namespace {
std::vector<std::pair<size_t,size_t>> tuningSpans(const std::string& text) {
    std::vector<std::pair<size_t,size_t>> spans;size_t offset=0;
    while((offset=text.find("<tuning",offset))!=std::string::npos) {
        const size_t after=offset+7;
        if(after<text.size()&&!std::isspace(static_cast<unsigned char>(text[after]))&&text[after]!='>'&&text[after]!='/') {offset=after;continue;}
        const size_t close=text.find('>',after);if(close==std::string::npos)break;
        size_t end=close+1;
        if(close==0||text[close-1]!='/') {const size_t finish=text.find("</tuning>",end);if(finish==std::string::npos)break;end=finish+9;}
        spans.emplace_back(offset,end-offset);offset=end;
    }
    return spans;
}
std::string recordText(const TuningRecord& record) {
    tinyxml2::XMLDocument doc; auto root=doc.NewElement("game-tuning");doc.InsertEndChild(root);
    writeRecord(doc,root,record);tinyxml2::XMLPrinter printer;root->FirstChildElement()->Accept(&printer);return printer.CStr();
}
bool migrateTuningStore(const std::string& dir, const std::string& name, std::string* error) {
    std::ifstream input(dir + "/" + name);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    tinyxml2::XMLDocument doc;
    if (doc.Parse(text.c_str()) != tinyxml2::XML_SUCCESS) return true; // Normal loader quarantines damaged files.
    const auto root = doc.FirstChildElement("game-tuning");
    if (!root || root->IntAttribute("version", 1) >= 2) return true;
    // Every persisted tuning is a saved user selection, including old nameless tunings.
    // Built-in is represented by no tuning record; leave every record byte-identical.
    std::string migrated = text;
    const auto start = migrated.find("<game-tuning"); const auto stop = migrated.find('>', start);
    if (start == std::string::npos || stop == std::string::npos) return false;
    std::string tag = migrated.substr(start, stop - start);
    tag = std::regex_replace(tag, std::regex(R"(\s+version\s*=\s*["'][^"']*["'])"), "");
    tag += " version=\"2\""; migrated.replace(start, stop - start, tag);
    return replaceFile(dir, name, migrated.data(), migrated.size(), error);
}
}

TuningStore::TuningStore(std::string dir) : mDir(std::move(dir)) {}

void TuningStore::load(const Policy& policy) {
    mPolicy = &policy;
    mUsers.clear();
    diagnostics.clear();
    std::vector<std::string> names;
    if (DIR* d = ::opendir(mDir.c_str())) {
        while (const dirent* entry = ::readdir(d)) names.push_back(entry->d_name);
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    const auto quarantine = [&](const std::string& name, const std::string& why) {
        diagnostics.push_back(name + ": " + why);
        ::rename((mDir + "/" + name).c_str(), (mDir + "/" + name + ".bad").c_str());
    };
    for (const auto& name : names) {
        const int user = userOf(name, "");
        if (user >= 0) {
            std::string migrationError;
            if (!migrateTuningStore(mDir, name, &migrationError)) { diagnostics.push_back(migrationError); continue; }
            tinyxml2::XMLDocument doc;
            const auto root = doc.LoadFile((mDir + "/" + name).c_str()) == tinyxml2::XML_SUCCESS ? doc.FirstChildElement("game-tuning") : nullptr;
            if (!root || (textAttr(root, "version") != "1" && textAttr(root, "version") != "2")) { quarantine(name, "Unreadable tuning file"); continue; }
            UserData data;
            for (auto e = root->FirstChildElement("tuning"); e; e = e->NextSiblingElement("tuning")) {
                try {
                    TuningRecord r = readRecord(e);
                    std::string error;
                    if (!normalize(policy, r, &error)) {
                        // Missing archives and temporarily unavailable runtime OPPs must not
                        // erase a saved selection on the next unrelated store write.
                        if (r.baseProfile.rfind("@builtin/", 0) != 0 || !validPackage(r.package) || r.mode < 1 || r.mode > 3)
                            throw std::runtime_error(error);
                        diagnostics.push_back(name + "/tuning " + r.package + ": Deferred archive validation: " + error);
                    }
                    if (data.tunings.size() >= kMaxTunings) throw std::runtime_error("limit");
                    if (!data.tunings.emplace(std::make_pair(r.package, r.mode), r).second) throw std::runtime_error("Duplicate entry");
                } catch (const std::exception& error) {
                    diagnostics.push_back(name + "/tuning " + textAttr(e, "pkg") + ": " + error.what());
                }
            }
            for (auto e = root->FirstChildElement("armed"); e; e = e->NextSiblingElement("armed")) {
                const auto pkg = textAttr(e, "pkg");
                if (!validPackage(pkg) || (data.armed.size() >= kMaxArmed && !data.armed.count(pkg))) {
                    diagnostics.push_back(name + "/armed " + pkg + ": Invalid entry");
                    continue;
                }
                data.armed.insert(pkg);
            }
            std::ifstream rawInput(mDir + "/" + name);
            const std::string raw((std::istreambuf_iterator<char>(rawInput)), {});
            for (const auto& [start,size] : tuningSpans(raw)) {
                const auto text=raw.substr(start,size);
                tinyxml2::XMLDocument row;row.Parse(text.c_str());auto e=row.FirstChildElement("tuning");if(!e)continue;
                const auto key=std::make_pair(textAttr(e,"pkg"),intAttr(e,"mode",0));const auto found=data.tunings.find(key);
                if(found!=data.tunings.end())data.originalTunings[key]={text,recordText(found->second)};
            }
            mUsers[user].originalTunings = std::move(data.originalTunings);
            mUsers[user].tunings = std::move(data.tunings);
            mUsers[user].armed = std::move(data.armed);
            continue;
        }
        const int history = userOf(name, "-sensitivity");
        if (history < 0) {
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) ::unlink((mDir + "/" + name).c_str());
            continue;
        }
        tinyxml2::XMLDocument doc;
        const auto root = doc.LoadFile((mDir + "/" + name).c_str()) == tinyxml2::XML_SUCCESS ? doc.FirstChildElement("sensitivity") : nullptr;
        if (!root || textAttr(root, "version") != "1") { quarantine(name, "Unreadable sensitivity file"); continue; }
        auto& list = mUsers[history].sensitivity;
        for (auto e = root->FirstChildElement("entry"); e; e = e->NextSiblingElement("entry")) {
            try {
                SensitivityEntry s;
                s.package = textAttr(e, "pkg");
                s.mode = intAttr(e, "mode", 0);
                s.resource = intAttr(e, "resource", -2);
                s.deltaSteps = intAttr(e, "delta", 0);
                s.dFloor5 = doubleAttr(e, "dFloor5");
                s.dTail95 = doubleAttr(e, "dTail95");
                s.dPowerMw = doubleAttr(e, "dPower");
                s.dSkinPeakC = doubleAttr(e, "dSkin");
                s.baselineId = textAttr(e, "baseline"); s.resultId = textAttr(e, "result");
                e->QueryInt64Attribute("baselineRevision", &s.baselineRevision);
                e->QueryInt64Attribute("resultRevision", &s.resultRevision);
                s.analysisTargetFps = intAttr(e, "target", 0); s.diagnosis = intAttr(e, "diagnosis", 0);
                s.analysisVersion = intAttr(e, "analysisVersion", 0);
                s.renderScale = e->FloatAttribute("scale", -1); s.refreshRate = e->FloatAttribute("refresh", -1);
                if (!validPackage(s.package) || s.mode < 1 || s.mode > 3 || s.resource < -1 || s.resource > 2 ||
                    s.deltaSteps < -10 || s.deltaSteps > 10) throw std::runtime_error("Invalid entry");
                list.push_back(s);
            } catch (const std::exception& error) { diagnostics.push_back(name + "/entry: " + error.what()); }
        }
        // Keep only the newest entries per key.
        std::map<std::pair<std::string, int>, size_t> counts;
        for (auto it = list.rbegin(); it != list.rend(); ++it) ++counts[{it->package, it->mode}];
        std::vector<SensitivityEntry> kept;
        for (const auto& s : list) {
            auto& left = counts[{s.package, s.mode}];
            if (left-- > kMaxSensitivity) continue;
            kept.push_back(s);
        }
        list = std::move(kept);
    }
}

bool TuningStore::save(int user, const UserData& data, std::string* error) {
    tinyxml2::XMLDocument doc;
    doc.InsertEndChild(doc.NewDeclaration());
    auto root = doc.NewElement("game-tuning");
    root->SetAttribute("version", 2);
    doc.InsertEndChild(root);
    for (const auto& [key, record] : data.tunings) writeRecord(doc, root, record);
    for (const auto& pkg : data.armed) {
        auto e = doc.NewElement("armed");
        e->SetAttribute("pkg", pkg.c_str());
        root->InsertEndChild(e);
    }
    tinyxml2::XMLPrinter printer;
    doc.Print(&printer);
    std::string text=printer.CStr(), output;size_t offset=0;
    for(const auto& [start,size] : tuningSpans(text)) {
        output.append(text,offset,start-offset);
        const auto serialized=text.substr(start,size);
        tinyxml2::XMLDocument row;row.Parse(serialized.c_str());const auto e=row.FirstChildElement("tuning");
        const auto key=std::make_pair(textAttr(e,"pkg"),intAttr(e,"mode",0));
        const auto original=data.originalTunings.find(key), current=data.originalTunings.end();
        const auto record=data.tunings.find(key);
        if(original!=current&&record!=data.tunings.end()&&original->second.second==recordText(record->second)) output+=original->second.first;
        else output+=serialized;
        offset=start+size;
    }
    output.append(text,offset,std::string::npos);
    return replaceFile(mDir, "u" + std::to_string(user) + ".xml", output.data(), output.size(), error);
}

bool TuningStore::saveSensitivity(int user, const UserData& data, std::string* error) {
    tinyxml2::XMLDocument doc;
    doc.InsertEndChild(doc.NewDeclaration());
    auto root = doc.NewElement("sensitivity");
    root->SetAttribute("version", 1);
    doc.InsertEndChild(root);
    for (const auto& s : data.sensitivity) {
        auto e = doc.NewElement("entry");
        e->SetAttribute("pkg", s.package.c_str());
        e->SetAttribute("mode", s.mode);
        e->SetAttribute("resource", s.resource);
        e->SetAttribute("delta", s.deltaSteps);
        e->SetAttribute("dFloor5", number(s.dFloor5).c_str());
        e->SetAttribute("dTail95", number(s.dTail95).c_str());
        e->SetAttribute("dPower", number(s.dPowerMw).c_str());
        e->SetAttribute("dSkin", number(s.dSkinPeakC).c_str());
        e->SetAttribute("baseline", s.baselineId.c_str()); e->SetAttribute("result", s.resultId.c_str());
        e->SetAttribute("baselineRevision", s.baselineRevision); e->SetAttribute("resultRevision", s.resultRevision);
        e->SetAttribute("target", s.analysisTargetFps); e->SetAttribute("diagnosis", s.diagnosis);
        e->SetAttribute("analysisVersion", s.analysisVersion);
        e->SetAttribute("scale", s.renderScale); e->SetAttribute("refresh", s.refreshRate);
        root->InsertEndChild(e);
    }
    tinyxml2::XMLPrinter printer;
    doc.Print(&printer);
    return replaceFile(mDir, "u" + std::to_string(user) + "-sensitivity.xml", printer.CStr(),
                       static_cast<size_t>(printer.CStrSize()) - 1, error);
}

std::optional<TuningRecord> TuningStore::find(int user, const std::string& pkg, int mode) const {
    const auto u = mUsers.find(user);
    if (u == mUsers.end()) return std::nullopt;
    const auto it = u->second.tunings.find({pkg, mode});
    if (it == u->second.tunings.end()) return std::nullopt;
    return it->second;
}

bool TuningStore::put(int user, const TuningRecord& record, std::string* error) {
    try {
        TuningRecord checked = record;
        if (mPolicy) { if (!normalize(*mPolicy, checked, error)) return false; }
        else if (!validPackage(checked.package) || checked.mode < 1 || checked.mode > 3) return fail(error, "Invalid record");
        UserData next = mUsers.count(user) ? mUsers.at(user) : UserData{};
        const auto key = std::make_pair(checked.package, checked.mode);
        if (!next.tunings.count(key) && next.tunings.size() >= kMaxTunings) return fail(error, "limit");
        next.tunings[key] = std::move(checked);
        if (!save(user, next, error)) return false;
        mUsers[user].tunings = std::move(next.tunings);
        return true;
    } catch (const std::exception& e) { return fail(error, e.what()); }
}

bool TuningStore::erase(int user, const std::string& pkg, int mode) {
    try {
        const auto u = mUsers.find(user);
        if (u == mUsers.end() || !u->second.tunings.count({pkg, mode})) return false;
        UserData next = u->second;
        next.tunings.erase({pkg, mode});
        if (!save(user, next, nullptr)) return false;
        u->second.tunings = std::move(next.tunings);
        return true;
    } catch (const std::exception&) { return false; }
}

std::vector<TuningRecord> TuningStore::all(int user) const {
    std::vector<TuningRecord> result;
    const auto u = mUsers.find(user);
    if (u != mUsers.end()) for (const auto& [key, record] : u->second.tunings) result.push_back(record);
    return result;
}

std::vector<int> TuningStore::users() const {
    std::vector<int> result;
    for (const auto& [user, data] : mUsers) result.push_back(user);
    return result;
}

bool TuningStore::armed(int user, const std::string& pkg) const {
    const auto u = mUsers.find(user);
    return u != mUsers.end() && u->second.armed.count(pkg);
}

bool TuningStore::setArmed(int user, const std::string& pkg, bool armed, std::string* error) {
    try {
        if (!validPackage(pkg)) return fail(error, "Invalid package name");
        UserData next = mUsers.count(user) ? mUsers.at(user) : UserData{};
        if (armed) {
            if (!next.armed.count(pkg) && next.armed.size() >= kMaxArmed) return fail(error, "limit");
            next.armed.insert(pkg);
        } else next.armed.erase(pkg);
        if (!save(user, next, error)) return false;
        mUsers[user].armed = std::move(next.armed);
        return true;
    } catch (const std::exception& e) { return fail(error, e.what()); }
}

std::vector<std::string> TuningStore::armedPackages(int user) const {
    const auto u = mUsers.find(user);
    return u == mUsers.end() ? std::vector<std::string>{} : std::vector<std::string>(u->second.armed.begin(), u->second.armed.end());
}

bool TuningStore::addSensitivity(int user, const SensitivityEntry& entry, std::string* error) {
    try {
        if (!validPackage(entry.package) || entry.mode < 1 || entry.mode > 3 || entry.resource < -1 || entry.resource > 2 ||
            entry.deltaSteps < -10 || entry.deltaSteps > 10 || !std::isfinite(entry.dFloor5) || !std::isfinite(entry.dTail95) ||
            !std::isfinite(entry.dPowerMw) || !std::isfinite(entry.dSkinPeakC) ||
            !std::isfinite(entry.renderScale) || !std::isfinite(entry.refreshRate) ||
            (entry.renderScale != -1 && (entry.renderScale <= 0 || entry.renderScale > 1)) ||
            (entry.refreshRate != -1 && entry.refreshRate <= 0))
            return fail(error, "Invalid sensitivity entry");
        UserData next = mUsers.count(user) ? mUsers.at(user) : UserData{};
        next.sensitivity.push_back(entry);
        size_t count = std::count_if(next.sensitivity.begin(), next.sensitivity.end(), [&](const auto& s) {
            return s.package == entry.package && s.mode == entry.mode;
        });
        for (auto it = next.sensitivity.begin(); count > kMaxSensitivity && it != next.sensitivity.end();) {
            if (it->package == entry.package && it->mode == entry.mode) { it = next.sensitivity.erase(it); --count; }
            else ++it;
        }
        if (!saveSensitivity(user, next, error)) return false;
        mUsers[user].sensitivity = std::move(next.sensitivity);
        return true;
    } catch (const std::exception& e) { return fail(error, e.what()); }
}

bool TuningStore::invalidateEvidence(int user, const std::string& pkg, int mode, const std::string& id, std::string* error) {
    auto next = mUsers[user];
    auto& entries = next.sensitivity;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const SensitivityEntry& e) {
        return e.package == pkg && e.mode == mode && (e.baselineId.empty() || e.resultId.empty() ||
            e.baselineId == id || e.resultId == id);
    }), entries.end());
    // Invalidate derived evidence before publishing the new range. A crash can only lose evidence.
    if (!saveSensitivity(user, next, error)) return false;
    mUsers[user].sensitivity = next.sensitivity;
    auto it = next.tunings.find({pkg, mode});
    if (it != next.tunings.end() && it->second.pendingProbe && it->second.pendingProbe->baseline == id) {
        it->second.pendingProbe.reset();
        if (!save(user, next, error)) return false;
        mUsers[user] = std::move(next);
    }
    return true;
}

std::vector<SensitivityEntry> TuningStore::sensitivity(int user, const std::string& pkg, int mode) const {
    std::vector<SensitivityEntry> result;
    const auto u = mUsers.find(user);
    if (u != mUsers.end()) for (const auto& s : u->second.sensitivity) if (s.package == pkg && s.mode == mode) result.push_back(s);
    return result;
}
} // namespace gamepolicy
