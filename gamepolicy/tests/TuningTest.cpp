// SPDX-License-Identifier: Apache-2.0
#include "Tests.h"
#include "Policy.h"
#include "Tuning.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <dirent.h>

using namespace gamepolicy;
namespace {
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }

Opps sample(const char* stock) {
    tinyxml2::XMLDocument xml;
    require(xml.LoadFile(stock) == tinyxml2::XML_SUCCESS, "Cannot load real fixture");
    auto env = xml.FirstChildElement("GameOptPolicy")->FirstChildElement("PreEnv");
    Opps tables;
    for (auto kind = env->FirstChildElement(); kind; kind = kind->NextSiblingElement())
        for (auto cluster = kind->FirstChildElement("cluster"); cluster; cluster = cluster->NextSiblingElement("cluster")) {
            std::string name = cluster->Attribute("name");
            int r = name == "Gold" ? 0 : name == "Prime" ? 1 : 2;
            std::istringstream input(cluster->GetText());
            int f; while (input >> f) tables[r].push_back(f);
        }
    return tables;
}
void loadDevice(Policy& policy, const char* stock, const char* overlay) {
    std::string error;
    require(policy.load(stock, overlay, &error), error);
    policy.resolve(sample(stock), {6, 2}, 0xFF);
}
std::vector<double> sweep() {
    std::vector<double> up, all;
    for (int i = 0; i <= 60; ++i) up.push_back(20.0 + i * 0.5);
    all = up;
    all.insert(all.end(), up.rbegin(), up.rend());
    return all;
}
std::string describe(const std::string& key, int mode) { return key + "/" + std::to_string(mode); }

bool sameBands(const std::vector<Band>& a, const std::vector<Band>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) for (int r = 0; r < 3; ++r) {
        if (a[i].bounds[r].floor != b[i].bounds[r].floor || a[i].bounds[r].ceiling != b[i].bounds[r].ceiling ||
            a[i].units[r].floor != b[i].units[r].floor || a[i].units[r].ceiling != b[i].units[r].ceiling) return false;
    }
    return true;
}
bool sameHints(const Mode& a, const Mode& b) {
    return a.perfHints.size() == b.perfHints.size() && std::equal(a.perfHints.begin(), a.perfHints.end(), b.perfHints.begin(),
        [](const auto& x, const auto& y) { return x.opcode == y.opcode && x.value == y.value; });
}
bool sameThreads(const Mode& a, const Mode& b) {
    return a.bindCore.size() == b.bindCore.size() && a.schedulers.size() == b.schedulers.size() &&
        std::equal(a.bindCore.begin(), a.bindCore.end(), b.bindCore.begin(), [](const auto& x, const auto& y) {
            return x.name == y.name && x.mask == y.mask;
        }) && std::equal(a.schedulers.begin(), a.schedulers.end(), b.schedulers.begin(), [](const auto& x, const auto& y) {
            return x.name == y.name && x.priority == y.priority && x.enabled == y.enabled;
        });
}
bool sameScenes(const Mode& a, const Mode& b) {
    if (a.scenes.size() != b.scenes.size()) return false;
    for (const auto& [id, scene] : a.scenes) {
        const auto other = b.scenes.find(id);
        if (other == b.scenes.end() || other->second.timeMs != scene.timeMs) return false;
        if (!sameBands({scene.frequencies}, {other->second.frequencies})) return false;
    }
    return true;
}
bool sameOptional(const std::optional<std::array<int, 3>>& a, const std::optional<std::array<int, 3>>& b) {
    return a.has_value() == b.has_value() && (!a || *a == *b);
}

// Stock parity: the tuned mode must request exactly what the stock mode requests.
void expectParity(const Policy& policy, const Mode& stock, const Mode& tuned, const std::string& what) {
    require(tuned.linearBands && tuned.bands.size() == stockSteps(stock).size(), what + ": Step count");
    require(tuned.thermalCase == stock.thermalCase && tuned.delayMs == stock.delayMs, what + ": Case or delay");
    require(sameOptional(tuned.fpsAdjust, stock.fpsAdjust) && sameOptional(tuned.jankAdjust, stock.jankAdjust) &&
            tuned.jankAdjustTime == stock.jankAdjustTime, what + ": Boost settings");
    require(sameHints(tuned, stock), what + ": Perf hints");
    require(sameThreads(tuned, stock), what + ": Thread rules");
    require(sameScenes(tuned, stock), what + ": Scene rows");
    require(tuned.boostLengthMs == stock.boostLengthMs && tuned.dropSensitivity == stock.dropSensitivity &&
            tuned.hysteresisDeciC == stock.hysteresisDeciC && tuned.skinTargetMilliC == stock.skinTargetMilliC &&
            tuned.powerTargetMw == stock.powerTargetMw, what + ": Expert defaults");
    const double hysteresis = policy.overlay.hysteresis;
    int previousStock = -1, previousTuned = -1;
    for (double t : sweep()) {
        const size_t bs = Policy::band(stock, t, previousStock, hysteresis), bt = Policy::band(tuned, t, previousTuned, hysteresis);
        require(bs == bt, what + ": Row at " + std::to_string(t));
        const int row = stock.bands.empty() ? -1 : static_cast<int>(bs);
        require(Policy::resources(stock, row) == Policy::resources(tuned, row), what + ": Request at " + std::to_string(t));
        require(Policy::resources(stock, row, true) == Policy::resources(tuned, row, true), what + ": Boost request");
        require(Policy::resources(stock, row, false, true) == Policy::resources(tuned, row, false, true), what + ": Jank request");
        const auto a = Policy::evaluate(stock, t, 0, previousStock, hysteresis);
        const auto b = Policy::evaluate(tuned, t, 0, previousTuned, hysteresis);
        require(a.band == b.band && a.thermalCase == b.thermalCase && a.resources == b.resources, what + ": Decision");
        previousStock = static_cast<int>(bs); previousTuned = static_cast<int>(bt);
    }
}

size_t parity(const Policy& policy) {
    size_t cases = 0;
    for (const auto& [key, modes] : policy.games) for (int mode = 1; mode <= 3; ++mode) {
        TuningRecord record;
        record.package = key; record.mode = mode; record.baseMode = gameModeToBase(policy, mode);
        std::string error;
        require(normalize(policy, record, &error), describe(key, mode) + ": " + error);
        require(record.baseProfile == key, "Own key not filled");
        Mode tuned; Selection selection{};
        require(build(policy, record, &tuned, &selection, &error), describe(key, mode) + ": " + error);
        const Selection stock = policy.select(key, mode);
        require(selection.mode == &tuned && selection.profile == stock.profile && selection.threadPlacement == stock.threadPlacement,
                describe(key, mode) + ": Selection");
        expectParity(policy, *stock.mode, tuned, describe(key, mode));
        // Explicit scene gate and own thread rules still match stock.
        TuningRecord explicitFields = record;
        explicitFields.sceneMode = 1;
        explicitFields.threadRules = kThreadsOwn;
        Mode again;
        require(build(policy, explicitFields, &again, nullptr, &error), error);
        expectParity(policy, *stock.mode, again, describe(key, mode) + " scenes-on");
        ++cases;
    }
    return cases;
}

size_t linear(const Policy& policy) {
    size_t cases = 0;
    for (const auto& [key, modes] : policy.games) for (const auto& [name, stock] : modes) {
        Mode mode;
        mode.linearBands = true;
        for (const auto& step : stockSteps(stock)) mode.bands.push_back({step.temperature, {}, step.units});
        for (double hysteresis : {0.0, 0.5, policy.overlay.hysteresis, 2.0}) {
            int ps = -1, pl = -1;
            for (double t : sweep()) {
                const size_t a = Policy::band(stock, t, ps, hysteresis), b = Policy::linearBand(mode, t, pl, hysteresis);
                require(Policy::band(mode, t, pl, hysteresis) == b, key + "/" + name + ": band() ignores linearBands");
                require(a == b || (stock.bands.empty() && b == 0), key + "/" + name + ": Linear row at " + std::to_string(t));
                ps = static_cast<int>(a); pl = static_cast<int>(b);
            }
        }
        ++cases;
    }
    return cases;
}

TuningRecord baseline(const Policy& policy, const std::string& key, int mode) {
    TuningRecord record;
    record.package = key; record.mode = mode; record.baseMode = gameModeToBase(policy, mode);
    std::string error;
    require(normalize(policy, record, &error), error);
    return record;
}

