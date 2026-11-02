// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Tuning.h"
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace gamepolicy {
constexpr int kTemperatureUnavailable = -273000;
constexpr int kMinValidSamples = 110;
enum SampleFlag { kFlagReady = 1, kFlagScene = 2, kFlagFpsBoost = 4, kFlagJankBoost = 8, kFlagSensorFailed = 16 };
enum Goal { kGoalFrames = 1, kGoalBalanced = 2, kGoalEfficiency = 3 };
enum Reason { kTargetMet = 1, kFpsShort = 2, kCpuBound = 4, kGpuBound = 8, kThermalLimited = 16,
              kOverprovisioned = 32, kHot = 64, kNoPower = 128 };
enum ChangeKind { kChangeMax = 1, kChangeMin = 2, kChangeThermal = 3, kChangeFrameBoost = 4 };
enum AnalysisError { kOk = 0, kNotFound = 1, kNotEnoughData = 2, kMixed = 3 };

enum Diagnosis { kUnclear = 0, kGameCap = 1, kLikelyGameCap = 2, kFpsLimited = 3,
                 kPerformanceLimited = 4, kThermalLimit = 5, kMixedLimits = 6 };
enum Evidence { kAppCadence = 1, kPlateau = 2, kHeadroom = 4, kSaturation = 8,
                kThermalEvidence = 16, kThrottleCeiling = 32, kGameplayRange = 64 };
struct CapSegment { int startMs = 0, endMs = 0, targetFps = 0; bool confirmed = false; };
struct Cause {
    int diagnosis = kUnclear, ceilingFps = 0, analysisTargetFps = 0, desiredTargetFps = -1;
    int evidence = 0, classifiedSamples = 0, shortfallSamples = 0;
    float coverage = 0;
    bool confirmed = false;
    std::array<int, 7> counts{};
    std::vector<CapSegment> segments;
};
constexpr int kAnalysisVersion = 2;

struct Sample {             // Field order = RecordingSample = CSV column order.
    int timeMs = 0; float fps = -1, fpsMin = -1; int jankFrames = 0, frames = 0;
    std::array<int, 3> cur{};  std::array<int, 3> cap{-1, -1, -1};
    int skinMilliC = kTemperatureUnavailable, cpuMilliC = kTemperatureUnavailable, gpuMilliC = kTemperatureUnavailable;
    int powerMw = -1, band = -1, thermalCase = 0, flags = 0, mode = 1, profileGeneration = 0;
    int sceneId = -1, maxFrameTimeUs = -1;
    std::array<int, 2> cpuUtilPermille{-1, -1};
    int gpuBusyPercent = -1, batteryMilliC = kTemperatureUnavailable, batteryPercent = -1;
    float requestedFps = -1, throttleFps = -1;   // Frame rate signals reported last in the second, -1 when none.
    int desiredTargetFps = -1;
    float renderScale = -1, refreshRate = -1;
    bool selectedGameplay = false; // An explicitly kept range supplies gameplay provenance, never scene IDs.
};
struct Summary {            // Field order = DESIGN 9.3, then the SPEC v2 additions.
    int validSamples = 0; float avgFps = 0, medianFps = 0, lowFps1 = 0, lowFps5 = 0; int targetFps = 0;
    float hitRate = 0, stability = 0, jankPercent = -1;
    float avgSkinC = -1, maxSkinC = -1, maxCpuC = -1, maxGpuC = -1; int avgPowerMw = -1;
    std::array<int, 3> avgCur{}; std::array<float, 3> pegged{}; float throttled = 0; bool mixed = false;
    float floor5Fps = 0, tail95 = -1, jankPer1000 = 0; std::vector<int> scenesSeen;
    float floorRatio = -1;  // Q05(fps / target) over all targets; -1 when unknown, see steadyRatio().
    Cause cause;
    float renderScale = -1, refreshRate = -1;
};
using Ladders = std::array<std::vector<int>, 3>;   // Ascending; Policy::ladder(r).

bool hasTemperature(int milliC);
bool valid(const Sample&);                          // Ready, fps >= 1 and timeMs >= 10000.
Summary summarize(const std::vector<Sample>&, const Ladders&);
Cause classify(const std::vector<Sample>&);
Summary tuningSummary(const std::vector<Sample>&, const Ladders&);

// Frame rate a signal pair resolves to: the smaller positive value, snapped to the nearest common target within 3 %,
// else rounded. -1 when neither is positive.
int signalTarget(float requestedFps, float throttleFps);
// Target of every sample in input order: its own signal, else the session P95 detector. -1 when unresolved.
std::vector<int> sampleTargets(const std::vector<Sample>&);
// Steady floor as a fraction of the target (floorRatio, or floor5Fps / targetFps for older summaries).
float steadyRatio(const Summary&);

struct Wanted { std::array<int, 3> target{}; std::array<int, 3> cap{}; std::array<int, 3> want{}; // -1 less, 0 keep, 1 more.
                int thermalCase = 0; int reasons = 0; float medianSkinC = 35; };
Wanted wanted(const std::vector<Sample>&, const Summary&, const ProfileView& recorded, const Ladders&, int goal);

// Compare the configuration captured in a record, including all resolved tuning fields.
bool sameConfiguration(const ProfileView&, const ProfileView&);
// Live selections have resolved details; reselection of the same mode and values keeps its generation.
int nextProfileGeneration(int generation, int previousMode, const ProfileView& previous,
                          int mode, const ProfileView& next);
struct RecordEvidence { std::string package; int mode = 1; ProfileView profile; Summary summary; };
struct Candidate { std::string profileKey; int baseMode = 1; std::string tunedPackage; int tunedMode = 0; ProfileView profile;
                   bool hasExpected = false; Summary expected; };
struct Ranked { Candidate candidate; double score = 0; std::array<int, 3> directions{}; };
// Returns up to 3, ascending score; *error = kNotEnoughData (< 110 valid) or kMixed.
std::vector<Ranked> recommend(const std::vector<Sample>&, const ProfileView& recorded,
                              const std::vector<Candidate>&, const Ladders&, int goal, int* reasons, int* error,
                              const std::vector<RecordEvidence>& history = {});

// Effect of one probe, measured between two non-mixed records of the same game and mode.
struct Sensitivity { int resource = -1; int deltaSteps = 0;
                     double dFloor5 = 0, dTail95 = 0, dPowerMw = 0, dSkinPeakC = 0; };
// resource/deltaSteps come from the stored probe; unknown differences (-1 sources) become 0.
Sensitivity closeProbe(int resource, int deltaSteps, const Summary& baseline, const Summary& next);
bool compatibleProbe(const Summary& baseline, const Summary& next);
// Unknown display provenance cannot establish a controlled hardware-only comparison.
bool compatibleDisplay(float baselineScale, float baselineRefresh, float nextScale, float nextRefresh);

struct Change { int kind = 0; int resource = -1; int before = 0; int after = 0; };
struct Proposal { TuningRecord tuning; std::vector<Change> changes; int reasons = 0; ProfileView preview;
                  int probeResource = -1; int probeDeltaSteps = 0; };
using ProposalKey = std::tuple<int, std::string, int>;
struct PendingProposal { TuningRecord tuning; ProbeRec probe; };
// Cache a normalized tuning for the matching setTuning lookup, preserving its key before moving it.
void cacheProposal(std::map<ProposalKey, PendingProposal>&, int userId, TuningRecord tuning, ProbeRec probe);
// current = stored tuning or the default described in DESIGN 10.3 (caller builds it). history = stored sensitivity
// entries of the same game and mode, oldest first. Returns at most one change.
bool autoTune(const Policy&, const std::vector<Sample>&, const ProfileView& recorded,
              const TuningRecord& current, int goal, const std::vector<Sensitivity>& history,
              Proposal* out, int* error);
} // namespace gamepolicy
