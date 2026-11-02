// SPDX-License-Identifier: Apache-2.0
#include "Analysis.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace gamepolicy {
namespace {
constexpr std::array<int, 8> kTargets{30, 40, 45, 60, 90, 120, 144, 165};
constexpr std::array<double, 3> kWeights{1.0, 0.5, 1.0};
constexpr double kUnknownSensitivity = 1.0;   // FPS per watt assumed for a resource without history.
constexpr double kPositiveFloorGain = 2.0;    // FPS per step; below this a probe counts as no sensitivity.
constexpr int kResourceOrder[3] = {2, 0, 1};  // GPU, gold, prime.
// Initial classifier thresholds; kept together so device validation can tune one policy.
constexpr int kCauseWindow = 30, kCapSeconds = 60, kMaxGapMs = 2000;
constexpr double kSignalCoverage = .90, kPlateauTolerance = .05, kCadenceAgreement = .03;
constexpr double kShortfallRatio = .90, kFloorRatio = .95, kMinimumRatio = .90;
constexpr double kPacingBudget = 1.5, kSlowBudget = 1.2, kSustained = .70, kDominant = .80;
constexpr int kHeadroomGpu = 70, kHeadroomCpu = 650, kBusyGpu = 90, kBusyCpu = 900;
constexpr double kPeggedRatio = .97, kReductionFloor = .98, kReductionTail = .5;

int topOf(const std::vector<int>& ladder) { return ladder.empty() ? 0 : ladder.back(); }

// Largest index whose value is at or below the request, 0 when none.
int indexAtOrBelow(const std::vector<int>& ladder, int value) {
    const auto it = std::upper_bound(ladder.begin(), ladder.end(), value);
    return it == ladder.begin() ? 0 : static_cast<int>(it - ladder.begin()) - 1;
}
int shiftValue(const std::vector<int>& ladder, int value, int steps) {
    if (ladder.empty()) return 0;
    const int top = static_cast<int>(ladder.size()) - 1;
    return ladder[std::clamp(indexAtOrBelow(ladder, value) + steps, 0, top)];
}
int atOrAbove(const std::vector<int>& ladder, double x) {
    for (int v : ladder) if (v >= x) return v;
    return topOf(ladder);
}

// Index floor(p/100*(n-1)) on ascending data, computed in integers.
template <typename T> T percentile(const std::vector<T>& sorted, int p) {
    return sorted[(static_cast<size_t>(p) * (sorted.size() - 1)) / 100];
}

std::vector<const Sample*> validSamples(const std::vector<Sample>& samples) {
    std::vector<const Sample*> out;
    for (const auto& s : samples) if (valid(s)) out.push_back(&s);
    return out;
}

bool measuredHeadroom(const std::vector<const Sample*>& samples) {
    if (samples.empty()) return false;
    std::array<std::vector<int>, 3> sensors;
    int bands = 0, pegged = 0;
    for (const auto* p : samples) {
        if (p->flags & kFlagSensorFailed) return false;
        if (p->gpuBusyPercent >= 0) sensors[2].push_back(p->gpuBusyPercent);
        for (int r = 0; r < 2; ++r) if (p->cpuUtilPermille[r] >= 0) sensors[r].push_back(p->cpuUtilPermille[r]);
        if (p->band > 0) return false;
        bands += p->band == 0;
        bool atCap = false;
        for (int r = 0; r < 3; ++r) atCap |= p->cap[r] > 0 && p->cur[r] >= kPeggedRatio * p->cap[r];
        pegged += atCap;
    }
    if (bands < kSignalCoverage * samples.size() || pegged >= kSustained * samples.size()) return false;
    for (int r = 0; r < 3; ++r) {
        auto& values = sensors[r];
        if (values.size() < kSignalCoverage * samples.size()) return false;
        std::sort(values.begin(), values.end());
        if (percentile(values, 95) > (r == 2 ? kHeadroomGpu : kHeadroomCpu)) return false;
    }
    return true;
}

bool measuredSaturation(const std::vector<const Sample*>& samples) {
    for (int r = 0; r < 3; ++r) {
        int known = 0, busy = 0;
        for (const auto* p : samples) {
            const int value = r == 2 ? p->gpuBusyPercent : p->cpuUtilPermille[r];
            known += value >= 0; busy += value >= (r == 2 ? kBusyGpu : kBusyCpu);
        }
        if (known >= kSignalCoverage * samples.size() && busy >= kSustained * samples.size()) return true;
    }
    return false;
}

constexpr double kSnapTolerance = 0.03;       // Signal to common target distance, as a fraction of the signal.
constexpr float kMaxSignalFps = 1000;
constexpr int kWarmupSamples = 2;             // Samples left out after a target change.

bool usableSignal(float fps) { return std::isfinite(fps) && fps > 0 && fps <= kMaxSignalFps; }

// Today's detector: the lowest common target at or above 97 % of the session P95.
int detectedTarget(const std::vector<const Sample*>& v) {
    if (v.empty()) return -1;
    std::vector<float> fps;
    for (const auto* p : v) fps.push_back(p->fps);
    std::sort(fps.begin(), fps.end());
    const double p95 = percentile(fps, 95);
    for (int t : kTargets) if (t >= 0.97 * p95) return t;
    return 165;
}

int sampleSignal(const Sample& s) { return signalTarget(s.requestedFps, s.throttleFps); }
// The detector for samples without a signal, run on those samples only so signalled parts do not skew it.
int fallbackTarget(const std::vector<const Sample*>& v) {
    std::vector<const Sample*> unsignalled;
    for (const auto* p : v) if (sampleSignal(*p) <= 0) unsignalled.push_back(p);
    return detectedTarget(unsignalled.empty() ? v : unsignalled);
}

struct Judged { const Sample* sample; int target; bool counted; };   // counted: outside the warm-up after a change.
// Per valid sample, in order: its own target, and whether it counts for floor, jank and tail figures.
std::vector<Judged> judge(const std::vector<const Sample*>& v) {
    const int fallback = fallbackTarget(v);
    std::vector<Judged> out;
    int previous = 0, skip = 0;
    for (const auto* p : v) {
        const int signal = signalTarget(p->requestedFps, p->throttleFps);
        const int target = signal > 0 ? signal : fallback;
        // The change happens inside this sample's second, so it and the one after it are left out.
        if (!out.empty() && target != previous) skip = kWarmupSamples;
        out.push_back({p, target, skip == 0});
        if (skip > 0) --skip;
        previous = target;
    }
    return out;
}

int clampGoal(int goal) { return goal < kGoalFrames || goal > kGoalEfficiency ? kGoalBalanced : goal; }

int skinTarget(const ProfileView& recorded, int overlayDefault) {
    if (recorded.skinTargetMilliC == -1) return -1;
    if (recorded.skinTargetMilliC > 0) return recorded.skinTargetMilliC;
    // No target unless the profile or the overlay sets one.
    return overlayDefault > 0 ? overlayDefault : -1;
}

// General step rule without hysteresis.
size_t selectedStep(const std::vector<Step>& steps, double temperatureC) {
    size_t selected = 0;
    for (size_t i = 1; i < steps.size(); ++i) if (temperatureC >= steps[i].temperature) selected = i;
    return selected;
}
int capAt(const ProfileView& profile, int r, double temperatureC, const std::vector<int>& ladder) {
    if (profile.steps.empty()) return topOf(ladder);
    const int value = profile.steps[selectedStep(profile.steps, temperatureC)].units[r].ceiling;
    return value < 0 ? topOf(ladder) : value;
}
int floorAt(const ProfileView& profile, int r, double temperatureC) {
    if (profile.steps.empty()) return 0;
    const int value = profile.steps[selectedStep(profile.steps, temperatureC)].units[r].floor;
    return value < 0 ? 0 : value;
}

bool sameSteps(const std::vector<Step>& a, const std::vector<Step>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i].temperature - b[i].temperature) > 1e-9) return false;
        for (int r = 0; r < 3; ++r)
            if (a[i].units[r].floor != b[i].units[r].floor || a[i].units[r].ceiling != b[i].units[r].ceiling) return false;
    }
    return true;
}
bool sameConfigurationFields(const ProfileView& a, const ProfileView& b) {
    if (a.profileKey != b.profileKey || a.baseMode != b.baseMode || a.tuned != b.tuned || a.variantId != b.variantId ||
        a.thermalCase != b.thermalCase || !sameSteps(a.steps, b.steps) || a.uclampMin != b.uclampMin ||
        a.gpuDefaultHz != b.gpuDefaultHz || a.keepCoresOnline != b.keepCoresOnline ||
        a.frameBoostSteps != b.frameBoostSteps || a.jankBoostSteps != b.jankBoostSteps ||
        a.startDelayMs != b.startDelayMs || a.threadRules != b.threadRules || a.threadRuleCount != b.threadRuleCount ||
        a.sceneCount != b.sceneCount || a.sceneMode != b.sceneMode || a.boostLengthMs != b.boostLengthMs ||
        a.dropSensitivity != b.dropSensitivity || a.dropLowerPermille != b.dropLowerPermille || a.dropUpperPermille != b.dropUpperPermille || a.hysteresisDeciC != b.hysteresisDeciC ||
        a.skinTargetMilliC != b.skinTargetMilliC || a.powerTargetMw != b.powerTargetMw ||
        a.scenes.size() != b.scenes.size() || a.threads.size() != b.threads.size()) return false;
    for (size_t i = 0; i < a.scenes.size(); ++i) {
        const auto& x = a.scenes[i]; const auto& y = b.scenes[i];
        if (x.sceneId != y.sceneId || x.enabled != y.enabled || x.holdMs != y.holdMs) return false;
        for (int r = 0; r < 3; ++r)
            if (x.units[r].floor != y.units[r].floor || x.units[r].ceiling != y.units[r].ceiling) return false;
    }
    for (size_t i = 0; i < a.threads.size(); ++i) {
        const auto& x = a.threads[i]; const auto& y = b.threads[i];
        if (x.name != y.name || x.mask != y.mask || x.rtPriority != y.rtPriority) return false;
    }
    return true;
}
} // namespace