void checkUnits(const Policy& policy, const Band& band, const std::string& what) {
    for (int r = 0; r < 3; ++r) {
        const auto ladder = policy.ladder(r);
        const auto& u = band.units[r];
        require(u.ceiling >= 0 && std::binary_search(ladder.begin(), ladder.end(), u.ceiling), what + ": Ceiling off ladder");
        require(u.ceiling <= policy.overlay.ceilings[r], what + ": Hard ceiling exceeded");
        require(u.floor == -1 || (std::binary_search(ladder.begin(), ladder.end(), u.floor) && u.floor <= u.ceiling), what + ": Floor");
        if (r == 2) {
            const auto& table = policy.tables()[2];
            require(band.bounds[r].ceiling >= 0 && table.at(band.bounds[r].ceiling) == u.ceiling, what + ": GPU ceiling round trip");
            require(u.floor < 0 ? band.bounds[r].floor == -1 : table.at(band.bounds[r].floor) == u.floor, what + ": GPU floor round trip");
        } else {
            require(band.bounds[r].ceiling == u.ceiling / 1000 && band.bounds[r].floor == (u.floor < 0 ? -1 : u.floor / 1000),
                    what + ": CPU encoding");
        }
    }
}

size_t offsets(const Policy& policy) {
    size_t cases = 0;
    const std::vector<std::pair<std::array<int, 3>, std::array<int, 3>>> sets = {
        {{10, 10, 10}, {0, 0, 0}}, {{-10, -10, -10}, {0, 0, 0}}, {{0, 0, 0}, {10, 2, 10}}, {{0, 0, 0}, {-10, -10, -10}},
        {{10, -10, 5}, {-10, 2, 3}}, {{3, 3, 3}, {-3, -3, -3}}, {{10, 10, 10}, {10, 2, 10}}};
    for (const auto& [key, modes] : policy.games) for (const auto& [maxOff, minOff] : sets) {
        TuningRecord record = baseline(policy, key, 1);
        record.maxOffsets = maxOff; record.minOffsets = minOff;
        record.frameBoost = 1; record.jankBoost = 1; record.frameBoostSteps = {10, 10, 10}; record.jankBoostSteps = {10, 10, 10};
        Mode tuned; std::string error;
        require(build(policy, record, &tuned, nullptr, &error), error);
        const auto stock = stockSteps(*policy.select(key, 1).mode);
        require(tuned.bands.size() == stock.size(), "Offsets changed the step count");
        for (size_t i = 0; i < tuned.bands.size(); ++i) {
            const std::string what = key + " step " + std::to_string(i);
            checkUnits(policy, tuned.bands[i], what);
            for (int r = 0; r < 3; ++r) {
                const auto ladder = policy.ladder(r);
                const int top = static_cast<int>(ladder.size()) - 1;
                const auto idx = [&](int v) { return static_cast<int>(std::upper_bound(ladder.begin(), ladder.end(), v) - ladder.begin()) - 1; };
                const int ceiling = stock[i].units[r].ceiling, floor = stock[i].units[r].floor;
                if (maxOff[r] != 0 && ceiling >= 0)
                    require(tuned.bands[i].units[r].ceiling == ladder[std::clamp(idx(ceiling) + maxOff[r], 0, top)], what + ": Max offset");
                if (floor < 0 && minOff[r] < 0) require(tuned.bands[i].units[r].floor == -1, what + ": No floor must stay none");
                if (minOff[r] == 0 && maxOff[r] == 0) {
                    require(tuned.bands[i].units[r].floor == floor, what + ": Zero offset changed floor");
                }
            }
        }
        for (const auto* rows : {&tuned.boostedBands, &tuned.jankBands}) {
            require(rows->size() == tuned.bands.size(), "Boost row count");
            for (size_t i = 0; i < rows->size(); ++i) {
                checkUnits(policy, (*rows)[i], key + " boost " + std::to_string(i));
                for (int r = 0; r < 3; ++r) {
                    require((*rows)[i].units[r].ceiling >= tuned.bands[i].units[r].ceiling, "Boost lowered a ceiling");
                    require((*rows)[i].units[r].floor >= tuned.bands[i].units[r].floor, "Boost lowered a floor");
                }
            }
        }
        ++cases;
    }
    return cases;
}

std::array<Bounds, 3> pick(const Policy& policy, int lowFloor, int highCeiling) {
    std::array<Bounds, 3> u{};
    for (int r = 0; r < 3; ++r) {
        const auto ladder = policy.ladder(r);
        u[r] = {lowFloor < 0 ? -1 : ladder.at(lowFloor), ladder.at(ladder.size() - 1 - highCeiling)};
    }
    return u;
}

std::string reject(const Policy& policy, TuningRecord record) {
    std::string error;
    require(!normalize(policy, record, &error) && !error.empty(), "Invalid record accepted");
    return error;
}

