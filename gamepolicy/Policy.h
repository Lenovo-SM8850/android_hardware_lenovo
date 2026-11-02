// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
namespace tinyxml2 { class XMLDocument; }

namespace gamepolicy {
bool validDropWindow(int lower, int upper);
bool allowedCase(int code);
using Opps = std::array<std::vector<int>, 3>; // Gold/Prime kHz, GPU Hz (descending).
struct Bounds { int floor = -1; int ceiling = -1; };
// bounds are request-encoded (CPU MHz, GPU table index); units hold the same row as kHz/Hz, -1 = none.
struct Band { double temperature; std::array<Bounds, 3> bounds; std::array<Bounds, 3> units{}; };
struct Scene { int timeMs; Band frequencies; };
struct PerfHint { int opcode; int value; };
struct BindCore { std::string name; uint64_t mask; };
struct ThreadScheduler { std::string name; int priority; bool enabled; };
struct Mode {
    int thermalCase = 0;
    std::vector<Band> bands;
    std::vector<PerfHint> perfHints{};
    std::vector<BindCore> bindCore{};
    std::vector<ThreadScheduler> schedulers{};
    int delayMs = 0;
    std::optional<std::array<int, 3>> fpsAdjust{};
    std::optional<std::array<int, 3>> jankAdjust{};
    int jankAdjustTime = 0;
    std::vector<Band> boostedBands{};
    std::vector<Band> jankBands{};
    std::map<int, Scene> scenes{}; // Profile-level SceneOpt, inherited by every mode.
    bool linearBands = false;      // Tuned modes: general step rule instead of the positive-count branches.
    int boostLengthMs = 0;
    int dropLowerPermille = -1, dropUpperPermille = -1;
    int dropSensitivity = -1;       // Legacy tuning field; the detector uses a fixed trigger.
    int hysteresisDeciC = -1;      // -1 = overlay hysteresis.
    double inheritedHysteresis = -1; // Archived overlay value, retaining sub-deciC precision.
    int skinTargetMilliC = -1;
    int powerTargetMw = -1;
};
struct Alias { std::string profile; bool threadPlacement = true; };
struct Overlay {
    std::map<int, std::string> modes;
    std::map<std::string, Alias> aliases;
    std::array<int, 3> ceilings{};
    std::array<std::string, 3> oppPaths;
    std::string sensor;
    double hysteresis = 1;
    int skinTargetMilliC = -1;
    int sampleMs = 1000;
};
struct Source { std::string key; std::string name; std::vector<std::string> packages; };
struct Selection { const Mode* mode; std::string profile; bool threadPlacement; };
struct Decision { int thermalCase = 0; int band = -1; std::vector<int> resources; };
// Reserved v1 source-key protocol: @builtin/current/<epoch>/<generation> is a catalogue marker;
// @builtin/pending requests a later retry without advancing any app checkpoint.
// getSourceProfile(@builtin/history/<epoch>/<package>/<after-generation>, game-mode) returns
// one matching transition (profileKey = immutable key, variantId = generation,
// variantName = old XML version), or an empty profileKey at end of history.
// @builtin/<epoch>/<generation>/<entry> resolves a complete historical selection, independent
// of the current package, aliases and base-mode mapping. Ordinary sources are unchanged.
struct ArchivedBuiltin {
    std::string key, version, profile;
    uint64_t generation = 0;
    int gameMode = 1;
    bool placement = true;
    std::vector<std::string> packages, excluded;
    Mode mode;
};
class Policy;
struct BuiltinHistory {
    std::mutex mutex;
    uint64_t generation = 0;
    std::string epoch;
    std::map<std::string, ArchivedBuiltin> entries;
    std::string directory;
    int64_t retryAtMs = 0;
    std::shared_ptr<Policy> runtime;
    std::shared_ptr<Policy> checkpoint;
    std::shared_ptr<tinyxml2::XMLDocument> document;
    int64_t fileStamp = -1;
    uint64_t fileSize = 0;
};
class Policy {
  public:
    Overlay overlay;
    std::map<std::string, std::map<std::string, Mode>> games;
    std::map<std::string, std::vector<int>> sceneLists; // Metadata, never an admission filter.
    std::vector<std::string> diagnostics;
    std::string version;
    std::vector<Source> sources;
    uint64_t archiveGeneration() const;
    std::string archiveEpoch() const;
    std::string archiveMarker() const;
    std::shared_ptr<const Policy> archiveRuntime() const;
    // Explicitly isolate simulated/reloaded daemon instances, retaining parsed runtime data.
    void resetArchiveRegistry() { mHistory = std::make_shared<BuiltinHistory>(); }
    bool archiveReady() const;
    // Captures resolved physical values, including all source modes. On failure the
    // durable checkpoint is retained and existing archives remain available for replay.
    bool updateBuiltinArchive(const std::string& dir, std::string* error);
    bool retryBuiltinArchive(std::string* error);
    std::optional<ArchivedBuiltin> archived(const std::string& key) const;
    std::optional<ArchivedBuiltin> history(const std::string& package, int mode, uint64_t after) const;
    bool load(const std::string& stock, const std::string& overrides, std::string* error);
    // Resolve every index once, before any session can request a resource.
    void resolve(const Opps& actual, const std::array<int, 2>& coreCounts = {}, uint64_t present = 0);
    Selection select(const std::string& package, int mode) const;
    bool aliasesValid(std::string* error) const;
    static size_t band(const Mode& mode, double temperature, int previous, double hysteresis);
    static size_t linearBand(const Mode& mode, double temperature, int previous, double hysteresis);
    static int positiveCount(const Mode& mode);
    const Opps& tables() const { return mTables; }
    std::vector<int> ladder(int r) const; // Ascending runtime values at or below the hard ceiling.
    int cores(int r) const { return mCores[r]; }
    std::vector<int> declared(int r) const { return mDeclared[r]; }
    uint64_t presentMask() const { return mPresent; }
    static std::vector<int> resources(const Band& band);
    static std::vector<int> resources(const Mode& mode, int band, bool boosted = false, bool jank = false);
    static bool fpsEnabled(const Mode& mode, const std::string& profile,
                           std::optional<int> scene = {});
    static bool jankEnabled(const Mode& mode, const std::string& profile, std::optional<int> scene = {});
    static bool sceneEnabled(const std::string& profile, std::optional<int> scene);
    static uint64_t mask(const std::string& text);
    static Decision evaluate(const Mode& mode, std::optional<double> temperature,
                             int64_t sampleAgeMs, int previous, double hysteresis);
  private:
    std::shared_ptr<BuiltinHistory> mHistory = std::make_shared<BuiltinHistory>();
    std::shared_ptr<Policy> mUnresolved;
    Opps mDeclared;
    Opps mTables;
    std::array<int, 2> mCores{};
    uint64_t mPresent = 0;
    bool mResolved = false;
};

// Monotonic milliseconds are supplied by the caller, making deadlines testable.
class SessionGate {
  public:
    void start(int64_t now, int delayMs) { mDeadline = now + delayMs; }
    void modeChanged() { mDeadline = 0; }
    bool ready(int64_t now) const { return now >= mDeadline; }
    int64_t deadline() const { return mDeadline; }
  private:
    int64_t mDeadline = 0;
};

// The deadline belongs to the applied request, even if acquisition finishes
// after that deadline. Never discard an expired request's wake-up.
class RequestExpiry {
  public:
    void set(int64_t deadline) { mDeadline = deadline; }
    int64_t limit(int64_t next) const { return mDeadline > 0 && mDeadline < next ? mDeadline : next; }
    bool due(int64_t now) const { return mDeadline > 0 && now >= mDeadline; }
  private:
    int64_t mDeadline = 0;
};

class PerfLease {
  public:
    bool due(int64_t now) const { return now >= mExpires - 1000; }
    void acquired(int64_t now) { mExpires = now + 3000; }
    void reset() { mExpires = 0; }
  private:
    int64_t mExpires = 0;
};

class FpsController {
  public:
    void reset(int64_t now);
    void invalidate() { mCount = mHead = 0; cancel(); }
    void cancel() { mBoostUntil = 0; }
    bool sample(int64_t now, float fps, bool enabled, int boostMs = 0, int sensitivity = -1, int lowerPermille = -1, int upperPermille = -1);
    bool sample(int64_t now, float fps, bool enabled, const Mode& mode) {
        return sample(now, fps, enabled, mode.boostLengthMs, mode.dropSensitivity, mode.dropLowerPermille, mode.dropUpperPermille);
    }
    bool active(int64_t now) const { return now < mBoostUntil; }
    int64_t deadline() const { return mBoostUntil; }
    std::array<float, 5> history() const;
  private:
    struct Sample { int64_t time; float fps; };
    std::array<Sample, 5> mSamples{};
    size_t mHead = 0, mCount = 0;
    int64_t mStart = 0, mBoostUntil = 0;
};

class SceneController {
  public:
    bool report(int64_t now, int scene, const Mode& mode);
    void cancel() { mRow = nullptr; mDeadline = 0; }
    void reset() { cancel(); mScene.reset(); }
    std::optional<int> scene() const { return mScene; }
    bool active(int64_t now) const { return mRow && now < mDeadline; }
    int64_t deadline() const { return mDeadline; }
    // One request only: scene > FPS > jank > normal. Mode hints always apply.
    std::vector<int> resources(int64_t now, const Mode& mode, int band, bool boosted, bool jank = false) const;
  private:
    std::optional<int> mScene;
    const Scene* mRow = nullptr;
    int64_t mDeadline = 0;
};

class JankController {
  public:
    bool sample(int64_t now, int appJank, int frames, bool enabled, bool blocked, int boostMs = 0) {
        if (!enabled || blocked || boostMs <= 0 || frames <= 0 || appJank <= 0 || appJank > frames ||
            (mLast && now - *mLast < 500)) return false;
        mLast = now; mUntil = now + boostMs; return true;
    }
    void cancel() { mUntil = 0; }
    void reset() { cancel(); mLast.reset(); }
    bool active(int64_t now) const { return now < mUntil; }
    int64_t deadline() const { return mUntil; }
  private:
    std::optional<int64_t> mLast;
    int64_t mUntil = 0;
};
// related_cpus is a whitespace-separated list, independent of online CPU state.
int coreCount(const std::string& relatedCpus);
// Bit mask of the CPUs in a related_cpus list; 0 when malformed.
uint64_t cpuMask(const std::string& relatedCpus);
} // namespace gamepolicy