bool sameConfiguration(const ProfileView& a, const ProfileView& b) {
    // Old records without scene/thread details cannot establish equality for those configurations.
    if ((a.sceneCount > 0 && a.scenes.empty()) || (a.threadRuleCount > 0 && a.threads.empty())) return false;
    return sameConfigurationFields(a, b);
}

int nextProfileGeneration(int generation, int previousMode, const ProfileView& previous,
                          int mode, const ProfileView& next) {
    return generation == 0 || previousMode != mode || !sameConfigurationFields(previous, next) ? generation + 1 : generation;
}

void cacheProposal(std::map<ProposalKey, PendingProposal>& proposals, int userId, TuningRecord tuning, ProbeRec probe) {
    const ProposalKey key{userId, tuning.package, tuning.mode};
    proposals[key] = {std::move(tuning), std::move(probe)};
}

bool hasTemperature(int milliC) { return milliC > kTemperatureUnavailable; }

bool valid(const Sample& s) { return (s.flags & kFlagReady) && s.fps >= 1 && s.timeMs >= 10000; }

int signalTarget(float requestedFps, float throttleFps) {
    double c = 0;
    for (float x : {requestedFps, throttleFps}) if (usableSignal(x) && (c == 0 || x < c)) c = x;
    if (c <= 0) return -1;
    int best = 0;
    for (int t : kTargets)
        if (std::fabs(t - c) <= kSnapTolerance * c && (best == 0 || std::fabs(t - c) < std::fabs(best - c))) best = t;
    return best ? best : std::max(1, static_cast<int>(std::lround(c)));
}

std::vector<int> sampleTargets(const std::vector<Sample>& samples) {
    const int fallback = fallbackTarget(validSamples(samples));
    std::vector<int> out;
    out.reserve(samples.size());
    for (const auto& s : samples) {
        const int signal = signalTarget(s.requestedFps, s.throttleFps);
        out.push_back(signal > 0 ? signal : fallback);
    }
    return out;
}

float steadyRatio(const Summary& s) {
    if (s.floorRatio >= 0) return s.floorRatio;
    return s.targetFps > 0 ? static_cast<float>(static_cast<double>(s.floor5Fps) / s.targetFps) : 0.f;
}