void validation(const Policy& policy) {
    const TuningRecord good = baseline(policy, "default", 1);
    auto bad = [&](const char* what, const auto& mutate) {
        TuningRecord r = good;
        mutate(r);
        std::string error;
        require(!normalize(policy, r, &error) && !error.empty(), std::string("Accepted: ") + what);
    };
    auto fine = [&](const char* what, const auto& mutate) {
        TuningRecord r = good;
        mutate(r);
        std::string error;
        require(normalize(policy, r, &error), std::string("Rejected: ") + what + " (" + error + ")");
    };
    bad("empty package", [](auto& r) { r.package = ""; });
    bad("package character", [](auto& r) { r.package = "bad pkg"; });
    bad("package length", [](auto& r) { r.package = std::string(256, 'a'); });
    fine("package length 255", [](auto& r) { r.package = std::string(255, 'a'); });
    bad("mode 0", [](auto& r) { r.mode = 0; });
    bad("mode 4", [](auto& r) { r.mode = 4; });
    bad("unknown base", [](auto& r) { r.baseProfile = "no.such.game"; });
    bad("base mode 0", [](auto& r) { r.baseMode = 0; });
    bad("base mode 5", [](auto& r) { r.baseMode = 5; });
    std::string maximumKey;
    for (const auto& [key, modes] : policy.games) if (modes.count("chaoshen")) { maximumKey = key; break; }
    require(!maximumKey.empty(), "No stock key has a maximum mode");
    fine("maximum mode", [&](auto& r) { r.baseMode = kBaseMaximum; r.baseProfile = maximumKey; });
    bad("maximum mode missing", [&](auto& r) {
        r.baseMode = kBaseMaximum;
        for (const auto& [key, modes] : policy.games) if (!modes.count("chaoshen")) { r.baseProfile = key; return; }
        r.baseProfile = "no.such.game";
    });
    bad("max offset 11", [](auto& r) { r.maxOffsets[0] = 11; });
    bad("max offset -11", [](auto& r) { r.maxOffsets[2] = -11; });
    bad("min offset 11", [](auto& r) { r.minOffsets[0] = 11; });
    bad("min offset -11", [](auto& r) { r.minOffsets[2] = -11; });
    bad("prime min offset 3", [](auto& r) { r.minOffsets[1] = 3; });
    fine("prime min offset 2", [](auto& r) { r.minOffsets[1] = 2; });
    fine("offset edges", [](auto& r) { r.maxOffsets = {10, -10, 10}; r.minOffsets = {10, -10, -10}; });
    bad("five steps", [&](auto& r) { r.steps.assign(5, Step{}); for (int i = 0; i < 5; ++i) { r.steps[i].temperature = 20 + i; r.steps[i].units = pick(policy, -1, 0); } });
    auto steps = [&](std::vector<double> temps) {
        return [&, temps](TuningRecord& r) {
            r.steps.clear();
            for (double t : temps) r.steps.push_back({t, pick(policy, 1, 0)});
        };
    };
    fine("four steps", steps({7, 20, 40.5, 60}));
    bad("temperature below 20", steps({0, 19.5}));
    bad("temperature above 60", steps({0, 60.5}));
    bad("temperature not a half step", steps({0, 30.25}));
    bad("temperature equal", steps({0, 30, 30}));
    bad("temperature decreasing", steps({0, 40, 30}));
    bad("temperature NaN", [&](auto& r) { steps({0, 30})(r); r.steps[1].temperature = std::nan(""); });
    bad("step value off ladder", [&](auto& r) { steps({0})(r); r.steps[0].units[0].ceiling += 1; });
    bad("step floor off ladder", [&](auto& r) { steps({0})(r); r.steps[0].units[1].floor += 1; });
    bad("step min above max", [&](auto& r) { steps({0})(r); r.steps[0].units[2] = {policy.ladder(2).back(), policy.ladder(2).front()}; });
    bad("step ceiling -3", [&](auto& r) { steps({0})(r); r.steps[0].units[0].ceiling = -3; });
    fine("step none values", [&](auto& r) { steps({0})(r); r.steps[0].units[0] = {-1, -1}; });
    {
        TuningRecord r = good;
        steps({5, 30})(r);
        std::string error;
        require(normalize(policy, r, &error) && r.steps[0].temperature == 0, "Step 0 temperature not forced to 0");
    }
    bad("thermal case 0", [](auto& r) { r.thermalCase = 0; });
    bad("thermal case 250", [](auto& r) { r.thermalCase = 250; });
    for (int c : {kKeep, 100, 200, 300, 400}) fine("thermal case", [&](auto& r) { r.thermalCase = c; });
    bad("uclamp 101", [](auto& r) { r.uclampMin = 101; });
    bad("uclamp 7", [](auto& r) { r.uclampMin = 7; });
    bad("uclamp -5", [](auto& r) { r.uclampMin = -5; });
    for (int v : {kKeep, -1, 0, 5, 100}) fine("uclamp", [&](auto& r) { r.uclampMin = v; });
    bad("gpu default off ladder", [&](auto& r) { r.gpuDefaultHz = policy.ladder(2)[0] + 1; });
    bad("gpu default above hard ceiling", [&](auto& r) { r.gpuDefaultHz = policy.overlay.ceilings[2] + 1; });
    fine("gpu default ladder", [&](auto& r) { r.gpuDefaultHz = policy.ladder(2)[1]; });
    fine("gpu default off", [](auto& r) { r.gpuDefaultHz = -1; });
    bad("cores 2", [](auto& r) { r.keepCoresOnline = 2; });
    bad("frame boost 2", [](auto& r) { r.frameBoost = 2; });
    bad("jank boost -1", [](auto& r) { r.jankBoost = -1; });
    bad("frame steps two", [](auto& r) { r.frameBoostSteps = {1, 2}; });
    bad("frame steps 11", [](auto& r) { r.frameBoostSteps = {1, 2, 11}; });
    bad("jank steps -1", [](auto& r) { r.jankBoostSteps = {-1, 2, 3}; });
    fine("boost steps", [](auto& r) { r.frameBoostSteps = {0, 5, 10}; r.jankBoostSteps = {1, 2, 3}; });
    bad("delay 500", [](auto& r) { r.startDelayMs = 500; });
    bad("delay 31000", [](auto& r) { r.startDelayMs = 31000; });
    bad("delay -1", [](auto& r) { r.startDelayMs = -1; });
    fine("delay 30000", [](auto& r) { r.startDelayMs = 30000; });
    bad("threads -1", [](auto& r) { r.threadRules = -1; });
    bad("threads 4", [](auto& r) { r.threadRules = 4; });
    // SPEC-v2 fields.
    bad("scene mode 2", [](auto& r) { r.sceneMode = 2; });
    auto rows = [&](int count) {
        return [&, count](TuningRecord& r) {
            r.scenes.clear();
            for (int i = 0; i < count; ++i) r.scenes.push_back({i, true, 1000, pick(policy, 1, 1)});
        };
    };
    fine("32 scenes", rows(32));
    bad("33 scenes", rows(33));
    bad("scene id -1", [&](auto& r) { rows(1)(r); r.scenes[0].sceneId = -1; });
    bad("scene id 100001", [&](auto& r) { rows(1)(r); r.scenes[0].sceneId = 100001; });
    fine("scene id 100000", [&](auto& r) { rows(1)(r); r.scenes[0].sceneId = 100000; });
    bad("scene id duplicate", [&](auto& r) { rows(2)(r); r.scenes[1].sceneId = 0; });
    bad("scene hold 99", [&](auto& r) { rows(1)(r); r.scenes[0].holdMs = 99; });
    bad("scene hold 150", [&](auto& r) { rows(1)(r); r.scenes[0].holdMs = 150; });
    bad("scene hold 10100", [&](auto& r) { rows(1)(r); r.scenes[0].holdMs = 10100; });
    fine("scene hold edges", [&](auto& r) { rows(2)(r); r.scenes[0].holdMs = 100; r.scenes[1].holdMs = 10000; });
    bad("scene value off ladder", [&](auto& r) { rows(1)(r); r.scenes[0].units[1].ceiling += 1; });
    bad("scene min above max", [&](auto& r) { rows(1)(r); r.scenes[0].units[0] = {policy.ladder(0).back(), policy.ladder(0).front()}; });
    auto threads = [&](int count) {
        return [&, count](TuningRecord& r) {
            r.threadRules = kThreadsCustom;
            r.customThreads.clear();
            for (int i = 0; i < count; ++i) r.customThreads.push_back({"thr" + std::to_string(i), 0x3, -1});
        };
    };
    fine("16 thread rules", threads(16));
    bad("17 thread rules", threads(17));
    bad("empty thread name", [&](auto& r) { threads(1)(r); r.customThreads[0].name = ""; });
    bad("long thread name", [&](auto& r) { threads(1)(r); r.customThreads[0].name = std::string(16, 'a'); });
    fine("15 char thread name", [&](auto& r) { threads(1)(r); r.customThreads[0].name = std::string(15, 'a'); });
    bad("zero thread mask", [&](auto& r) { threads(1)(r); r.customThreads[0].mask = 0; });
    bad("absent CPU mask", [&](auto& r) { threads(1)(r); r.customThreads[0].mask = 0x100; });
    bad("high CPU mask", [&](auto& r) { threads(1)(r); r.customThreads[0].mask = 1ULL << 63; });
    fine("present CPU mask", [&](auto& r) { threads(1)(r); r.customThreads[0].mask = 0xFF; });
    {
        Policy unknown;
        unknown.games = policy.games;
        unknown.overlay = policy.overlay;
        TuningRecord custom = good;
        threads(1)(custom);
        std::string error;
        require(!normalize(unknown, custom, &error) && error == "Invalid thread CPU mask",
                "Unknown topology rejects custom masks");
        TuningRecord defaults = good;
        require(normalize(unknown, defaults, &error), "Unknown topology preserves default tuning");
    }
    bad("rt priority 89", [&](auto& r) { threads(1)(r); r.customThreads[0].rtPriority = 89; });
    bad("rt priority 100", [&](auto& r) { threads(1)(r); r.customThreads[0].rtPriority = 100; });
    bad("rt priority 0", [&](auto& r) { threads(1)(r); r.customThreads[0].rtPriority = 0; });
    fine("rt priority edges", [&](auto& r) { threads(2)(r); r.customThreads[0].rtPriority = 90; r.customThreads[1].rtPriority = 99; });
    fine("8 rt rules", [&](auto& r) { threads(8)(r); for (auto& t : r.customThreads) t.rtPriority = 95; });
    bad("9 rt rules", [&](auto& r) { threads(9)(r); for (auto& t : r.customThreads) t.rtPriority = 95; });
    bad("boost length 0", [](auto& r) { r.boostLengthMs = 0; });
    bad("boost length 50", [](auto& r) { r.boostLengthMs = 50; });
    bad("boost length 150", [](auto& r) { r.boostLengthMs = 150; });
    bad("boost length 5100", [](auto& r) { r.boostLengthMs = 5100; });
    fine("boost length edges", [](auto& r) { r.boostLengthMs = 100; });
    fine("boost length max", [](auto& r) { r.boostLengthMs = 5000; });
    bad("sensitivity 0", [](auto& r) { r.dropSensitivity = 0; });
    bad("sensitivity 4", [](auto& r) { r.dropSensitivity = 4; });
    bad("hysteresis -1", [](auto& r) { r.hysteresisDeciC = -1; });
    bad("hysteresis 31", [](auto& r) { r.hysteresisDeciC = 31; });
    fine("hysteresis edges", [](auto& r) { r.hysteresisDeciC = 0; });
    fine("hysteresis max", [](auto& r) { r.hysteresisDeciC = 30; });
    bad("skin target 34500", [](auto& r) { r.skinTargetMilliC = 34500; });
    bad("skin target 48500", [](auto& r) { r.skinTargetMilliC = 48500; });
    bad("skin target 40100", [](auto& r) { r.skinTargetMilliC = 40100; });
    bad("skin target 0", [](auto& r) { r.skinTargetMilliC = 0; });
    fine("skin target off", [](auto& r) { r.skinTargetMilliC = -1; });
    fine("skin target edges", [](auto& r) { r.skinTargetMilliC = 35000; });
    fine("skin target max", [](auto& r) { r.skinTargetMilliC = 48000; });
    bad("power target 1500", [](auto& r) { r.powerTargetMw = 1500; });
    bad("power target 12500", [](auto& r) { r.powerTargetMw = 12500; });
    bad("power target 2100", [](auto& r) { r.powerTargetMw = 2100; });
    fine("power target off", [](auto& r) { r.powerTargetMw = -1; });
    fine("power target edges", [](auto& r) { r.powerTargetMw = 2000; });
    fine("power target max", [](auto& r) { r.powerTargetMw = 12000; });
    bad("variant id characters", [](auto& r) { r.variantId = "a b"; });
    bad("variant id length", [](auto& r) { r.variantId = std::string(65, 'a'); });
    fine("variant", [](auto& r) { r.variantId = std::string(64, 'a'); r.variantName = std::string(64, 'n'); });
    bad("variant name length", [](auto& r) { r.variantName = std::string(65, 'n'); });
    bad("probe resource", [](auto& r) { r.pendingProbe = ProbeRec{3, 1, ""}; });
    bad("probe delta", [](auto& r) { r.pendingProbe = ProbeRec{0, 11, ""}; });
    bad("probe baseline", [](auto& r) { r.pendingProbe = ProbeRec{0, 1, "../x"}; });
    bad("probe time", [](auto& r) { r.pendingProbe = ProbeRec{0, 1, "", -1}; });
    fine("probe", [](auto& r) { r.pendingProbe = ProbeRec{-1, -2, "12_com.example"}; });
    require(!reject(policy, [&] { auto r = good; r.maxOffsets[1] = 40; return r; }()).empty(), "Error text missing");
}

