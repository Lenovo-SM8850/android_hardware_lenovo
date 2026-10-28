// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace gamepolicy {
namespace {
const std::array<std::string, 3> kNames = {"Gold", "Prime", "Gpu"};
const std::array<int, 3> kMin = {0x40800000, 0x40800200, 0x42804000};
const std::array<int, 3> kMax = {0x40804000, 0x40804200, 0x42808000};
std::string attr(const tinyxml2::XMLElement* e, const char* key) {
    const char* s = e ? e->Attribute(key) : nullptr;
    if (!s) throw std::runtime_error(std::string("Missing attribute ") + key);
    return s;
}
int integer(const std::string& s) {
    size_t end;
    const int n = std::stoi(s, &end, 0);
    if (end != s.size()) throw std::runtime_error("Invalid integer: " + s);
    return n;
}
std::string value(const tinyxml2::XMLElement* e) {
    if (!e || !e->GetText()) throw std::runtime_error("Missing element text");
    std::istringstream in(e->GetText());
    std::string s; in >> s; return s;
}
int resource(const std::string& name) {
    for (int i = 0; i < 3; ++i) if (kNames[i] == name) return i;
    return -1;
}
bool supported(const std::string& mode) {
    return mode == "Balance" || mode == "Savage" || mode == "PowerSave";
}
bool allowedCase(int code) { return code == 0 || code == 100 || code == 200 || code == 300 || code == 400; }
} // namespace

bool Policy::load(const std::string& stock, const std::string& overrides, std::string* error) {
    *this = Policy{};
    try {
        tinyxml2::XMLDocument doc, ov;
        if (doc.LoadFile(stock.c_str()) != tinyxml2::XML_SUCCESS ||
            ov.LoadFile(overrides.c_str()) != tinyxml2::XML_SUCCESS)
            throw std::runtime_error("Cannot read policy XML");
        auto root = doc.FirstChildElement("GameOptPolicy");
        if (!root) throw std::runtime_error("Missing GameOptPolicy");
        version = attr(root, "version");
        auto env = root->FirstChildElement("PreEnv");
        if (!env) throw std::runtime_error("Missing PreEnv");
        for (auto kind = env->FirstChildElement(); kind; kind = kind->NextSiblingElement()) {
            for (auto c = kind->FirstChildElement("cluster"); c; c = c->NextSiblingElement("cluster")) {
                int r = resource(attr(c, "name"));
                if (r < 0) continue;
                if (integer(attr(c, "min")) != kMin[r] || integer(attr(c, "max")) != kMax[r])
                    throw std::runtime_error("Unexpected frequency resource opcode");
                std::istringstream frequencies(c->GetText() ? c->GetText() : "");
                int f;
                while (frequencies >> f) {
                    if (f <= 0) throw std::runtime_error("Invalid declared OPP");
                    mDeclared[r].push_back(f);
                }
            }
        }
        for (const auto& table : mDeclared) if (table.empty()) throw std::runtime_error("Missing OPP table");
        auto config = ov.FirstChildElement("game-policy");
        if (!config || attr(config, "version") != "1") throw std::runtime_error("Unsupported overlay");
        auto safety = config->FirstChildElement("safety");
        overlay.sensor = attr(safety, "sensor");
        overlay.hysteresis = std::stod(attr(safety, "hysteresisC"));
        overlay.sampleMs = integer(attr(safety, "sampleMs"));
        if (!std::isfinite(overlay.hysteresis) || overlay.hysteresis < 0 || overlay.hysteresis > 5 ||
            overlay.sampleMs < 100 || overlay.sampleMs > 1000 || overlay.sensor.empty())
            throw std::runtime_error("Invalid safety settings");
        for (auto c = safety->FirstChildElement("resource"); c; c = c->NextSiblingElement("resource")) {
            const int r = resource(attr(c, "name"));
            if (r < 0) throw std::runtime_error("Unknown safety resource");
            overlay.ceilings[r] = integer(attr(c, "ceiling"));
            overlay.oppPaths[r] = attr(c, "opps");
        }
        for (int r = 0; r < 3; ++r) if (overlay.ceilings[r] <= 0 || overlay.oppPaths[r].empty())
            throw std::runtime_error("Missing hard ceiling/OPP path");
        for (auto m = config->FirstChildElement("mode"); m; m = m->NextSiblingElement("mode")) {
            const auto name = attr(m, "profile");
            if (!supported(name)) throw std::runtime_error("Unsupported mode mapping");
            overlay.modes[integer(attr(m, "id"))] = name;
        }
        for (auto a = config->FirstChildElement("alias"); a; a = a->NextSiblingElement("alias")) {
            Alias alias{attr(a, "profile"), true};
            if (a->Attribute("threadPlacement")) {
                const auto flag = attr(a, "threadPlacement");
                if (flag != "true" && flag != "false") throw std::runtime_error("Invalid alias flag");
                alias.threadPlacement = flag == "true";
            }
            if (!overlay.aliases.emplace(attr(a, "pkg"), alias).second)
                throw std::runtime_error("Duplicate alias");
        }
        auto policies = root->FirstChildElement("GamePolicy");
        if (!policies) throw std::runtime_error("Missing GamePolicy");
        for (auto game = policies->FirstChildElement("Game"); game; game = game->NextSiblingElement("Game")) {
            std::map<std::string, Mode> modes;
            auto sensor = game->FirstChildElement("ThermalTempType");
            if (sensor && integer(value(sensor)) != 11) throw std::runtime_error("Unsupported sensor type");
            for (auto m = game->FirstChildElement("Mode"); m; m = m->NextSiblingElement("Mode")) {
                const auto name = attr(m, "name");
                if (!supported(name)) continue; // Later phases own combined modes and other controls.
                auto ms = m->FirstChildElement("ThermalTempType");
                if (ms && integer(value(ms)) != 11) throw std::runtime_error("Unsupported mode sensor");
                Mode mode;
                auto scene = m->FirstChildElement("ThermalSceneCode");
                if (!scene) scene = game->FirstChildElement("ThermalSceneCode");
                mode.thermalCase = scene ? integer(value(scene)) : 0;
                if (!allowedCase(mode.thermalCase)) {
                    diagnostics.push_back(attr(game, "pkg") + "/" + name + ": Rejected thermal case " + std::to_string(mode.thermalCase));
                    mode.thermalCase = 0;
                }
                auto policy = m->FirstChildElement("Policy");
                if (policy) for (auto t = policy->FirstChildElement("TempLevel"); t; t = t->NextSiblingElement("TempLevel")) {
                    Band b{std::stod(attr(t, "temp")), {}};
                    if (!std::isfinite(b.temperature) || (!mode.bands.empty() && b.temperature < mode.bands.back().temperature))
                        throw std::runtime_error("Invalid temperature bands");
                    for (auto item = t->FirstChildElement("item"); item; item = item->NextSiblingElement("item")) {
                        if (attr(item, "type") != "freq") continue;
                        int r = resource(attr(item, "name"));
                        if (r < 0) continue;
                        const auto pair = value(item);
                        const auto split = pair.find('_');
                        if (split == std::string::npos) throw std::runtime_error("Invalid frequency pair");
                        b.bounds[r] = {integer(pair.substr(0, split)), integer(pair.substr(split + 1))};
                    }
                    mode.bands.push_back(b);
                }
                modes.emplace(name, std::move(mode));
            }
            if (!modes.count("Balance")) throw std::runtime_error("Game missing Balance");
            std::istringstream packages(attr(game, "pkg"));
            std::string pkg;
            while (std::getline(packages, pkg, ',')) {
                const auto first = pkg.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                pkg = pkg.substr(first, pkg.find_last_not_of(" \t\r\n") - first + 1);
                if (!games.emplace(pkg, modes).second) throw std::runtime_error("Duplicate package: " + pkg);
            }
        }
        if (!games.count("default")) throw std::runtime_error("Missing default game");
        std::string aliasDiagnostics;
        aliasesValid(&aliasDiagnostics);
        if (!aliasDiagnostics.empty()) diagnostics.push_back(aliasDiagnostics);
        return true;
    } catch (const std::exception& e) { *error = e.what(); return false; }
}