Cause classify(const std::vector<Sample>& samples) {
    Cause out;
    const auto judged = judge(validSamples(samples));
    std::vector<const Sample*> segment;
    std::vector<std::pair<int, int>> caps; // Cadence and usable seconds of qualifying windows.
    std::vector<float> observedCadences; // Includes successful higher-target windows.
    int allWindowSamples = 0;
    int likely = 0, capped = 0, performance = 0, total = 0;
    auto window = [&]() {
        if (segment.size() < kCauseWindow) { segment.clear(); return; }
        const int n = static_cast<int>(segment.size());
        allWindowSamples += n;
        std::vector<float> fps, minimum, timing, requests;
        int desired = segment.front()->desiredTargetFps;
        std::vector<float> throttles;
        int thermal = 0, pegged = 0, throttled = 0;
        for (const auto* s : segment) {
            fps.push_back(s->fps);
            if (s->fpsMin >= 0) minimum.push_back(s->fpsMin);
            if (s->maxFrameTimeUs >= 0) timing.push_back(s->maxFrameTimeUs / 1000.f);
            if (usableSignal(s->requestedFps)) requests.push_back(s->requestedFps);
            if (usableSignal(s->throttleFps)) throttles.push_back(s->throttleFps);
            bool peg = false;
            for (int r = 0; r < 3; ++r) peg |= s->cap[r] > 0 && s->cur[r] >= kPeggedRatio * s->cap[r];
            pegged += peg;
            thermal += s->band > 0;

        }
        for (auto* values : {&fps, &minimum, &timing, &requests, &throttles}) std::sort(values->begin(), values->end());
        observedCadences.push_back(percentile(fps, 50));
        if (desired <= 0) { out.counts[kUnclear] += n; total += n; segment.clear(); return; }
        out.desiredTargetFps = std::max(out.desiredTargetFps, desired);
        const bool shortfall = percentile(fps, 50) < kShortfallRatio * desired;
        if (!shortfall) { segment.clear(); return; }
        total += n;
        int cadence = signalTarget(percentile(fps, 50), -1);
        const bool request = requests.size() >= kSignalCoverage * n;
        if (request) cadence = signalTarget(percentile(requests, 50), -1);
        const bool lowerThrottle = throttles.size() >= kSignalCoverage * n && percentile(throttles, 50) <= cadence * (1 + kCadenceAgreement);
        if (lowerThrottle) cadence = signalTarget(percentile(throttles, 50), -1);
        int near = 0, voted = 0;
        for (const auto* s : segment) {
            near += std::fabs(s->fps - cadence) <= kPlateauTolerance * cadence;
            voted += usableSignal(s->requestedFps) && std::fabs(s->requestedFps - cadence) <= kCadenceAgreement * cadence;
            throttled += usableSignal(s->throttleFps) && s->throttleFps <= cadence * (1 + kCadenceAgreement);
        }
        const bool direct = segment.front()->selectedGameplay && request && voted >= kSignalCoverage * n && cadence <= kShortfallRatio * desired &&
                            !lowerThrottle && throttled < kSignalCoverage * n;
        const bool paced = near >= kSignalCoverage * n && minimum.size() >= kSignalCoverage * n &&
            timing.size() >= kSignalCoverage * n && percentile(fps, 5) >= kFloorRatio * cadence &&
            percentile(minimum, 5) >= kMinimumRatio * cadence && percentile(timing, 95) <= kPacingBudget * 1000 / cadence;
        int label = kUnclear;
        if (direct) {
            out.evidence |= kAppCadence | kGameplayRange;
            if (paced) { label = kGameCap; capped += n; out.evidence |= kPlateau; }
        }
        if (!direct && !request && !lowerThrottle && paced && segment.front()->selectedGameplay &&
            measuredHeadroom(segment) && pegged < kSustained * n && thermal == 0) {
            label = kLikelyGameCap; likely += n;
            out.evidence |= kPlateau | kHeadroom | kGameplayRange;
        }
        if (measuredSaturation(segment)) {
            // Residual shortfall is measured against the app vote, otherwise the captured desired target.
            int deficit = 0;
            const int target = direct ? cadence : desired;
            for (const auto* s : segment)
                deficit += s->fps < kShortfallRatio * target && s->maxFrameTimeUs > kSlowBudget * 1000000 / target;
            if (deficit >= kSustained * n) {
                label = thermal >= kSustained * n ? kThermalLimit : kPerformanceLimited;
                performance += n; out.evidence |= kSaturation;
                if (label == kThermalLimit) out.evidence |= kThermalEvidence;
            }
        }
        if (label == kUnclear && throttled >= kSignalCoverage * n && paced) {
            label = kFpsLimited; out.evidence |= kThrottleCeiling;
        }
        // An app vote alone is intent, not actionable evidence of a cap. Residual deficits need saturation.
        const bool supportedDirect = direct && (label == kGameCap || label == kPerformanceLimited || label == kThermalLimit);
        if (supportedDirect || label == kLikelyGameCap) {
            caps.emplace_back(cadence, n);
            out.segments.push_back({segment.front()->timeMs, segment.back()->timeMs, cadence, supportedDirect});
        }
        out.counts[label] += n;
        segment.clear();
    };
    const Sample* previous = nullptr;
    int transition = 0;
    for (const auto& j : judged) {
        const Sample* s = j.sample;
        if (previous && (s->timeMs - previous->timeMs > kMaxGapMs || s->timeMs <= previous->timeMs ||
            s->sceneId != previous->sceneId || s->mode != previous->mode || s->profileGeneration != previous->profileGeneration ||
            s->desiredTargetFps != previous->desiredTargetFps || s->renderScale != previous->renderScale ||
            s->refreshRate != previous->refreshRate || sampleSignal(*s) != sampleSignal(*previous))) {
            window(); transition = kWarmupSamples;
        }
        previous = s;
        if (transition > 0) { --transition; continue; }
        if (!j.counted) continue;
        segment.push_back(s);
        if (segment.size() == kCauseWindow) window();
    }
    window();
    out.shortfallSamples = total;
    int best = kUnclear;
    for (int i = 1; i <= kMixedLimits; ++i) if (out.counts[i] > out.counts[best]) best = i;
    out.classifiedSamples = total - out.counts[kUnclear];
    out.coverage = total ? static_cast<float>(out.counts[best]) / total : 0;
    if (total && out.coverage >= kDominant) out.diagnosis = best;
    if (performance >= (1 - kDominant) * total && capped + likely >= (1 - kDominant) * total && performance && capped + likely)
        out.diagnosis = kMixedLimits;
    int seconds = 0, n = 0;
    bool agree = true;
    for (const auto& c : caps) {
        if (!n) n = c.first;
        agree &= std::abs(c.first - n) <= kCadenceAgreement * n;
        seconds += c.second;
    }
    // Any sustained faster cadence disproves a single global ceiling, even when it meets the desired target.
    const bool higherCadence = n > 0 && std::any_of(observedCadences.begin(), observedCadences.end(),
        [&](float fps) { return fps > n * (1 + kPlateauTolerance); });
    if (agree && !higherCadence && seconds >= kCapSeconds && seconds >= kDominant * allWindowSamples) {
        out.ceilingFps = n;
        out.confirmed = (out.evidence & kAppCadence) && likely == 0;
        if (out.confirmed) out.analysisTargetFps = n;
    } else if (out.diagnosis == kGameCap || out.diagnosis == kLikelyGameCap) {
        out.diagnosis = kUnclear;
    }
    return out;
}

