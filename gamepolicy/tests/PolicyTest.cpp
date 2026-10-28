// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include <tinyxml2.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gamepolicy;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, const std::string& text) { std::ofstream(path) << text; }
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
void real(const char* stock, const char* overlay) {
    Policy policy; std::string error;
    require(policy.load(stock, overlay, &error), error.c_str());
    require(policy.aliasesValid(&error), "Device overlay contains dangling alias");
    const Policy unresolved = policy;
    const auto actual = sample(stock);
    policy.resolve(actual);
    int clamped = 0;
    for (const auto& [pkg, definitions] : unresolved.games) for (const auto& [name, mode] : definitions) {
        for (size_t i = 0; i < mode.bands.size(); ++i) for (int r = 0; r < 3; ++r) {
            const auto& before = mode.bands[i].bounds[r];
            const int last = actual[r].size() - 1;
            if (before.floor > last || before.ceiling > last) {
                const auto& after = policy.games.at(pkg).at(name).bands[i].bounds[r];
                require(after.floor >= 0 && after.ceiling >= 0, "Clamped real row lost a bound");
                require(r == 2 && after.floor == last, "Real GPU floor did not clamp to edge");
                ++clamped;
            }
        }
    }
    require(clamped == (policy.version == "20260907" ? 3 : 28), "Unexpected invalid-row coverage");
    if (policy.version == "20260907") {
        const auto& battery = *policy.select("com.miHoYo.Nap", 3).mode;
        require(battery.bands.size() == 4 && battery.bands[0].temperature == 0, "Nap fixture changed");
        for (double temp : {42.0, 45.0, 90.0}) {
            require(Policy::band(battery, temp, -1, 1) == 2, "Nap Battery must use stock row 3 at >=42C");
            require(Policy::evaluate(battery, temp, 0, -1, 1).resources == Policy::resources(battery.bands[2]),
                    "Nap Battery requested the wrong stock row");
        }
    } else {
        require(policy.select("com.tencent.tmgp.pubgmhd", 2).mode->thermalCase == 400, "PUBG Savage case 400 lost");
        require(policy.select("com.tencent.tmgp.projectg", 2).mode->thermalCase == 400, "PUBG sibling case 400 lost");
    }
    size_t modes = 0;
    for (const auto& [pkg, definitions] : policy.games) {
        for (int id : {1, 2, 3, 0, 4}) {
            auto selected = policy.select(pkg, id);
            const auto alias = policy.overlay.aliases.find(pkg);
            require(selected.profile == (alias == policy.overlay.aliases.end() ? pkg : alias->second.profile) && selected.mode,
                    "Real package or overriding alias did not resolve");
            require(!selected.mode->bands.empty(), "Real game has no frequency bands");
            for (double temperature : {20.0, 40.0, 42.0, 45.0, 47.0, 65.0}) {
                auto band = Policy::band(*selected.mode, temperature, -1, 1);
                require(band < selected.mode->bands.size(), "Invalid real band selection");
                auto args = Policy::resources(selected.mode->bands[band]);
                require(args.size() % 2 == 0 && args.size() <= 12, "Invalid resource bundle");
            }
        }
        modes += definitions.size();
    }
    require(policy.select("com.example.unlisted", 1).profile == "default", "Missing default");
    auto fallback = policy.select("default", 999);
    require(fallback.mode == policy.select("default", 1).mode, "Unmapped mode fallback");
    for (const auto& [pkg, alias] : policy.overlay.aliases) {
        const auto selected = policy.select(pkg, 2);
        require(selected.profile == alias.profile, "Real alias did not resolve to its own profile");
    }
    std::cout << "REAL_POLICY_OK version=" << policy.version << " packages=" << policy.games.size()
              << " modes=" << modes << " clamped=" << clamped << " aliases=" << policy.overlay.aliases.size()
              << " diagnostics=" << policy.diagnostics.size() << '\n';
    for (const auto& diagnostic : policy.diagnostics) std::cout << "DIAGNOSTIC " << diagnostic << '\n';
}
void synthetic(const std::filesystem::path& directory) {
    const auto stock = directory / "policy.xml", overlay = directory / "overlay.xml";
    write(stock, R"(<GameOptPolicy version="test"><PreEnv>
      <CPU><cluster name="Gold" min="0x40800000" max="0x40804000">384000 1000000 2000000</cluster>
      <cluster name="Prime" min="0x40800200" max="0x40804200">768000 2000000 3000000</cluster></CPU>
      <GPU><cluster name="Gpu" min="0x42804000" max="0x42808000">1000000000 800000000 600000000</cluster></GPU>
      </PreEnv><GamePolicy><Game pkg="default"><ThermalTempType>11</ThermalTempType>
      <Mode name="Balance"><ThermalSceneCode>200</ThermalSceneCode><Policy>
      <TempLevel temp="25"><item type="freq" name="Gold">1_2</item><item type="freq" name="Prime">2_-1</item><item type="freq" name="Gpu">2_0</item></TempLevel>
      <TempLevel temp="42"><item type="freq" name="Gold">-2_9</item></TempLevel>
      <TempLevel temp="45"><item type="freq" name="Gpu">19_1</item></TempLevel>
      <TempLevel temp="47"><item type="freq" name="Gpu">-1_9</item></TempLevel>
      </Policy></Mode><Mode name="Savage"><ThermalSceneCode>300</ThermalSceneCode><Policy><TempLevel temp="0"/></Policy></Mode>
      <Mode name="PowerSave"><ThermalSceneCode>100</ThermalSceneCode><Policy><TempLevel temp="0"/></Policy></Mode>
      <Mode name="Godlike"><ThermalSceneCode>400</ThermalSceneCode><Policy><TempLevel temp="nonsense"/></Policy></Mode></Game>
      <Game pkg="game.one, game.two, game.cn"><Mode name="Balance"><ThermalSceneCode>400</ThermalSceneCode><Policy><TempLevel temp="25"/></Policy></Mode></Game>
      </GamePolicy></GameOptPolicy>)");
    const std::string header = R"(<game-policy version="1"><mode id="1" profile="Balance"/><mode id="2" profile="Savage"/><mode id="3" profile="PowerSave"/>
      <safety sensor="quiet-therm" hysteresisC="1" sampleMs="1000">
      <resource name="Gold" ceiling="1800000" opps="unused"/>
      <resource name="Prime" ceiling="2500000" opps="unused"/>
      <resource name="Gpu" ceiling="900000000" opps="unused"/></safety>)";
    write(overlay, header + R"(<alias pkg="global" profile="game.cn" threadPlacement="false"/>
      <alias pkg="game.one" profile="default"/><alias pkg="removed" profile="no.such.game"/>
      <alias pkg="game.two" profile="no.such.game"/><alias pkg="chain" profile="game.one"/></game-policy>)");
    const auto chainOverlay = directory / "overlay-chain.xml";
    write(chainOverlay, header + R"(<alias pkg="chain" profile="game.one"/><alias pkg="game.one" profile="default"/></game-policy>)");
    Policy policy; std::string error;
    require(policy.load(stock, overlay, &error), error.c_str());
    require(!policy.aliasesValid(&error) && error.find("no.such.game") != std::string::npos, "Dangling alias accepted");
    require(policy.select("removed", 1).profile == "default", "Dangling alias fallback");
    require(policy.select("game.two", 1).profile == "game.two", "Dangling alias discarded direct stock package");
    require(policy.select("global", 2).profile == "game.cn" && !policy.select("global", 2).threadPlacement, "Alias or flag ignored");
    require(policy.select("game.one", 1).profile == "default", "Alias did not override direct stock package");
    require(policy.select("game.two", 2).mode == policy.select("game.two", 1).mode, "Missing per-game mode fallback");
    require(error.find("Chained alias chain -> game.one") != std::string::npos && policy.select("chain", 1).profile == "default",
            "Alias chain accepted or followed");
    Policy chained; std::string chainError;
    require(chained.load(stock, chainOverlay, &chainError) && !chained.aliasesValid(&chainError) &&
            chainError.find("Chained alias") != std::string::npos &&
            std::any_of(chained.diagnostics.begin(), chained.diagnostics.end(), [](const auto& d) { return d.find("Chained alias") != std::string::npos; }),
            "Alias chain must load nonfatally with a diagnostic");
    require(policy.select("game.two", 1).mode->thermalCase == 400, "Thermal case 400 rejected");
    require(policy.select("default", 2).mode->thermalCase == 300 && policy.select("default", 3).mode->thermalCase == 100, "Mode mapping");
    const auto& mode = *policy.select("default", 1).mode;
    require(Policy::band(mode, 41.9, -1, 1) == 0, "Initial anchor");
    require(Policy::band(mode, 42, 0, 1) == 1, "Heating must tighten immediately");
    require(Policy::band(mode, 41, 1, 1) == 1, "Cooling hysteresis boundary");
    require(Policy::band(mode, 40.9, 1, 1) == 0, "Cooling did not relax");
    require(Policy::band(mode, 46, 3, 1) == 3, "Upper cooling hysteresis");
    require(Policy::band(mode, 39, 3, 1) == 0, "Multi-band cooling");
    require(Policy::band(mode, 44.5, 3, 1) == 1, "Stock deep cooling must bypass adjacent-level hysteresis");
    Mode zeroAnchor{200, {{0, {}}, {40, {}}, {42, {}}, {44, {}}}};
    require(Policy::band(zeroAnchor, 90, -1, 1) == 2, "Positive count must cap zero-anchor at third row");
    require(Policy::band(zeroAnchor, 41.5, 2, 1) == 2, "Stock adjacent cooling guard");
    Mode twoPositive{200, {{30, {}}, {40, {}}}};
    require(Policy::band(twoPositive, 90, -1, 1) == 0 && Policy::band(twoPositive, 0, 1, 1) == 1,
            "Stock unmatched positive count must retain previous level");
    Opps actual = {{{384000, 900000, 1500000, 2100000}, {768000, 1900000, 2400000, 3100000}, {1100000000, 850000000, 650000000, 550000000}}};
    policy.resolve(actual);
    const auto& resolved = policy.select("default", 1).mode->bands;
    require(resolved[0].bounds[0].floor == 900 && resolved[0].bounds[0].ceiling == 1500, "CPU OPP rounding/ceiling");
    require(resolved[0].bounds[1].floor == 2400 && resolved[0].bounds[1].ceiling == 2400, "Floor must clip to hard ceiling");
    require(resolved[0].bounds[2].floor == 3 && resolved[0].bounds[2].ceiling == 1, "GPU frequency to actual pwrlevel");
    require(resolved[1].bounds[0].floor == 384 && resolved[1].bounds[0].ceiling == 1500, "CPU independent edge clamping");
    require(resolved[2].bounds[2].floor == 3 && resolved[2].bounds[2].ceiling == 2, "Bad GPU floor discarded valid ceiling");
    require(resolved[3].bounds[2].floor == -1, "Omitted floor became a boost");
    require(resolved[3].bounds[2].ceiling == 3, "Bad GPU ceiling was not clamped independently");
    require(Policy::resources(resolved[0]) == std::vector<int>({0x40800000, 900, 0x40804000, 1500, 0x40800200, 2400, 0x40804200, 2400, 0x42804000, 3, 0x42808000, 1}), "Wrong QTI bundle");
    const auto& safeMode = *policy.select("default", 1).mode;
    auto valid = Policy::evaluate(safeMode, 45.0, 0, -1, 1);
    require(valid.thermalCase == 200 && !valid.resources.empty(), "Valid sample did not enable policy");
    for (auto invalid : {std::optional<double>{}, std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
                         std::optional<double>{126}}) {
        auto decision = Policy::evaluate(safeMode, invalid, 0, valid.band, 1);
        require(decision.thermalCase == 0 && decision.resources.empty() && decision.band == -1,
                "Invalid sensor must clear both thermal case and frequency request");
    }
    auto stale = Policy::evaluate(safeMode, 45.0, 1501, valid.band, 1);
    require(stale.thermalCase == 0 && stale.resources.empty(), "Stale sensor retained policy");
    auto restored = Policy::evaluate(safeMode, 45.0, 0, stale.band, 1);
    require(restored.thermalCase == 200 && restored.resources == valid.resources, "Valid sensor did not restore policy");
    Policy unavailable; error.clear();
    require(unavailable.load(stock, overlay, &error), "Reload failed");
    unavailable.resolve({});
    require(Policy::resources(unavailable.select("default", 1).mode->bands[0]).empty(), "Missing OPPs must release all bounds");
    std::cout << "POLICY_TESTS_OK modes stock-bands hysteresis OPPs per-bound-clamp aliases sensor-failure-recovery\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 6, "Usage: check WUJI_XML WUJI_OVERLAY BALDUR_XML BALDUR_OVERLAY TEMP_DIR");
        real(argv[1], argv[2]); real(argv[3], argv[4]);
        std::filesystem::create_directories(argv[5]);
        synthetic(argv[5]);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