bool Policy::aliasesValid(std::string* error) const {
    bool valid = true;
    for (const auto& [pkg, alias] : overlay.aliases) {
        if (games.count(pkg)) {
            *error += "Shadowed alias " + pkg + " -> " + alias.profile + ": Own stock entry takes precedence; ";
        }
        if (!games.count(alias.profile)) {
            *error += "Dangling alias " + pkg + " -> " + alias.profile + "; "; valid = false;
        }
        if (overlay.aliases.count(alias.profile)) {
            *error += "Chained alias " + pkg + " -> " + alias.profile + "; "; valid = false;
        }
    }
    return valid;
}

void Policy::resolve(const Opps& actual) {
    if (mResolved) return;
    mResolved = true;
    Opps tables = actual;
    for (int r = 0; r < 3; ++r) {
        auto& table = tables[r];
        table.erase(std::remove_if(table.begin(), table.end(), [](int f) { return f <= 0; }), table.end());
        std::sort(table.begin(), table.end());
        table.erase(std::unique(table.begin(), table.end()), table.end());
        if (r == 2) std::reverse(table.begin(), table.end());
    }
    for (auto& [pkg, modes] : games) for (auto& [name, mode] : modes) for (auto& b : mode.bands) {
        for (int r = 0; r < 3; ++r) {
            auto& bound = b.bounds[r];
            const auto& declared = mDeclared[r];
            const auto& table = tables[r];
            if (table.empty()) {
                diagnostics.push_back(pkg + "/" + name + "/" + kNames[r] + ": Missing OPP table");
                bound = {}; continue;
            }
            const int last = static_cast<int>(declared.size()) - 1;
            const Bounds original = bound;
            // -1 omits a request. All other indices are independently bounded;
            // a bad floor must never discard an otherwise valid ceiling.
            if (bound.floor != -1) bound.floor = std::clamp(bound.floor, 0, last);
            if (bound.ceiling != -1) bound.ceiling = std::clamp(bound.ceiling, 0, last);
            if (bound.floor != original.floor || bound.ceiling != original.ceiling) {
                diagnostics.push_back(pkg + "/" + name + "/" + kNames[r] + " temp=" + std::to_string(b.temperature) +
                    ": Clamped indices " + std::to_string(original.floor) + "_" + std::to_string(original.ceiling) +
                    " -> " + std::to_string(bound.floor) + "_" + std::to_string(bound.ceiling));
            }
            // Both bounds are expressed as frequencies until the last conversion. Never
            // round a request upward or let a floor exceed the hard/effective ceiling.
            auto roundDown = [&](int wanted) {
                int found = -1;
                for (int f : table) if (f <= wanted) found = std::max(found, f);
                return found;
            };
            int ceiling = roundDown(std::min(bound.ceiling < 0 ? overlay.ceilings[r] : declared[bound.ceiling], overlay.ceilings[r]));
            int floor = bound.floor < 0 ? -1 : roundDown(std::min(declared[bound.floor], ceiling));
            if (ceiling < 0 || (bound.floor >= 0 && floor < 0)) {
                diagnostics.push_back(pkg + "/" + name + "/" + kNames[r] + ": No safe OPP");
                bound = {}; continue;
            }
            if (r == 2) {
                bound.floor = floor < 0 ? -1 : std::find(table.begin(), table.end(), floor) - table.begin();
                bound.ceiling = std::find(table.begin(), table.end(), ceiling) - table.begin();
            } else {
                bound.floor = floor < 0 ? -1 : floor / 1000;
                bound.ceiling = ceiling / 1000;
            }
        }
    }
}