Summary tuningSummary(const std::vector<Sample>& samples, const Ladders& ladders) {
    const Cause cause = classify(samples);
    auto adjusted = samples;
    for (auto& s : adjusted) {
        const auto segment = std::find_if(cause.segments.begin(), cause.segments.end(), [&](const CapSegment& c) {
            return c.confirmed && s.timeMs >= c.startMs && s.timeMs <= c.endMs;
        });
        if (segment != cause.segments.end()) {
            s.requestedFps = static_cast<float>(segment->targetFps); s.throttleFps = -1;
        } else if (cause.counts[kLikelyGameCap] > 0 && sampleSignal(s) <= 0 && s.desiredTargetFps > 0) {
            // Achieved cadence inferred without a signal must not replace the captured desired target.
            s.requestedFps = static_cast<float>(s.desiredTargetFps);
        }
    }
    Summary s = summarize(adjusted, ladders);
    s.cause = cause;
    return s;
}

Summary summarize(const std::vector<Sample>& samples, const Ladders& ladders) {
    Summary s;
    std::set<int> scenes;
    for (const auto& sample : samples) if (sample.sceneId >= 0) scenes.insert(sample.sceneId);
    s.scenesSeen.assign(scenes.begin(), scenes.end());
    const auto v = validSamples(samples);
    const size_t n = v.size();
    s.validSamples = static_cast<int>(n);
    if (n == 0) return s;

    std::vector<float> fps;
    double fpsSum = 0;
    for (const auto* p : v) { fps.push_back(p->fps); fpsSum += p->fps; }
    std::sort(fps.begin(), fps.end());
    s.avgFps = static_cast<float>(fpsSum / n);
    s.medianFps = percentile(fps, 50);
    s.lowFps1 = percentile(fps, 1);
    s.lowFps5 = percentile(fps, 5);
    // Every sample is judged against its own target; the summary target is the one held longest (ties: higher).
    const auto judged = judge(v);
    std::map<int, int> held;
    for (const auto& j : judged) ++held[j.target];
    s.targetFps = 0;
    int heldMost = 0;
    for (const auto& [target, count] : held) if (count >= heldMost) { s.targetFps = target; heldMost = count; }

    // Steady floor: Q05 of fps at the main target; ratio: Q05 of fps over target at all targets. Warm-up samples
    // are left out of both, floor, jank and tail alike.
    std::vector<float> steadyFps, anyFps;
    std::vector<double> ratios, anyRatios;
    for (const auto& j : judged) {
        const double ratio = static_cast<double>(j.sample->fps) / j.target;
        if (j.target == s.targetFps) anyFps.push_back(j.sample->fps);
        anyRatios.push_back(ratio);
        if (!j.counted) continue;
        if (j.target == s.targetFps) steadyFps.push_back(j.sample->fps);
        ratios.push_back(ratio);
    }
    if (steadyFps.empty()) steadyFps = anyFps;
    if (ratios.empty()) ratios = anyRatios;
    std::sort(steadyFps.begin(), steadyFps.end());
    std::sort(ratios.begin(), ratios.end());
    s.floor5Fps = percentile(steadyFps, 5);
    s.floorRatio = static_cast<float>(percentile(ratios, 5));

    size_t hit = 0, steady = 0;
    long long jank = 0, frames = 0, budgetFrames = 0;
    for (const auto& j : judged) {
        const Sample* p = j.sample;
        if (p->fps >= 0.9 * j.target) ++hit;
        if (std::fabs(p->fps - s.medianFps) <= 0.1 * s.medianFps) ++steady;
        if (!j.counted) continue;
        jank += p->jankFrames; frames += p->frames; budgetFrames += j.target;
    }
    s.hitRate = static_cast<float>(static_cast<double>(hit) / n);
    s.stability = static_cast<float>(static_cast<double>(steady) / n);
    s.jankPercent = frames > 0 ? static_cast<float>(100.0 * jank / frames) : -1.f;
    s.jankPer1000 = static_cast<float>(1000.0 * jank / static_cast<double>(budgetFrames));

    // Frame time tail: window maximum over the sample's own frame budget, Q95 of max(0, ratio - 1).
    std::vector<double> tail;
    for (const auto& j : judged)
        if (j.counted && j.sample->maxFrameTimeUs >= 0)
            tail.push_back(std::max(0.0, j.sample->maxFrameTimeUs / 1000.0 / (1000.0 / j.target) - 1));
    if (!tail.empty()) {
        std::sort(tail.begin(), tail.end());
        s.tail95 = static_cast<float>(percentile(tail, 95));
    }

    auto temperature = [&](int Sample::*field, float* average, float* maximum) {
        double sum = 0; int count = 0; int peak = 0; bool any = false;
        for (const auto* p : v) {
            const int t = p->*field;
            if (!hasTemperature(t)) continue;
            sum += t; ++count; peak = any ? std::max(peak, t) : t; any = true;
        }
        if (average) *average = count ? static_cast<float>(sum / count / 1000.0) : -1.f;
        if (maximum) *maximum = any ? static_cast<float>(peak / 1000.0) : -1.f;
    };
    temperature(&Sample::skinMilliC, &s.avgSkinC, &s.maxSkinC);
    temperature(&Sample::cpuMilliC, nullptr, &s.maxCpuC);
    temperature(&Sample::gpuMilliC, nullptr, &s.maxGpuC);

    double power = 0; int powerCount = 0;
    size_t throttled = 0;
    std::array<double, 3> curSum{}; std::array<int, 3> curCount{}; std::array<size_t, 3> pegged{};
    for (const auto* p : v) {
        if (p->powerMw >= 0) { power += p->powerMw; ++powerCount; }
        if (p->band > 0) ++throttled;
        for (int r = 0; r < 3; ++r) {
            if (p->cur[r] > 0) { curSum[r] += p->cur[r]; ++curCount[r]; }
            const int cap = p->cap[r] < 0 ? topOf(ladders[r]) : p->cap[r];
            if (cap > 0 && p->cur[r] >= 0.97 * cap) ++pegged[r];
        }
    }
    s.avgPowerMw = powerCount ? static_cast<int>(std::lround(power / powerCount)) : -1;
    for (int r = 0; r < 3; ++r) {
        s.avgCur[r] = curCount[r] ? static_cast<int>(std::lround(curSum[r] / curCount[r])) : 0;
        s.pegged[r] = static_cast<float>(static_cast<double>(pegged[r]) / n);
    }
    s.throttled = static_cast<float>(static_cast<double>(throttled) / n);
    for (const auto* p : v) if (p->mode != v[0]->mode || p->profileGeneration != v[0]->profileGeneration) { s.mixed = true; break; }
    int capturedTarget = -1; float capturedScale = -1, capturedRefresh = -1;
    for (const auto* p : v) {
        if (p->desiredTargetFps > 0) {
            if (capturedTarget > 0 && capturedTarget != p->desiredTargetFps) s.mixed = true;
            capturedTarget = p->desiredTargetFps;
        }
        if (p->renderScale > 0) {
            if (capturedScale > 0 && capturedScale != p->renderScale) s.mixed = true;
            capturedScale = p->renderScale;
        }
        if (p->refreshRate > 0) {
            if (capturedRefresh > 0 && capturedRefresh != p->refreshRate) s.mixed = true;
            capturedRefresh = p->refreshRate;
        }
    }
    s.renderScale = capturedScale; s.refreshRate = capturedRefresh;
    s.cause = classify(samples);
    return s;
}