bool sameRecord(const TuningRecord& a, const TuningRecord& b) {
    const auto units = [](const std::array<Bounds, 3>& x, const std::array<Bounds, 3>& y) {
        for (int r = 0; r < 3; ++r) if (x[r].floor != y[r].floor || x[r].ceiling != y[r].ceiling) return false;
        return true;
    };
    if (a.package != b.package || a.mode != b.mode || a.enabled != b.enabled || a.baseProfile != b.baseProfile ||
        a.baseMode != b.baseMode || a.maxOffsets != b.maxOffsets || a.minOffsets != b.minOffsets ||
        a.steps.size() != b.steps.size() || a.thermalCase != b.thermalCase || a.uclampMin != b.uclampMin ||
        a.gpuDefaultHz != b.gpuDefaultHz || a.keepCoresOnline != b.keepCoresOnline || a.frameBoost != b.frameBoost ||
        a.frameBoostSteps != b.frameBoostSteps || a.jankBoost != b.jankBoost || a.jankBoostSteps != b.jankBoostSteps ||
        a.startDelayMs != b.startDelayMs || a.threadRules != b.threadRules || a.sceneMode != b.sceneMode ||
        a.scenes.size() != b.scenes.size() || a.customThreads.size() != b.customThreads.size() ||
        a.boostLengthMs != b.boostLengthMs || a.dropSensitivity != b.dropSensitivity || a.dropLowerPermille != b.dropLowerPermille || a.dropUpperPermille != b.dropUpperPermille ||
        a.hysteresisDeciC != b.hysteresisDeciC || a.skinTargetMilliC != b.skinTargetMilliC ||
        a.powerTargetMw != b.powerTargetMw || a.variantId != b.variantId || a.variantName != b.variantName ||
        a.pendingProbe.has_value() != b.pendingProbe.has_value()) return false;
    for (size_t i = 0; i < a.steps.size(); ++i)
        if (a.steps[i].temperature != b.steps[i].temperature || !units(a.steps[i].units, b.steps[i].units)) return false;
    for (size_t i = 0; i < a.scenes.size(); ++i)
        if (a.scenes[i].sceneId != b.scenes[i].sceneId || a.scenes[i].enabled != b.scenes[i].enabled ||
            a.scenes[i].holdMs != b.scenes[i].holdMs || !units(a.scenes[i].units, b.scenes[i].units)) return false;
    for (size_t i = 0; i < a.customThreads.size(); ++i)
        if (a.customThreads[i].name != b.customThreads[i].name || a.customThreads[i].mask != b.customThreads[i].mask ||
            a.customThreads[i].rtPriority != b.customThreads[i].rtPriority) return false;
    if (a.pendingProbe && (a.pendingProbe->resource != b.pendingProbe->resource ||
        a.pendingProbe->deltaSteps != b.pendingProbe->deltaSteps || a.pendingProbe->baseline != b.pendingProbe->baseline ||
        a.pendingProbe->armedAtEpochMs != b.pendingProbe->armedAtEpochMs)) return false;
    return true;
}

TuningRecord fullRecord(const Policy& policy) {
    TuningRecord r;
    r.package = "com.example.full"; r.mode = 2; r.enabled = false;
    r.baseProfile = "default"; r.baseMode = kBasePerformance;
    r.maxOffsets = {1, -2, 3}; r.minOffsets = {-1, 2, 0};
    r.steps = {{0, pick(policy, -1, 3)}, {38.5, pick(policy, 2, 2)}, {44.0, pick(policy, 3, 1)}};
    r.thermalCase = 300; r.uclampMin = 55; r.gpuDefaultHz = policy.ladder(2)[2]; r.keepCoresOnline = 1;
    r.frameBoost = 1; r.frameBoostSteps = {1, 2, 3}; r.jankBoost = 0; r.jankBoostSteps = {4, 5, 6};
    r.startDelayMs = 3000; r.threadRules = kThreadsCustom;
    r.sceneMode = 1; r.scenes = {{12, true, 800, pick(policy, 1, 4)}, {7, false, 10000, pick(policy, -1, 0)}};
    r.customThreads = {{"UnityMain", 0xC0, 95}, {"RenderThread", 0x3F, -1}};
    r.boostLengthMs = 700; r.dropSensitivity = kKeep; r.dropLowerPermille = 600; r.dropUpperPermille = 980; r.hysteresisDeciC = 15; r.skinTargetMilliC = 42500; r.powerTargetMw = 6500;
    r.variantId = "perf-120_hot"; r.variantName = "Performance 120 fps hot";
    r.pendingProbe = ProbeRec{2, -1, "1760000000000_com.example.full", 1793300000123};
    return r;
}

void sampleDirectory(const fs::path& dir, std::vector<std::string>* names) {
    names->clear();
    if (DIR* d = ::opendir(dir.c_str())) {
        while (const dirent* e = ::readdir(d)) if (e->d_name[0] != '.') names->push_back(e->d_name);
        ::closedir(d);
    }
    std::sort(names->begin(), names->end());
}

