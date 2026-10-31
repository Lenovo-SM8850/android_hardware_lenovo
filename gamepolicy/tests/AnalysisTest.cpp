// SPDX-License-Identifier: Apache-2.0
#include "Tests.h"
#include "Analysis.h"
#include "Policy.h"
#include "Tuning.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace gamepolicy;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

Opps tablesOf(const char* stock) {
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

constexpr const char* kGame = "com.miHoYo.GenshinImpact";
constexpr int kReady = kFlagReady;

struct Spec {
    std::vector<float> fps;
    std::array<int, 3> cur{};
    std::array<int, 3> cap{1996800, 4396800, 726000000};
    int skin = 38000, band = 0;
    std::function<void(Sample&, int)> tweak;
};
std::vector<Sample> make(const Spec& spec, int count = 300) {
    std::vector<Sample> samples;
    for (int i = 0; i < count; ++i) {
        Sample s;
        s.timeMs = 1000 * (i + 1);
        s.fps = spec.fps[i % spec.fps.size()];
        s.fpsMin = s.fps - 1;
        s.frames = 60;
        s.jankFrames = i % 10 == 0 ? 1 : 0;
        s.cur = spec.cur; s.cap = spec.cap;
        s.skinMilliC = spec.skin; s.cpuMilliC = 45000; s.gpuMilliC = 45000;
        s.powerMw = 4000; s.band = spec.band; s.thermalCase = 200;
        s.flags = kReady; s.mode = 1; s.profileGeneration = 1;
        if (spec.tweak) spec.tweak(s, i);
        samples.push_back(s);
    }
    return samples;
}

// Straightforward re-implementation used as the expectation for summarize().
struct Reference {
    int valid = 0; double avg = 0, median = 0, low1 = 0, low5 = 0; int target = 0;
    double hit = 0, stable = 0, jankPercent = -1, jankPer1000 = 0, tail95 = -1;
    double avgSkin = -1, maxSkin = -1, maxCpu = -1, maxGpu = -1; int avgPower = -1;
    std::array<int, 3> avgCur{}; std::array<double, 3> pegged{}; double throttled = 0; bool mixed = false;
    std::vector<int> scenes;
};
Reference reference(const std::vector<Sample>& all, const Ladders& ladders) {
    Reference ref;
    std::vector<Sample> v;
    for (const auto& s : all) if ((s.flags & 1) != 0 && s.fps >= 1 && s.timeMs >= 10000) v.push_back(s);
    const size_t n = v.size();
    ref.valid = static_cast<int>(n);
    std::set<int> scenes;
    for (const auto& s : all) if (s.sceneId >= 0) scenes.insert(s.sceneId);
    ref.scenes.assign(scenes.begin(), scenes.end());
    if (!n) return ref;
    auto pick = [&](std::vector<double> x, double p) {
        std::sort(x.begin(), x.end());
        return x[static_cast<size_t>(std::floor(p / 100.0 * (static_cast<double>(x.size()) - 1) + 1e-9))];
    };
    std::vector<double> fps;
    for (const auto& s : v) fps.push_back(s.fps);
    for (double f : fps) ref.avg += f;
    ref.avg /= n;
    ref.median = pick(fps, 50); ref.low1 = pick(fps, 1); ref.low5 = pick(fps, 5);
    const double p95 = pick(fps, 95);
    ref.target = 165;
    for (int t : {30, 40, 45, 60, 90, 120, 144, 165}) if (t >= 0.97 * p95) { ref.target = t; break; }
    double jank = 0, frames = 0;
    std::vector<double> tail;
    for (const auto& s : v) {
        ref.hit += s.fps >= 0.9 * ref.target;
        ref.stable += std::fabs(s.fps - ref.median) <= 0.1 * ref.median;
        jank += s.jankFrames; frames += s.frames;
        if (s.maxFrameTimeUs >= 0) tail.push_back(std::max(0.0, s.maxFrameTimeUs / 1000.0 / (1000.0 / ref.target) - 1));
        ref.throttled += s.band > 0;
        ref.mixed = ref.mixed || s.mode != v[0].mode || s.profileGeneration != v[0].profileGeneration;
    }
    ref.hit /= n; ref.stable /= n; ref.throttled /= n;
    ref.jankPercent = frames > 0 ? 100 * jank / frames : -1;
    ref.jankPer1000 = 1000 * jank / (ref.target * static_cast<double>(n));
    if (!tail.empty()) ref.tail95 = pick(tail, 95);
    auto temp = [&](int Sample::*field, double* avg, double* max) {
        double sum = 0; int count = 0;
        for (const auto& s : v) {
            if (s.*field <= kTemperatureUnavailable) continue;
            sum += s.*field / 1000.0; ++count;
            if (*max < 0 || s.*field / 1000.0 > *max) *max = s.*field / 1000.0;
        }
        if (avg && count) *avg = sum / count;
    };
    double unused = -1;
    temp(&Sample::skinMilliC, &ref.avgSkin, &ref.maxSkin);
    temp(&Sample::cpuMilliC, &unused, &ref.maxCpu);
    temp(&Sample::gpuMilliC, &unused, &ref.maxGpu);
    double power = 0; int powerCount = 0;
    for (const auto& s : v) if (s.powerMw >= 0) { power += s.powerMw; ++powerCount; }
    if (powerCount) ref.avgPower = static_cast<int>(std::lround(power / powerCount));
    for (int r = 0; r < 3; ++r) {
        double sum = 0; int count = 0, pegged = 0;
        for (const auto& s : v) {
            if (s.cur[r] > 0) { sum += s.cur[r]; ++count; }
            const double cap = s.cap[r] < 0 ? ladders[r].back() : s.cap[r];
            if (s.cur[r] >= 0.97 * cap) ++pegged;
        }
        ref.avgCur[r] = count ? static_cast<int>(std::lround(sum / count)) : 0;
        ref.pegged[r] = static_cast<double>(pegged) / n;
    }
    return ref;
}
void checkSummary(const std::string& name, const Summary& s, const Reference& ref) {
    auto bad = [&](const char* field) { throw std::runtime_error(name + ": summary field " + field + " differs"); };
    if (s.validSamples != ref.valid) bad("validSamples");
    if (!near(s.avgFps, ref.avg)) bad("avgFps");
    if (!near(s.medianFps, ref.median)) bad("medianFps");
    if (!near(s.lowFps1, ref.low1)) bad("lowFps1");
    if (!near(s.lowFps5, ref.low5)) bad("lowFps5");
    if (!near(s.floor5Fps, ref.low5)) bad("floor5Fps");
    if (s.targetFps != ref.target) bad("targetFps");
    if (!near(s.hitRate, ref.hit)) bad("hitRate");
    if (!near(s.stability, ref.stable)) bad("stability");
    if (!near(s.jankPercent, ref.jankPercent)) bad("jankPercent");
    if (!near(s.jankPer1000, ref.jankPer1000)) bad("jankPer1000");
    if (!near(s.tail95, ref.tail95)) bad("tail95");
    if (!near(s.avgSkinC, ref.avgSkin)) bad("avgSkinC");
    if (!near(s.maxSkinC, ref.maxSkin)) bad("maxSkinC");
    if (!near(s.maxCpuC, ref.maxCpu)) bad("maxCpuC");
    if (!near(s.maxGpuC, ref.maxGpu)) bad("maxGpuC");
    if (s.avgPowerMw != ref.avgPower) bad("avgPowerMw");
    for (int r = 0; r < 3; ++r) {
        if (s.avgCur[r] != ref.avgCur[r]) bad("avgCur");
        if (!near(s.pegged[r], ref.pegged[r])) bad("pegged");
    }
    if (!near(s.throttled, ref.throttled)) bad("throttled");
    if (s.mixed != ref.mixed) bad("mixed");
    if (s.scenesSeen != ref.scenes) bad("scenesSeen");
}

struct World {
    Policy policy;
    Ladders ladders;
    TuningRecord current;
    ProfileView recorded;
};
World world(const char* xml, const char* overlay) {
    World w;
    std::string error;
    require(w.policy.load(xml, overlay, &error), error);
    w.policy.resolve(tablesOf(xml), {6, 2}, 0xFF);
    for (int r = 0; r < 3; ++r) w.ladders[r] = w.policy.ladder(r);
    w.current.package = kGame; w.current.mode = 1; w.current.baseMode = kBaseBalanced;
    w.current.skinTargetMilliC = 41000; // Explicit target for the recommendation scenarios.
    require(normalize(w.policy, w.current, &error), error);
    Mode mode; Selection selection{};
    require(build(w.policy, w.current, &mode, &selection, &error), error);
    w.recorded = view(w.policy, mode, w.current.baseProfile, w.current.baseMode, false, w.current.threadRules);
    return w;
}

Proposal tune(const World& w, const std::vector<Sample>& samples, const TuningRecord& current, int goal,
              const std::vector<Sensitivity>& history = {}, const ProfileView* recorded = nullptr) {
    Proposal p;
    int error = -1;
    require(autoTune(w.policy, samples, recorded ? *recorded : w.recorded, current, goal, history, &p, &error) && error == kOk,
            "autoTune failed with error " + std::to_string(error));
    return p;
}
void expectCap(const std::string& name, const Proposal& p, int resource, bool lower) {
    require(p.changes.size() == 1, name + ": expected exactly one change, got " + std::to_string(p.changes.size()));
    const Change& c = p.changes[0];
    require(c.kind == kChangeMax && c.resource == resource, name + ": wrong change kind/resource");
    require(lower ? c.after < c.before : c.after > c.before, name + ": wrong change direction");
    require(p.probeResource == resource && p.probeDeltaSteps == (lower ? -1 : 1), name + ": wrong probe fields");
    for (int r = 0; r < 3; ++r)
        require(p.tuning.maxOffsets[r] == (r == resource ? (lower ? -1 : 1) : 0), name + ": offsets must change for one resource only");
}
ProfileView withCeiling(ProfileView p, int resource, int ceiling) {
    for (auto& step : p.steps) {
        step.units[resource].ceiling = ceiling;
        if (step.units[resource].floor > ceiling) step.units[resource].floor = ceiling;
    }
    return p;
}
Candidate candidate(const std::string& key, const ProfileView& profile) {
    Candidate c;
    c.profileKey = key; c.profile = profile; c.baseMode = 1;
    return c;
}
Summary expectedSummary(int power, float floor5, float maxSkin) {
    Summary s;
    s.validSamples = 200; s.targetFps = 60; s.floor5Fps = floor5; s.tail95 = 0.2f; s.maxSkinC = maxSkin; s.avgPowerMw = power;
    return s;
}

void scenario1(const World& w) {
    Spec spec;
    spec.fps = {50, 52}; spec.cur = {1440000, 2112000, 726000000};
    const auto samples = make(spec);
    const Summary s = summarize(samples, w.ladders);
    checkSummary("S1", s, reference(samples, w.ladders));
    require(s.validSamples == 291 && s.targetFps == 60 && s.tail95 == -1 && s.scenesSeen.empty(), "S1 fixed summary values");
    require(near(s.pegged[2], 1.0) && near(s.pegged[0], 0.0), "S1 pegged GPU only");
    const Wanted want = wanted(samples, s, w.recorded, w.ladders, kGoalFrames);
    require((want.reasons & (kFpsShort | kGpuBound)) == (kFpsShort | kGpuBound) && !(want.reasons & kCpuBound), "S1 reasons");
    require(want.target[2] == 1000000000 && want.want[2] == 1, "S1 GPU target is three steps above the cap");
    const Proposal p = tune(w, samples, w.current, kGoalFrames);
    expectCap("S1", p, 2, false);
    require(p.changes[0].before == 726000000 && p.changes[0].after == 826000000, "S1 GPU 726 -> 826 MHz");
    require(p.tuning.maxOffsets[2] == 1 && p.tuning.minOffsets == std::array<int, 3>{}, "S1 offsets");
    require(p.preview.steps[0].units[2].ceiling == 826000000, "S1 preview");
    require((p.reasons & (kFpsShort | kGpuBound)) == (kFpsShort | kGpuBound), "S1 proposal reasons");
}

void scenario2(const World& w) {
    Spec spec;
    spec.fps = {40, 41, 42, 43, 44}; spec.cur = {1440000, 768000, 500000000};
    spec.cap = {1996800, 768000, 726000000};
    spec.tweak = [](Sample& s, int i) {
        s.maxFrameTimeUs = 20000 + (i % 5) * 5000;   // 20..40 ms.
        s.sceneId = i % 5 == 0 ? 7 : i % 5 == 1 ? 3 : i % 5 == 2 ? 0 : -1;
        s.jankFrames = i % 4 == 0 ? 2 : 0;
    };
    const auto samples = make(spec);
    const Summary s = summarize(samples, w.ladders);
    checkSummary("S2", s, reference(samples, w.ladders));
    require(s.targetFps == 45 && s.tail95 >= 0 && s.scenesSeen == std::vector<int>({0, 3, 7}), "S2 fixed summary values");
    const Wanted want = wanted(samples, s, w.recorded, w.ladders, kGoalBalanced);
    require((want.reasons & (kFpsShort | kCpuBound)) == (kFpsShort | kCpuBound) && !(want.reasons & kGpuBound), "S2 reasons");
    require(want.want[1] == 1 && want.target[1] == 1132800, "S2 Prime target");
    const Proposal p = tune(w, samples, w.current, kGoalBalanced);
    expectCap("S2 unknown probes GPU first", p, 2, false);
    require(p.changes[0].before == 726000000 && p.changes[0].after == 826000000, "S2 GPU 726 -> 826 MHz");

    const Candidate a = candidate("A", withCeiling(w.recorded, 2, 646000000));
    const Candidate b = candidate("B", withCeiling(w.recorded, 1, 1132800));
    const Candidate c = candidate("C", withCeiling(w.recorded, 1, 4396800));
    int reasons = 0, error = -1;
    auto ranked = recommend(samples, w.recorded, {a, b, c}, w.ladders, kGoalBalanced, &reasons, &error);
    require(error == kOk && ranked.size() == 3 && (reasons & kCpuBound), "S2 recommend result");
    require(ranked[0].candidate.profileKey == "B" && ranked[0].directions[1] == 1, "S2 B first with Prime direction up");
    require(ranked[1].candidate.profileKey == "A" && ranked[2].candidate.profileKey == "C", "S2 A then C");
    require(ranked[0].score <= ranked[1].score && ranked[1].score <= ranked[2].score, "S2 ascending scores");
    require(ranked[1].directions[2] == -1 && ranked[0].directions[2] == 0, "S2 GPU directions");

    // Stored records replace the heuristic: good and cheap first, violations never in the top group.
    auto stored = [&](const std::string& key, int power, float floor5, float skin) {
        Candidate x = candidate(key, withCeiling(w.recorded, 1, 1267200));
        x.profile.thermalCase = 300;
        x.hasExpected = true; x.expected = expectedSummary(power, floor5, skin);
        return x;
    };
    Candidate same = candidate("Same", w.recorded);
    same.hasExpected = true; same.expected = expectedSummary(1000, 60, 35);
    auto measured = [&](const std::vector<Candidate>& choices) {
        std::vector<RecordEvidence> history;
        std::vector<Candidate> configs = choices;
        for (auto& c : configs) if (c.hasExpected) {
            c.tunedPackage = "game." + c.profileKey; c.tunedMode = 1;
            history.push_back({c.tunedPackage, 1, c.profile, c.expected});
        }
        return recommend(samples, w.recorded, configs, w.ladders, kGoalBalanced, &reasons, &error, history);
    };
    ranked = measured({b, stored("D", 3000, 58, 39), stored("E", 2500, 58, 39),
                      stored("F", 1000, 58, 42), stored("G", 1000, 40, 39), same});
    require(error == kOk && ranked.size() == 3, "S2 stored recommend result");
    require(ranked[0].candidate.profileKey == "E" && ranked[1].candidate.profileKey == "D" &&
            ranked[2].candidate.profileKey == "B", "S2 stored ranking: power order, then heuristic");
    ranked = measured({stored("F", 1000, 58, 42), stored("G", 1000, 40, 39), same});
    require(ranked.size() == 2 && ranked[0].candidate.profileKey == "G" && ranked[1].candidate.profileKey == "F",
            "S2 quality shortfall ranks before a thermal violation; unchanged candidate skipped");
}

void scenario3(const World& w) {
    Spec spec;
    spec.fps = {59.5f, 60.0f, 60.5f}; spec.cur = {1113600, 768000, 422000000};
    const auto samples = make(spec);
    const Summary s = summarize(samples, w.ladders);
    checkSummary("S3", s, reference(samples, w.ladders));
    require(s.targetFps == 60 && near(s.hitRate, 1.0), "S3 fixed summary values");
    const Wanted want = wanted(samples, s, w.recorded, w.ladders, kGoalEfficiency);
    require((want.reasons & (kTargetMet | kOverprovisioned)) == (kTargetMet | kOverprovisioned), "S3 reasons");
    require(want.want == std::array<int, 3>({-1, -1, -1}) && want.target[0] == 1228800 && want.target[2] == 461000000, "S3 wanted");
    // Gold has the most headroom among the resources that can still move down (Prime sits on its lowest step).
    Proposal p = tune(w, samples, w.current, kGoalEfficiency);
    expectCap("S3", p, 0, true);
    require(p.changes[0].before == 1996800 && p.changes[0].after == 1900800, "S3 Gold 1996.8 -> 1900.8 MHz");
    // History decides: GPU shows no gain per watt while Gold does.
    p = tune(w, samples, w.current, kGoalEfficiency, {{2, 1, 0.2, 0, 800, 0.3}, {0, 1, 3.0, 0, 500, 0.4}});
    expectCap("S3 history", p, 2, true);
    require(p.changes[0].before == 726000000 && p.changes[0].after == 646000000, "S3 GPU 726 -> 646 MHz");
    // The frames goal never trades quality for power, the balanced goal needs more margin than 59.5 / 60.
    p = tune(w, samples, w.current, kGoalFrames);
    require(p.changes.empty() && p.probeResource == -1 && p.tuning.maxOffsets == std::array<int, 3>{}, "S3 frames keeps");
    require(p.preview.steps[0].units[2].ceiling == 726000000, "S3 preview unchanged");
    p = tune(w, samples, w.current, kGoalBalanced);
    require(p.changes.size() == 1 && p.changes[0].after < p.changes[0].before, "S3 balanced lowers");
}

void scenario4(const World& w) {
    Spec spec;
    spec.fps = {45, 46, 47, 48}; spec.cur = {1555200, 2112000, 646000000}; spec.skin = 42000; spec.band = 2;
    spec.tweak = [](Sample& s, int i) { s.maxFrameTimeUs = 18000 + (i % 3) * 1000; };
    const auto samples = make(spec);
    const Summary s = summarize(samples, w.ladders);
    checkSummary("S4", s, reference(samples, w.ladders));
    require(near(s.throttled, 1.0) && near(s.maxSkinC, 42.0), "S4 fixed summary values");
    const Wanted want = wanted(samples, s, w.recorded, w.ladders, kGoalFrames);
    require((want.reasons & (kFpsShort | kThermalLimited | kHot)) == (kFpsShort | kThermalLimited | kHot), "S4 reasons");
    require(want.thermalCase == 300, "S4 wanted case 300 (heuristic, not the auto-adjust decision)");
    Proposal p = tune(w, samples, w.current, kGoalFrames);
    require(p.changes.size() == 1 && p.changes[0].kind == kChangeThermal && p.changes[0].before == 200 && p.changes[0].after == 100,
            "S4 CHANGE_THERMAL 200 -> 100");
    require(p.tuning.maxOffsets == std::array<int, 3>{} && p.tuning.minOffsets == std::array<int, 3>{}, "S4 offsets untouched");
    require(p.tuning.thermalCase == 100 && p.preview.thermalCase == 100 && p.probeResource == -1 && p.probeDeltaSteps == -1,
            "S4 thermal probe fields");
    require((p.reasons & kHot) != 0, "S4 proposal reasons");
    // Already at the lowest case: lower the least efficient resource instead (GPU runs highest relative to its ceiling).
    TuningRecord low = w.current;
    low.thermalCase = 100;
    Mode mode; Selection selection{}; std::string err;
    require(normalize(w.policy, low, &err) && build(w.policy, low, &mode, &selection, &err), err);
    const ProfileView lowView = view(w.policy, mode, low.baseProfile, low.baseMode, true, low.threadRules);
    p = tune(w, samples, low, kGoalFrames, {}, &lowView);
    expectCap("S4 lowest case", p, 2, true);
}

void scenario5(const World& w) {
    Spec spec;
    spec.fps = {50, 52}; spec.cur = {1440000, 2112000, 726000000};
    int error = -1, reasons = 0;
    {   // (a) Exactly 109 valid samples is short, 110 is enough.
        for (int ready : {100, 109, 110}) {
            spec.tweak = [ready](Sample& s, int i) { if (i < 20 || i >= 20 + ready) s.flags = 0; };
            const auto samples = make(spec);
            const bool enough = ready >= kMinValidSamples;
            require(summarize(samples, w.ladders).validSamples == ready, "S5 valid count");
            const auto ranked = recommend(samples, w.recorded, {candidate("A", withCeiling(w.recorded, 2, 646000000))},
                                          w.ladders, kGoalBalanced, &reasons, &error);
            require(enough ? (error == kOk && ranked.size() == 1) : (error == kNotEnoughData && ranked.empty()), "S5a recommend");
            Proposal p;
            const bool ok = autoTune(w.policy, samples, w.recorded, w.current, kGoalFrames, {}, &p, &error);
            require(enough ? (ok && error == kOk) : (!ok && error == kNotEnoughData), "S5a autoTune");
        }
    }
    {   // (b) A profile generation change after sample 150 makes the record mixed.
        spec.tweak = [](Sample& s, int i) { if (i >= 150) s.profileGeneration = 2; };
        const auto samples = make(spec);
        const Summary s = summarize(samples, w.ladders);
        require(s.mixed && s.validSamples == 291, "S5b mixed summary");
        const auto ranked = recommend(samples, w.recorded, {candidate("A", withCeiling(w.recorded, 2, 646000000))},
                                      w.ladders, kGoalBalanced, &reasons, &error);
        require(error == kMixed && ranked.empty(), "S5b recommend");
        Proposal p;
        require(!autoTune(w.policy, samples, w.recorded, w.current, kGoalFrames, {}, &p, &error) && error == kMixed, "S5b autoTune");
        // A mode change mixes too.
        spec.tweak = [](Sample& sample, int i) { if (i >= 150) sample.mode = 2; };
        require(summarize(make(spec), w.ladders).mixed, "S5b mode change mixes");
    }
}

void scenario6(const World& w) {
    Spec first;
    first.fps = {50, 52}; first.cur = {1440000, 2112000, 726000000};
    const auto baseline = make(first);
    const Proposal p1 = tune(w, baseline, w.current, kGoalFrames);
    expectCap("S6 first", p1, 2, false);

    // After applying: GPU runs at the new 826 MHz ceiling, floor improved by 4 fps for 500 mW.
    Spec second;
    second.fps = {54, 55, 56, 57}; second.cur = {1440000, 2112000, 826000000};
    second.cap = {1996800, 4396800, 826000000};
    second.tweak = [](Sample& s, int) { s.powerMw = 4500; s.skinMilliC = 39000; };
    const auto next = make(second);
    Mode mode; Selection selection{}; std::string err;
    require(build(w.policy, p1.tuning, &mode, &selection, &err), err);
    const ProfileView applied = view(w.policy, mode, p1.tuning.baseProfile, p1.tuning.baseMode, true, p1.tuning.threadRules);

    const Summary before = summarize(baseline, w.ladders), after = summarize(next, w.ladders);
    const Sensitivity closed = closeProbe(p1.probeResource, p1.probeDeltaSteps, before, after);
    require(closed.resource == 2 && closed.deltaSteps == 1 && near(closed.dFloor5, 4.0) && near(closed.dPowerMw, 500) &&
            near(closed.dSkinPeakC, 1.0) && near(closed.dTail95, 0.0), "S6 closeProbe values");
    Summary withTiming = before, withTiming2 = after;
    withTiming.tail95 = 0.9f; withTiming2.tail95 = 0.4f; withTiming2.avgPowerMw = -1;
    const Sensitivity partial = closeProbe(0, -1, withTiming, withTiming2);
    require(near(partial.dTail95, -0.5) && partial.dPowerMw == 0 && partial.resource == 0 && partial.deltaSteps == -1,
            "S6 closeProbe with unknown power");

    // Still short: GPU is not the pegged resource now, yet the positive GPU history decides.
    Spec third = second;
    third.cap = {1996800, 768000, 826000000};
    third.cur = {1440000, 768000, 500000000};
    third.tweak = [](Sample& s, int) { s.powerMw = 4500; };
    const auto again = make(third);
    Proposal p2 = tune(w, again, p1.tuning, kGoalFrames, {closed}, &applied);
    require(p2.changes.size() == 1 && p2.changes[0].kind == kChangeMax && p2.changes[0].resource == 2 &&
            p2.probeResource == 2 && p2.probeDeltaSteps == 1, "S6 GPU is raised again");
    require(p2.tuning.maxOffsets == std::array<int, 3>({0, 0, 2}) && p2.changes[0].before == 826000000 &&
            p2.changes[0].after == 902000000, "S6 GPU raised again to 902 MHz");
    // Without history GPU is still probed first, even though Prime is pegged.
    p2 = tune(w, again, p1.tuning, kGoalFrames, {}, &applied);
    require(p2.changes.size() == 1 && p2.changes[0].resource == 2 && p2.probeResource == 2, "S6 GPU first without history");
    // A GPU probe without gain is not repeated: the next untested resource (gold) is probed.
    const Sensitivity flat{2, 1, 0.5, 0, 100, 0};
    p2 = tune(w, again, p1.tuning, kGoalFrames, {flat}, &applied);
    require(p2.changes.size() == 1 && p2.changes[0].resource == 0, "S6 Gold second despite pegged Prime");
    p2 = tune(w, again, p1.tuning, kGoalFrames, {flat, {0, 1, 0, 0, 100, 0}}, &applied);
    require(p2.changes.size() == 1 && p2.changes[0].resource == 1, "S6 Prime third after GPU and Gold");
    Spec fourth = third;
    fourth.cur = {1440000, 2112000, 826000000};
    fourth.cap = {1996800, 4396800, 826000000};   // Only GPU is pegged.
    p2 = tune(w, make(fourth), p1.tuning, kGoalFrames, {flat}, &applied);
    require(p2.changes.size() == 1 && p2.changes[0].resource == 0 && p2.tuning.maxOffsets[2] == 1 && p2.tuning.maxOffsets[0] == 1,
            "S6 flat GPU history moves on to gold");
    // Too close to the skin target: no raise at all.
    third.skin = 40250;
    p2 = tune(w, make(third), p1.tuning, kGoalFrames, {closed}, &applied);
    require(p2.changes.empty(), "S6 no raise within 1 C of the target");
}

void scenario7(const World& w) {
    Spec spec;
    spec.fps = {50, 52}; spec.cur = {1440000, 2112000, 726000000};
    // Flat 38 C, then a linear rise of 0.04 C/s over the last 60 s: peak 40.4 C stays below the 41 C target, but the
    // 15 s forecast (41.0 C) is above target - 0.5 C and below target + 0.5 C.
    spec.tweak = [](Sample& s, int) {
        s.skinMilliC = s.timeMs <= 240000 ? 38000 : 38000 + (s.timeMs - 240000) / 25;
    };
    const auto samples = make(spec);
    const Summary s = summarize(samples, w.ladders);
    require(near(s.maxSkinC, 40.4), "S7 peak skin");
    const Proposal p = tune(w, samples, w.current, kGoalFrames);
    expectCap("S7", p, 2, true);
    require(p.changes[0].before == 726000000 && p.changes[0].after == 646000000, "S7 GPU 726 -> 646 MHz");
    require(p.tuning.thermalCase == kKeep && p.preview.thermalCase == 200 && (p.reasons & kHot) != 0, "S7 case untouched");
    // Without the rise the same record is quality short and raises GPU.
    Spec flat = spec;
    flat.tweak = nullptr;
    expectCap("S7 flat", tune(w, make(flat), w.current, kGoalFrames), 2, false);
    // A cool record that meets the target with margin and is balanced: ample lowering is allowed only when cool.
    Spec hot = spec;
    hot.fps = {59.5f, 60.0f, 60.5f}; hot.cur = {1113600, 768000, 422000000}; hot.skin = 40300;
    hot.tweak = nullptr;
    const Proposal q = tune(w, make(hot), w.current, kGoalEfficiency);
    require(q.changes.empty(), "S7 no lowering for efficiency when the forecast leaves no 1 C margin");
}

// Frame rate the game asks for: every sample is judged against its own target.
void scenario8(const World& w) {
    // Resolution of a signal pair.
    require(signalTarget(-1, -1) == -1 && signalTarget(0, -1) == -1 && signalTarget(std::nanf(""), -1) == -1 &&
            signalTarget(INFINITY, -1) == -1 && signalTarget(5000, -1) == -1, "S8 no usable signal");
    require(signalTarget(60, -1) == 60 && signalTarget(59.94f, -1) == 60 && signalTarget(-1, 44) == 45 &&
            signalTarget(30.5f, -1) == 30 && signalTarget(121, -1) == 120, "S8 snapped to a common target within 3 %");
    require(signalTarget(48, -1) == 48 && signalTarget(50, -1) == 50 && signalTarget(42.5f, -1) == 43 &&
            signalTarget(0.2f, -1) == 1, "S8 rounded when no common target is near");
    require(signalTarget(60, 45) == 45 && signalTarget(45, 60) == 45 && signalTarget(60, -1) == 60 &&
            signalTarget(-1, 90) == 90, "S8 the smaller positive signal wins");

    // 30 fps menu, then 60 fps battle; the two seconds after the switch dip.
    Spec spec;
    spec.fps = {30}; spec.cur = {1113600, 768000, 422000000};
    spec.tweak = [](Sample& s, int i) {
        const bool battle = i >= 150;
        s.fps = battle ? 60.f : 30.f;
        s.requestedFps = battle ? 60.f : 30.f;
        s.maxFrameTimeUs = battle ? 16000 : 33000;
        if (i == 150 || i == 151) { s.fps = 20.f; s.maxFrameTimeUs = 100000; s.jankFrames = 5; }
    };
    const auto mixed = make(spec);
    const auto targets = sampleTargets(mixed);
    require(targets.size() == mixed.size() && targets[0] == 30 && targets[149] == 30 && targets[150] == 60 &&
            targets[299] == 60, "S8 per-sample targets");
    const Summary s = summarize(mixed, w.ladders);
    require(s.validSamples == 291 && s.targetFps == 60, "S8 the longer held target is the summary target");
    require(near(s.floor5Fps, 60) && near(steadyRatio(s), 1.0) && near(s.floorRatio, 1.0), "S8 floor skips the warm-up");
    require(near(s.tail95, 0), "S8 tail uses each sample's own budget");
    require(near(s.hitRate, 289.0 / 291), "S8 hit rate against own targets, warm-up included");
    require(near(s.jankPer1000, 1000.0 * 28 / (30 * 141 + 60 * 148), 1e-3), "S8 jank per 1000 uses each sample's own budget");

    auto stripped = mixed;
    for (auto& sample : stripped) sample.requestedFps = sample.throttleFps = -1;
    const Summary s0 = summarize(stripped, w.ladders);
    require(s0.targetFps == 60 && s0.hitRate < 0.55 && s0.tail95 > 0.9 && steadyRatio(s0) < 0.6,
            "S8 without signals the 30 fps part counts as missed frames");
    const auto raised = [](const Proposal& p) {
        for (const auto& c : p.changes) if (c.kind == kChangeMax && c.after > c.before) return true;
        return false;
    };
    require(raised(tune(w, stripped, w.current, kGoalBalanced)), "S8 without signals the mixed record asks for more");
    require(!raised(tune(w, mixed, w.current, kGoalBalanced)), "S8 with signals the mixed record is not short");
    Summary stored = expectedSummary(4000, 60, 38);
    stored.floorRatio = 0.5f;
    Summary storedOld = expectedSummary(4000, 60, 38);
    require(near(steadyRatio(stored), 0.5) && near(steadyRatio(storedOld), 1.0), "S8 stored ratio, derived for older summaries");

    // No signals at all: unchanged results, the session detector still picks the target.
    Spec plain;
    plain.fps = {30, 60, 45, 60, 60, 59};
    const auto none = make(plain);
    const Summary sn = summarize(none, w.ladders);
    checkSummary("S8 none", sn, reference(none, w.ladders));
    require(near(sn.floorRatio, sn.floor5Fps / sn.targetFps, 1e-6) && sn.floor5Fps == sn.lowFps5, "S8 none: ratio and floor");
    const auto noneTargets = sampleTargets(none);
    for (int t : noneTargets) require(t == sn.targetFps, "S8 none: detector target for every sample");

    // Throttle 45 with a 60 vote: the target is 45.
    Spec capped;
    capped.fps = {45};
    capped.tweak = [](Sample& sample, int) { sample.requestedFps = 60; sample.throttleFps = 45; };
    const Summary sc = summarize(make(capped), w.ladders);
    require(sc.targetFps == 45 && near(sc.hitRate, 1.0) && near(steadyRatio(sc), 1.0), "S8 throttle 45 with vote 60");
    capped.tweak = [](Sample& sample, int) { sample.requestedFps = 60; };
    const Summary sv = summarize(make(capped), w.ladders);
    require(sv.targetFps == 60 && near(sv.hitRate, 0.0), "S8 vote 60 alone");
}
void budgetsAndWarmBands(const World& source) {
    World w = source;
    w.current.skinTargetMilliC = -1; // This scenario explicitly disables the target.
    Spec shortfall;
    shortfall.fps = {50, 52}; shortfall.cur = {1440000, 2112000, 726000000}; shortfall.skin = 44000;
    ProfileView off = w.recorded;
    off.skinTargetMilliC = -1;
    expectCap("skin budget off", tune(w, make(shortfall), w.current, kGoalFrames, {}, &off), 2, false);
    const Wanted wantedOff = wanted(make(shortfall), summarize(make(shortfall), w.ladders), off, w.ladders, kGoalEfficiency);
    require(!(wantedOff.reasons & kHot) && wantedOff.thermalCase == off.thermalCase, "Off target causes no thermal reduction");
    off.powerTargetMw = 3500;
    const std::vector<Sensitivity> history{{2, 1, 0.1, 0, 500, 0}, {0, 1, 4, 0, 500, 0}, {1, 1, 3, 0, 500, 0}};
    expectCap("power budget lowers least efficient domain", tune(w, make(shortfall), w.current, kGoalFrames, history, &off), 2, true);
    TuningRecord edited = w.current;
    edited.skinTargetMilliC = -1; edited.powerTargetMw = 3500;
    expectCap("newly edited budgets apply to old record", tune(w, make(shortfall), edited, kGoalFrames, history), 2, true);
    off.powerTargetMw = 4000;
    require(tune(w, make(shortfall), w.current, kGoalFrames, {}, &off).changes.empty(),
            "Power at budget blocks probes whose estimated mean power exceeds it");
    off.powerTargetMw = 4250;
    const auto predicted = tune(w, make(shortfall), w.current, kGoalFrames, {{2, 1, 4, 0, 1000, 0}}, &off);
    require(predicted.probeResource != 2, "Measured power sensitivity rejects an over-budget raise");
    off.powerTargetMw = -1;
    expectCap("power budget off", tune(w, make(shortfall), w.current, kGoalFrames, {}, &off), 2, false);

    TuningRecord warm = w.current;
    warm.steps = w.recorded.steps;
    warm.steps[0].units[2].ceiling = w.ladders[2].back();
    Mode mode; Selection selection{}; std::string error;
    require(build(w.policy, warm, &mode, &selection, &error), error);
    ProfileView warmView = view(w.policy, mode, warm.baseProfile, warm.baseMode, true, warm.threadRules);
    warmView.skinTargetMilliC = -1;
    const auto proposal = tune(w, make(shortfall), warm, kGoalFrames, {}, &warmView);
    require(proposal.changes.size() == 1 && proposal.changes[0].resource == 2 && proposal.tuning.maxOffsets[2] == 1 &&
            proposal.preview.steps[0].units[2].ceiling == warmView.steps[0].units[2].ceiling &&
            proposal.preview.steps.back().units[2].ceiling > warmView.steps.back().units[2].ceiling,
            "Saturated cold band still permits a useful warm-band probe");
}

void proposalCache(const World& w) {
    Spec spec; spec.fps = {50, 52}; spec.cur = {1440000, 2112000, 726000000};
    const Proposal proposal = tune(w, make(spec), w.current, kGoalFrames);
    require(proposal.probeResource >= 0, "Cache fixture has a probe");
    TuningRecord tuning = proposal.tuning;
    std::string error;
    require(normalize(w.policy, tuning, &error), error);
    const TuningRecord expected = tuning;
    const ProbeRec probe{proposal.probeResource, proposal.probeDeltaSteps, "baseline", 1234};
    std::map<ProposalKey, PendingProposal> proposals;
    cacheProposal(proposals, 0, std::move(tuning), probe);
    const auto found = proposals.find(ProposalKey{0, expected.package, expected.mode});
    require(found != proposals.end(), "Applied tuning finds proposal under its original package");
    require(found->second.tuning.package == expected.package && found->second.tuning.mode == expected.mode &&
            found->second.tuning.maxOffsets == expected.maxOffsets &&
            found->second.probe.resource == proposal.probeResource &&
            found->second.probe.deltaSteps == proposal.probeDeltaSteps &&
            found->second.probe.baseline == "baseline" && found->second.probe.armedAtEpochMs == 1234,
            "Cached tuning retains the proposal and probe payload");
    require(proposals.count(ProposalKey{0, "", expected.mode}) == 0, "No moved-from package key");
    auto otherMode = expected; otherMode.mode = 2;
    cacheProposal(proposals, 0, otherMode, probe);
    cacheProposal(proposals, 10, expected, probe);
    auto replacement = expected; ++replacement.maxOffsets[2];
    cacheProposal(proposals, 0, replacement, probe);
    require(proposals.size() == 3 && proposals.at({0, expected.package, expected.mode}).tuning.maxOffsets == replacement.maxOffsets &&
            proposals.at({0, expected.package, 2}).tuning.mode == 2 &&
            proposals.at({10, expected.package, expected.mode}).tuning.maxOffsets == expected.maxOffsets,
            "Proposal replacement and lookup preserve user and mode isolation");
    std::cout << "PROPOSAL_CACHE_OK original-package probe-payload user-mode-isolation\n";
}

void selectionGenerations(const World& w) {
    const int initial = nextProfileGeneration(0, 0, {}, 1, w.recorded);
    require(initial == 1, "First selection gets a configuration generation");
    int resumed = nextProfileGeneration(initial, 1, w.recorded, 1, w.recorded);
    resumed = nextProfileGeneration(resumed, 1, w.recorded, 1, w.recorded);
    require(resumed == initial, "Repeated unchanged resumes keep the generation");
    auto renamed = w.recorded; renamed.variantName = "renamed";
    require(nextProfileGeneration(initial, 1, w.recorded, 1, renamed) == initial, "Display label is not configuration");
    // A resolved live view may contain only disabled scheduler rules, with no applied thread details.
    ProfileView disabled; disabled.threadRuleCount = 1;
    require(nextProfileGeneration(initial, 1, disabled, 1, disabled) == initial && !sameConfiguration(disabled, disabled),
            "Live equality does not relax the missing-details gate for stored evidence");
    const std::vector<std::function<void(ProfileView&)>> changes{
        [](auto& p) { p.profileKey += ".other"; }, [](auto& p) { ++p.baseMode; },
        [](auto& p) { p.tuned = !p.tuned; }, [](auto& p) { p.variantId = "other"; },
        [](auto& p) { ++p.steps[0].units[2].ceiling; }, [](auto& p) { ++p.steps[0].units[0].floor; },
        [](auto& p) { ++p.steps[0].temperature; }, [](auto& p) { ++p.thermalCase; },
        [](auto& p) { ++p.uclampMin; }, [](auto& p) { ++p.gpuDefaultHz; },
        [](auto& p) { p.keepCoresOnline = !p.keepCoresOnline; }, [](auto& p) { p.frameBoostSteps.push_back(1); },
        [](auto& p) { p.jankBoostSteps.push_back(1); }, [](auto& p) { ++p.startDelayMs; },
        [](auto& p) { ++p.threadRules; }, [](auto& p) { ++p.threadRuleCount; }, [](auto& p) { ++p.sceneCount; },
        [](auto& p) { ++p.sceneMode; }, [](auto& p) { p.scenes.push_back({}); },
        [](auto& p) { p.threads.push_back({"other", 1, -1}); }, [](auto& p) { ++p.boostLengthMs; },
        [](auto& p) { ++p.dropSensitivity; }, [](auto& p) { ++p.hysteresisDeciC; },
        [](auto& p) { ++p.skinTargetMilliC; }, [](auto& p) { ++p.powerTargetMw; }};
    for (const auto& change : changes) {
        auto next = w.recorded; change(next);
        require(nextProfileGeneration(initial, 1, w.recorded, 1, next) == initial + 1,
                "Configuration edits advance the generation");
    }
    require(nextProfileGeneration(initial, 1, w.recorded, 2, w.recorded) == initial + 1,
            "Game mode changes advance generation even when resolved caps match");

    Spec spec; spec.fps = {50, 52}; spec.cur = {1440000, 2112000, 726000000};
    spec.tweak = [=](Sample& s, int i) { s.profileGeneration = i < 150 ? initial : resumed; };
    const auto samples = make(spec);
    require(!summarize(samples, w.ladders).mixed, "Unchanged resume does not mix a recording");
    int reasons = 0, error = -1;
    const std::vector<Candidate> candidates{candidate("alternative", withCeiling(w.recorded, 2, 646000000))};
    require(!recommend(samples, w.recorded, candidates, w.ladders,
                       kGoalBalanced, &reasons, &error).empty() && error == kOk,
            "Unchanged resume still permits recommendations");
    require(tune(w, samples, w.current, kGoalFrames).probeResource >= 0, "Unchanged resume still permits a probe");
    auto changed = w.recorded; ++changed.uclampMin;
    const int edited = nextProfileGeneration(initial, 1, w.recorded, 1, changed);
    spec.tweak = [=](Sample& s, int i) { s.profileGeneration = i < 150 ? initial : edited; };
    const auto mixed = make(spec);
    require(summarize(mixed, w.ladders).mixed, "Real change mixes a recording even without a cap change");
    require(recommend(mixed, w.recorded, candidates, w.ladders,
                      kGoalBalanced, &reasons, &error).empty() && error == kMixed,
            "Real change still blocks recommendations");
    Proposal proposal;
    require(!autoTune(w.policy, mixed, w.recorded, w.current, kGoalFrames, {}, &proposal, &error) && error == kMixed,
            "Real change still blocks auto-adjust");
    std::cout << "SELECTION_GENERATION_OK unchanged-resume real-change-mixed suggestions-gate\n";
}

void signatureEvidence(const World& w) {
    Spec spec; spec.fps = {60}; spec.cur = {1440000, 2112000, 726000000};
    spec.tweak = [](Sample& s, int) { s.maxFrameTimeUs = 17000; };
    const auto samples = make(spec);
    const Summary summary = summarize(samples, w.ladders);
    Candidate near = candidate("near", withCeiling(w.recorded, 2, 826000000));
    Candidate far = candidate("far", withCeiling(w.recorded, 2, 902000000));
    near.tunedPackage = "game.near"; near.tunedMode = 1;
    far.tunedPackage = "game.far"; far.tunedMode = 1;
    std::vector<RecordEvidence> history{{"game.near", 1, near.profile, summary},
                                        {"game.far", 1, far.profile, summary},
                                        {"game.third", 1, w.recorded, summary}};
    history[0].summary.avgPowerMw = 5000;
    history[1].summary.avgPowerMw = 2000;
    int reasons, error;
    auto rank = [&](const ProfileView& profile = ProfileView{}) {
        return recommend(samples, profile.profileKey.empty() ? w.recorded : profile, {near, far}, w.ladders,
                         kGoalBalanced, &reasons, &error, history);
    };
    for (int feature = 0; feature < 3; ++feature) {
        history[1].summary = summary; history[1].summary.avgPowerMw = 2000;
        if (feature == 0) { history[1].summary.targetFps = 120; history[1].summary.floorRatio = 1; }
        if (feature == 1) history[1].summary.avgCur = {0, 0, 0};
        if (feature == 2) { history[1].summary.lowFps1 = 30; history[1].summary.tail95 = 0.8f; }
        const auto ranked = rank();
        require(ranked.size() == 2 && ranked[0].candidate.profileKey == "near" && ranked[0].candidate.hasExpected,
                "Nearest signature beats cheaper unrelated target, cost quadrant, or timing distribution");
    }
    history[1].summary = summary; history[1].summary.avgPowerMw = 2000;
    require(rank()[0].candidate.profileKey == "far", "Equal signatures rank by mean power");
    history[1].summary.avgCur = {0, 0, 0};
    require(rank()[0].candidate.profileKey == "near", "Nearby signature wins when power budget is off");
    ProfileView budget = w.recorded; budget.powerTargetMw = 4500;
    require(rank(budget)[0].candidate.profileKey == "far", "Measured power budget violation ranks after compliant result");
    budget.powerTargetMw = -1;
    require(rank(budget)[0].candidate.profileKey == "near", "Disabling the power budget restores nearest-signature preference");
    budget.powerTargetMw = 4500; history[1].summary.avgCur = summary.avgCur;
    history[1].summary.floorRatio = 0.5f;
    require(rank(budget)[0].candidate.profileKey == "far", "Power violation ranks after quality violation");
    history[1].summary.maxSkinC = 42;
    require(rank(budget)[0].candidate.profileKey == "near", "Thermal violation ranks after power violation");
    budget.skinTargetMilliC = -1;
    require(rank(budget)[0].candidate.profileKey == "far", "Off skin target removes measured thermal penalty");
    history[1].summary = summary;

    // The gate counts distinct games, not records, modes, invalid records, or caller-provided expectations.
    history[2].package = history[1].package;
    near.hasExpected = true; near.expected = summary;
    for (const auto& item : rank()) require(!item.candidate.hasExpected, "Two games use heuristic despite duplicate records");
    history[2].package = "game.third"; history[2].summary.mixed = true;
    for (const auto& item : rank()) require(!item.candidate.hasExpected, "Mixed record does not open history gate");
    history[2].summary.mixed = false; history[2].summary.validSamples = 109;
    for (const auto& item : rank()) require(!item.candidate.hasExpected, "Short record does not open history gate");
    history[2].summary = summary;

    const ProfileView matching = history[0].profile;
    const std::vector<std::function<void(ProfileView&)>> mismatches{
        [](auto& p) { p.profileKey += ".old"; }, [](auto& p) { ++p.baseMode; },
        [](auto& p) { p.variantId = "old"; }, [](auto& p) { p.tuned = !p.tuned; },
        [](auto& p) { p.steps[0].units[2].ceiling = 1; }, [](auto& p) { ++p.boostLengthMs; },
        [](auto& p) { ++p.powerTargetMw; }, [](auto& p) { p.scenes.push_back({}); },
        [](auto& p) { p.threads.push_back({"changed", 1, -1}); }};
    for (const auto& mismatch : mismatches) {
        history[0].profile = matching; mismatch(history[0].profile);
        const auto ranked = rank();
        const auto item = std::find_if(ranked.begin(), ranked.end(), [](const auto& x) { return x.candidate.profileKey == "near"; });
        require(item != ranked.end() && !item->candidate.hasExpected, "Different stored configuration is not evidence");
    }
    history[0].profile = matching;
    // Fallback ranking also consumes the budget using a cap-ratio estimate of mean power.
    history.clear(); budget = w.recorded; budget.powerTargetMw = -1;
    const double without = rank(budget)[0].score;
    budget.powerTargetMw = 2000;
    require(rank(budget)[0].score >= without + 10000, "Heuristic recommendation consumes power budget");
}
} // namespace