Wanted wanted(const std::vector<Sample>& samples, const Summary& raw, const ProfileView& recorded,
              const Ladders& ladders, int goalIn) {
    const Summary s = raw.cause.segments.empty() ? raw : tuningSummary(samples, ladders);
    const int goal = clampGoal(goalIn);
    Wanted w;
    const auto v = validSamples(samples);
    const double H = s.hitRate;
    const bool inferred = s.cause.counts[kLikelyGameCap] > 0;
    const bool hasCap = !s.cause.segments.empty();
    const bool safeCapReduction = steadyRatio(s) >= kReductionFloor && s.tail95 >= 0 && s.tail95 <= kReductionTail &&
        measuredHeadroom(v);
    for (int r = 0; r < 3; ++r) {
        const auto& ladder = ladders[r];
        w.want[r] = 0;
        if (ladder.empty() || v.empty()) continue;
        std::vector<int> caps, curs;
        for (const auto* p : v) { caps.push_back(p->cap[r] < 0 ? topOf(ladder) : p->cap[r]); curs.push_back(p->cur[r]); }
        std::sort(caps.begin(), caps.end());
        std::sort(curs.begin(), curs.end());
        const int cap = percentile(caps, 50);
        const int p95cur = percentile(curs, 95);
        const double P = s.pegged[r];
        int target = cap, want = 0;
        if (H < 0.90 && P >= 0.30) { target = shiftValue(ladder, cap, 3); want = 1; }
        else if (H >= 0.97 && P < 0.10) {
            target = atOrAbove(ladder, p95cur * (goal == kGoalEfficiency ? 1.05 : 1.15));
            if (target < cap) want = -1; else target = cap;
        }
        if (goal == kGoalFrames) {
            if (H < 0.97 && P >= 0.15 && want != 1) { target = shiftValue(ladder, cap, 2); want = 1; }
            if (want == -1 && !(H >= 0.99 && P < 0.05)) { target = cap; want = 0; }
        }
        w.target[r] = target; w.cap[r] = cap; w.want[r] = want;
        if ((inferred && w.want[r] != 0) ||
            (hasCap && w.want[r] < 0 && !safeCapReduction)) { w.want[r] = 0; w.target[r] = cap; }
    }

    std::vector<int> skins;
    for (const auto* p : v) if (hasTemperature(p->skinMilliC)) skins.push_back(p->skinMilliC);
    if (!skins.empty()) { std::sort(skins.begin(), skins.end()); w.medianSkinC = static_cast<float>(percentile(skins, 50) / 1000.0); }

    const bool more = w.want[0] == 1 || w.want[1] == 1 || w.want[2] == 1;
    const bool less = w.want[0] == -1 || w.want[1] == -1 || w.want[2] == -1;
    if (H >= 0.97 && !inferred) w.reasons |= kTargetMet;
    if (H < 0.90) w.reasons |= kFpsShort;
    if (w.want[0] == 1 || w.want[1] == 1) w.reasons |= kCpuBound;
    if (w.want[2] == 1) w.reasons |= kGpuBound;
    const int skinLimit = skinTarget(recorded, -1);
    if (H < 0.90 && !more && ((skinLimit > 0 && s.maxSkinC >= skinLimit / 1000.0 - 1) || s.throttled >= 0.30))
        w.reasons |= kThermalLimited;
    if (less) w.reasons |= kOverprovisioned;
    if (skinLimit > 0 && s.maxSkinC * 1000.0 >= skinLimit) w.reasons |= kHot;
    if (s.avgPowerMw == -1) w.reasons |= kNoPower;

    const int current = recorded.thermalCase;
    w.thermalCase = current;
    if (goal == kGoalFrames && (w.reasons & kThermalLimited) && !inferred)
        w.thermalCase = std::max(current, std::min(current + 100, 300));
    else if (goal == kGoalEfficiency && (w.reasons & kHot)) w.thermalCase = std::max(current - 100, 100);
    return w;
}