void store(const Policy& policy, const fs::path& temp) {
    const fs::path dir = temp / "tuning-store";
    fs::remove_all(dir);
    std::string error;
    {
        TuningStore s(dir.string());
        s.load(policy);
        require(s.all(0).empty() && s.diagnostics.empty(), "Fresh store not empty");
        TuningRecord full = fullRecord(policy);
        require(s.put(0, full, &error), error);
        TuningRecord simple = baseline(policy, "default", 1);
        require(s.put(0, simple, &error), error);
        TuningRecord other = baseline(policy, "com.example.other", 3);
        other.maxOffsets = {-3, 0, 4};
        require(s.put(10, other, &error), error);
        require(s.setArmed(0, "com.example.full", true, &error) && s.setArmed(0, "com.example.armed", true, &error) &&
                s.setArmed(0, "com.example.gone", true, &error) && s.setArmed(0, "com.example.gone", false, &error), error);
        for (int i = 0; i < 25; ++i) {
            SensitivityEntry entry;
            entry.package = "com.example.full"; entry.mode = 2; entry.resource = i % 3; entry.deltaSteps = i % 5 - 2;
            entry.dFloor5 = i * 0.25; entry.dTail95 = -i * 0.125; entry.dPowerMw = 100.5 + i; entry.dSkinPeakC = 0.1 * i;
            require(s.addSensitivity(0, entry, &error), error);
        }
        SensitivityEntry kept;
        kept.package = "com.example.full"; kept.mode = 1; kept.resource = 2; kept.deltaSteps = 1;
        require(s.addSensitivity(0, kept, &error), error);
        kept.package = "bad pkg";
        require(!s.addSensitivity(0, kept, &error), "Invalid sensitivity accepted");
        require(s.sensitivity(0, "com.example.full", 2).size() == 20, "Sensitivity not trimmed to 20");
        require(s.find(0, "com.example.full", 2) && sameRecord(*s.find(0, "com.example.full", 2), full), "Stored record differs in memory");
        TuningRecord invalid = full; invalid.mode = 9;
        require(!s.put(0, invalid, &error) && !s.find(0, "com.example.full", 9), "Invalid record stored");
        require(s.erase(10, "com.example.other", 3) && !s.erase(10, "com.example.other", 3), "Erase result");
        require(s.put(10, other, &error), error);
    }
    std::vector<std::string> names;
    sampleDirectory(dir, &names);
    require(std::none_of(names.begin(), names.end(), [](const auto& n) { return n.find(".tmp") != std::string::npos; }), "Temporary file left behind");
    write(dir / "u7.xml", "this is not xml <<<");
    write(dir / "u8-sensitivity.xml", "garbage");
    write(dir / "u9.xml", R"(<game-tuning version="1"><tuning pkg="a.b" mode="9"/><tuning pkg="good.pkg" mode="2" baseMode="2"/>
        <tuning pkg="good.pkg" mode="2"/><tuning pkg="dup..pkg" mode="1" maxOffsets="99,0,0"/><armed pkg="bad pkg"/><armed pkg="ok.pkg"/></game-tuning>)");
    {
        TuningStore s(dir.string());
        s.load(policy);
        const TuningRecord full = fullRecord(policy);
        const auto found = s.find(0, "com.example.full", 2);
        require(found && sameRecord(*found, full), "Reloaded record differs field by field");
        require(found->pendingProbe && found->pendingProbe->armedAtEpochMs == 1793300000123 &&
                s.users() == std::vector<int>({0, 9, 10}), "Probe arm time and users survive daemon restart");
        require(s.all(0).size() == 2 && s.all(10).size() == 1 && sameRecord(*s.find(10, "com.example.other", 3), [&] {
            TuningRecord o = baseline(policy, "com.example.other", 3); o.maxOffsets = {-3, 0, 4}; return o; }()), "Reloaded set differs");
        require(s.armed(0, "com.example.full") && s.armed(0, "com.example.armed") && !s.armed(0, "com.example.gone") &&
                s.armedPackages(0).size() == 2, "Armed set differs");
        const auto history = s.sensitivity(0, "com.example.full", 2);
        require(history.size() == 20 && history.front().dFloor5 == 5 * 0.25 && history.back().dFloor5 == 24 * 0.25 &&
                history.back().deltaSteps == 24 % 5 - 2 && history.back().dSkinPeakC == 0.1 * 24 && history.back().dTail95 == -24 * 0.125 &&
                history.back().dPowerMw == 124.5 && history.back().resource == 0, "Sensitivity differs after reload");
        require(s.sensitivity(0, "com.example.full", 1).size() == 1, "Second sensitivity key lost");
        require(!fs::exists(dir / "u7.xml") && fs::exists(dir / "u7.xml.bad"), "Garbage tuning file not renamed to .bad");
        require(!fs::exists(dir / "u8-sensitivity.xml") && fs::exists(dir / "u8-sensitivity.xml.bad"), "Garbage sensitivity file not renamed");
        require(s.find(9, "good.pkg", 2) && s.all(9).size() == 1 && s.armed(9, "ok.pkg") && !s.armed(9, "bad pkg") &&
                s.diagnostics.size() >= 5, "Invalid entries not skipped with diagnostics");
        require(s.armedPackages(9).size() == 1, "Invalid armed entry kept");
        require(s.put(7, baseline(policy, "default", 2), &error) && fs::exists(dir / "u7.xml"), "Store unusable after quarantine");
    }
    // Limits.
    const fs::path limits = temp / "tuning-limits";
    fs::remove_all(limits);
    TuningStore s(limits.string());
    s.load(policy);
    for (int i = 0; i < 300; ++i) for (int mode = 1; mode <= 3; ++mode) {
        TuningRecord r = baseline(policy, "p" + std::to_string(i), mode);
        require(s.put(1, r, &error), "Put " + std::to_string(i) + "/" + std::to_string(mode) + ": " + error);
    }
    TuningRecord extra = baseline(policy, "p300", 1);
    require(!s.put(1, extra, &error) && error == "limit" && s.all(1).size() == 900, "900-entry limit not enforced");
    TuningRecord replace = baseline(policy, "p5", 2);
    replace.maxOffsets[0] = 4;
    require(s.put(1, replace, &error) && s.find(1, "p5", 2)->maxOffsets[0] == 4, "Replacing an entry at the limit failed");
    for (int i = 0; i < 300; ++i) require(s.setArmed(1, "a" + std::to_string(i), true, &error), error);
    require(!s.setArmed(1, "a300", true, &error) && error == "limit" && s.armedPackages(1).size() == 300, "300-entry armed limit not enforced");
    require(s.setArmed(1, "a5", true, &error) && s.setArmed(1, "a5", false, &error) && s.setArmed(1, "a300", true, &error), "Armed slot not reusable");
    TuningStore reloaded(limits.string());
    reloaded.load(policy);
    require(reloaded.all(1).size() == 900 && reloaded.armedPackages(1).size() == 300 && reloaded.diagnostics.empty(), "Full store reload");
    {
        const auto migrationDir = dir / "migration"; fs::create_directories(migrationDir);
        const std::string saved = "<tuning pkg=\"com.example.kept\" mode=\"1\" variant=\"custom\" variantName=\"Saved\" base=\"default\" baseMode=\"1\" skinTarget=\"41000\"/>";
        const std::string builtin = "<tuning pkg=\"com.example.auto\" mode=\"1\" base=\"default\" baseMode=\"1\" skinTarget=\"41000\" powerTarget=\"6000\" boostLength=\"500\" dropSensitivity=\"2\"/>";
        const auto file=migrationDir/"u0.xml";
        write(file,"<game-tuning version=\"1\">"+saved+builtin+"</game-tuning>");
        TuningStore migrated(migrationDir.string());migrated.load(policy);
        std::ifstream in(file);std::string once((std::istreambuf_iterator<char>(in)),{});
        require(once.find(saved)!=std::string::npos,"Migration changed saved profile bytes");
        require(once.find(builtin)!=std::string::npos,"Migration changed a saved legacy tuning");
        migrated.load(policy);std::ifstream again(file);std::string twice((std::istreambuf_iterator<char>(again)),{});
        require(once==twice,"Migration ran twice");
        require(migrated.setArmed(0,"com.example.other",true,&error),error);
        std::ifstream savedAgain(file);std::string kept((std::istreambuf_iterator<char>(savedAgain)),{});
        require(kept.find(saved)!=std::string::npos&&kept.find(builtin)!=std::string::npos,"Unrelated store write changed saved profile bytes");
        const std::string already="<game-tuning version=\"2\">"+saved+"</game-tuning>";
        write(file,already);migrated.load(policy);std::ifstream after(file);std::string unchanged((std::istreambuf_iterator<char>(after)),{});
        require(unchanged==already,"New schema store migrated again");
        for(int level:{1,2,3}) {
            write(file,"<game-tuning version=\"2\"><tuning pkg=\"com.example.legacy\" mode=\"1\" variant=\"legacy\" variantName=\"Saved\" base=\"default\" baseMode=\"1\" dropSensitivity=\""+std::to_string(level)+"\"/></game-tuning>");
            migrated.load(policy);auto old=migrated.find(0,"com.example.legacy",1);
            require(old&&old->dropLowerPermille==600&&old->dropUpperPermille==(level==1?900:level==3?980:965),"Legacy level mapping");
        }
        write(file,"<game-tuning version=\"2\"><tuning pkg=\"com.example.invalid\" mode=\"1\" variant=\"saved\" base=\"default\" baseMode=\"1\" dropLower=\"900\" dropUpper=\"600\" dropSensitivity=\"3\"/></game-tuning>");
        migrated.load(policy);const auto invalid=migrated.find(0,"com.example.invalid",1);
        require(invalid&&invalid->dropLowerPermille==-1&&invalid->dropUpperPermille==-1&&invalid->dropSensitivity==kKeep,"Invalid stored window did not fall back to stock");
        std::cout<<"G1B_STORE_MIGRATION_OK builtins saved-bytes idempotent new-schema legacy-windows\n";
    }

}