Selection Policy::select(const std::string& pkg, int mode) const {
    std::string profile = pkg;
    bool threads = true;
    auto alias = overlay.aliases.find(pkg);
    if (!games.count(pkg) && alias != overlay.aliases.end() && games.count(alias->second.profile) &&
        !overlay.aliases.count(alias->second.profile)) {
        profile = alias->second.profile; threads = alias->second.threadPlacement;
    }
    if (!games.count(profile)) profile = "default";
    const auto& modes = games.at(profile);
    auto mapped = overlay.modes.find(mode);
    std::string name = mapped == overlay.modes.end() ? "Balance" : mapped->second;
    auto selected = modes.find(name);
    if (selected == modes.end()) selected = modes.find("Balance");
    return {&selected->second, profile, threads};
}

size_t Policy::band(const Mode& mode, double temperature, int previous, double hysteresis) {
    if (mode.bands.empty()) return 0;
    size_t positive = std::count_if(mode.bands.begin(), mode.bands.end(),
                                  [](const Band& band) { return band.temperature > 0; });
    const size_t prior = previous >= 0 && static_cast<size_t>(previous) < mode.bands.size() ? previous : 0;
    // Match the positive-count branches and adjacent-level cooling guard. The
    // thresholds remain in their original positions; a zero anchor is not removed.
    if (positive == 1) return 0;
    if (positive != 3 && positive != 4) return prior;
    size_t selected = 0;
    for (size_t i = 1; i < positive; ++i) if (temperature >= mode.bands[i].temperature) selected = i;
    if (prior == selected + 1 && temperature >= mode.bands[prior].temperature - hysteresis) return prior;
    return selected;
}

Decision Policy::evaluate(const Mode& mode, std::optional<double> temperature,
                          int64_t sampleAgeMs, int previous, double hysteresis) {
    if (!temperature || !std::isfinite(*temperature) || *temperature < -20 || *temperature > 125 ||
        sampleAgeMs < 0 || sampleAgeMs > 1500) return {};
    Decision decision;
    decision.thermalCase = mode.thermalCase;
    if (!mode.bands.empty()) {
        decision.band = band(mode, *temperature, previous, hysteresis);
        decision.resources = resources(mode.bands[decision.band]);
    }
    return decision;
}

std::vector<int> Policy::resources(const Band& band) {
    std::vector<int> args;
    for (int r = 0; r < 3; ++r) {
        if (band.bounds[r].floor >= 0) { args.push_back(kMin[r]); args.push_back(band.bounds[r].floor); }
        if (band.bounds[r].ceiling >= 0) { args.push_back(kMax[r]); args.push_back(band.bounds[r].ceiling); }
    }
    return args;
}
} // namespace gamepolicy