std::vector<Ranked> recommend(const std::vector<Sample>& samples, const ProfileView& recorded,
                              const std::vector<Candidate>& candidates, const Ladders& ladders, int goalIn,
                              int* reasons, int* error, const std::vector<RecordEvidence>& history) {
    if (error) *error = kOk;
    if (reasons) *reasons = 0;
    const int goal = clampGoal(goalIn);
    const auto v = validSamples(samples);
    if (v.size() < static_cast<size_t>(kMinValidSamples)) { if (error) *error = kNotEnoughData; return {}; }
    const Summary s = tuningSummary(samples, ladders);
    if (s.mixed) { if (error) *error = kMixed; return {}; }
    const Wanted w = wanted(samples, s, recorded, ladders, goal);
    if (reasons) *reasons = w.reasons;

    std::vector<double> skins;
    for (const auto* p : v) skins.push_back(hasTemperature(p->skinMilliC) ? p->skinMilliC / 1000.0 : w.medianSkinC);
    const double medianSkin = w.medianSkinC;
    const int skinLimitMilliC = skinTarget(recorded, -1);
    std::set<std::string> games;
    for (const auto& h : history)
        if (!h.package.empty() && h.summary.validSamples >= kMinValidSamples && !h.summary.mixed) games.insert(h.package);
    // Cost quadrant uses mean frequency / hardware maximum, with 50 % separating high and low cost.
    // Distribution distance uses target-normalized frame-time Q50/Q95/Q99 and the measured timing tail.
    auto distance = [&](const Summary& e) {
        auto cost = [&](const Summary& x, int r) {
            return topOf(ladders[r]) > 0 ? static_cast<double>(x.avgCur[r]) / topOf(ladders[r]) : 0.0;
        };
        const bool cpu = std::max(cost(s, 0), cost(s, 1)) >= 0.5;
        const bool gpu = cost(s, 2) >= 0.5;
        double d = (cpu != (std::max(cost(e, 0), cost(e, 1)) >= 0.5)) + (gpu != (cost(e, 2) >= 0.5));
        d += std::abs(s.targetFps - e.targetFps) / static_cast<double>(std::max({1, s.targetFps, e.targetFps}));
        for (auto field : {&Summary::medianFps, &Summary::lowFps5, &Summary::lowFps1}) {
            const double x = s.targetFps / std::max(1.f, s.*field), y = e.targetFps / std::max(1.f, e.*field);
            d += std::min(1.0, std::abs(x - y));
        }
        d += s.tail95 >= 0 && e.tail95 >= 0 ? std::min(1.f, std::abs(s.tail95 - e.tail95))
                                                         : (s.tail95 >= 0) != (e.tail95 >= 0);
        return d;
    };

    std::array<int, 3> capIndex{};
    for (int r = 0; r < 3; ++r) capIndex[r] = indexAtOrBelow(ladders[r], w.cap[r]);

    std::vector<Ranked> ranked;
    for (const auto& c : candidates) {
        if (sameSteps(c.profile.steps, recorded.steps) && c.profile.thermalCase == recorded.thermalCase && !s.cause.confirmed) continue;
        Ranked item;
        item.candidate = c;
        item.candidate.hasExpected = false;
        double nearest = 8;
        if (games.size() >= 3) for (const auto& h : history) {
            if (h.summary.validSamples < kMinValidSamples || h.summary.mixed || h.summary.targetFps <= 0) continue;
            if (!c.tunedPackage.empty() && (h.package != c.tunedPackage || h.mode != c.tunedMode)) continue;
            if (!sameConfiguration(h.profile, c.profile)) continue;
            const double d = distance(h.summary);
            if (!item.candidate.hasExpected || d < nearest) {
                nearest = d; item.candidate.hasExpected = true; item.candidate.expected = h.summary;
            }
        }
        for (int r = 0; r < 3; ++r) {
            const int index = indexAtOrBelow(ladders[r], capAt(c.profile, r, medianSkin, ladders[r]));
            item.directions[r] = (index > capIndex[r]) - (index < capIndex[r]);
        }
        // Eligibility is a hard gate before either measured or heuristic ranking.
        const bool inferred = s.cause.counts[kLikelyGameCap] > 0;
        const bool capAware = !s.cause.segments.empty();
        const bool safetyReduction = (skinLimitMilliC > 0 && s.maxSkinC * 1000 >= skinLimitMilliC) ||
            (recorded.powerTargetMw > 0 && s.avgPowerMw > recorded.powerTargetMw);
        bool unsupported = false;
        if (capAware) {
            // Check every breakpoint as well as observed temperatures, rather than only the median row.
            auto temperatures = skins;
            for (const auto& step : recorded.steps) temperatures.push_back(step.temperature);
            for (const auto& step : c.profile.steps) temperatures.push_back(step.temperature);
            for (double temperature : temperatures) for (int r = 0; r < 3; ++r) {
                const int before = capAt(recorded, r, temperature, ladders[r]);
                const int after = capAt(c.profile, r, temperature, ladders[r]);
                unsupported |= after > before && (inferred || w.want[r] <= 0);
                unsupported |= after < before && !safetyReduction && (inferred || w.want[r] >= 0);
            }
            const bool residual = s.cause.counts[kPerformanceLimited] > 0 || s.cause.counts[kThermalLimit] > 0;
            unsupported |= c.profile.thermalCase > recorded.thermalCase &&
                (inferred || !residual || !(w.reasons & kFpsShort));
        }
        if (unsupported) continue;
        if (item.candidate.hasExpected) {
            const Summary& e = item.candidate.expected;
            const bool violation = skinLimitMilliC > 0 && e.maxSkinC >= 0 && e.maxSkinC * 1000.0 >= skinLimitMilliC;
            const bool powerViolation = recorded.powerTargetMw > 0 && e.avgPowerMw > recorded.powerTargetMw;
            const bool quality = e.targetFps > 0 && steadyRatio(e) >= 0.95f && (e.tail95 < 0 || e.tail95 <= 1.0);
            const double power = (e.avgPowerMw >= 0 ? e.avgPowerMw : 100000) / 1000.0;
            item.score = -1000 + (violation ? 100000 : 0) + (powerViolation ? 10000 : 0) +
                         (quality ? 0 : 2000) + nearest * 100 + std::min(0.999, power / 1000);
        } else {
            double score = 0;
            for (int r = 0; r < 3; ++r) {
                if (ladders[r].empty() || w.target[r] <= 0) continue;
                double sum = 0;
                for (double skin : skins) {
                    const int cap = capAt(c.profile, r, skin, ladders[r]);
                    if (cap > 0) sum += std::fabs(std::log2(static_cast<double>(cap) / w.target[r]));
                }
                score += kWeights[r] * sum / skins.size();
                if (w.want[r] == 1 && capAt(c.profile, r, medianSkin, ladders[r]) < w.cap[r]) score += 2.0;
                if (floorAt(c.profile, r, medianSkin) > w.target[r]) score += 1.0;
            }
            score += 0.5 * std::abs(c.profile.thermalCase - w.thermalCase) / 100.0;
            if (recorded.powerTargetMw > 0 && s.avgPowerMw >= 0) {
                // Without matching measurements, scale mean power by the weighted cap ratios.
                double ratio = 0, weight = 0;
                for (int r = 0; r < 3; ++r) if (w.cap[r] > 0 && !ladders[r].empty()) {
                    ratio += kWeights[r] * capAt(c.profile, r, medianSkin, ladders[r]) / w.cap[r];
                    weight += kWeights[r];
                }
                if (weight > 0 && s.avgPowerMw * ratio / weight > recorded.powerTargetMw) score += 10000;
            }
            item.score = score;
        }
        ranked.push_back(std::move(item));
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.score != b.score) return a.score < b.score;
        if (a.candidate.profileKey != b.candidate.profileKey) return a.candidate.profileKey < b.candidate.profileKey;
        if (a.candidate.baseMode != b.candidate.baseMode) return a.candidate.baseMode < b.candidate.baseMode;
        if (a.candidate.tunedPackage != b.candidate.tunedPackage) return a.candidate.tunedPackage < b.candidate.tunedPackage;
        return a.candidate.tunedMode < b.candidate.tunedMode;
    });
    if (ranked.size() > 3) ranked.resize(3);
    return ranked;
}