void analysisTests(const char* baldurXml, const char* baldurOverlay) {
    const World w = world(baldurXml, baldurOverlay);
    require(w.recorded.steps.size() == 3 && w.recorded.thermalCase == 200, "Genshin balanced recorded profile");
    require(w.recorded.steps[0].units[0].ceiling == 1996800 && w.recorded.steps[0].units[1].ceiling == 768000 &&
            w.recorded.steps[0].units[2].ceiling == 726000000, "Genshin balanced step 0");
    Sample sample;
    require(!valid(sample), "Default sample is not valid");
    sample.flags = kFlagReady; sample.fps = 30; sample.timeMs = 9999;
    require(!valid(sample), "Samples before 10 s are not valid");
    sample.timeMs = 10000;
    require(valid(sample), "Ready sample with fps >= 1 at 10 s is valid");
    sample.fps = 0.9f;
    require(!valid(sample), "fps below 1 is not valid");
    require(hasTemperature(0) && !hasTemperature(kTemperatureUnavailable), "Temperature availability");
    require(summarize({}, w.ladders).validSamples == 0, "Empty summary");
    scenario1(w);
    scenario2(w);
    scenario3(w);
    scenario4(w);
    scenario5(w);
    scenario6(w);
    scenario7(w);
    scenario8(w);
    budgetsAndWarmBands(w);
    proposalCache(w);
    selectionGenerations(w);
    signatureEvidence(w);
    std::cout << "analysis tests passed\n";
}
