// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Policy.h"
#include <map>
#include <set>

namespace gamepolicy {
constexpr int kKeep = -2;
enum BaseMode { kBaseBalanced = 1, kBasePerformance = 2, kBaseBattery = 3, kBaseMaximum = 4 };
enum ThreadRules { kThreadsOff = 0, kThreadsOwn = 1, kThreadsBase = 2, kThreadsCustom = 3 };
const char* baseModeName(int baseMode);          // "Balance", "Savage", "PowerSave", "chaoshen"; nullptr if invalid.
int baseModeOf(const std::string& modeName);     // Inverse, 0 if unknown.
int gameModeToBase(const Policy&, int gameMode); // overlay.modes[gameMode] -> base id, default kBaseBalanced.

struct Step { double temperature = 0; std::array<Bounds, 3> units{}; }; // Bounds{floor = min, ceiling = max}.
struct SceneRowRec { int sceneId = 0; bool enabled = true; int holdMs = 0; std::array<Bounds, 3> units{}; };
struct ThreadRuleRec { std::string name; uint64_t mask = 0; int rtPriority = -1; };
struct ProbeRec { int resource = -1; int deltaSteps = 0; std::string baseline; int64_t armedAtEpochMs = 0; };
struct SensitivityEntry {
    std::string package; int mode = 1;
    int resource = -1, deltaSteps = 0;
    double dFloor5 = 0, dTail95 = 0, dPowerMw = 0, dSkinPeakC = 0;
};

struct TuningRecord {
    std::string package;
    int mode = 1;               // GameManager mode 1..3.
    bool enabled = true;
    std::string baseProfile;    // Stock key or "default"; "" = own (filled by normalize).
    int baseMode = kBaseBalanced;
    std::array<int, 3> maxOffsets{};
    std::array<int, 3> minOffsets{};
    std::vector<Step> steps;    // Empty = base steps.
    int thermalCase = kKeep, uclampMin = kKeep, gpuDefaultHz = kKeep, keepCoresOnline = kKeep;
    int frameBoost = kKeep;  std::vector<int> frameBoostSteps;
    int jankBoost = kKeep;   std::vector<int> jankBoostSteps;
    int startDelayMs = kKeep;
    int threadRules = kThreadsOwn;
    int sceneMode = kKeep;                      // 0 off, 1 on.
    std::vector<SceneRowRec> scenes;            // Empty = own stock rows.
    std::vector<ThreadRuleRec> customThreads;   // Used when threadRules == kThreadsCustom.
    int boostLengthMs = kKeep, dropSensitivity = kKeep, hysteresisDeciC = kKeep;
    int dropLowerPermille = -1, dropUpperPermille = -1;
    int skinTargetMilliC = kKeep, powerTargetMw = kKeep;
    std::string variantId, variantName;
    std::optional<ProbeRec> pendingProbe;
};

struct ProfileView {           // Mirrors AIDL PolicyProfile.
    std::string profileKey; int baseMode = kBaseBalanced; bool tuned = false;
    int thermalCase = 0; std::vector<Step> steps;
    int uclampMin = -1, gpuDefaultHz = -1; bool keepCoresOnline = false;
    std::vector<int> frameBoostSteps, jankBoostSteps;
    int startDelayMs = 0, threadRules = kThreadsOwn, threadRuleCount = 0, sceneCount = 0;
    int sceneMode = 0;
    std::vector<SceneRowRec> scenes;
    std::vector<ThreadRuleRec> threads;
    int boostLengthMs = 0, dropSensitivity = -1, hysteresisDeciC = -1;
    int dropLowerPermille = -1, dropUpperPermille = -1;
    int skinTargetMilliC = -1, powerTargetMw = -1;
    std::string variantId, variantName;
};

// Steps of a resolved stock mode, normalized so linearBand() equals band() on the stock mode:
// positive in {3,4} -> bands[0..positive-1]; otherwise bands[0] only. Step 0 temperature = 0.
std::vector<Step> stockSteps(const Mode& mode);

// Fills an empty baseProfile with the own key (Policy::select(package, mode).profile) and checks every
// range of the tuning model; returns false with *error on the first violation.
bool normalize(const Policy&, TuningRecord& record, std::string* error);

// Builds the tuned Mode. selection receives profile = own key and threadPlacement per threadRules.
// A disabled record returns false with error "disabled" (callers use the stock selection).
bool build(const Policy&, const TuningRecord&, Mode* out, Selection* selection, std::string* error);

ProfileView view(const Policy&, const Mode& mode, const std::string& key, int baseMode, bool tuned, int threadRules);

bool validPackage(const std::string& package);

// Not thread-safe; the service guards it with its own mutex.
class TuningStore {
  public:
    static constexpr size_t kMaxTunings = 900, kMaxArmed = 300, kMaxSensitivity = 20;
    explicit TuningStore(std::string dir = "/data/vendor/gamepolicy/tuning");
    void load(const Policy&);                                    // All u<N>.xml; a bad file is renamed to .bad.
    std::optional<TuningRecord> find(int user, const std::string& pkg, int mode) const;
    bool put(int user, const TuningRecord&, std::string* error); // Atomic save; fails on limits.
    bool erase(int user, const std::string& pkg, int mode);
    std::vector<TuningRecord> all(int user) const;
    std::vector<int> users() const;
    bool armed(int user, const std::string& pkg) const;
    bool setArmed(int user, const std::string& pkg, bool armed, std::string* error);
    std::vector<std::string> armedPackages(int user) const;
    bool addSensitivity(int user, const SensitivityEntry&, std::string* error);
    std::vector<SensitivityEntry> sensitivity(int user, const std::string& pkg, int mode) const;
    std::vector<std::string> diagnostics;
  private:
    struct UserData {
        std::map<std::pair<std::string, int>, TuningRecord> tunings;
        std::map<std::pair<std::string, int>, std::pair<std::string, std::string>> originalTunings;
        std::set<std::string> armed;
        std::vector<SensitivityEntry> sensitivity;
    };
    bool save(int user, const UserData& data, std::string* error);
    bool saveSensitivity(int user, const UserData& data, std::string* error);
    std::map<int, UserData> mUsers;
    std::string mDir;
    const Policy* mPolicy = nullptr;
};
} // namespace gamepolicy