bool compatibleDisplay(float baselineScale, float baselineRefresh, float nextScale, float nextRefresh) {
    return std::isfinite(baselineScale) && std::isfinite(baselineRefresh) &&
        baselineScale > 0 && baselineRefresh > 0 && baselineScale == nextScale && baselineRefresh == nextRefresh;
}

bool compatibleProbe(const Summary& baseline, const Summary& next) {
    return !baseline.mixed && !next.mixed && baseline.validSamples >= kMinValidSamples &&
        next.validSamples >= kMinValidSamples && baseline.targetFps == next.targetFps &&
        baseline.cause.desiredTargetFps == next.cause.desiredTargetFps &&
        baseline.cause.analysisTargetFps == next.cause.analysisTargetFps &&
        baseline.cause.diagnosis == next.cause.diagnosis &&
        compatibleDisplay(baseline.renderScale, baseline.refreshRate, next.renderScale, next.refreshRate);
}

Sensitivity closeProbe(int resource, int deltaSteps, const Summary& baseline, const Summary& next) {
    Sensitivity s;
    s.resource = resource;
    s.deltaSteps = deltaSteps;
    s.dFloor5 = static_cast<double>(next.floor5Fps) - baseline.floor5Fps;
    if (baseline.tail95 >= 0 && next.tail95 >= 0) s.dTail95 = static_cast<double>(next.tail95) - baseline.tail95;
    if (baseline.avgPowerMw >= 0 && next.avgPowerMw >= 0) s.dPowerMw = next.avgPowerMw - baseline.avgPowerMw;
    if (baseline.maxSkinC >= 0 && next.maxSkinC >= 0) s.dSkinPeakC = static_cast<double>(next.maxSkinC) - baseline.maxSkinC;
    return s;
}

namespace {
struct Thermal {
    bool known = false;
    double lastC = 0, peakC = 0, pred15C = 0, pred30C = 0;
    bool risk = false, severe = false;
    bool cool = true;   // No raise is allowed when false.
};

// Least-squares slope over the last 60 s of valid readings; flat when the window is too short.
Thermal assess(const std::vector<const Sample*>& v, int targetMilliC) {
    Thermal t;
    if (targetMilliC == -1) return t;
    std::vector<const Sample*> readings;
    for (const auto* p : v) if (hasTemperature(p->skinMilliC)) readings.push_back(p);
    if (readings.empty()) return t;
    t.known = true;
    for (const auto* p : readings) t.peakC = std::max(t.peakC, p->skinMilliC / 1000.0);
    t.lastC = readings.back()->skinMilliC / 1000.0;
    const int from = readings.back()->timeMs - 60000;
    std::vector<const Sample*> window;
    for (const auto* p : readings) if (p->timeMs >= from) window.push_back(p);
    double slope = 0;
    if (window.size() >= 3 && window.back()->timeMs - window.front()->timeMs >= 10000) {
        double mx = 0, my = 0;
        for (const auto* p : window) { mx += p->timeMs / 1000.0; my += p->skinMilliC / 1000.0; }
        mx /= window.size(); my /= window.size();
        double sxy = 0, sxx = 0;
        for (const auto* p : window) {
            const double dx = p->timeMs / 1000.0 - mx;
            sxy += dx * (p->skinMilliC / 1000.0 - my); sxx += dx * dx;
        }
        if (sxx > 0) slope = sxy / sxx;
    }
    t.pred15C = t.lastC + slope * 15;
    t.pred30C = t.lastC + slope * 30;
    const double target = targetMilliC / 1000.0;
    t.risk = t.peakC >= target || t.pred15C >= target - 0.5;
    t.severe = t.pred15C >= target + 0.5;
    t.cool = std::max(t.lastC, t.pred30C) <= target - 1.0;
    return t;
}

struct Sens { int count = 0; double floorPerStep = 0, wattsPerStep = 0; };

std::array<Sens, 3> aggregate(const std::vector<Sensitivity>& history) {
    std::array<Sens, 3> out{};
    for (int r = 0; r < 3; ++r) {
        std::vector<const Sensitivity*> entries;
        for (const auto& h : history) if (h.resource == r && h.deltaSteps != 0) entries.push_back(&h);
        if (entries.size() > 3) entries.erase(entries.begin(), entries.end() - 3);
        for (const auto* e : entries) {
            out[r].floorPerStep += e->dFloor5 / e->deltaSteps;
            out[r].wattsPerStep += e->dPowerMw / e->deltaSteps / 1000.0;
        }
        out[r].count = static_cast<int>(entries.size());
        if (out[r].count) { out[r].floorPerStep /= out[r].count; out[r].wattsPerStep /= out[r].count; }
    }
    return out;
}
double perWatt(const Sens& s) { return s.count ? s.floorPerStep / std::max(0.1, s.wattsPerStep) : kUnknownSensitivity; }

struct Trial { TuningRecord record; ProfileView view; Change change; };
} // namespace