void modeBuilds(const Policy& policy) {
    std::string error;
    // Disabled records fall back to stock.
    TuningRecord off = baseline(policy, "default", 1);
    off.enabled = false;
    Mode unused;
    require(!build(policy, off, &unused, nullptr, &error) && error == "disabled", "Disabled record built");
    require(baseModeName(1) == std::string("Balance") && baseModeName(2) == std::string("Savage") &&
            baseModeName(3) == std::string("PowerSave") && baseModeName(4) == std::string("chaoshen") &&
            !baseModeName(0) && !baseModeName(5), "Base mode names");
    for (int i = 1; i <= 4; ++i) require(baseModeOf(baseModeName(i)) == i, "Base mode inverse");
    require(baseModeOf("Godlike") == 0 && gameModeToBase(policy, 1) == kBaseBalanced && gameModeToBase(policy, 2) == kBasePerformance &&
            gameModeToBase(policy, 3) == kBaseBattery && gameModeToBase(policy, 77) == kBaseBalanced, "Game mode mapping");

    // Custom scenes and threads.
    TuningRecord custom = baseline(policy, "default", 1);
    custom.sceneMode = 1;
    custom.scenes = {{3, true, 1500, pick(policy, 2, 3)}, {9, false, 500, pick(policy, -1, 0)}, {40, true, 200, pick(policy, -1, 0)}};
    custom.threadRules = kThreadsCustom;
    custom.customThreads = {{"UnityMain", 0xC0, 95}, {"Render", 0x3F, -1}, {"Audio", 0x01, 90}};
    custom.boostLengthMs = 900; custom.dropSensitivity = 3; custom.hysteresisDeciC = 25;
    custom.skinTargetMilliC = 43000; custom.powerTargetMw = 7000;
    Mode tuned; Selection selection{};
    require(build(policy, custom, &tuned, &selection, &error), error);
    require(tuned.scenes.size() == 2 && tuned.scenes.count(3) && tuned.scenes.count(40) && !tuned.scenes.count(9), "Custom scene rows");
    const auto& row = tuned.scenes.at(3);
    require(row.timeMs == 1500, "Scene hold");
    checkUnits(policy, row.frequencies, "scene");
    require(row.frequencies.units[0].floor == policy.ladder(0).at(2) && row.frequencies.units[2].ceiling == policy.ladder(2).at(policy.ladder(2).size() - 4), "Scene units");
    require(tuned.scenes.at(40).frequencies.units[1].floor == -1 && tuned.scenes.at(40).frequencies.units[0].ceiling == policy.ladder(0).back(),
            "Scene without floor or ceiling uses the ladder top");
    require(tuned.bindCore.size() == 3 && tuned.bindCore[0].name == "UnityMain" && tuned.bindCore[0].mask == 0xC0 &&
            tuned.schedulers.size() == 2 && tuned.schedulers[1].name == "Audio" && tuned.schedulers[1].priority == 90 &&
            tuned.schedulers[0].priority == 95 && tuned.schedulers[0].enabled && selection.threadPlacement, "Custom thread rules");
    require(tuned.boostLengthMs == 900 && tuned.dropSensitivity == 3 && tuned.hysteresisDeciC == 25 &&
            tuned.skinTargetMilliC == 43000 && tuned.powerTargetMw == 7000, "Expert fields stored in the mode");
    require(selection.profile == "default" && selection.mode == &tuned, "Selection of a tuned mode");
    {
        TuningRecord none = custom;
        none.sceneMode = 0; none.threadRules = kThreadsOff;
        Mode m; Selection s{};
        require(build(policy, none, &m, &s, &error) && m.scenes.empty() && m.bindCore.empty() && m.schedulers.empty() && !s.threadPlacement,
                "Scenes or threads off");
        none.sceneMode = kKeep; none.scenes.clear(); none.threadRules = kThreadsBase;
        require(build(policy, none, &m, &s, &error) && sameScenes(m, *policy.select("default", 1).mode) && s.threadPlacement, "Scene KEEP");
    }
    // The band selection and the FPS controller read their settings from the mode.
    Mode hot = tuned;
    hot.bands = {{0, {}, {}}, {40, {}, {}}};
    hot.hysteresisDeciC = 30;
    require(Policy::band(hot, 41, -1, 1) == 1 && Policy::band(hot, 38, 1, 1) == 1 && Policy::band(hot, 36.9, 1, 1) == 0, "Mode hysteresis 3.0");
    hot.hysteresisDeciC = 0;
    require(Policy::band(hot, 39.9, 1, 1) == 0, "Mode hysteresis 0");
    hot.hysteresisDeciC = -1;
    require(Policy::band(hot, 39.5, 1, 1) == 1 && Policy::band(hot, 38.9, 1, 1) == 0, "Overlay hysteresis used when unset");
    for (int level : {1, 2, 3}) {
        Mode m; m.dropSensitivity = level; m.boostLengthMs = 800;
        FpsController fps; fps.reset(0);
        for (int64_t t = 3000; t <= 7000; t += 1000) fps.sample(t, 60, true, m);
        const bool boosted = fps.sample(8000, 55, true, m);
        require(boosted && (!boosted || fps.deadline() == 8800), "Fixed FPS trigger " + std::to_string(level));
    }
    {
        FpsController fps; fps.reset(0);
        for (int64_t t = 3000; t <= 7000; t += 1000) fps.sample(t, 60, true);
        require(!fps.sample(8000, 55, true) && !fps.active(8000), "Missing boost length applied a boost");
    }
    {
        TuningRecord configured=baseline(policy,"default",1); configured.dropLowerPermille=500;configured.dropUpperPermille=900;
        Mode chosen;require(normalize(policy,configured,&error)&&build(policy,configured,&chosen,nullptr,&error),error);
        const auto shown=view(policy,chosen,"default",1,true,kThreadsOwn);
        require(chosen.dropLowerPermille==500&&chosen.dropUpperPermille==900&&shown.dropLowerPermille==500&&shown.dropUpperPermille==900,"Chosen window lost in profile");
        configured.dropLowerPermille=configured.dropUpperPermille=-1;
        require(build(policy,configured,&chosen,nullptr,&error)&&chosen.dropLowerPermille==-1&&chosen.dropUpperPermille==-1,"Window clear did not restore stock");
    }
    for (const auto& bounds : {std::pair<int,int>{600,965}, {100,995}, {500,900}, {650,980}}) {
        require(validDropWindow(bounds.first,bounds.second), "Valid frame window rejected");
        for (int which : {0,1}) for (float epsilon : {-0.01f,0.0f,0.01f}) {
            const double ratio=(which ? bounds.second : bounds.first)/1000.0;
            // Four identical prior samples make this exactly the requested boundary.
            const float boundary=static_cast<float>(240.0*ratio/(5.0-ratio));
            const float value=boundary+epsilon;
            FpsController fps; fps.reset(0);
            for(int t=3000;t<=6000;t+=1000) fps.sample(t,60,true,800,-1,bounds.first,bounds.second);
            const double mean=static_cast<float>((240.0f+value)/5.0f);
            const bool expected=value > bounds.first/1000.0*mean && value < bounds.second/1000.0*mean && value>=10 && 60-value <= 60*.8;
            require(fps.sample(7000,value,true,800,-1,bounds.first,bounds.second)==expected,"Frame window boundary");
        }
    }
    for (const auto& bounds : {std::pair<int,int>{95,965},{600,1000},{600,605},{605,600},{601,965},{-1,965}}) {
        require(!validDropWindow(bounds.first,bounds.second),"Invalid window accepted");
        TuningRecord invalid=baseline(policy,"default",1);invalid.dropLowerPermille=bounds.first;invalid.dropUpperPermille=bounds.second;
        require(!normalize(policy,invalid,&error),"Invalid chosen window accepted");
        FpsController fps; fps.reset(0);
        for(int t=3000;t<=7000;t+=1000)fps.sample(t,60,true,800,-1,bounds.first,bounds.second);
        require(fps.sample(8000,55,true,800,-1,bounds.first,bounds.second),"Invalid runtime window did not use stock");
    }
    std::cout << "G1B_WINDOWS_OK strict-bounds invalid-stock-fallback\n";
    // GPU default and cores.
    TuningRecord hints = baseline(policy, "default", 1);
    hints.gpuDefaultHz = policy.ladder(2)[1]; hints.uclampMin = 35; hints.keepCoresOnline = 1;
    Mode m;
    require(build(policy, hints, &m, nullptr, &error), error);
    const auto view1 = view(policy, m, "default", 1, true, kThreadsOwn);
    require(view1.gpuDefaultHz == policy.ladder(2)[1] && view1.uclampMin == 35 && view1.keepCoresOnline &&
            view1.tuned && view1.profileKey == "default", "View of a hinted mode");
    hints.gpuDefaultHz = -1; hints.uclampMin = -1; hints.keepCoresOnline = 0;
    require(build(policy, hints, &m, nullptr, &error) && std::none_of(m.perfHints.begin(), m.perfHints.end(),
            [](const auto& h) { return h.opcode == 0x40CAC000 || h.opcode == 0x42800000 || h.opcode == 0x41000000 || h.opcode == 0x41000200; }),
            "Hints not removed");
    const auto view2 = view(policy, m, "default", 1, true, kThreadsOwn);
    require(view2.gpuDefaultHz == -1 && view2.uclampMin == -1 && !view2.keepCoresOnline, "View after removal");
    // Boost handling.
    TuningRecord boost = baseline(policy, "default", 1);
    boost.frameBoost = 0; boost.jankBoost = 1;
    const Mode& stockDefault = *policy.select("default", 1).mode;
    require(build(policy, boost, &m, nullptr, &error) && !m.fpsAdjust &&
            m.jankAdjust == stockDefault.jankAdjust &&
            m.jankAdjustTime == stockDefault.jankAdjustTime, "Boost switches");
    boost.jankBoost = 0; boost.frameBoost = 1; boost.frameBoostSteps = {4, 5, 6};
    require(build(policy, boost, &m, nullptr, &error) && !m.jankAdjust && m.jankAdjustTime == 0 && m.fpsAdjust &&
            *m.fpsAdjust == std::array<int, 3>{4, 5, 6}, "Boost steps");
    const auto v3 = view(policy, m, "default", 1, true, kThreadsOwn);
    require(v3.frameBoostSteps == std::vector<int>({4, 5, 6}) && v3.jankBoostSteps.empty(), "Boost view");
    // Views of stock and tuned modes agree on the steps.
    for (const auto& [key, modes] : policy.games) {
        const Mode& stock = *policy.select(key, 1).mode;
        const auto stockView = view(policy, stock, key, 1, false, kThreadsOwn);
        Mode t;
        require(build(policy, baseline(policy, key, 1), &t, nullptr, &error), error);
        const auto tunedView = view(policy, t, key, 1, true, kThreadsOwn);
        require(stockView.steps.size() == tunedView.steps.size() && stockView.sceneCount == tunedView.sceneCount &&
                stockView.threadRuleCount == tunedView.threadRuleCount && stockView.uclampMin == tunedView.uclampMin &&
                stockView.gpuDefaultHz == tunedView.gpuDefaultHz && stockView.keepCoresOnline == tunedView.keepCoresOnline &&
                stockView.thermalCase == tunedView.thermalCase && stockView.frameBoostSteps == tunedView.frameBoostSteps &&
                stockView.hysteresisDeciC == tunedView.hysteresisDeciC && stockView.boostLengthMs == stock.boostLengthMs &&
                stockView.dropSensitivity == -1 && stockView.sceneMode == tunedView.sceneMode, key + ": View mismatch");
        for (size_t i = 0; i < stockView.steps.size(); ++i)
            require(stockView.steps[i].temperature == tunedView.steps[i].temperature &&
                    stockView.steps[i].units[0].ceiling == tunedView.steps[i].units[0].ceiling, key + ": View step");
    }
    // Stock steps survive the editor round trip.
    size_t rejected = 0;
    for (const auto& [key, modes] : policy.games) for (const auto& [name, stock] : modes) {
        TuningRecord r = baseline(policy, key, 1);
        r.baseMode = baseModeOf(name);
        r.steps = stockSteps(stock);
        if (!normalize(policy, r, &error)) { ++rejected; std::cout << "TUNING_STOCK_STEPS_REJECTED " << key << "/" << name << ": " << error << '\n'; }
    }
    std::cout << "TUNING_STOCK_STEPS rejected=" << rejected << '\n';
}

