// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include "Tuning.h"
#include "Tests.h"
#include <tinyxml2.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

using namespace gamepolicy;
namespace {
namespace fs = std::filesystem;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::string contents(const fs::path& path) {
    std::ifstream f(path); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
Policy load(const char* stock, const char* overlay) {
    tinyxml2::XMLDocument doc;
    check(doc.LoadFile(stock) == tinyxml2::XML_SUCCESS, "Archive fixture missing");
    Opps opps;
    for (auto kind = doc.FirstChildElement("GameOptPolicy")->FirstChildElement("PreEnv")->FirstChildElement();
         kind; kind = kind->NextSiblingElement()) {
        for (auto cluster = kind->FirstChildElement("cluster"); cluster; cluster = cluster->NextSiblingElement("cluster")) {
            std::string name = cluster->Attribute("name");
            const int r = name == "Gold" ? 0 : name == "Prime" ? 1 : 2;
            std::istringstream input(cluster->GetText());
            int value; while (input >> value) opps[r].push_back(value);
        }
    }
    Policy p; std::string error;
    check(p.load(stock, overlay, &error), error.c_str()); p.resolve(opps, {6, 2}, 0xff);
    return p;
}
Policy fresh(const Policy& original, const std::string& version) {
    Policy p = original; p.version = version;
    p.resetArchiveRegistry();
    return p;
}
void device(const char* stock, const char* overlay, const fs::path& dir) {
    fs::create_directories(dir);
    const auto path = (dir / "history").string();
    std::string error;
    auto a = fresh(load(stock, overlay), "A");
    // Every parsed source/mode is persisted and re-read, including unused empty modes.
    a.games["test.empty"]["Balance"] = a.games.at("default").at("Balance");
    a.games["test.empty"]["chaoshen"] = Mode{};
#ifdef GAMEPOLICY_IO_FAULT_TEST
    check(archiveRoundTrip(a), "Every carried mode must round-trip exactly, including empty Maximum");
#endif
    // A controlled profile ensures every exact field is covered on both carried XMLs.
    auto& modes = a.games["test.game"];
    modes = a.games.at("default");
    auto& mode = modes.at("Balance");
    mode.thermalCase = 0;
    mode.bindCore = {{"Render", 3}, {"Worker", 4}};
    mode.schedulers = {{"Render", 95, true}, {"Worker", 94, false}};
    mode.perfHints.push_back({0x41000000, 2});
    mode.perfHints.push_back({0x41000200, 1});
    mode.fpsAdjust = std::array<int, 3>{1, 1, 1};
    mode.jankAdjust = std::array<int, 3>{1, 1, 1}; mode.jankAdjustTime = 1000;
    mode.boostedBands = mode.bands; mode.jankBands = mode.bands;
    mode.bands.front().units[2] = {}; mode.bands.front().bounds[2] = {};
    mode.scenes[7] = {1234, mode.bands.front()};
    a.overlay.modes = {{1, "Balance"}, {2, "Savage"}, {3, "PowerSave"}};
    a.overlay.aliases["test.alias"] = {"test.game", false};
    check(a.updateBuiltinArchive(path, &error), error.c_str());
    check(a.archiveGeneration() == 1 && !a.history("test.game", 1, 0), "First boot invented history");
    const auto baseline = contents(dir / "history/builtins.xml");
    check(baseline.find("name=\"chaoshen\"") != std::string::npos, "Maximum source data not captured");
    auto everyMode = fresh(a, "A");
    check(everyMode.updateBuiltinArchive(path, &error), "Round-trip failed for a carried source/mode");
    check(contents(dir / "history/builtins.xml") == baseline, "Repeated boot changed checkpoint");

    auto b = fresh(a, "B");
    // Version, diagnostics, hint ordering, source labels and unreachable metadata are excluded.
    b.diagnostics.push_back("Ignored diagnostic"); b.sceneLists["test.game"] = {999};
    std::reverse(b.games["test.game"]["Balance"].perfHints.begin(), b.games["test.game"]["Balance"].perfHints.end());
    check(b.updateBuiltinArchive(path, &error), error.c_str());
    check(b.archiveGeneration() == 2 && !b.history("test.game", 1, 1), "Version-only change archived");

    auto c = fresh(b, "C");
    c.games["test.game"]["Balance"].delayMs += 1000;
    check(c.updateBuiltinArchive(path, &error), error.c_str());
    const auto old = c.history("test.game", 1, 2);
    check(old && old->version == "B" && old->generation == 3, "Changed mode not archived");
    check(!c.history("test.game", 2, 2) && !c.history("test.game", 3, 2), "Unchanged mode archived");
    const auto alias = c.history("test.alias", 1, 2);
    check(alias && !alias->placement, "Alias placement not archived");
    TuningRecord r; r.package = "test.game"; r.baseProfile = old->key;
    Mode built; Selection selection{};
    check(build(c, r, &built, &selection, &error), error.c_str());
    check(built.delayMs == mode.delayMs && built.thermalCase == 0 && !built.linearBands,
          "Archive replay rebuilt lossy fields");
    check(built.schedulers.size() == mode.schedulers.size() && built.schedulers[0].priority == 95 &&
          built.bindCore[0].mask == 3 && built.perfHints.size() == mode.perfHints.size(), "Archive exact rules lost");
    check(selection.profile == "test.game" && selection.threadPlacement, "Archive identity lost");
    for (size_t i = 0; i < built.bands.size(); ++i) for (bool boosted : {false, true})
        check(Policy::resources(built, i, boosted) == Policy::resources(b.games.at("test.game").at("Balance"), i, boosted),
              "Archived physical requests changed on serialization");
    for (size_t i = 0; i < built.bands.size(); ++i)
        check(Policy::resources(built, i, false, true) == Policy::resources(b.games.at("test.game").at("Balance"), i, false, true),
              "Archived jank requests changed on serialization");
    check(built.scenes.at(7).timeMs == 1234 && Policy::resources(built.scenes.at(7).frequencies) ==
          Policy::resources(b.games.at("test.game").at("Balance").scenes.at(7).frequencies), "Archived scenes lost");
    r.package = "test.alias"; r.baseProfile = alias->key;
    check(build(c, r, &built, &selection, &error) && !selection.threadPlacement, "Alias replay placement lost");

    auto d = fresh(c, "D");
    d.games["test.game"]["Balance"].bindCore[0].mask = 7;
    check(d.updateBuiltinArchive(path, &error) && d.history("test.game", 1, 3), "Thread-only change missed");
    auto e = fresh(d, "E");
    for (auto& hint : e.games["test.game"]["Balance"].perfHints)
        if (hint.opcode == 0x41000000) hint.value = 1;
    check(e.updateBuiltinArchive(path, &error) && e.history("test.game", 1, 4), "Core-only change missed");
    auto f = fresh(e, "F");
    auto& boost = f.games["test.game"]["Balance"].boostedBands[0];
    boost.units[0].ceiling = f.ladder(0).front();
    check(f.updateBuiltinArchive(path, &error) && f.history("test.game", 1, 5), "Boost-only change missed");

    auto g = fresh(f, "G");
    g.games.erase("test.game");
    g.games["default"]["Balance"].delayMs += 2000;
    check(g.updateBuiltinArchive(path, &error), error.c_str());
    check(g.history("test.game", 1, 6) && g.history("test.alias", 1, 6) &&
          g.history("test.unknown", 1, 6), "Removed/default/alias transition lost");
    // Successive offline updates remain individually discoverable, including a reused version.
    auto h = fresh(g, "A");
    h.games["default"]["Balance"].delayMs += 1000;
    check(h.updateBuiltinArchive(path, &error), error.c_str());
    auto first = h.history("test.game", 1, 2);
    auto second = h.history("test.game", 1, first->generation);
    check(first && second && first->generation < second->generation && h.history("test.unknown", 1, 7),
          "Successive updates or rollback lost");

    // Reload with no old source at all: persisted active archive still normalizes and builds.
    auto reboot = fresh(load(stock, overlay), "A");
    reboot.games = h.games; reboot.overlay = h.overlay;
    check(reboot.updateBuiltinArchive(path, &error), error.c_str());
    r.package = "test.game"; r.baseProfile = old->key;
    r.variantName = "Historical name";
    TuningStore store((dir / "tunings").string()); store.load(reboot);
    check(store.put(0, r, &error), error.c_str());
    TuningStore reload((dir / "tunings").string()); reload.load(reboot);
    const auto active = reload.find(0, r.package, 1);
    check(active && build(reboot, *active, &built, &selection, &error), "Archived active tuning lost after reboot");
    check(selection.profile == "test.game" && built.thermalCase == 0, "Original identity not retained on reboot");
    auto pending = fresh(reboot, "A");
    TuningStore deferred((dir / "tunings").string()); deferred.load(pending);
    check(deferred.find(0, r.package, 1).has_value(), "Unavailable archive tuning dropped while loading");
    check(deferred.setArmed(0, "test.other", true, &error), error.c_str());
    TuningStore deferredReload((dir / "tunings").string()); deferredReload.load(pending);
    check(deferredReload.find(0, r.package, 1).has_value(), "Unrelated save erased unavailable archive");
    check(pending.updateBuiltinArchive(path, &error), error.c_str());
    check(build(pending, *deferredReload.find(0, r.package, 1), &built, &selection, &error) &&
          selection.profile == "test.game", "Later archive retry did not revive stored selection");

    // Physical values are reconstructed under the current hard caps/topology.
    auto limited = fresh(load(stock, overlay), "A"); limited.games = h.games; limited.overlay = h.overlay;
    for (int resource = 0; resource < 3; ++resource) limited.overlay.ceilings[resource] = limited.ladder(resource).front();
    check(limited.updateBuiltinArchive(path, &error), error.c_str());
    const auto safe = limited.archived(old->key);
    check(bool(safe), "Safe archive missing");
    for (const auto* rows : {&safe->mode.bands, &safe->mode.boostedBands, &safe->mode.jankBands})
        for (const auto& row : *rows) for (int resource = 0; resource < 3; ++resource)
            check(row.units[resource].ceiling <= limited.overlay.ceilings[resource], "Archive exceeded current hard cap");

    auto failed = fresh(h, "I"); failed.games["default"]["Balance"].delayMs += 1000;
    const auto committed = contents(dir / "history/builtins.xml");
    // A directory at the temporary-file path forces a real write failure, even as root.
    fs::create_directory(dir / "history/builtins.xml.tmp");
    check(!failed.updateBuiltinArchive(path, &error), "Write failure accepted");
    check(contents(dir / "history/builtins.xml") == committed, "Failed write advanced durable checkpoint");
    fs::remove(dir / "history/builtins.xml.tmp");
#ifdef GAMEPOLICY_IO_FAULT_TEST
    archiveIoFault(true, false);
    check(!failed.updateBuiltinArchive(path, &error), "Directory-open failure accepted");
    archiveIoFault(false, true);
    check(!failed.updateBuiltinArchive(path, &error) && !failed.archiveReady(), "Directory-fsync failure accepted");
    archiveIoFault(false, false);
#endif
    check(failed.updateBuiltinArchive(path, &error), error.c_str());
    check(failed.archiveReady(), "Successful retry did not publish checkpoint");
    check(failed.history("test.unknown", 1, 8).has_value(), "Failed transition could not retry");
    std::ofstream(dir / "history/builtins.xml") << "<broken>";
    const auto oldEpoch = failed.archiveEpoch();
    check(failed.updateBuiltinArchive(path, &error), "Corrupt snapshot did not recover");
    check(failed.archiveEpoch() != oldEpoch && failed.archiveGeneration() == 1, "Recovery did not create a new epoch");
    bool preserved = false;
    for (const auto& file : fs::directory_iterator(dir / "history"))
        if (file.path().filename().string().find("builtins.xml.bad-") == 0 && contents(file.path()) == "<broken>") preserved = true;
    check(preserved, "Damaged snapshot not preserved");
    check(!failed.archived(old->key), "Lost-epoch key resolved in a different epoch");
    check(build(failed, r, &built, &selection, &error), "Lost-epoch tuning did not fall back");
    check(r.variantName == "Historical name", "Lost-epoch fallback changed the saved name");
    check(selection.profile == failed.select(r.package, r.mode).profile, "Lost-epoch fallback targeted another game");
    std::cout << "BUILTIN_ARCHIVE_DEVICE_PASS " << stock << '\n';
}
void equivalent(const fs::path& dir) {
    fs::create_directories(dir);
    const auto overlay = dir / "overlay.xml", one = dir / "one.xml", two = dir / "two.xml";
    std::ofstream(overlay) << R"(<game-policy version="1"><mode id="1" profile="Balance"/>
      <mode id="2" profile="Savage"/><mode id="3" profile="PowerSave"/>
      <safety sensor="quiet-therm" hysteresisC="1" sampleMs="1000">
      <resource name="Gold" ceiling="2000000" opps="unused"/>
      <resource name="Prime" ceiling="3000000" opps="unused"/>
      <resource name="Gpu" ceiling="1000000000" opps="unused"/></safety></game-policy>)";
    const std::string original = R"(<GameOptPolicy version="one"><PreEnv>
      <CPU><cluster name="Gold" min="0x40800000" max="0x40804000">1000000 2000000</cluster>
      <cluster name="Prime" min="0x40800200" max="0x40804200">2000000 3000000</cluster></CPU>
      <GPU><cluster name="Gpu" min="0x42804000" max="0x42808000">1000000000 600000000</cluster></GPU>
      </PreEnv><GamePolicy><Game pkg="default"><Mode name="Balance"><Policy>
      <TempLevel temp="25"><item type="freq" name="Gold">0_1</item></TempLevel>
      </Policy></Mode><Mode name="Savage"><Policy><TempLevel temp="25"/></Policy></Mode>
      </Game></GamePolicy></GameOptPolicy>)";
    std::ofstream(one) << original;
    auto changed = original;
    changed.replace(changed.find("version=\"one\""), 13, "version=\"two\"");
    changed.replace(changed.find(">1000000 2000000<"), 17, ">500000 1000000 2000000<");
    changed.replace(changed.find(">0_1<"), 5, ">1_2<");
    // Index numbers, declaration formatting and version differ; effective physical rows do not.
    std::ofstream(two) << "<?xml version=\"1.0\"?>\n<!-- whitespace -->\n" << changed;
    const auto onePath = one.string(), twoPath = two.string(), overlayPath = overlay.string();
    auto a = load(onePath.c_str(), overlayPath.c_str());
    auto b = load(twoPath.c_str(), overlayPath.c_str());
    std::string error;
    check(a.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    check(b.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    check(!b.history("test.unknown", 1, 0), "Equivalent resolved indices archived");
    // Removing a selectable mode archives its old dedicated selection before Balance fallback.
    b.games["test.game"] = b.games.at("default");
    b.games["test.game"]["Savage"].delayMs = 7000;
    b.version = "three";
    check(b.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    const auto gen = b.archiveGeneration();
    b.games["test.game"].erase("Savage"); b.version = "four";
    check(b.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    auto old = b.history("test.game", 2, gen);
    check(old && old->mode.delayMs == 7000, "Removed mode fallback history lost");
    // Same-version changes update the baseline but never create versioned copies.
    const auto before = b.archiveGeneration();
    b.games["default"]["Balance"].delayMs += 1000;
    check(b.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    check(b.archiveGeneration() == before && !b.history("test.unknown", 1, before), "Same-version copy created");
    Policy incomplete;
    check(incomplete.load(onePath, overlayPath, &error), error.c_str());
    incomplete.resolve({}, {6, 2}, 0xff);
    check(!incomplete.updateBuiltinArchive((dir / "history").string(), &error), "Incomplete runtime captured");
    check(!incomplete.archiveReady(), "Incomplete runtime exposed a baseline");
    b.version = "hysteresis-one"; b.overlay.hysteresis = 1.25;
    auto& hysteresis = b.games["default"]["Balance"];
    const auto row = hysteresis.bands.front();
    hysteresis.linearBands = true; hysteresis.bands = {row, row, row};
    hysteresis.bands[0].temperature = 30; hysteresis.bands[1].temperature = 40; hysteresis.bands[2].temperature = 50;
    check(b.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    const auto hysteresisGen = b.archiveGeneration();
    b.version = "hysteresis-two"; b.overlay.hysteresis = 1.75;
    check(b.updateBuiltinArchive((dir / "history").string(), &error), error.c_str());
    const auto exact = b.history("test.unknown", 1, hysteresisGen);
    check(exact && exact->mode.inheritedHysteresis == 1.25 &&
          Policy::band(exact->mode, 38.72, 1, 3) == 0 && Policy::band(exact->mode, 38.76, 1, 3) == 1,
          "Fractional overlay hysteresis lost precision");
    // Reusing version labels with identical old content must not create another copy.
    const auto reuse = (dir / "reuse").string();
    auto versionA = fresh(a, "A");
    check(versionA.updateBuiltinArchive(reuse, &error), error.c_str());
    auto versionB = fresh(versionA, "B"); versionB.games["default"]["Balance"].delayMs = 1000;
    check(versionB.updateBuiltinArchive(reuse, &error), error.c_str());
    auto againA = fresh(versionA, "A"); check(againA.updateBuiltinArchive(reuse, &error), error.c_str());
    auto againB = fresh(versionB, "B"); check(againB.updateBuiltinArchive(reuse, &error), error.c_str());
    check(!againB.history("test.unknown", 1, 3), "Version reuse duplicated identical archived content");
    // Runtime safety is re-applied even to a syntactically valid but unsafe stored mode.
    const auto savedPath = fs::path(reuse) / "builtins.xml";
    tinyxml2::XMLDocument unsafe;
    check(unsafe.LoadFile(savedPath.c_str()) == tinyxml2::XML_SUCCESS, "Missing safety snapshot");
    for (auto entry = unsafe.FirstChildElement("builtins")->FirstChildElement("content"); entry;
         entry = entry->NextSiblingElement("content")) {
        entry->FirstChildElement("mode")->SetAttribute("thermal", 999);
        entry->FirstChildElement("mode")->SetAttribute("skin", 48000);
    }
    check(unsafe.SaveFile(savedPath.c_str()) == tinyxml2::XML_SUCCESS, "Cannot write unsafe test fixture");
    auto safer = fresh(versionB, "B"); safer.overlay.skinTargetMilliC = 40000;
    check(safer.updateBuiltinArchive(reuse, &error), error.c_str());
    auto clamped = safer.history("test.unknown", 1, 0);
    check(clamped && clamped->mode.thermalCase == 0 && clamped->mode.skinTargetMilliC == 40000,
          "Historical thermal or skin safety not revalidated");
    // Real runtime inputs appear after startup. The same process can resolve again.
    Policy late;
    check(late.load(onePath, overlayPath, &error), error.c_str());
    for (int r = 0; r < 3; ++r) {
        const auto cluster = dir / ("cpu" + std::to_string(r)); fs::create_directories(cluster);
        late.overlay.oppPaths[r] = (cluster / "table").string();
    }
    late.resolve({}, {}, 0);
    check(!late.updateBuiltinArchive((dir / "late").string(), &error), "Incomplete topology unexpectedly captured");
    for (int r = 0; r < 3; ++r) {
        std::ofstream table(late.overlay.oppPaths[r]);
        for (int f : a.tables()[r]) table << f << ' ';
        if (r < 2) std::ofstream(fs::path(late.overlay.oppPaths[r]).parent_path() / "related_cpus") << (r == 0 ? "0 1 2 3 4 5" : "6 7");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5100));
    check(late.retryBuiltinArchive(&error) && late.archiveReady() && late.archiveRuntime(),
          "Late runtime topology did not recover without a reboot");
    check(late.select("default", 1).mode->bands.front().units[0].ceiling > 0,
          "Recovered runtime did not replace the incomplete built-in selection");
    std::cout << "BUILTIN_ARCHIVE_EQUIVALENCE_PASS formatting physical-indices removed-mode same-version incomplete-runtime\n";
}
}
void archiveTests(const char* first, const char* firstOverlay, const char* second, const char* secondOverlay,
                  const char* temp) {
    device(first, firstOverlay, fs::path(temp) / "archive-first");
    device(second, secondOverlay, fs::path(temp) / "archive-second");
    equivalent(fs::path(temp) / "archive-equivalent");
    std::cout << "BUILTIN_ARCHIVE_TESTS_PASS\n";
}

// Interactive transport used by the JVM repository test. Queries invoke the same
// decoder as getSourceProfile(), with real persisted snapshots and archive keys.
int archiveProtocol(const char* stock, const char* overlay, const char* directory) {
    Policy policy = load(stock, overlay);
    std::string command, error;
    while (std::getline(std::cin, command)) {
        try {
            if (command == "A" || command == "B" || command == "C" || command == "RESET") {
                if (command == "RESET") {
                    fs::remove(fs::path(directory) / "builtins.xml");
                    policy = load(stock, overlay);
                }
                policy.version = command == "RESET" ? "C" : command;
                policy.games["test.game"] = policy.games.at("default");
                for (auto& [name, mode] : policy.games["test.game"])
                    mode.delayMs = policy.version == "A" ? 1000 : policy.version == "B" ? 2000 : 3000;
                check(policy.updateBuiltinArchive(directory, &error), error.c_str());
                std::cout << policy.archiveMarker() << std::endl;
            } else {
                std::istringstream input(command); std::string query; int mode = 0;
                input >> query >> mode;
                const auto result = archiveSourceProfile(policy, query, mode);
                std::cout << result.profileKey << '\t' << result.variantId << '\t' << result.variantName << std::endl;
            }
        } catch (const std::exception& e) { std::cout << "ERROR " << e.what() << std::endl; }
    }
    return 0;
}