bool autoTune(const Policy& policy, const std::vector<Sample>& samples, const ProfileView& recorded,
              const TuningRecord& current, int goalIn, const std::vector<Sensitivity>& history,
              Proposal* out, int* error) {
    try {
        if (error) *error = kOk;
        if (!out) return false;
        const int goal = clampGoal(goalIn);
        Ladders ladders;
        for (int r = 0; r < 3; ++r) ladders[r] = policy.ladder(r);
        const auto v = validSamples(samples);
        if (v.size() < static_cast<size_t>(kMinValidSamples)) { if (error) *error = kNotEnoughData; return false; }
        const Summary s = tuningSummary(samples, ladders);
        if (s.mixed) { if (error) *error = kMixed; return false; }
        TuningRecord base = current;
        Mode oldMode;
        Selection oldSelection{};
        if (!normalize(policy, base, nullptr) || !build(policy, base, &oldMode, &oldSelection, nullptr)) {
            if (error) *error = kNotFound;
            return false;
        }
        const ProfileView oldView = view(policy, oldMode, base.baseProfile, base.baseMode, true, base.threadRules);
        if (oldView.steps.empty()) { if (error) *error = kNotFound; return false; }

        ProfileView budgets = recorded;
        if (base.skinTargetMilliC != kKeep) budgets.skinTargetMilliC = base.skinTargetMilliC;
        if (base.powerTargetMw != kKeep) budgets.powerTargetMw = base.powerTargetMw;
        const Wanted w = wanted(samples, s, budgets, ladders, goal);
        const int targetMilliC = skinTarget(budgets, policy.overlay.skinTargetMilliC);
        const Thermal thermal = assess(v, targetMilliC);
        const auto sens = aggregate(history);

        auto finish = [&](const Trial* trial, int probeResource, int probeDelta) {
            out->tuning = trial ? trial->record : base;
            out->changes.clear();
            if (trial) out->changes.push_back(trial->change);
            out->reasons = w.reasons | (thermal.risk ? kHot : 0);
            out->preview = trial ? trial->view : oldView;
            out->probeResource = trial ? probeResource : -1;
            out->probeDeltaSteps = trial ? probeDelta : 0;
            return true;
        };
        auto tryCap = [&](int r, int delta) -> std::optional<Trial> {
            Trial t;
            t.record = base;
            const int value = t.record.maxOffsets[r] + delta;
            if (value < -10 || value > 10) return std::nullopt;
            t.record.maxOffsets[r] = value;
            Mode mode;
            Selection selection{};
            if (!build(policy, t.record, &mode, &selection, nullptr)) return std::nullopt;
            t.view = view(policy, mode, t.record.baseProfile, t.record.baseMode, true, t.record.threadRules);
            if (t.view.steps.empty()) return std::nullopt;
            if (delta > 0 && budgets.powerTargetMw > 0 && s.avgPowerMw >= 0) {
                const int oldCap = capAt(oldView, r, w.medianSkinC, ladders[r]);
                const int newCap = capAt(t.view, r, w.medianSkinC, ladders[r]);
                const double extra = sens[r].count ? std::max(0.0, sens[r].wattsPerStep * 1000)
                    : oldCap > 0 ? s.avgPowerMw * kWeights[r] / 2.5 * std::max(0.0, (newCap - oldCap) / static_cast<double>(oldCap)) : 0;
                if (s.avgPowerMw + extra > budgets.powerTargetMw) return std::nullopt;
            }
            for (size_t i = 0; i < oldView.steps.size() && i < t.view.steps.size(); ++i) {
                const int before = oldView.steps[i].units[r].ceiling, after = t.view.steps[i].units[r].ceiling;
                const int oldCap = before < 0 ? topOf(ladders[r]) : before;
                const int newCap = after < 0 ? topOf(ladders[r]) : after;
                if (delta > 0 ? newCap <= oldCap : newCap >= oldCap) continue;
                t.change = {kChangeMax, r, oldCap, newCap};
                return t;
            }
            return std::nullopt;
        };
        auto tryCase = [&](int delta) -> std::optional<Trial> {
            const int effective = oldView.thermalCase;
            const int next = effective + 100 * delta;
            if (effective < 100 || effective > 400 || next < 100 || next > 400) return std::nullopt;
            Trial t;
            t.record = base;
            t.record.thermalCase = next;
            Mode mode;
            Selection selection{};
            if (!build(policy, t.record, &mode, &selection, nullptr)) return std::nullopt;
            t.view = view(policy, mode, t.record.baseProfile, t.record.baseMode, true, t.record.threadRules);
            t.change = {kChangeThermal, -1, effective, next};
            return t;
        };
        auto relative = [&](int r) {
            const int top = topOf(ladders[r]);
            return top > 0 ? static_cast<double>(s.avgCur[r]) / top : 0.0;
        };
        auto headroom = [&](int r) {
            return indexAtOrBelow(ladders[r], w.cap[r]) - indexAtOrBelow(ladders[r], w.target[r]);
        };

        // Thermal risk: one lowering change, never a raise.
        const bool powerRisk = budgets.powerTargetMw > 0 && s.avgPowerMw > budgets.powerTargetMw;
        if (thermal.risk || powerRisk) {
            if (thermal.severe) {
                if (auto t = tryCase(-1)) return finish(&*t, -1, -1);
            }
            std::vector<std::pair<int, Trial>> options;
            for (int r : kResourceOrder) if (auto t = tryCap(r, -1)) options.emplace_back(r, std::move(*t));
            if (!options.empty()) {
                size_t best = 0;
                for (size_t i = 1; i < options.size(); ++i) {
                    const int a = options[i].first, b = options[best].first;
                    const double wa = perWatt(sens[a]), wb = perWatt(sens[b]);
                    if (wa < wb || (wa == wb && relative(a) > relative(b))) best = i;
                }
                return finish(&options[best].second, options[best].first, -1);
            }
            if (thermal.risk && !thermal.severe) {
                if (auto t = tryCase(-1)) return finish(&*t, -1, -1);
            }
            return finish(nullptr, -1, 0);
        }

        const bool targetKnown = s.targetFps > 0;
        const bool tailBad = s.tail95 >= 0 && s.tail95 > 1.0;
        const bool short_ = targetKnown && (steadyRatio(s) < 0.95f || tailBad);
        if (short_) {
            if (s.cause.counts[kLikelyGameCap] > 0) return finish(nullptr, -1, 0);
            if (!thermal.cool) return finish(nullptr, -1, 0);
            // Positive sensitivity first, then unknown resources in GPU, Gold, Prime order.
            int pick = -1;
            std::optional<Trial> picked;
            for (int r = 0; r < 3; ++r) {
                if (!sens[r].count || sens[r].floorPerStep < kPositiveFloorGain) continue;
                auto t = tryCap(r, 1);
                if (t && (pick < 0 || sens[r].floorPerStep > sens[pick].floorPerStep)) { pick = r; picked = std::move(t); }
            }
            if (pick < 0)
                for (int r : kResourceOrder) {
                    if (sens[r].count) continue;
                    if (auto t = tryCap(r, 1)) { pick = r; picked = std::move(t); break; }
                }
            if (pick >= 0) return finish(&*picked, pick, 1);
            return finish(nullptr, -1, 0);
        }

        const bool ample = s.cause.counts[kLikelyGameCap] > 0 ? false : !s.cause.segments.empty() ? targetKnown && steadyRatio(s) >= kReductionFloor && s.tail95 >= 0 && s.tail95 <= kReductionTail
            : goal == kGoalFrames ? false
            : goal == kGoalEfficiency ? targetKnown
            : targetKnown && steadyRatio(s) >= kReductionFloor && (s.tail95 < 0 || s.tail95 <= kReductionTail);
        if (ample && thermal.cool) {
            int pick = -1;
            std::optional<Trial> picked;
            for (int r : kResourceOrder) {
                if (w.want[r] != -1) continue;
                auto t = tryCap(r, -1);
                if (!t) continue;
                if (pick < 0 || perWatt(sens[r]) < perWatt(sens[pick]) ||
                    (perWatt(sens[r]) == perWatt(sens[pick]) && headroom(r) > headroom(pick))) {
                    pick = r; picked = std::move(t);
                }
            }
            if (pick >= 0) return finish(&*picked, pick, -1);
        }
        return finish(nullptr, -1, 0);
    } catch (...) {
        if (error) *error = kNotFound;
        return false;
    }
}
} // namespace gamepolicy