void chaoshen(const Policy& policy, const char* stock) {
    tinyxml2::XMLDocument xml;
    require(xml.LoadFile(stock) == tinyxml2::XML_SUCCESS, "Cannot load fixture");
    size_t present = 0;
    for (auto game = xml.FirstChildElement("GameOptPolicy")->FirstChildElement("GamePolicy")->FirstChildElement("Game"); game;
         game = game->NextSiblingElement("Game")) {
        bool has = false;
        for (auto m = game->FirstChildElement("Mode"); m; m = m->NextSiblingElement("Mode")) {
            const std::string name = m->Attribute("name");
            has = has || name == "chaoshen" || name == "Godlike";
        }
        std::istringstream packages(game->Attribute("pkg"));
        std::string key;
        std::getline(packages, key, ',');
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        const auto it = policy.games.find(key);
        require(it != policy.games.end(), "Stock key missing: " + key);
        if (!has) { require(!it->second.count("chaoshen"), key + ": Unexpected maximum mode"); continue; }
        const bool diagnosed = std::any_of(policy.diagnostics.begin(), policy.diagnostics.end(),
            [&](const auto& d) { return d.find("/chaoshen:") != std::string::npos; });
        require(it->second.count("chaoshen") || diagnosed, key + ": Maximum mode neither parsed nor diagnosed");
        ++present;
    }
    require(present > 40, "Too few maximum modes");
    size_t parsed = 0;
    for (const auto& [key, modes] : policy.games) {
        const auto it = modes.find("chaoshen");
        if (it == modes.end()) continue;
        ++parsed;
        for (int id = -3; id <= 20; ++id) {
            require(policy.select(key, id).mode != &it->second, key + ": select() returned the maximum mode");
            require(policy.select("alias.of." + key, id).mode != &it->second, "select() returned the maximum mode by alias");
        }
        TuningRecord r;
        r.package = key; r.mode = 2; r.baseMode = kBaseMaximum; r.threadRules = kThreadsBase;
        std::string error;
        require(normalize(policy, r, &error), key + ": " + error);
        Mode tuned;
        require(build(policy, r, &tuned, nullptr, &error), key + ": " + error);
        expectParity(policy, it->second, tuned, key + "/chaoshen");
    }
    for (const auto& [id, name] : policy.overlay.modes) require(name != "chaoshen" && name != "Godlike", "Overlay maps to the maximum mode");
    require(parsed > 40, "Too few parsed maximum modes");
    std::cout << "TUNING_CHAOSHEN version=" << policy.version << " stock_groups=" << present << " parsed_keys=" << parsed << '\n';
}

