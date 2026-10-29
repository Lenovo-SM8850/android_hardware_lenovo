// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gamepolicy {
using Opps = std::array<std::vector<int>, 3>; // Gold/Prime kHz, GPU Hz (descending).
struct Bounds { int floor = -1; int ceiling = -1; };
struct Band { double temperature; std::array<Bounds, 3> bounds; };
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
};
struct Alias { std::string profile; bool threadPlacement = true; };
struct Overlay {
    std::map<int, std::string> modes;
    std::map<std::string, Alias> aliases;
    std::array<int, 3> ceilings{};
    std::array<std::string, 3> oppPaths;
    std::string sensor;
    double hysteresis = 1;
    int sampleMs = 1000;
};
struct Selection { const Mode* mode; std::string profile; bool threadPlacement; };
struct Decision { int thermalCase = 0; int band = -1; std::vector<int> resources; };
class Policy {
  public:
    Overlay overlay;
    std::map<std::string, std::map<std::string, Mode>> games;
    std::map<std::string, std::vector<int>> sceneLists; // Metadata, never an admission filter.
    std::vector<std::string> diagnostics;
    std::string version;
    bool load(const std::string& stock, const std::string& overrides, std::string* error);
    // Resolve every index once, before any session can request a resource.
    void resolve(const Opps& actual, const std::array<int, 2>& coreCounts = {});
    Selection select(const std::string& package, int mode) const;
    bool aliasesValid(std::string* error) const;
    static size_t band(const Mode& mode, double temperature, int previous, double hysteresis);
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
    Opps mDeclared;
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
    void invalidate() { mCount = mHead = 0; mDip = false; cancel(); }
    void cancel() { mBoostUntil = 0; }
    bool sample(int64_t now, float fps, bool enabled);
    bool active(int64_t now) const { return now < mBoostUntil; }
    int64_t deadline() const { return mBoostUntil; }
    std::array<float, 5> history() const;
  private:
    struct Sample { int64_t time; float fps; };
    std::array<Sample, 16> mSamples{};
    size_t mHead = 0, mCount = 0;
    int64_t mStart = 0, mBoostUntil = 0, mLastBoost = 0;
    bool mDip = false;
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
    bool sample(int64_t now, int appJank, int frames, bool enabled, bool blocked) {
        if (!enabled || blocked || frames <= 0 || appJank <= 0 || appJank > frames ||
            (mLast && now - *mLast < 500)) return false;
        mLast = now; mUntil = now + 100; return true;
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
} // namespace gamepolicy
