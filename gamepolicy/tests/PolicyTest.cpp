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
    size_t hints = 0, bound = 0, rt = 0, delays = 0;
    std::map<int, size_t> accepted, dropped;
    for (const auto& [pkg, modes] : policy.games) for (const auto& [name, mode] : modes) {
        hints += mode.perfHints.size(); bound += mode.bindCore.size(); rt += mode.schedulers.size();
        delays += mode.delayMs > 0;
        for (const auto& hint : mode.perfHints) {
            ++accepted[hint.opcode];
            require((hint.opcode == 0x40CAC000 && hint.value >= 0 && hint.value <= 100) ||
                    hint.opcode == 0x42800000, "Unallowlisted real hint");
        }
    }
    for (const auto& diagnostic : policy.diagnostics) {
        auto pos = diagnostic.find("/PerfHint 0x");
        if (pos != std::string::npos) ++dropped[std::stoi(diagnostic.substr(pos + 10), nullptr, 16)];
    }
    require(hints > 100 && bound > 200 && rt >= 6 && delays >= 3, "Missing phase 2 real coverage");
    if (policy.version == "20260907")
        require(hints == 198 && bound == 509 && rt == 102 && delays == 22, "Unexpected wuji phase 2 counts");
    else if (policy.version == "20260721")
        require(hints == 174 && bound == 385 && rt == 12 && delays == 10, "Unexpected baldur phase 2 counts");
    require(dropped[0x41000000] > 0 && dropped[0x41000200] > 0, "Unmapped core resources accepted");
    require(policy.version != "20260721" || dropped[0x40400000] > 0, "Unmapped extra resource accepted");
    std::cout << "PHASE2_COUNTS version=" << policy.version << " perf_pairs=" << hints << " bind_rules=" << bound
              << " rt_rules=" << rt << " delayed_modes=" << delays << '\n';
    for (auto [opcode, count] : accepted)
        std::cout << "PERF_ACCEPT opcode=0x" << std::hex << opcode << std::dec << " count=" << count << '\n';
    for (auto [opcode, count] : dropped) if (count)
        std::cout << "PERF_DROP opcode=0x" << std::hex << opcode << std::dec << " count=" << count << '\n';
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
            require(Policy::evaluate(battery, temp, 0, -1, 1).resources == Policy::resources(battery, 2),
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
      <BindCore><tid name="Main">C0</tid></BindCore><DelayTime>5000</DelayTime>
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
      </Mode><Mode name="Savage"><DelayTime>0</DelayTime><BindCore><tid name="Worker">3f</tid>
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
    require(mode.perfHints.size() == 2 && mode.bindCore.size() == 1 && mode.bindCore[0].mask == 0xC0 &&
            mode.schedulers.size() == 3 && mode.delayMs == 5000, "Phase 2 parse/inheritance");
    const auto& savage = *policy.select("default", 2).mode;
    require(savage.bindCore.size() == 1 && savage.bindCore[0].name == "Worker" && savage.delayMs == 0,
            "Mode rules/delay did not override Game defaults");
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
    copy.delayMs = 1; require(!sameThreadRules(mode, copy), "Delay diff missed"); copy = mode;
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
        threadRules();
        threadRecovery(argv[5]);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
