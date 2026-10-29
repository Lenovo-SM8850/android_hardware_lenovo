// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include "Threads.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

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
    policy.resolve(actual, {6, 2});
    size_t sceneRows = 0, metadata = 0;
    for (const auto& [pkg, list] : policy.sceneLists) metadata += list.size();
    for (const auto& [pkg, modes] : policy.games) for (const auto& [name, mode] : modes) {
        sceneRows += mode.scenes.size();
        require(mode.scenes.size() == modes.at("Balance").scenes.size(), "Profile scene inheritance lost");
        for (const auto& [id, scene] : mode.scenes) {
            require(scene.timeMs > 0 && id >= 0 && id <= 100000, "Invalid real scene row");
            for (int r = 0; r < 3; ++r) {
                const auto& b = scene.frequencies.bounds[r];
                for (int v : {b.floor, b.ceiling}) if (v >= 0)
                    require((r == 2 ? actual[2].at(v) : v * 1000) <= policy.overlay.ceilings[r],
                            "Real scene row exceeded hard device ceiling");
                require(b.floor < 0 || (r == 2 ? b.floor >= b.ceiling : b.floor <= b.ceiling),
                        "Real scene floor exceeded effective ceiling");
            }
        }
    }
    require(sceneRows == (policy.version == "20260907" ? 42 : 108), "Unexpected real scene row count");
    require(policy.sceneLists.size() == 2 && metadata > 20, "SceneList metadata missing");
    const auto pubg = policy.select("com.tencent.tmgp.pubgmhd", 1);
    const auto sgame = policy.select("com.tencent.tmgp.sgame", 1);
    require(pubg.mode->scenes.size() == (policy.version == "20260907" ? 5 : 16) &&
            sgame.mode->scenes.size() == 2, "SceneOpt profile row counts");
    std::cout << "SCENE_COUNTS version=" << policy.version << " profile_rows=" << pubg.mode->scenes.size() + 2
              << " inherited_rows=" << sceneRows << " metadata_profiles=" << policy.sceneLists.size()
              << " metadata_ids=" << metadata << " hard_ceilings=OK\n";
    size_t hints = 0, bound = 0, rt = 0, delays = 0, fps = 0;
    std::map<int, size_t> accepted, dropped;
    for (const auto& [pkg, modes] : policy.games) for (const auto& [name, mode] : modes) {
        hints += mode.perfHints.size(); bound += mode.bindCore.size(); rt += mode.schedulers.size();
        delays += mode.delayMs > 0;
        fps += Policy::fpsEnabled(mode, pkg);
        for (const auto& hint : mode.perfHints) {
            ++accepted[hint.opcode];
            require((hint.opcode == 0x40CAC000 && hint.value >= 0 && hint.value <= 100) ||
                    hint.opcode == 0x42800000 ||
                    (hint.opcode == 0x41000000 && hint.value >= 0 && hint.value <= 6) ||
                    (hint.opcode == 0x41000200 && hint.value >= 0 && hint.value <= 2), "Unallowlisted real hint");
        }
    }
    for (const auto& diagnostic : policy.diagnostics) {
        auto pos = diagnostic.find("/PerfHint 0x");
        if (pos != std::string::npos) ++dropped[std::stoi(diagnostic.substr(pos + 10), nullptr, 16)];
    }
    require(hints > 100 && bound > 200 && rt >= 6 && delays >= 3, "Missing phase 2 real coverage");
    if (policy.version == "20260907")
        require(accepted[0x40CAC000] == 191 && accepted[0x42800000] == 7 && bound == 509 && rt == 102 && delays == 22,
                "Unexpected wuji retained phase 2 counts");
    else if (policy.version == "20260721")
        require(accepted[0x40CAC000] == 174 && bound == 385 && rt == 12 && delays == 10,
                "Unexpected baldur retained phase 2 counts");
    require(accepted[0x41000000] > 0 && accepted[0x41000200] > 0, "Core resources missing");
    require(accepted[0x41000000] + accepted[0x41000200] == (policy.version == "20260907" ? 51 : 16) &&
            dropped[0x41000100] == (policy.version == "20260907" ? 2 : 0), "Unexpected phase 3 min-core counts");
    require(policy.version != "20260721" || dropped[0x40400000] > 0, "Unmapped extra resource accepted");
    std::cout << "PHASE3_COUNTS version=" << policy.version << " perf_pairs=" << hints << " bind_rules=" << bound
              << " rt_rules=" << rt << " delayed_modes=" << delays << " fps_enabled_modes=" << fps
              << " min_core_admitted=" << accepted[0x41000000] + accepted[0x41000200]
              << " min_core_dropped=" << dropped[0x41000000] + dropped[0x41000100] + dropped[0x41000200] << '\n';
    for (auto [opcode, count] : accepted)
        std::cout << "PERF_ACCEPT opcode=0x" << std::hex << opcode << std::dec << " count=" << count << '\n';
    for (auto [opcode, count] : dropped) if (count)
        std::cout << "PERF_DROP opcode=0x" << std::hex << opcode << std::dec << " count=" << count << '\n';
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
            require(Policy::evaluate(battery, temp, 0, -1, 1).resources == Policy::resources(battery, 2),
                    "Nap Battery requested the wrong stock row");
        }
    } else {
        require(policy.select("com.tencent.tmgp.pubgmhd", 2).mode->thermalCase == 400, "PUBG Savage case 400 lost");
        require(policy.select("com.tencent.tmgp.projectg", 2).mode->thermalCase == 400, "PUBG sibling case 400 lost");
    }
    size_t modes = 0;
    for (const auto& [pkg, definitions] : policy.games) {
        for (const auto& [name, definition] : definitions)
            for (const auto* bands : {&definition.boostedBands, &definition.jankBands}) for (const auto& boosted : *bands) {
            for (int r = 0; r < 3; ++r) {
                const auto& b = boosted.bounds[r];
                for (int v : {b.floor, b.ceiling}) if (v >= 0)
                    require((r == 2 ? actual[2].at(v) : v * 1000) <= policy.overlay.ceilings[r],
                            "Real boosted row exceeded hard device ceiling");
                require(b.floor < 0 || (r == 2 ? b.floor >= b.ceiling : b.floor <= b.ceiling),
                        "Real boosted floor exceeded effective ceiling");
            }
        }
        for (int id : {1, 2, 3, 0, 4}) {
            auto selected = policy.select(pkg, id);
            require(selected.profile == pkg && selected.mode && selected.threadPlacement,
                    "Real stock package did not take precedence");
            require(!selected.mode->bands.empty(), "Real game has no frequency bands");
            for (double temperature : {20.0, 40.0, 42.0, 45.0, 47.0, 65.0}) {
                auto band = Policy::band(*selected.mode, temperature, -1, 1);
                require(band < selected.mode->bands.size(), "Invalid real band selection");
                auto args = Policy::resources(selected.mode->bands[band]);
                require(args.size() % 2 == 0 && args.size() <= 12, "Invalid resource bundle");
                const auto combined = Policy::evaluate(*selected.mode, temperature, 0, -1, 1).resources;
                require(combined.size() == args.size() + 2 * selected.mode->perfHints.size() &&
                        std::equal(args.begin(), args.end(), combined.begin()), "Real hint order/bundle");
            }
        }
        modes += definitions.size();
    }
    require(policy.select("com.example.unlisted", 1).profile == "default", "Missing default");
    auto fallback = policy.select("default", 999);
    require(fallback.mode == policy.select("default", 1).mode, "Unmapped mode fallback");
    for (const auto& [pkg, alias] : policy.overlay.aliases) {
        if (policy.games.count(pkg)) continue;
        const auto selected = policy.select(pkg, 2);
        require(selected.profile == alias.profile && selected.threadPlacement == alias.threadPlacement,
                "Real fallback alias did not resolve to its profile and flag");
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
      <SceneOpt><Scene id="1" time="1000"><item type="freq" name="Gold">-2_99</item>
      <item type="freq" name="Prime">2_-1</item><item type="freq" name="Gpu">99_0</item></Scene></SceneOpt>
      <SceneOpt><Scene id="7" time="200"><item type="freq" name="Gold">1_1</item></Scene></SceneOpt>
      <BindCore><tid name="Main">C0</tid></BindCore><DelayTime>5000</DelayTime>
      <FpsAdjustLevel>1,2,3</FpsAdjustLevel><FpsAdjustTime>0</FpsAdjustTime>
      <JankAdjustLevel>3,2,1</JankAdjustLevel><JankAdjustTime>1234</JankAdjustTime>
      <Mode name="Balance"><ThermalSceneCode>200</ThermalSceneCode><Policy>
      <TempLevel temp="25"><item type="freq" name="Gold">1_2</item><item type="freq" name="Prime">2_-1</item><item type="freq" name="Gpu">2_0</item></TempLevel>
      <TempLevel temp="42"><item type="freq" name="Gold">-2_9</item></TempLevel>
      <TempLevel temp="45"><item type="freq" name="Gpu">19_1</item></TempLevel>
      <TempLevel temp="47"><item type="freq" name="Gpu">-1_9</item></TempLevel>
      </Policy><PerfHint><opcode id="0x40CAC000" time="0">50</opcode><opcode id="0x42800000" time="0">1</opcode>
      <opcode id="0x40CAC000" time="1">60</opcode><opcode id="0x12345678" time="0">1</opcode>
      <opcode id="0x40CAC000" time="0">101</opcode><opcode id="0x42800000" time="0">3</opcode>
      <opcode id="bad" time="0">1</opcode><opcode time="0">1</opcode></PerfHint>
      <ThreadScheduler name="Thread-1" priority="95">1</ThreadScheduler>
      <ThreadScheduler name="Thread-13" priority="99">1</ThreadScheduler>
      <ThreadScheduler name="Disabled" priority="90">0</ThreadScheduler>
      <ThreadScheduler name="Invalid" priority="89">1</ThreadScheduler>
      <ThreadScheduler name="Invalid" priority="100">1</ThreadScheduler>
      <ThreadScheduler name="Invalid" priority="95">2</ThreadScheduler>
      </Mode><Mode name="Savage"><FpsAdjustLevel>4,5,6</FpsAdjustLevel><DelayTime>0</DelayTime><BindCore><tid name="Worker">3f</tid>
      <tid name="Bad">0</tid><tid name="Bad">GG</tid><unknown/></BindCore>
      <ThermalSceneCode>300</ThermalSceneCode><Policy><TempLevel temp="0"/></Policy></Mode>
      <Mode name="PowerSave"><ThermalSceneCode>100</ThermalSceneCode><Policy><TempLevel temp="0"/></Policy></Mode>
      <Mode name="Godlike"><ThermalSceneCode>400</ThermalSceneCode><Policy><TempLevel temp="nonsense"/></Policy></Mode></Game>
      <Game pkg="game.one, game.two, game.cn"><Mode name="Balance"><ThermalSceneCode>400</ThermalSceneCode><Policy><TempLevel temp="25"/></Policy></Mode></Game>
      <Game pkg="hint.only"><Mode name="Balance"><PerfHint><opcode id="0x40CAC000" time="0">20</opcode></PerfHint></Mode></Game>
      <Game pkg="order"><Mode name="Balance"><BindCore><tid name="Thread-1">3C</tid><tid name="Thread-13">70</tid>
      <tid name="Thread-13">C0</tid></BindCore></Mode></Game>
      </GamePolicy></GameOptPolicy>)");
    const std::string header = R"(<game-policy version="1"><mode id="1" profile="Balance"/><mode id="2" profile="Savage"/><mode id="3" profile="PowerSave"/>
      <safety sensor="quiet-therm" hysteresisC="1" sampleMs="1000">
      <resource name="Gold" ceiling="1800000" opps="unused"/>
      <resource name="Prime" ceiling="2500000" opps="unused"/>
      <resource name="Gpu" ceiling="900000000" opps="unused"/></safety>)";
    write(overlay, header + R"(<alias pkg="global" profile="game.cn" threadPlacement="false"/>
      <alias pkg="game.one" profile="default" threadPlacement="false"/><alias pkg="removed" profile="no.such.game"/>
      <alias pkg="game.two" profile="no.such.game"/><alias pkg="chain" profile="game.one"/></game-policy>)");
    const auto chainOverlay = directory / "overlay-chain.xml";
    write(chainOverlay, header + R"(<alias pkg="chain" profile="game.one"/><alias pkg="game.one" profile="default"/></game-policy>)");
    Policy policy; std::string error;
    require(policy.load(stock, overlay, &error), error.c_str());
    require(!policy.aliasesValid(&error) && error.find("no.such.game") != std::string::npos, "Dangling alias accepted");
    require(policy.select("removed", 1).profile == "default", "Dangling alias fallback");
    require(policy.select("game.two", 1).profile == "game.two", "Dangling alias discarded direct stock package");
    require(policy.select("global", 2).profile == "game.cn" && !policy.select("global", 2).threadPlacement, "Alias or flag ignored");
    require(policy.select("game.one", 1).profile == "game.one" && policy.select("game.one", 1).threadPlacement,
            "Alias displaced direct stock package or its thread placement");
    require(policy.select("game.two", 2).mode == policy.select("game.two", 1).mode, "Missing per-game mode fallback");
    require(error.find("Chained alias chain -> game.one") != std::string::npos && policy.select("chain", 1).profile == "default",
            "Alias chain accepted or followed");
    Policy chained; std::string chainError;
    require(chained.load(stock, chainOverlay, &chainError) && !chained.aliasesValid(&chainError) &&
            chainError.find("Chained alias") != std::string::npos &&
            std::any_of(chained.diagnostics.begin(), chained.diagnostics.end(), [](const auto& d) { return d.find("Chained alias") != std::string::npos; }),
            "Alias chain must load nonfatally with a diagnostic");
    const auto shadowedOverlay = directory / "overlay-shadowed.xml";
    write(shadowedOverlay, header + R"(<alias pkg="game.one" profile="default" threadPlacement="false"/></game-policy>)");
    Policy shadowed; std::string shadowedError;
    require(shadowed.load(stock, shadowedOverlay, &shadowedError) && shadowed.aliasesValid(&shadowedError) &&
            shadowedError.find("Shadowed alias game.one -> default") != std::string::npos &&
            std::any_of(shadowed.diagnostics.begin(), shadowed.diagnostics.end(), [](const auto& d) {
                return d.find("Shadowed alias game.one -> default") != std::string::npos;
            }), "Shadowed alias must remain valid with a diagnostic");
    require(shadowed.select("game.one", 1).profile == "game.one" && shadowed.select("game.one", 1).threadPlacement,
            "Shadowed alias changed stock selection or thread placement");
    std::cout << "ALIAS_PRECEDENCE_OK own-stock fallback-alias thread-placement dangling chained shadowed-warning\n";
    require(policy.select("game.two", 1).mode->thermalCase == 400, "Thermal case 400 rejected");
    require(policy.select("default", 2).mode->thermalCase == 300 && policy.select("default", 3).mode->thermalCase == 100, "Mode mapping");
    const auto& mode = *policy.select("default", 1).mode;
    require(mode.perfHints.size() == 2 && mode.bindCore.size() == 1 && mode.bindCore[0].mask == 0xC0 &&
            mode.schedulers.size() == 3 && mode.delayMs == 5000, "Phase 2 parse/inheritance");
    const auto& savage = *policy.select("default", 2).mode;
    require(savage.bindCore.size() == 1 && savage.bindCore[0].name == "Worker" && savage.delayMs == 0,
            "Mode rules/delay did not override Game defaults");
    require(mode.fpsAdjust == std::optional<std::array<int, 3>>({1, 2, 3}) &&
            savage.fpsAdjust == std::optional<std::array<int, 3>>({4, 5, 6}) &&
            mode.jankAdjust == std::optional<std::array<int, 3>>({3, 2, 1}) && mode.jankAdjustTime == 1234 &&
            Policy::fpsEnabled(mode, "default"), "Adjustment inheritance/override or ignored time");
    require(policy.select("default", 3).mode->bindCore[0].name == "Main", "Game BindCore inheritance");
    const auto& ordered = *policy.select("order", 1).mode;
    require(ordered.bindCore.size() == 2 && ordered.bindCore[0].name == "Thread-13" && ordered.bindCore[0].mask == 0xC0,
            "Source map ordering or duplicate-name replacement lost");
    auto diagnostic = [&](const char* text) {
        return std::any_of(policy.diagnostics.begin(), policy.diagnostics.end(), [&](const auto& d) {
            return d.find(text) != std::string::npos;
        });
    };
    require(diagnostic("Nonzero duration") && diagnostic("Unmapped resource") && diagnostic("Value out of range") &&
            diagnostic("Invalid FIFO rule") && diagnostic("Invalid hexadecimal CPU mask") && diagnostic("Empty CPU mask"),
            "Invalid phase 2 entries missing diagnostics");
    require(Policy::mask("C0") == 192 && Policy::mask("0x3f") == 63 &&
            Policy::mask("8000000000000000") == (uint64_t{1} << 63), "Hex mask encoding");
    for (const auto& text : {"", "0", "-1", "+1", "xyz", "C0junk", "10000000000000000"}) {
        bool rejected = false;
        try { Policy::mask(text); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid mask accepted");
    }
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
    auto combined = Policy::resources(safeMode, 0);
    auto frequencies = Policy::resources(resolved[0]);
    require(std::equal(frequencies.begin(), frequencies.end(), combined.begin()) &&
            combined.size() == frequencies.size() + 4 && combined[combined.size()-4] == 0x40CAC000 &&
            combined.back() == 2, "Perf hints must follow frequency pairs and resolve GPU OPPs");
    const auto hintOnly = Policy::evaluate(*policy.select("hint.only", 1).mode, 30.0, 0, -1, 1);
    require(hintOnly.band == -1 && hintOnly.resources == std::vector<int>({0x40CAC000, 20}),
            "Hint-only mode did not yield a lock bundle");
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
void threadRules() {
    require(bindMatches("xUnityMain", "UnityMain") && !schedulerMatches("xUnityMain", "UnityMain"), "Matcher distinction");
    require(bindMatches("Thread-13", "Thread-1") && schedulerMatches("Thread-13", "Thread-1"), "Thread prefix semantics");
    require(!bindMatches("Thread-1", "Thread-13") && !schedulerMatches("Thread-1", "Thread-13"), "Reverse thread prefix");
    require(!bindMatches("Any", "") && !schedulerMatches("Any", ""), "Empty matcher");
    Mode mode;
    mode.bindCore = {{"Thread-1", 0xC0}, {"Thread-13", 0x3F}, {"Main", 0xFF}};
    mode.schedulers = {{"Thread-1", 95, true}, {"Thread-13", 99, true}, {"Main", 90, false}};
    const std::vector<Task> tasks = {{101, 10, "Thread-1"}, {102, 11, "Thread-13"}, {103, 12, "xMain"}, {104, 13, "Other"}};
    auto desired = placements(tasks, mode, 0x7F);
    require(desired.size() == 3 && desired[0].mask == 0x40 && desired[0].priority == 95 &&
            desired[1].mask == 0x40 && desired[1].priority == 99 && desired[2].priority == 0,
            "Rule precedence, possible CPU intersection or disabled scheduler");
    require(placements(tasks, mode, 0x3F)[0].mask == 0 && placements({tasks[2]}, mode, 0).empty(), "Empty CPU intersection");
    auto copy = mode;
    copy.thermalCase = 400; copy.perfHints = {{0x40CAC000, 10}};
    require(sameThreadRules(mode, copy), "Unrelated mode fields triggered thread restore");
    copy.delayMs = 1; require(sameThreadRules(mode, copy), "Whole-policy delay affected thread rule equality"); copy = mode;
    copy.bindCore[0].mask = 1; require(!sameThreadRules(mode, copy), "Mask diff missed"); copy = mode;
    copy.schedulers[0].priority = 96; require(!sameThreadRules(mode, copy), "RT diff missed"); copy = mode;
    copy.schedulers[0].enabled = false; require(!sameThreadRules(mode, copy), "Enable diff missed");
    auto late = tasks; late.push_back({105, 14, "Thread-14"});
    require(placements(late, mode, 0xFF).size() == desired.size() + 1, "Late thread was not discovered");
    auto renamed = desired[0]; renamed.task.comm = "Thread-14";
    require(!samePlacement(desired[0], renamed), "Renamed thread must restore before reapply");
    renamed = desired[0]; ++renamed.task.start;
    require(!samePlacement(desired[0], renamed) && samePlacement(desired[0], desired[0]), "Recycled TID diff missed");
    require(placements({tasks[3]}, mode, 0xFF).empty(), "Unmatched thread selected");
    std::cout << "THREAD_RULES_OK substring prefix Thread-1-vs-Thread-13 precedence late-tasks rename reuse diff\n";
}
void feedback(const std::filesystem::path& directory) {
    FpsController controller;
    controller.reset(0);
    require(!controller.sample(2999, 60, true) && controller.history().back() == 0,
            "FPS warm-up admitted a sample");
    for (int64_t t = 3000; t <= 7000; t += 1000) require(!controller.sample(t, 60, true), "Stable FPS boosted");
    require(controller.sample(8000, 50, true), "FPS trigger band did not boost");
    require(controller.active(8499) && !controller.active(8500), "FPS restore must occur at 500 ms");
    require(!controller.sample(8100, 45, true) && controller.deadline() == 8500, "Burst accumulated/extended boost");
    controller.cancel();
    require(!controller.active(8100), "Mode/temperature cancellation retained boost");
    controller.reset(0);
    require(!controller.active(8000) && controller.history().back() == 0, "New process retained FPS history");
    for (int64_t t = 3000; t <= 6000; t += 1000) controller.sample(t, 60, true);
    require(!controller.sample(7000, 9, true) && controller.history().back() == 9, "FPS below 10 not rejected/retained");
    for (float value : {19.0f, 20.0f}) {
        controller.reset(0);
        controller.sample(3000, 10, true); controller.sample(4000, 10, true);
        controller.sample(5000, 10, true); controller.sample(6000, 100, true);
        require(controller.sample(7000, value, true) == (value == 20), "80 percent rejection edge");
    }
    for (const auto& edge : {std::array<float, 3>{110, 60, 0.01f}, {807, 772, -0.01f}}) {
        for (float offset : {0.0f, edge[2]}) {
            controller.reset(0);
            for (int64_t t = 3000; t <= 6000; t += 1000) controller.sample(t, edge[0], true);
            const float value = edge[1] + offset;
            require(controller.sample(7000, value, true) == (offset != 0), "Strict trigger band boundary");
        }
    }
    controller.reset(0);
    require(!controller.sample(3000, std::numeric_limits<float>::infinity(), true) &&
            !controller.sample(4000, std::numeric_limits<float>::quiet_NaN(), true), "Nonfinite FPS accepted");
    SessionGate gate;
    gate.start(1000, 5000);
    require(!gate.ready(1000) && !gate.ready(5999) && gate.ready(6000), "Whole-policy delay boundary");
    gate.start(1000, 5000); gate.modeChanged();
    require(gate.ready(1001), "Same-process mode change failed to release gate");
    gate.start(2000, 5000);
    require(!gate.ready(6000), "New process failed to restart gate");
    gate.start(2000, 0); require(gate.ready(2000), "Zero delay added startup wait");
    require(coreCount("0 1 2 3 4 5\n") == 6 && coreCount("6 7\n") == 2 &&
            coreCount("") == 0 && coreCount("0 0") == 0 && coreCount("0 x") == 0 &&
            coreCount("-1") == 0, "Runtime CPU topology parsing");

    const auto stock = directory / "policy.xml", overlay = directory / "overlay.xml";
    Policy parsed; std::string error;
    require(parsed.load(stock, overlay, &error), "FPS fixture load");
    Mode mode = *parsed.select("default", 1).mode;
    mode.fpsAdjust = std::array<int, 3>{INT32_MAX, 1, 2};
    mode.perfHints = {{0x41000000, 6}, {0x41000200, 2}, {0x41000000, 0}, {0x41000200, 0},
                     {0x41000000, -1}, {0x41000000, 7}, {0x41000200, 3}, {0x40CAC000, 50}};
    parsed.games["default"]["Balance"] = mode;
    const Opps opps = {{{384000, 1000000, 1500000, 2000000}, {768000, 2000000, 3000000},
                        {1000000000, 800000000, 600000000}}};
    parsed.resolve(opps, {6, 2});
    const auto& resolved = *parsed.select("default", 1).mode;
    require(resolved.perfHints.size() == 5 && resolved.perfHints[0].value == 6 && resolved.perfHints[1].value == 2 &&
            resolved.perfHints[2].value == 0 && resolved.perfHints[3].value == 0,
            "Min-core topology range checking");
    require(Policy::fpsEnabled(resolved, "default") &&
            !Policy::fpsEnabled(resolved, "com.tencent.tmgp.pubgmhd") &&
            !Policy::fpsEnabled(resolved, "com.tencent.tmgp.sgame") &&
            !Policy::fpsEnabled(resolved, "com.tencent.tmgp.projectg") &&
            !Policy::fpsEnabled(resolved, "com.tencent.tmgp.sgamece"), "Scene-gated FPS feedback enabled");
    controller.reset(0);
    for (int64_t t = 3000; t <= 8000; t += 1000)
        require(!controller.sample(t, t < 8000 ? 60 : 50, false), "Disabled profile boosted");
    require(resolved.boostedBands[0].bounds[0].floor == 1500 &&
            resolved.boostedBands[0].bounds[1].floor == 2000 &&
            resolved.boostedBands[0].bounds[2].floor == 1, "Boost index delta or safe rounding incorrect");
    require(resolved.boostedBands[1].bounds[1].floor == -1 &&
            resolved.boostedBands[1].bounds[2].floor == -1 &&
            resolved.boostedBands[3].bounds[2].floor == -1, "Boost created omitted floor");
    for (const auto& band : resolved.boostedBands) for (int r = 0; r < 3; ++r) {
        const auto& bounds = band.bounds[r];
        if (r < 2) {
            require(bounds.floor <= parsed.overlay.ceilings[r] / 1000 &&
                    bounds.ceiling <= parsed.overlay.ceilings[r] / 1000, "CPU boost exceeded hard ceiling");
            require(bounds.floor < 0 || bounds.floor <= bounds.ceiling, "CPU boost floor exceeds ceiling");
        } else {
            for (int index : {bounds.floor, bounds.ceiling}) if (index >= 0)
                require(opps[2].at(index) <= parsed.overlay.ceilings[2], "GPU boost exceeded hard ceiling");
            require(bounds.floor < 0 || bounds.floor >= bounds.ceiling, "GPU boost floor exceeds ceiling");
        }
    }
    const auto boosted = Policy::resources(resolved, 0, true);
    const auto normal = Policy::resources(resolved, 0);
    const auto hintSize = resolved.perfHints.size() * 2;
    require(boosted == Policy::resources(resolved, 0, true) &&
            std::equal(boosted.end() - hintSize, boosted.end(), normal.end() - hintSize),
            "Boost accumulated or changed hints");
    Policy unknown = parsed;
    // A separate unresolved policy verifies fail-closed topology admission, including zero votes.
    require(unknown.load(stock, overlay, &error), "Topology fixture reload");
    unknown.games["default"]["Balance"].perfHints = {{0x41000000, 0}, {0x41000200, 0}};
    unknown.resolve(opps);
    require(unknown.select("default", 1).mode->perfHints.empty(), "Unknown topology admitted min-core votes");
    std::cout << "FPS_CONTROLLER_OK history warm-up rejection edges restore no-accumulation cancel disabled\n"
              << "DELAY_GATE_OK whole-policy mode-release new-process zero-delay\n"
              << "MIN_CORES_OK topology ranges unknown-topology\n"
              << "BOOST_RESOLUTION_OK deltas roundDown omissions hints hard-ceilings\n";
}
void scenes(const std::filesystem::path& directory) {
    Policy policy; std::string error;
    require(policy.load(directory / "policy.xml", directory / "overlay.xml", &error), error.c_str());
    const Opps actual = {{{384000, 900000, 1500000, 2100000}, {768000, 1900000, 2400000, 3100000},
                          {1100000000, 850000000, 650000000, 550000000}}};
    policy.resolve(actual, {6, 2});
    const auto& mode = *policy.select("default", 1).mode;
    require(mode.scenes.size() == 2 && policy.select("default", 2).mode->scenes.size() == 2,
            "Repeated SceneOpt or profile inheritance lost");
    const auto& row = mode.scenes.at(1).frequencies;
    require(row.bounds[0].floor == 384 && row.bounds[0].ceiling == 1500 &&
            row.bounds[1].floor == 2400 && row.bounds[1].ceiling == 2400 &&
            row.bounds[2].floor == 3 && row.bounds[2].ceiling == 1,
            "Scene indices did not clamp, round down and obey hard ceilings");
    SceneController scene;
    require(!scene.scene() && !scene.active(0), "Initial scene enabled");
    for (const auto& profile : {"com.tencent.tmgp.pubgmhd", "com.tencent.tmgp.projectg"}) {
        require(!Policy::fpsEnabled(mode, profile), "PUBG without scene enabled");
        for (int id : {1, 5, 100, 102, 103, 104, 110, 111})
            require(Policy::fpsEnabled(mode, profile, id) == (id == 103 || id == 104), "PUBG scene FPS gate");
    }
    for (const auto& profile : {"com.tencent.tmgp.sgame", "com.tencent.tmgp.sgamece"}) {
        require(!Policy::fpsEnabled(mode, profile), "SGAME without scene enabled");
        for (int id : {1, 5, 7, 6, 106})
            require(Policy::fpsEnabled(mode, profile, id) == (id == 7), "SGAME scene FPS gate");
    }
    for (const auto& profile : {"com.tencent.tmgp.pubgmhd", "com.tencent.tmgp.sgame"}) {
        Policy profiles = policy;
        profiles.games[profile] = {{"Balance", mode}};
        profiles.games["global.own"] = {{"Balance", mode}};
        profiles.overlay.aliases["global.own"] = {profile, false};
        profiles.overlay.aliases["global.fallback"] = {profile, false};
        const auto own = profiles.select("global.own", 1);
        const auto fallback = profiles.select("global.fallback", 1);
        const int enabledScene = std::string(profile) == "com.tencent.tmgp.pubgmhd" ? 103 : 7;
        require(own.profile == "global.own" && Policy::fpsEnabled(*own.mode, own.profile) &&
                Policy::jankEnabled(*own.mode, own.profile), "Own stock entry inherited alias scene gates");
        require(fallback.profile == profile && !Policy::fpsEnabled(*fallback.mode, fallback.profile) &&
                !Policy::jankEnabled(*fallback.mode, fallback.profile) &&
                !Policy::fpsEnabled(*fallback.mode, fallback.profile, 1) &&
                !Policy::jankEnabled(*fallback.mode, fallback.profile, 1) &&
                Policy::fpsEnabled(*fallback.mode, fallback.profile, enabledScene) &&
                Policy::jankEnabled(*fallback.mode, fallback.profile, enabledScene),
                "Fallback alias did not use resolved profile scene gates");
    }
    std::cout << "ALIAS_SCENE_GATES_OK own-stock ungated fallback PUBG/SGAME FPS/jank gated\n";
    require(scene.report(100, 1, mode) && scene.active(1099) && !scene.active(1100), "Scene apply/expire boundary");
    require(!scene.report(200, 1, mode) && scene.deadline() == 1200, "Repeated scene failed to refresh its row");
    auto expected = Policy::resources(row);
    for (const auto& hint : mode.perfHints) { expected.push_back(hint.opcode); expected.push_back(hint.value); }
    require(scene.resources(200, mode, 0, true) == expected &&
            scene.resources(200, mode, 0, false) == expected, "FPS stacked on scene or mode hints lost");
    require(scene.resources(1200, mode, 0, false) == Policy::resources(mode, 0), "Scene expiry failed to restore band");
    require(scene.report(300, 7, mode) && scene.deadline() == 500, "New scene failed to replace deadline");
    require(scene.report(400, 103, mode) && !scene.active(400) && scene.scene() == 103,
            "Unmapped scene failed to cancel row or update FPS gate");
    require(!scene.report(400, -1, mode) && !scene.report(400, 100001, mode) && scene.scene() == 103,
            "Out-of-range scene changed state");
    scene.report(500, 1, mode); scene.cancel();
    require(!scene.active(501) && scene.scene() == 1 &&
            scene.resources(501, mode, 0, false) == Policy::resources(mode, 0), "Sensor failure retained scene row");
    scene.reset();
    require(!scene.scene() && !scene.active(501), "Mode/session end retained scene state");
    FpsController fps;
    fps.reset(0);
    for (int64_t t = 3000; t <= 7000; t += 1000) fps.sample(t, 60, true);
    scene.report(7800, 1, mode);
    require(!fps.sample(8000, 50, !scene.active(8000)), "FPS feedback boosted during scene row");
    require(fps.sample(8800, 50, !scene.active(8800)), "Scene expiry left FPS feedback suppressed");
    std::cout << "SCENE_CONTROLLER_OK apply expire replace cancel reset hints no-FPS-stacking\n"
              << "SCENE_RESOLUTION_OK clamp roundDown hard-ceilings inheritance\n"
              << "SCENE_FPS_GATES_OK PUBG 1,5,100,102,103,104,110,111 SGAME 7,6,106 no-scene siblings\n";
}
void jankFeedback(const std::filesystem::path& directory) {
    JankController jank;
    require(!jank.sample(0, 0, 60, true, false) && !jank.sample(0, 61, 60, true, false) &&
            !jank.sample(0, 1, 0, true, false) && !jank.sample(0, 1, 60, false, false) &&
            !jank.sample(0, 1, 60, true, true), "Invalid/disabled/blocked jank boosted");
    require(jank.sample(1000, 1, 60, true, false) && jank.active(1099) && !jank.active(1100),
            "Jank boost must last exactly 100 ms");
    require(!jank.sample(1499, 1, 60, true, false) && jank.sample(1500, 1, 60, true, false),
            "Jank rate limit must be one boost per 500 ms");
    jank.cancel(); require(!jank.active(1501), "Jank cancellation retained boost");
    jank.reset(); require(jank.sample(0, 1, 60, true, false), "New session retained jank rate limit");
    Policy policy; std::string error;
    require(policy.load(directory / "policy.xml", directory / "overlay.xml", &error), error.c_str());
    const Opps actual = {{{384000, 1000000, 1500000, 2000000}, {768000, 2000000, 3000000},
                          {1000000000, 800000000, 600000000}}};
    policy.resolve(actual, {6, 2});
    const auto& mode = *policy.select("default", 1).mode;
    require(Policy::jankEnabled(mode, "default") && !Policy::jankEnabled(mode, "com.tencent.tmgp.pubgmhd") &&
            Policy::jankEnabled(mode, "com.tencent.tmgp.pubgmhd", 103) &&
            !Policy::jankEnabled(mode, "com.tencent.tmgp.pubgmhd", 110) &&
            !Policy::jankEnabled(mode, "com.tencent.tmgp.pubgmhd", 111) &&
            !Policy::jankEnabled(mode, "com.tencent.tmgp.sgame", 6) &&
            Policy::jankEnabled(mode, "com.tencent.tmgp.sgame", 7), "Jank scene gating changed");
    Mode onlyJank = mode; onlyJank.fpsAdjust.reset();
    require(Policy::jankEnabled(onlyJank, "default"), "Jank incorrectly requires FPS adjustment levels");
    onlyJank.jankAdjustTime = 0;
    require(!Policy::jankEnabled(onlyJank, "default"), "Disabled jank time admitted a boost");
    require(mode.jankBands[0].bounds[0].floor == 1500 && mode.jankBands[0].bounds[1].floor == 2000 &&
            mode.jankBands[0].bounds[2].floor == 1, "Jank delta resolution or hard ceilings incorrect");
    SceneController scene;
    const auto fps = Policy::resources(mode, 0, true);
    const auto jankArgs = Policy::resources(mode, 0, false, true);
    require(scene.resources(1000, mode, 0, false, true) == jankArgs &&
            scene.resources(1000, mode, 0, true, true) == fps, "FPS did not take priority over jank");
    scene.report(1000, 1, mode);
    require(scene.resources(1001, mode, 0, true, true) == scene.resources(1001, mode, 0, false),
            "FPS or jank stacked on a scene row");
    std::cout << "JANK_CONTROLLER_OK 100ms one-per-500ms app-only counts scene-gates hard-ceilings scene>FPS>jank\n";
}
void recurringCost(const std::filesystem::path& directory) {
    FpsController timed;
    timed.reset(0);
    for (int64_t now = 3000; now < 8000; now += 500) require(!timed.sample(now, 60, true), "Stable half-second FPS boosted");
    require(timed.sample(8000, 50, true), "Half-second FPS dip missed");
    // The previous five-value history [60,60,60,50,50] has mean 56 and would
    // boost again on this consecutive dip; a recovery/cooldown is now required.
    require(50 > 0.6 * 56 && 50 < 0.965 * 56 && !timed.sample(8500, 50, true),
            "Consecutive samples re-triggered the same dip");
    require(!timed.sample(9000, 60, true) && timed.sample(9500, 50, true), "Recovery failed to re-arm FPS feedback");
    timed.reset(0);
    for (int64_t now = 3000; now < 8000; now += 500) timed.sample(now, 60, true);
    require(timed.sample(8000, 50, true), "Cooldown fixture missed initial dip");
    for (int64_t now : {8500, 9000, 9500}) require(!timed.sample(now, 50, true), "FPS dip cooldown ended early");
    require(timed.sample(10000, 50, true), "Persistent dip failed to retry after two seconds");
    require(!timed.sample(16000, 45, true) && timed.history()[3] == 0,
            "Five-second window retained stale samples");
    timed.reset(0); timed.sample(3000, 60, true);
    require(!timed.sample(3399, 50, true) && timed.history().back() == 60,
            "Sub-400 ms callback entered the FPS window");
    require(timed.sample(3400, 50, true), "400 ms callback was delayed/dropped");
    std::cout << "FPS_TIME_WINDOW_OK half-second cadence five-second expiry recovery two-second retry 400ms admission\n";
    FpsController stale, refreshed;
    stale.reset(0); refreshed.reset(0);
    for (int64_t now = 3000; now <= 7000; now += 1000) {
        stale.sample(now, 60, true); refreshed.sample(now, 60, true);
    }
    refreshed.invalidate();
    require(stale.sample(8000, 50, true), "Retained history no longer demonstrates the previous transition trigger");
    require(!refreshed.sample(8000, 50, true) && refreshed.history().back() == 50,
            "Changed context retained a stale mean or restarted warm-up");
    std::cout << "FPS_CONTEXT_RESET_OK old_stale_mean_boosts new_context_does_not warmup_unchanged\n";
    ThreadScanCadence scans;
    scans.reset(0);
    int count = 0;
    for (int64_t now = 0; now < 60000; now += 1000) count += scans.due(now);
    require(count == 20, "Thread scan warm-up/5-second cadence");
    scans.reset(60001);
    require(scans.due(60001) && !scans.due(61000) && scans.due(61001), "Rule change failed to restart cadence");
    // A 100 ms request can already have expired when acquisition returns.
    RequestExpiry expiry;
    expiry.set(1100);
    require(expiry.limit(2000) == 1100 && expiry.due(1200), "Expired boost lost its immediate wake-up");
    expiry.set(0); // The immediate expiry pass has selected the base request.
    require(expiry.limit(2000) == 2000 && !expiry.due(1200), "Base request retained a boost deadline");
    scans.reset(0);
    int64_t nextSample = 0, lastScan = -1, largestGap = 0;
    int warmupScans = 0;
    for (int64_t now = 0; now < 31000;) {
        if (now >= nextSample) nextSample = now + 700;
        if (scans.due(now, 700)) {
            if (lastScan >= 0) largestGap = std::max(largestGap, now - lastScan);
            if (now < 10000) ++warmupScans;
            lastScan = now;
        }
        now = std::min(nextSample, scans.deadline());
    }
    require(warmupScans == 15 && largestGap == 5000, "700 ms sampling delayed a thread scan beyond five seconds");
    std::cout << "WORKER_DEADLINES_OK expired_request_immediate scan_700ms_max_gap=5000\n";
    PerfLease lease;
    int renewals = 0;
    for (int64_t now = 0; now < 10000; now += 1000) {
        if (lease.due(now)) { ++renewals; lease.acquired(now); }
    }
    require(renewals == 5, "Stable perf request did not renew every two seconds");
    lease.reset(); require(lease.due(9500), "Changed/released request failed to acquire immediately");
    lease.acquired(10000);
    require(!lease.due(11999) && lease.due(12000) && lease.due(13000), "Perf lease expiry safety margin");

    const auto journalPath = directory / "cost-threads";
    Threads threads([](const auto&) {}, [](const auto&) {}, journalPath.string());
    Mode empty;

    int ready[2]; require(pipe(ready) == 0, "Cannot create thread fixture pipe");
    const pid_t child = fork(); require(child >= 0, "Cannot fork thread fixture");
    if (child == 0) {
        close(ready[0]);
        std::array<std::thread, 3> workers;
        for (auto& worker : workers) worker = std::thread([&] {
            pthread_setname_np(pthread_self(), "CostWorker");
            const char byte = 'x'; (void)!::write(ready[1], &byte, 1);
            for (;;) pause();
        });
        for (;;) pause();
    }
    struct Child {
        pid_t pid;
        ~Child() { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
    } cleanup{child};
    close(ready[1]);
    for (int i = 0; i < 3; ++i) { char byte; require(read(ready[0], &byte, 1) == 1, "Thread fixture not ready"); }
    close(ready[0]);
    std::ifstream stat("/proc/" + std::to_string(child) + "/stat");
    std::string line, field; std::getline(stat, line);
    std::istringstream fields(line.substr(line.rfind(')') + 1));
    for (int i = 3; i <= 22; ++i) fields >> field;
    const auto start = std::stoll(field);
    for (int i = 0; i < 20; ++i) threads.apply(child, start, empty, true, i * 1000);
    require(threads.scans() == 0 && threads.journalWrites() == 0, "Empty rules performed recurring I/O");
    cpu_set_t allowed; CPU_ZERO(&allowed);
    require(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "Host affinity unavailable");
    int cpu = 0; while (cpu < 64 && !CPU_ISSET(cpu, &allowed)) ++cpu;
    require(cpu < 64, "Thread fixture requires an allowed CPU below 64");
    Mode rules; rules.bindCore.push_back({"CostWorker", uint64_t{1} << cpu});
    threads.apply(child, start, rules, true, 0);
    require(threads.bound() == 3 && threads.scans() == 1 && threads.journalWrites() == 1,
            "New thread originals were not persisted once per scan");
    threads.apply(child, start, rules, true, 1000);
    require(threads.scans() == 2 && threads.journalWrites() == 1, "Unchanged scan rewrote the journal");
    threads.apply(child, start, empty, true, 2000);
    require(threads.bound() == 0 && threads.scans() == 2 && threads.journalWrites() == 2,
            "Rule removal failed to restore without scanning");
    std::cout << "RECURRING_COST_OK scans_60s=20 old=60 stable_renewals_10s=5 old=10"
              << " journal_writes_3_threads=1 old=3 empty_rules_scans=0\n";
}
void threadRecovery(const std::filesystem::path& directory) {
    cpu_set_t possible; CPU_ZERO(&possible);
    for (int i = 0; i < 8; ++i) CPU_SET(i, &possible);
    CPU_SET(CPU_SETSIZE - 1, &possible);
    auto equal = [](const cpu_set_t& a, const cpu_set_t& b) {
        for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &a) != CPU_ISSET(i, &b)) return false;
        return true;
    };
    ThreadOriginal original;
    original.placement.task = {100, 123, "Mainloop"};
    CPU_ZERO(&original.affinity); CPU_SET(6, &original.affinity);
    original.policy = SCHED_RR; original.priority = 23;
    original.bound = true;
    int affinityCalls = 0, schedulerCalls = 0;
    bool fallback;
    auto affinity = [&](const cpu_set_t& mask) {
        ++affinityCalls;
        if (affinityCalls == 1) {
            require(equal(mask, original.affinity), "Original affinity not attempted first");
            return EINVAL;
        }
        require(equal(mask, possible), "Fallback must include all possible CPUs");
        return 0;
    };
    auto scheduler = [&](int policy, int priority) {
        ++schedulerCalls;
        require(policy == original.policy && priority == original.priority, "Original scheduler values lost");
        return 0;
    };
    require(restoreThreadState(original, possible, affinity, scheduler, &fallback) == 0 &&
            fallback && affinityCalls == 2 && schedulerCalls == 0, "Cpuset fallback did not complete affinity-only restore");
    for (int error : {EPERM, EACCES, ESRCH}) {
        affinityCalls = 0;
        const int result = restoreThreadState(original, possible, [&](const cpu_set_t&) {
            ++affinityCalls; return error;
        }, scheduler, &fallback);
        require(result == (error == ESRCH ? 0 : error) && !fallback && affinityCalls == 1,
                "Only EINVAL may trigger affinity fallback; other errors must retain/retry");
    }
    affinityCalls = 0;
    require(restoreThreadState(original, possible, [&](const cpu_set_t&) {
        ++affinityCalls; return EINVAL;
    }, scheduler, &fallback) == EINVAL && fallback && affinityCalls == 2, "Failed fallback must retain recovery entry");
    original.bound = false; original.realtime = true;
    affinityCalls = schedulerCalls = 0;
    require(restoreThreadState(original, possible, affinity, scheduler, &fallback) == 0 &&
            !fallback && affinityCalls == 0 && schedulerCalls == 1, "RT-only restore touched affinity");
    require(restoreThreadState(original, possible, affinity, [](int, int) { return EPERM; }, &fallback) == EPERM,
            "Scheduler error must retain recovery entry");
    original.realtime = false;
    affinityCalls = schedulerCalls = 0;
    require(restoreThreadState(original, possible, affinity, scheduler, &fallback) == 0 &&
            affinityCalls == 0 && schedulerCalls == 0, "Untouched state restored");

    ThreadJournal journal;
    std::ifstream("/proc/sys/kernel/random/boot_id") >> journal.bootId;
    journal.pid = 7; journal.start = 1234;
    for (int flags = 0; flags < 4; ++flags) {
        original.bound = flags & 1; original.realtime = flags & 2;
        original.placement.task.tid = 100 + flags;
        journal.originals.push_back(original);
    }
    const auto path = directory / "threads-recovery";
    const auto serialized = encodeThreadJournal(journal);
    write(path, serialized);
    std::ifstream input(path);
    const std::string disk((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ThreadJournal decoded;
    require(decodeThreadJournal(disk, &decoded) == JournalStatus::Valid && decoded.pid == journal.pid &&
            decoded.start == journal.start && decoded.bootId == journal.bootId && decoded.originals.size() == 4,
            "Journal disk round trip failed");
    for (size_t i = 0; i < decoded.originals.size(); ++i) {
        const auto& before = journal.originals[i]; const auto& after = decoded.originals[i];
        require(before.bound == after.bound && before.realtime == after.realtime && equal(before.affinity, after.affinity) &&
                before.policy == after.policy && before.priority == after.priority &&
                before.placement.task.tid == after.placement.task.tid && before.placement.task.start == after.placement.task.start,
                "Journal lost flags or original state");
        affinityCalls = schedulerCalls = 0;
        require(restoreThreadState(after, possible, [&](const cpu_set_t&) { ++affinityCalls; return 0; },
                scheduler, &fallback) == 0 && affinityCalls == (before.bound ? 1 : 0) && schedulerCalls == (before.realtime ? 1 : 0),
                "Recovered flags did not restrict restoration");
    }
    for (const auto& version : {"1", "99"}) {
        const auto unsupported = std::string(version) + serialized.substr(serialized.find(' '));
        require(decodeThreadJournal(unsupported, &decoded) == JournalStatus::UnsupportedVersion && decoded.originals.empty(),
                "Unsupported journal version accepted");
        write(path, unsupported);
        std::vector<std::string> warnings;
        Threads executor([&](const auto& message) { warnings.push_back(message); }, [](const auto&) {}, path.string());
        executor.recover();
        require(warnings.size() == 1 && warnings[0].find("Unsupported thread recovery version") != std::string::npos,
                "Unsupported journal not diagnosed");
        std::ifstream replaced(path);
        const std::string empty((std::istreambuf_iterator<char>(replaced)), std::istreambuf_iterator<char>());
        require(decodeThreadJournal(empty, &decoded) == JournalStatus::Valid && decoded.originals.empty() && decoded.pid == 0,
                "Unsupported journal not discarded safely");
    }
    auto invalidFlags = serialized;
    const auto flag = invalidFlags.find(" 0 0 ", invalidFlags.find('\n'));
    require(flag != std::string::npos, "Missing journal flag fixture");
    invalidFlags[flag + 1] = '2';
    require(decodeThreadJournal(invalidFlags, &decoded) == JournalStatus::Invalid && decoded.originals.empty(),
            "Invalid journal flag accepted");
    std::cout << "THREAD_RECOVERY_OK cpuset-fallback error-retention subset-restore journal-flags old-version-discard\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 6, "Usage: check WUJI_XML WUJI_OVERLAY BALDUR_XML BALDUR_OVERLAY TEMP_DIR");
        real(argv[1], argv[2]); real(argv[3], argv[4]);
        std::filesystem::create_directories(argv[5]);
        synthetic(argv[5]);
        feedback(argv[5]);
        scenes(argv[5]);
        threadRules();
        threadRecovery(argv[5]);
        recurringCost(argv[5]);
        jankFeedback(argv[5]);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