void synthetic(const fs::path& temp) {
    const auto stock = temp / "tuning-policy.xml", overlay = temp / "tuning-overlay.xml";
    write(stock, R"(<GameOptPolicy version="test"><PreEnv>
      <CPU><cluster name="Gold" min="0x40800000" max="0x40804000">384000 1000000 2000000</cluster>
      <cluster name="Prime" min="0x40800200" max="0x40804200">768000 2000000 3000000</cluster></CPU>
      <GPU><cluster name="Gpu" min="0x42804000" max="0x42808000">1000000000 800000000 600000000</cluster></GPU>
      </PreEnv><GamePolicy><Game pkg="default"><ThermalTempType>11</ThermalTempType>
      <Mode name="Balance"><ThermalSceneCode>200</ThermalSceneCode><Policy><TempLevel temp="25"><item type="freq" name="Gold">1_2</item></TempLevel></Policy></Mode>
      <Mode name="chaoshen"><ThermalSceneCode>300</ThermalSceneCode><Policy>
        <TempLevel temp="50"><item type="freq" name="Gold">1_2</item></TempLevel>
        <TempLevel temp="40"><item type="freq" name="Gold">1_2</item></TempLevel></Policy></Mode></Game>
      <Game pkg="game.one, game.two" name="Game One"><Mode name="Balance"><Policy><TempLevel temp="25"/></Policy></Mode>
      <Mode name="chaoshen"><ThermalSceneCode>400</ThermalSceneCode><PerfHint><opcode id="0x40CAC000" time="0">100</opcode></PerfHint>
        <Policy><TempLevel temp="30"><item type="freq" name="Gold">2_2</item></TempLevel></Policy></Mode></Game>
      <Game pkg="game.three"><Mode name="Balance"><Policy><TempLevel temp="25"/></Policy></Mode>
      <Mode name="Godlike"><ThermalSceneCode>300</ThermalSceneCode><Policy><TempLevel temp="30"/></Policy></Mode></Game>
      <Game pkg="game.four"><Mode name="Balance"><Policy><TempLevel temp="25"/></Policy></Mode>
      <Mode name="chaoshen"><ThermalTempType>12</ThermalTempType></Mode></Game>
      </GamePolicy></GameOptPolicy>)");
    write(overlay, R"(<game-policy version="1"><mode id="1" profile="Balance"/><mode id="2" profile="Savage"/><mode id="3" profile="PowerSave"/>
      <safety sensor="quiet-therm" hysteresisC="1.5" sampleMs="1000" skinTarget="41000">
      <resource name="Gold" ceiling="1800000" opps="unused"/><resource name="Prime" ceiling="2500000" opps="unused"/>
      <resource name="Gpu" ceiling="900000000" opps="unused"/></safety>
      <alias pkg="global.one" profile="game.two"/><alias pkg="global.default" profile="default"/></game-policy>)");
    Policy policy; std::string error;
    require(policy.load(stock.string(), overlay.string(), &error), "Maximum-mode errors must not fail load: " + error);
    require(!policy.games.at("default").count("chaoshen") && policy.games.at("default").count("Balance"), "Decreasing temperatures kept");
    require(std::any_of(policy.diagnostics.begin(), policy.diagnostics.end(),
            [](const auto& d) { return d.rfind("default/chaoshen: ", 0) == 0; }), "Missing chaoshen diagnostic");
    require(std::any_of(policy.diagnostics.begin(), policy.diagnostics.end(),
            [](const auto& d) { return d.rfind("game.four/chaoshen: ", 0) == 0; }), "Missing sensor diagnostic");
    require(policy.games.at("game.one").at("chaoshen").thermalCase == 400 && policy.games.at("game.two").count("chaoshen") &&
            policy.games.at("game.three").at("chaoshen").thermalCase == 300 && policy.games.at("game.three").at("chaoshen").bands.size() == 1,
            "Maximum mode parse or Godlike name");
    require(policy.select("game.one", 2).mode != &policy.games.at("game.one").at("chaoshen"), "select() returned the maximum mode");
    require(policy.overlay.skinTargetMilliC == 41000 && policy.games.at("game.one").at("Balance").skinTargetMilliC == 41000 &&
            policy.games.at("game.one").at("Balance").hysteresisDeciC == -1, "Overlay skin target");
    // Sources.
    require(policy.sources.size() == 4, "Source count");
    const auto source = [&](const std::string& key) -> const Source& {
        for (const auto& s : policy.sources) if (s.key == key) return s;
        throw std::runtime_error("Missing source " + key);
    };
    require(source("default").name.empty() && source("default").packages == std::vector<std::string>({"default", "global.default"}), "Default source");
    require(source("game.one").name == "Game One" && source("game.one").packages ==
            std::vector<std::string>({"game.one", "game.two", "global.one"}), "Grouped source with alias");
    require(source("game.three").name.empty() && source("game.three").packages.size() == 1, "Source without name");
    // Present mask and tables.
    policy.resolve({{{384000, 1000000, 2000000}, {768000, 2000000, 3000000}, {600000000, 800000000, 1000000000}}}, {6, 2}, 0xFF);
    require(policy.presentMask() == 0xFF && policy.cores(0) == 6 && policy.cores(1) == 2, "Resolve inputs");
    require(policy.ladder(0) == std::vector<int>({384000, 1000000}) && policy.ladder(1) == std::vector<int>({768000, 2000000}) &&
            policy.ladder(2) == std::vector<int>({600000000, 800000000}) && policy.tables()[2].front() == 1000000000, "Ladders");
    require(policy.declared(0).size() == 3 && cpuMask("0 1 2 3") == 0xF && cpuMask("6 7") == 0xC0 && cpuMask("1 1") == 0 && cpuMask("x") == 0,
            "Declared tables or CPU mask");
    require(source("game.one").packages.size() == 3, "Sources are fixed after load");
    Mode a;
    a.bands = {{0, {}, {}}, {35, {}, {}}, {38, {}, {}}};
    require(Policy::linearBand(a, 30, -1, 1) == 0 && Policy::linearBand(a, 36, -1, 1) == 1 && Policy::linearBand(a, 40, -1, 1) == 2 &&
            Policy::linearBand(a, 37.5, 2, 1) == 2 && Policy::linearBand(a, 36.9, 2, 1) == 1 && Policy::linearBand(a, 34.5, 1, 1) == 1 &&
            Policy::linearBand(a, 33.9, 1, 1) == 0 && Policy::linearBand(a, 20, 9, 1) == 0 && Policy::linearBand({}, 40, -1, 1) == 0 &&
            Policy::positiveCount(a) == 2, "linearBand rules");
    std::cout << "TUNING_SYNTHETIC_OK sources=" << policy.sources.size() << " diagnostics=" << policy.diagnostics.size() << '\n';
}

void rangeProvenance(const Policy& policy, const fs::path& temp) {
    const auto dir = temp / "range-provenance";
    fs::create_directories(dir);
    TuningStore store(dir.string()); store.load(policy);
    auto tuning = fullRecord(policy);
    tuning.pendingProbe = ProbeRec{2, -1, "100_com.example.full", 1234, 7, 60, 1, 2, .85f, 120};
    std::string error;
    require(store.put(0, tuning, &error), error);
    SensitivityEntry e;
    e.package = tuning.package; e.mode = tuning.mode; e.resource = 2; e.deltaSteps = -1;
    e.baselineId = "100_com.example.full"; e.resultId = "200_com.example.full";
    e.baselineRevision = 7; e.resultRevision = 2; e.analysisTargetFps = 60; e.diagnosis = 1; e.analysisVersion = 2;
    e.renderScale = .85f; e.refreshRate = 120;
    require(store.addSensitivity(0, e, &error), error);
    TuningStore reload(dir.string()); reload.load(policy);
    require(reload.find(0, tuning.package, tuning.mode)->pendingProbe->baselineRevision == 7 &&
            reload.sensitivity(0, tuning.package, tuning.mode).back().resultRevision == 2, "Provenance round trip");
    const auto probe = *reload.find(0, tuning.package, tuning.mode)->pendingProbe;
    const auto evidence = reload.sensitivity(0, tuning.package, tuning.mode).back();
    require(probe.renderScale == .85f && probe.refreshRate == 120 && probe.analysisVersion == 2 &&
            evidence.renderScale == .85f && evidence.refreshRate == 120 && evidence.analysisVersion == 2,
            "Scale/refresh provenance persists through probe and history reload");
    std::cout << "F6_PROVENANCE_STORAGE_PASS probe history scale refresh version\n";
    require(reload.invalidateEvidence(0, tuning.package, tuning.mode, e.resultId, &error), error);
    require(reload.sensitivity(0, tuning.package, tuning.mode).empty(), "Editing result invalidates sensitivity");
    e.baselineId.clear(); e.resultId.clear();
    require(reload.addSensitivity(0, e, &error), error);
    require(reload.invalidateEvidence(0, tuning.package, tuning.mode, "100_com.example.full", &error), error);
    require(!reload.find(0, tuning.package, tuning.mode)->pendingProbe &&
            reload.sensitivity(0, tuning.package, tuning.mode).empty(), "Baseline edit cancels probe and legacy history");
    TuningStore restart(dir.string()); restart.load(policy);
    require(!restart.find(0, tuning.package, tuning.mode)->pendingProbe &&
            restart.sensitivity(0, tuning.package, tuning.mode).empty(), "Invalidation is durable");
    std::cout << "TUNING_RANGE_PROVENANCE_PASS persistence baseline result legacy\n";
}

void device(const char* stockXml, const char* overlayXml, const fs::path& temp, bool storeTests) {
    Policy policy;
    loadDevice(policy, stockXml, overlayXml);
    const size_t parityCases = parity(policy), linearCases = linear(policy), offsetCases = offsets(policy);
    validation(policy);
    modeBuilds(policy);
    chaoshen(policy, stockXml);
    if (storeTests) { store(policy, temp); rangeProvenance(policy, temp); }
    require(!policy.sources.empty() && policy.sources.size() <= policy.games.size(), "Sources");
    size_t aliased = 0;
    for (const auto& [alias, target] : policy.overlay.aliases) {
        if (policy.games.count(alias)) continue;
        bool found = false;
        for (const auto& s : policy.sources)
            if (std::find(s.packages.begin(), s.packages.end(), target.profile) != s.packages.end())
                found = std::find(s.packages.begin(), s.packages.end(), alias) != s.packages.end();
        require(found, "Alias missing from sources: " + alias);
        ++aliased;
    }
    std::cout << "TUNING_DEVICE_OK version=" << policy.version << " parity_cases=" << parityCases << " linear_modes=" << linearCases
              << " offset_cases=" << offsetCases << " sources=" << policy.sources.size() << " aliases=" << aliased << '\n';
}
} // namespace

void tuningTests(const char* wujiXml, const char* wujiOverlay, const char* baldurXml, const char* baldurOverlay,
                 const char* tempDir) {
    const fs::path temp = fs::path(tempDir) / "tuning";
    fs::create_directories(temp);
    synthetic(temp);
    device(wujiXml, wujiOverlay, temp / "wuji", true);
    device(baldurXml, baldurOverlay, temp / "baldur", false);
    std::cout << "TUNING_TESTS_OK\n";
}
