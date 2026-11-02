// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include "Recorder.h"
#include "Threads.h"
#include "Tuning.h"
#include <aidl/io/github/miner7222/gamepolicy/BnGamePolicy.h>
#include <aidl/vendor/lenovo/hardware/gametuning/BnGameTuning.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <tuple>

using aidl::io::github::miner7222::gamepolicy::BnGamePolicy;
using aidl::io::github::miner7222::gamepolicy::GameSession;
namespace gt = aidl::vendor::lenovo::hardware::gametuning;
using namespace std::chrono_literals;
namespace gamepolicy {
namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t epochNowMs() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
std::string read(const std::string& path) {
    std::string data;
    if (!android::base::ReadFileToString(path, &data)) return {};
    return data;
}
std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
// Leading integer of a sysfs file; trailing text such as " %" is ignored.
std::optional<int64_t> readNumber(const std::string& path) {
    const std::string text = read(path);
    if (text.empty()) return {};
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || errno) return {};
    return value;
}
std::optional<int> readMilliC(const std::string& path) {
    const auto value = readNumber(path);
    if (!value || *value < -20000 || *value > 125000) return {};
    return static_cast<int>(*value);
}
std::string dirOf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}
// Start time prevents a recycled PID from keeping another process's request alive.
std::optional<int64_t> identity(int pid, int uid) {
    struct stat st{};
    const std::string proc = "/proc/" + std::to_string(pid);
    if (stat(proc.c_str(), &st) || st.st_uid != static_cast<uid_t>(uid)) return {};
    const auto stat = read(proc + "/stat");
    auto end = stat.rfind(')');
    if (end == std::string::npos) return {};
    std::istringstream fields(stat.substr(end + 1));
    std::string field;
    for (int i = 3; i <= 22; ++i) {
        if (!(fields >> field)) return {};
        if (i == 3 && (field == "Z" || field == "X")) return {};
    }
    int64_t ticks = 0;
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), ticks);
    if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || ticks <= 0) return {};
    return ticks;
}

// Perf opcodes of the maximum rows, matching Policy.cpp.
constexpr int kGoldMax = 0x40804000;
constexpr int kPrimeMax = 0x40804200;
constexpr int kGpuMax = 0x42808000;
// Safety interlock thresholds (tuned sessions only).
constexpr double kInterlockSkinC = 48.0;
constexpr double kInterlockBatteryC = 45.0;
constexpr double kInterlockReleaseC = 3.0;
constexpr int kMaxListeners = 16;
constexpr size_t kMaxProposals = 64;
constexpr int kSampleChunk = 600;
constexpr int kIdCandidates = 8;   // Recorder bumps a colliding start time by one millisecond.

class Perf {
  public:
    Perf() {
        mLibrary = dlopen("libqti-perfd-client.so", RTLD_NOW | RTLD_LOCAL);
        if (mLibrary) {
            mAcquire = reinterpret_cast<Acquire>(dlsym(mLibrary, "perf_lock_acq"));
            mRelease = reinterpret_cast<Release>(dlsym(mLibrary, "perf_lock_rel"));
        }
        if (!mAcquire || !mRelease) LOG(ERROR) << "QTI perf client unavailable; frequency control disabled";
    }
    ~Perf() { release(); if (mLibrary) dlclose(mLibrary); }
    void release() {
        if (mHandle > 0 && mRelease) mRelease(mHandle);
        mHandle = 0; mArgs.clear(); mLease.reset();
    }
    void apply(const std::vector<int>& args) {
        if (args != mArgs) release();
        if (args.empty() || !mAcquire || !mRelease) return;
        const auto now = nowMs();
        if (mHandle > 0 && !mLease.due(now)) return;
        auto writable = args;
        // A stable request renews after two seconds; crash expiry remains three seconds.
        const int handle = mAcquire(mHandle, 3000, writable.data(), writable.size());
        if (handle <= 0) { LOG(ERROR) << "perf_lock_acq failed: " << handle; release(); return; }
        mHandle = handle; mArgs = args; mLease.acquired(now);
    }
    const std::vector<int>& args() const { return mArgs; }
  private:
    using Acquire = int (*)(int, int, int*, int);
    using Release = int (*)(int);
    void* mLibrary = nullptr;
    Acquire mAcquire = nullptr;
    Release mRelease = nullptr;
    int mHandle = 0;
    std::vector<int> mArgs;
    PerfLease mLease;
};

// ---- Conversions between the library model and the IGameTuning parcelables -----------------------
template <class Out, class In, class F>
std::vector<Out> mapAll(const std::vector<In>& input, F convert) {
    std::vector<Out> output;
    output.reserve(input.size());
    for (const auto& item : input) output.push_back(convert(item));
    return output;
}
gt::FrequencyRange toRange(const Bounds& b) {
    gt::FrequencyRange r;
    r.min = b.floor; r.max = b.ceiling;
    return r;
}
Bounds fromRange(const gt::FrequencyRange& r) { return Bounds{r.min, r.max}; }
gt::PolicyStep toStep(const Step& s) {
    gt::PolicyStep p;
    p.temperatureC = static_cast<float>(s.temperature);
    p.gold = toRange(s.units[0]); p.prime = toRange(s.units[1]); p.gpu = toRange(s.units[2]);
    return p;
}
Step fromStep(const gt::PolicyStep& p) {
    Step s;
    s.temperature = static_cast<double>(p.temperatureC);
    s.units = {fromRange(p.gold), fromRange(p.prime), fromRange(p.gpu)};
    return s;
}
gt::SceneRow toScene(const SceneRowRec& s) {
    gt::SceneRow r;
    r.sceneId = s.sceneId; r.enabled = s.enabled; r.holdMs = s.holdMs;
    r.gold = toRange(s.units[0]); r.prime = toRange(s.units[1]); r.gpu = toRange(s.units[2]);
    return r;
}
SceneRowRec fromScene(const gt::SceneRow& s) {
    SceneRowRec r;
    r.sceneId = s.sceneId; r.enabled = s.enabled; r.holdMs = s.holdMs;
    r.units = {fromRange(s.gold), fromRange(s.prime), fromRange(s.gpu)};
    return r;
}
gt::ThreadRule toThread(const ThreadRuleRec& t) {
    gt::ThreadRule r;
    r.name = t.name; r.cpuMask = static_cast<int64_t>(t.mask); r.rtPriority = t.rtPriority;
    return r;
}
ThreadRuleRec fromThread(const gt::ThreadRule& t) {
    return ThreadRuleRec{t.name, static_cast<uint64_t>(t.cpuMask), t.rtPriority};
}
gt::PolicyProfile toProfile(const ProfileView& v) {
    gt::PolicyProfile p;
    p.profileKey = v.profileKey; p.baseMode = v.baseMode; p.tuned = v.tuned; p.thermalCase = v.thermalCase;
    p.steps = mapAll<gt::PolicyStep>(v.steps, toStep);
    p.uclampMin = v.uclampMin; p.gpuDefaultHz = v.gpuDefaultHz; p.keepCoresOnline = v.keepCoresOnline;
    p.frameBoostSteps.assign(v.frameBoostSteps.begin(), v.frameBoostSteps.end());
    p.jankBoostSteps.assign(v.jankBoostSteps.begin(), v.jankBoostSteps.end());
    p.startDelayMs = v.startDelayMs; p.threadRules = v.threadRules;
    p.threadRuleCount = v.threadRuleCount; p.sceneCount = v.sceneCount;
    p.scenes = mapAll<gt::SceneRow>(v.scenes, toScene);
    p.sceneMode = v.sceneMode;
    p.threads = mapAll<gt::ThreadRule>(v.threads, toThread);
    p.boostLengthMs = v.boostLengthMs; p.dropSensitivity = v.dropSensitivity; p.dropLowerPermille = v.dropLowerPermille; p.dropUpperPermille = v.dropUpperPermille;
    p.hysteresisDeciC = v.hysteresisDeciC; p.skinTargetMilliC = v.skinTargetMilliC;
    p.powerTargetMw = v.powerTargetMw;
    p.variantId = v.variantId; p.variantName = v.variantName;
    return p;
}
gt::GameTuningConfig toConfig(int userId, const TuningRecord& r) {
    gt::GameTuningConfig c;
    c.userId = userId; c.packageName = r.package; c.mode = r.mode; c.enabled = r.enabled;
    c.baseProfile = r.baseProfile; c.baseMode = r.baseMode;
    c.maxOffsets.assign(r.maxOffsets.begin(), r.maxOffsets.end());
    c.minOffsets.assign(r.minOffsets.begin(), r.minOffsets.end());
    c.steps = mapAll<gt::PolicyStep>(r.steps, toStep);
    c.thermalCase = r.thermalCase; c.uclampMin = r.uclampMin; c.gpuDefaultHz = r.gpuDefaultHz;
    c.keepCoresOnline = r.keepCoresOnline;
    c.frameBoost = r.frameBoost;
    c.frameBoostSteps.assign(r.frameBoostSteps.begin(), r.frameBoostSteps.end());
    c.jankBoost = r.jankBoost;
    c.jankBoostSteps.assign(r.jankBoostSteps.begin(), r.jankBoostSteps.end());
    c.startDelayMs = r.startDelayMs; c.threadRules = r.threadRules; c.sceneMode = r.sceneMode;
    c.scenes = mapAll<gt::SceneRow>(r.scenes, toScene);
    c.customThreads = mapAll<gt::ThreadRule>(r.customThreads, toThread);
    c.boostLengthMs = r.boostLengthMs; c.dropSensitivity = r.dropSensitivity; c.dropLowerPermille = r.dropLowerPermille; c.dropUpperPermille = r.dropUpperPermille;
    c.hysteresisDeciC = r.hysteresisDeciC; c.skinTargetMilliC = r.skinTargetMilliC;
    c.powerTargetMw = r.powerTargetMw;
    c.variantId = r.variantId; c.variantName = r.variantName;
    return c;
}
bool fromConfig(const gt::GameTuningConfig& c, TuningRecord* r, std::string* error) {
    if (c.maxOffsets.size() != 3 || c.minOffsets.size() != 3) {
        *error = "Offsets need exactly three values";
        return false;
    }
    r->package = c.packageName; r->mode = c.mode; r->enabled = c.enabled;
    r->baseProfile = c.baseProfile; r->baseMode = c.baseMode;
    for (size_t i = 0; i < 3; ++i) { r->maxOffsets[i] = c.maxOffsets[i]; r->minOffsets[i] = c.minOffsets[i]; }
    r->steps = mapAll<Step>(c.steps, fromStep);
    r->thermalCase = c.thermalCase; r->uclampMin = c.uclampMin; r->gpuDefaultHz = c.gpuDefaultHz;
    r->keepCoresOnline = c.keepCoresOnline;
    r->frameBoost = c.frameBoost;
    r->frameBoostSteps.assign(c.frameBoostSteps.begin(), c.frameBoostSteps.end());
    r->jankBoost = c.jankBoost;
    r->jankBoostSteps.assign(c.jankBoostSteps.begin(), c.jankBoostSteps.end());
    r->startDelayMs = c.startDelayMs; r->threadRules = c.threadRules; r->sceneMode = c.sceneMode;
    r->scenes = mapAll<SceneRowRec>(c.scenes, fromScene);
    r->customThreads = mapAll<ThreadRuleRec>(c.customThreads, fromThread);
    r->boostLengthMs = c.boostLengthMs; r->dropSensitivity = c.dropSensitivity;
    r->dropLowerPermille = c.dropLowerPermille; r->dropUpperPermille = c.dropUpperPermille;
    if (r->dropLowerPermille == -1 && r->dropUpperPermille == -1 && c.dropSensitivity >= 1 && c.dropSensitivity <= 3) {
        r->dropLowerPermille = 600; r->dropUpperPermille = c.dropSensitivity == 1 ? 900 : c.dropSensitivity == 3 ? 980 : 965;
    }
    r->hysteresisDeciC = c.hysteresisDeciC; r->skinTargetMilliC = c.skinTargetMilliC;
    r->powerTargetMw = c.powerTargetMw;
    r->variantId = c.variantId; r->variantName = c.variantName;
    r->pendingProbe.reset();
    return true;
}
gt::RecordingSample toSample(const Sample& s, int targetFps) {
    gt::RecordingSample r;
    r.timeMs = s.timeMs; r.fps = s.fps; r.fpsMin = s.fpsMin; r.jankFrames = s.jankFrames; r.frames = s.frames;
    r.goldKHz = s.cur[0]; r.primeKHz = s.cur[1]; r.gpuHz = s.cur[2];
    r.goldCapKHz = s.cap[0]; r.primeCapKHz = s.cap[1]; r.gpuCapHz = s.cap[2];
    r.skinMilliC = s.skinMilliC; r.cpuMilliC = s.cpuMilliC; r.gpuMilliC = s.gpuMilliC;
    r.powerMw = s.powerMw; r.band = s.band; r.thermalCase = s.thermalCase; r.flags = s.flags;
    r.mode = s.mode; r.profileGeneration = s.profileGeneration;
    r.sceneId = s.sceneId; r.maxFrameTimeUs = s.maxFrameTimeUs;
    r.cpuUtilPermille.assign(s.cpuUtilPermille.begin(), s.cpuUtilPermille.end());
    r.gpuBusyPercent = s.gpuBusyPercent; r.batteryMilliC = s.batteryMilliC; r.batteryPercent = s.batteryPercent;
    r.targetFps = static_cast<float>(targetFps);
    r.requestedFps = s.requestedFps; r.throttleFps = s.throttleFps;
    r.desiredTargetFps = s.desiredTargetFps; r.renderScale = s.renderScale; r.refreshRate = s.refreshRate;
    return r;
}
gt::RecordingSummary toSummary(const Summary& s) {
    gt::RecordingSummary r;
    r.validSamples = s.validSamples; r.avgFps = s.avgFps; r.medianFps = s.medianFps;
    r.lowFps1 = s.lowFps1; r.lowFps5 = s.lowFps5; r.targetFps = s.targetFps;
    r.hitRate = s.hitRate; r.stability = s.stability; r.jankPercent = s.jankPercent;
    r.avgSkinC = s.avgSkinC; r.maxSkinC = s.maxSkinC; r.maxCpuC = s.maxCpuC; r.maxGpuC = s.maxGpuC;
    r.avgPowerMw = s.avgPowerMw;
    r.avgGoldKHz = s.avgCur[0]; r.avgPrimeKHz = s.avgCur[1]; r.avgGpuHz = s.avgCur[2];
    r.pegged.assign(s.pegged.begin(), s.pegged.end());
    r.throttled = s.throttled; r.mixed = s.mixed;
    r.floor5Fps = s.floor5Fps; r.tail95 = s.tail95; r.jankPer1000 = s.jankPer1000;
    r.scenesSeen.assign(s.scenesSeen.begin(), s.scenesSeen.end());
    r.diagnosis = s.cause.diagnosis; r.ceilingFps = s.cause.ceilingFps;
    r.analysisTargetFps = s.cause.analysisTargetFps; r.desiredTargetFps = s.cause.desiredTargetFps;
    r.confirmedCap = s.cause.confirmed; r.causeEvidence = s.cause.evidence;
    r.causeCoverage = s.cause.coverage; r.classifiedSamples = s.cause.classifiedSamples;
    r.shortfallSamples = s.cause.shortfallSamples;
    r.causeCounts.assign(s.cause.counts.begin(), s.cause.counts.end()); r.analysisVersion = kAnalysisVersion;
    return r;
}
gt::RecordingInfo toInfo(const RecordHeader& h) {
    gt::RecordingInfo r;
    r.id = h.id; r.userId = h.userId; r.packageName = h.package; r.mode = h.mode;
    r.startEpochMs = h.startEpochMs; r.durationMs = h.durationMs; r.sampleCount = h.sampleCount;
    r.endReason = h.endReason;
    r.profile = toProfile(h.profile);
    r.summary = toSummary(h.summary);
    r.originalDurationMs = h.originalDurationMs; r.originalSampleCount = h.originalSampleCount;
    r.keepStartMs = h.keepStartMs; r.keepEndMs = h.keepEndMs; r.rangeRevision = h.rangeRevision;
    return r;
}

bool sameUnits(const std::array<Bounds, 3>& a, const std::array<Bounds, 3>& b) {
    for (int r = 0; r < 3; ++r)
        if (a[r].floor != b[r].floor || a[r].ceiling != b[r].ceiling) return false;
    return true;
}
// Equality of everything a tuning edits; the variant label and the pending probe are not part of it.
bool sameTuning(const TuningRecord& a, const TuningRecord& b) {
    if (a.package != b.package || a.mode != b.mode || a.enabled != b.enabled || a.baseProfile != b.baseProfile ||
        a.baseMode != b.baseMode || a.maxOffsets != b.maxOffsets || a.minOffsets != b.minOffsets ||
        a.thermalCase != b.thermalCase || a.uclampMin != b.uclampMin || a.gpuDefaultHz != b.gpuDefaultHz ||
        a.keepCoresOnline != b.keepCoresOnline || a.frameBoost != b.frameBoost ||
        a.frameBoostSteps != b.frameBoostSteps || a.jankBoost != b.jankBoost ||
        a.jankBoostSteps != b.jankBoostSteps || a.startDelayMs != b.startDelayMs ||
        a.threadRules != b.threadRules || a.sceneMode != b.sceneMode || a.boostLengthMs != b.boostLengthMs ||
        a.dropSensitivity != b.dropSensitivity || a.dropLowerPermille != b.dropLowerPermille || a.dropUpperPermille != b.dropUpperPermille || a.hysteresisDeciC != b.hysteresisDeciC ||
        a.skinTargetMilliC != b.skinTargetMilliC || a.powerTargetMw != b.powerTargetMw ||
        a.steps.size() != b.steps.size() || a.scenes.size() != b.scenes.size() ||
        a.customThreads.size() != b.customThreads.size())
        return false;
    for (size_t i = 0; i < a.steps.size(); ++i)
        if (std::fabs(a.steps[i].temperature - b.steps[i].temperature) > 1e-9 ||
            !sameUnits(a.steps[i].units, b.steps[i].units)) return false;
    for (size_t i = 0; i < a.scenes.size(); ++i) {
        const auto& x = a.scenes[i];
        const auto& y = b.scenes[i];
        if (x.sceneId != y.sceneId || x.enabled != y.enabled || x.holdMs != y.holdMs ||
            !sameUnits(x.units, y.units)) return false;
    }
    for (size_t i = 0; i < a.customThreads.size(); ++i) {
        const auto& x = a.customThreads[i];
        const auto& y = b.customThreads[i];
        if (x.name != y.name || x.mask != y.mask || x.rtPriority != y.rtPriority) return false;
    }
    return true;
}
// Record ids are [0-9]+_[A-Za-z0-9._]+.
bool validRecordId(const std::string& id) {
    const auto split = id.find('_');
    if (split == std::string::npos || split == 0 || split + 1 >= id.size()) return false;
    for (size_t i = 0; i < split; ++i) if (id[i] < '0' || id[i] > '9') return false;
    return validPackage(id.substr(split + 1));
}

struct CpuTimes { uint64_t busy = 0, total = 0; };
std::map<int, CpuTimes> readCpuTimes() {
    std::map<int, CpuTimes> times;
    std::istringstream input(read("/proc/stat"));
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 4 || line.compare(0, 3, "cpu") != 0 || line[3] < '0' || line[3] > '9') continue;
        std::istringstream fields(line);
        std::string name;
        uint64_t v[8] = {};
        fields >> name;
        for (auto& value : v) fields >> value;
        // user nice system idle iowait irq softirq steal
        const uint64_t idle = v[3] + v[4];
        const uint64_t busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
        times[std::atoi(name.c_str() + 3)] = {busy, busy + idle};
    }
    return times;
}
} // namespace

class TuningBinder;

class Service final : public BnGamePolicy {
  public:
    Service(Policy policy, Threads threads) : mPolicy(std::move(policy)), mThreads(std::move(threads)) {
        mDeath = AIBinder_DeathRecipient_new([](void* cookie) {
            auto self = static_cast<Service*>(cookie);
            {
                std::lock_guard lock(self->mMutex);
                self->clear();
                self->mClient = nullptr;
                self->mGeneration = 0;
            }
            self->dispatchEvents();
        });
        // The service outlives its death recipient, so unlinking needs no cookie cleanup.
        AIBinder_DeathRecipient_setOnUnlinked(mDeath, [](void*) {});
        mListenerDeath = AIBinder_DeathRecipient_new([](void* cookie) {
            auto* entry = static_cast<ListenerCookie*>(cookie);
            entry->service->dropListener(entry->id);
        });
        // Each cookie is owned by the link; it is released when the link goes away.
        AIBinder_DeathRecipient_setOnUnlinked(mListenerDeath, [](void* cookie) {
            delete static_cast<ListenerCookie*>(cookie);
        });
        for (int r = 0; r < 3; ++r) mLadders[r] = mPolicy.ladder(r);
        mCase = android::base::GetIntProperty("vendor.lenovo.game_thermal_case", -1);
        setCase(0);
    }
    // Loads stored tunings and finalizes records left behind by a previous daemon.
    void loadState() {
        {
            std::lock_guard store(mStoreMutex);
            mStore.load(mPolicy);
            for (int user : mStore.users())
                for (const auto& record : mStore.all(user))
                    if (record.pendingProbe && record.pendingProbe->armedAtEpochMs > 0)
                        mProbeSince[Key{user, record.package, record.mode}] = record.pendingProbe->armedAtEpochMs;
            for (const auto& diagnostic : mStore.diagnostics) LOG(WARNING) << diagnostic;
        }
        {
            std::lock_guard files(mFilesMutex);
            mRecorder.recover(mLadders);
        }
    }

    // ---- IGamePolicy (session authority: system_server only) ----------------------------------
    ndk::ScopedAStatus setSession(const GameSession& session, const ndk::SpAIBinder& client) override {
        if (AIBinder_getCallingUid() != 1000) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        if (!client.get() || session.userId < 0 || session.uid < 0 || session.pid <= 0 ||
            session.uid / 100000 != session.userId || session.packageName.empty() || session.generation <= 0 ||
            session.processStartTimeTicks <= 0)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        EventFlush flush{this};   // Declared before the lock: listeners are called after it is released.
        std::lock_guard lock(mMutex);
        if (!adopt(client)) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        if (session.generation <= mGeneration) return ndk::ScopedAStatus::ok();
        mGeneration = session.generation;
        auto start = identity(session.pid, session.uid);
        if (!start || *start != session.processStartTimeTicks) {
            clear(); return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        const bool sameProcess = mSession && mSession->uid == session.uid && mSession->pid == session.pid &&
                mSession->packageName == session.packageName &&
                mSession->processStartTimeTicks == session.processStartTimeTicks;
        const bool same = sameProcess && mSession->mode == session.mode;
        // A replacement generation with the same mode keeps the selection it already has.
        if (!same) select(session.userId, session.packageName, session.mode);
        if (same) {
            // A replacement generation must not inherit the preceding scene or boost.
            mScene.reset();
            mFps.invalidate();
            mJank.cancel();
            mSignalTarget = 0;
        }
        if (!same) {
            std::optional<GameSession> replaced;
            if (!sameProcess) replaced = mSession;
            clear(false, !sameProcess, kEndStopped, false);
            if (replaced) emit(*replaced, false);
            if (!sameProcess) {
                const auto now = nowMs();
                mGate.start(now, mSelection.mode->delayMs);
                mFps.reset(now);
                mJank.reset();
            } else {
                mGate.modeChanged();
                mFps.invalidate();
            }
            LOG(INFO) << "Game session uid=" << session.uid << " pid=" << session.pid
                      << " package=" << session.packageName << " mode=" << session.mode;
        }
        mSession = session;
        recordingHooks(session);
        if (!same) emit(session, true);
        // Sample before the first request. Mode updates must not reuse a preceding band.
        update();
        mWake.notify_one();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus stopSession(int64_t generation, const ndk::SpAIBinder& client) override {
        if (AIBinder_getCallingUid() != 1000) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        EventFlush flush{this};
        std::lock_guard lock(mMutex);
        if (client.get() == mClient.get() && generation > mGeneration) { mGeneration = generation; clear(); }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus reportFps(int64_t generation, float fps, const ndk::SpAIBinder& client) override {
        return handleFrame(generation, fps, 0, 0, -1, false, -1, -1, client, false);
    }
    ndk::ScopedAStatus reportFrame(int64_t generation, float fps, int32_t appJankFrames,
                                  int32_t totalFrames, bool layerChanged,
                                  const ndk::SpAIBinder& client) override {
        return handleFrame(generation, fps, appJankFrames, totalFrames, -1, layerChanged, -1, -1, client, true);
    }
    ndk::ScopedAStatus reportFrameTiming(int64_t generation, float fps, int32_t appJankFrames,
                                         int32_t totalFrames, int32_t maxFrameTimeUs, bool layerChanged,
                                         float requestedFps, float throttleFps,
                                         const ndk::SpAIBinder& client) override {
        return handleFrame(generation, fps, appJankFrames, totalFrames, maxFrameTimeUs, layerChanged,
                           requestedFps, throttleFps, client, true);
    }
    // Precise reports come from the compositor's game layer; others from the task frame rate.
    ndk::ScopedAStatus handleFrame(int64_t generation, float fps, int32_t appJankFrames,
                                   int32_t totalFrames, int32_t maxFrameTimeUs, bool layerChanged,
                                   float requestedFps, float throttleFps,
                                   const ndk::SpAIBinder& client, bool precise) {
        if (AIBinder_getCallingUid() != 1000) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        std::lock_guard lock(mMutex);
        if (!mSession || generation != mGeneration || client.get() != mClient.get())
            return ndk::ScopedAStatus::ok();
        if (!std::isfinite(fps) || fps < 0 || appJankFrames < 0 || totalFrames < 0 || appJankFrames > totalFrames)
            return ndk::ScopedAStatus::ok();
        if (mRecorder.active())
            mRecorder.frame(fps, appJankFrames, totalFrames, maxFrameTimeUs < 0 ? -1 : maxFrameTimeUs,
                            requestedFps, throttleFps);
        // A change of the frame rate the game asks for resets the drop history like a layer change does.
        const int signal = signalTarget(requestedFps, throttleFps);
        const bool signalChanged = precise && mSignalTarget != 0 && signal != mSignalTarget;
        if (precise) mSignalTarget = signal;
        const bool reset = layerChanged || signalChanged;
        if (reset) { mFps.invalidate(); mJank.cancel(); }
        const auto now = nowMs();
        // Generation/client bind this report to the cached process lifetime. The
        // sample loop owns /proc validation and retires an exited/recycled process.
        if (mVerifiedStart != mSession->processStartTimeTicks || now - mVerifiedAt > 1500 ||
            !mGate.ready(now) || mSensorFailed || mBand < 0)
            return ndk::ScopedAStatus::ok();
        const auto& selection = mSelection;
        ++(precise ? mFpsSamples : mFallbackSamples);
        LOG(VERBOSE) << "Game fps=" << fps;
        if (mFps.sample(now, fps, !mScene.active(now) &&
                Policy::fpsEnabled(*selection.mode, selection.profile, mScene.scene()), *selection.mode)) {
            ++mFpsBoosts;
            mJank.cancel();
            applyRequest(Policy::resources(*selection.mode, mBand, true), now);
            mWake.notify_one();
        } else if (mJank.sample(now, appJankFrames, totalFrames,
                Policy::jankEnabled(*selection.mode, selection.profile, mScene.scene()),
                mScene.active(now) || mFps.active(now), selection.mode->jankAdjustTime)) {
            ++mJankBoosts;
            applyRequest(Policy::resources(*selection.mode, mBand, false, true), now);
            mWake.notify_one();
        } else if (reset) {
            applyCached();
        }
        if (now - mFpsLogTime >= 30000) {
            LOG(INFO) << "Game FPS feedback samples=" << mFpsSamples << " fallback_samples=" << mFallbackSamples
                      << " boosts=" << mFpsBoosts << " jank_boosts=" << mJankBoosts;
            mFpsLogTime = now; mFpsSamples = 0; mFallbackSamples = 0; mFpsBoosts = 0; mJankBoosts = 0;
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus reportScene(int64_t generation, int32_t scene, const ndk::SpAIBinder& client) override {
        if (AIBinder_getCallingUid() != 1000) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        std::lock_guard lock(mMutex);
        if (!mSession || generation != mGeneration || client.get() != mClient.get() || scene < 0 || scene > 100000)
            return ndk::ScopedAStatus::ok();
        const auto& selection = mSelection;
        const auto now = nowMs();
        const bool changed = mScene.report(now, scene, *selection.mode);
        if (changed) mFps.invalidate();
        else if (mScene.active(now)) mFps.cancel();
        if (changed || mScene.active(now)) mJank.cancel();
        if (mVerifiedStart == mSession->processStartTimeTicks &&
            now - mVerifiedAt < mPolicy.overlay.sampleMs && now < mNextSample &&
            !mSensorFailed && mBand >= 0 && static_cast<size_t>(mBand) < selection.mode->bands.size() &&
            (mWasReady || !mGate.ready(now))) {
            if (!mGate.ready(now) && !mScene.active(now)) releaseRequest();
            else applyRequest(mScene.resources(now, *selection.mode, mBand,
                    mFps.active(now), mJank.active(now)), now);
        } else update();
        if (changed) LOG(INFO) << "Game profile=" << selection.profile << " scene=" << scene
                              << " scene_row_applied=" << mScene.active(nowMs());
        mWake.notify_one();
        return ndk::ScopedAStatus::ok();
    }
    void run() {
        std::unique_lock lock(mMutex);
        for (;;) {
            // Listener calls and record bookkeeping run without the session lock.
            if (hasEvents()) { lock.unlock(); dispatchEvents(); lock.lock(); continue; }
            if (!mFinished.empty()) {
                const auto finished = mFinished.front();
                mFinished.pop_front();
                lock.unlock(); processFinished(finished); lock.lock();
                continue;
            }
            if (!mSession) {
                if (mRecorder.paused()) {
                    mWake.wait_until(lock, std::chrono::steady_clock::time_point(
                            std::chrono::milliseconds(*mRecorder.deadline())));
                    if (mRecorder.expired(nowMs())) finishRecord();
                } else mWake.wait(lock);
                continue;
            }
            const auto now = nowMs();
            if (mRequestExpiry.due(now)) { applyCached(); continue; }
            int64_t deadline = mNextSample;
            if (mSession && !mGate.ready(now)) deadline = std::min(deadline, mGate.deadline());
            deadline = mRequestExpiry.limit(deadline);
            const auto scan = mThreads.deadline();
            if (scan && mGate.ready(now)) deadline = std::min(deadline, *scan);
            if (mRecorder.active()) deadline = std::min(deadline, *mRecorder.deadline());
            if (mWake.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::milliseconds(deadline)))
                    != std::cv_status::timeout) continue;
            if (mRequestExpiry.due(nowMs())) { applyCached(); continue; }
            if (nowMs() >= mNextSample || (!mWasReady && mGate.ready(nowMs()))) update();
            else {
                if (scan && nowMs() >= *scan && mGate.ready(nowMs())) applyThreads();
                if (mRequestExpiry.due(nowMs())) applyCached();
            }
            if (mSession && mRecorder.due(nowMs())) recordSample();
        }
    }
  private:
    friend class TuningBinder;

    // ---- Types -------------------------------------------------------------------------------
    struct SessionEvent { int userId; std::string packageName; int mode; bool active; bool recording; };
    struct EventFlush {
        Service* service;
        ~EventFlush() { service->dispatchEvents(); }
    };
    struct ListenerCookie { Service* service; uint64_t id; };
    struct ListenerEntry {
        uint64_t id; int userId; std::shared_ptr<gt::ISessionListener> listener; ndk::SpAIBinder binder;
        ListenerCookie* cookie;
    };
    // A recording started by the session loop; closed out by processFinished().
    struct RecordInfo {
        int userId = 0; std::string package; int mode = 1; int64_t epochMs = 0;
        std::vector<std::string> before;   // Ids of that name that existed before the recording started.
        bool tainted = false;              // The interlock acted during the recording.
    };
    using Key = ProposalKey;
    // Result of resolving a game's selection, optionally through a tuning.
    struct Resolved {
        std::optional<Mode> tuned;
        Selection stock{nullptr, {}, true};
        std::string profile;
        bool placement = true;
        std::string error;         // The tuning is enabled but cannot be built.
        ProfileView view;
    };

    // ---- Selection ---------------------------------------------------------------------------
    // Pure with respect to the session: a record that is not enabled or not allowed yields the stock mode.
    Resolved resolveWith(const std::optional<TuningRecord>& record, const std::string& pkg, int mode,
                         bool allowTuned) const {
        Resolved r;
        r.stock = mPolicy.select(pkg, mode);
        r.profile = r.stock.profile;
        r.placement = r.stock.threadPlacement;
        const TuningRecord* applied = nullptr;
        if (record && record->enabled) {
            Mode built;
            Selection selection{nullptr, {}, true};
            if (!build(mPolicy, *record, &built, &selection, &r.error)) {
                // Keep the stock selection; the caller decides whether this is an error.
            } else if (allowTuned) {
                r.tuned = std::move(built);
                r.profile = selection.profile;
                r.placement = selection.threadPlacement;
                applied = &*record;
            }
        }
        if (applied) {
            r.view = view(mPolicy, *r.tuned, applied->baseProfile, applied->baseMode, true, applied->threadRules);
            r.view.variantId = applied->variantId;
            r.view.variantName = applied->variantName;
        } else {
            r.view = view(mPolicy, *r.stock.mode, r.stock.profile, gameModeToBase(mPolicy, mode), false,
                          r.stock.threadPlacement ? kThreadsOwn : kThreadsOff);
        }
        return r;
    }
    Resolved resolveStored(int userId, const std::string& pkg, int mode, bool allowTuned) {
        std::optional<TuningRecord> record;
        {
            std::lock_guard store(mStoreMutex);
            record = mStore.find(userId, pkg, mode);
        }
        return resolveWith(record, pkg, mode, allowTuned);
    }
    // Applies the stock selection or the stored tuning to the session. mMutex held.
    void select(int userId, const std::string& pkg, int mode) {
        mScene.cancel();   // Scene rows point into the mode that is about to be replaced.
        Resolved r = resolveStored(userId, pkg, mode, true);
        if (!r.error.empty() && r.error != "disabled") LOG(WARNING) << "Ignoring tuning " << pkg << ": " << r.error;
        mTunedMode.reset();
        mTunedMode = std::move(r.tuned);
        mSelection = {mTunedMode ? &*mTunedMode : r.stock.mode, r.profile, r.placement};
        mStockCase = r.stock.mode->thermalCase;
        mProfileGeneration = nextProfileGeneration(mProfileGeneration, mProfileMode, mView, mode, r.view);
        mProfileMode = mode;
        mView = std::move(r.view);
        mBand = -1;   // A band index belongs to the mode that produced it.
        mLoggedMode = nullptr;
        LOG(INFO) << "Game tuning profile=" << mSelection.profile << " tuned=" << bool(mTunedMode);
    }
    // A stored tuning changed; apply it when it belongs to the running session.
    void reapply(int userId, const std::string& pkg, int mode) {
        EventFlush flush{this};
        std::lock_guard lock(mMutex);
        if (!mSession || mSession->userId != userId || mSession->packageName != pkg || mSession->mode != mode) return;
        mScene.cancel();
        select(userId, pkg, mode);
        mGate.modeChanged(); mFps.invalidate(); mJank.cancel(); mScene.cancel();
        update();
        mWake.notify_one();
    }
    // Skin or battery too hot: tuned sessions give up the request and use the stock thermal case.
    void updateInterlock(const std::optional<double>& skinC) {
        if (!mTunedMode) { mInterlock = mBatteryInterlock = false; return; }
        const auto battery = readNumber(mBatteryDir + "/temp");
        const bool haveBattery = battery && *battery >= -200 && *battery <= 1250;
        const double batteryC = haveBattery ? *battery / 10.0 : 0;
        const bool hot = (skinC && *skinC >= kInterlockSkinC) || (haveBattery && batteryC >= kInterlockBatteryC);
        if (haveBattery && batteryC >= kInterlockBatteryC) mBatteryInterlock = true;
        const bool cool = (!skinC || *skinC <= kInterlockSkinC - kInterlockReleaseC) &&
                          (haveBattery ? batteryC <= kInterlockBatteryC - kInterlockReleaseC : !mBatteryInterlock);
        if (!mInterlock && hot) {
            mInterlock = true;
            releaseRequest();
            LOG(WARNING) << "Game thermal interlock engaged skin=" << (skinC ? *skinC : -1.0)
                         << " battery=" << (haveBattery ? batteryC : -1.0);
        } else if (mInterlock && cool) {
            mInterlock = mBatteryInterlock = false;
            LOG(INFO) << "Game thermal interlock released";
        }
    }

    // ---- Session lifetime --------------------------------------------------------------------
    bool adopt(const ndk::SpAIBinder& client) {
        if (client.get() == mClient.get()) return true;
        // Only one authority exists. Do not replace a live token with another caller.
        if (mClient.get()) return false;
        if (AIBinder_linkToDeath(client.get(), mDeath, this) != STATUS_OK) return false;
        mClient = client; mGeneration = 0; return true;
    }
    void clear(bool resetCase = true, bool resetThreads = true, int endReason = kEndStopped, bool notify = true) {
        if (mSession && notify) emit(*mSession, false);
        if (mRecorder.active()) mRecorder.pause(nowMs(), endReason);
        releaseRequest(); mSession.reset(); mBand = -1;
        mVerifiedStart = 0;
        mInterlock = mBatteryInterlock = false;
        mFps.invalidate();
        mJank.cancel();
        mScene.reset();
        mSignalTarget = 0;
        if (resetThreads) mThreads.clear();
        mLoggedMode = nullptr;
        if (resetCase) setCase(0);
    }
    void setCase(int code) {
        if (mCase == code) return;
        if (android::base::SetProperty("vendor.lenovo.game_thermal_case", std::to_string(code))) mCase = code;
        else LOG(ERROR) << "Cannot select thermal case " << code;
    }
    std::optional<double> temperature() {
        // Rediscover on failure, including thermal-zone renumbering after driver recovery.
        if (mSensor.empty()) {
            DIR* dir = opendir("/sys/class/thermal");
            if (!dir) return {};
            while (auto entry = readdir(dir)) {
                if (std::string(entry->d_name).find("thermal_zone") != 0) continue;
                auto path = std::string("/sys/class/thermal/") + entry->d_name;
                std::istringstream type(read(path + "/type"));
                std::string name; type >> name;
                if (name == mPolicy.overlay.sensor) { mSensor = path + "/temp"; break; }
            }
            closedir(dir);
        }
        std::istringstream reading(read(mSensor));
        double milli; std::string extra;
        if (!(reading >> milli) || (reading >> extra) || !std::isfinite(milli) || milli < -20000 || milli > 125000) {
            mSensor.clear(); return {};
        }
        return milli / 1000.0;
    }
    void update() {
        mNextSample = nowMs() + mPolicy.overlay.sampleMs;
        if (!mSession) { mThreads.clear(); setCase(0); return; }
        const auto start = identity(mSession->pid, mSession->uid);
        if (!start || *start != mSession->processStartTimeTicks) {
            LOG(INFO) << "Game process exited"; clear(true, true, kEndExited); return;
        }
        mVerifiedStart = *start; mVerifiedAt = nowMs();
        const auto& selection = mSelection;
        const bool ready = mGate.ready(nowMs());
        mWasReady = ready;
        if (!ready && !mScene.active(nowMs())) {
            releaseRequest(); mThreads.clear(); setCase(0); return;
        }
        // Scene requests are immediate; DelayTime continues to gate the normal policy.
        if (!ready) mThreads.clear();
        if (ready) applyThreads();
        const auto before = std::chrono::steady_clock::now();
        auto temp = temperature();
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - before).count();
        updateInterlock(temp);
        if (mRecInfo && mInterlock) mRecInfo->tainted = true;
        const auto decision = Policy::evaluate(*selection.mode, temp, age, mBand, mPolicy.overlay.hysteresis);
        setCase(ready ? (mInterlock ? mStockCase : decision.thermalCase) : 0);
        if (!temp || age > 1500) {
            if (!mSensorFailed) LOG(WARNING) << "Invalid/stale game temperature; releasing frequency request";
            mSensorFailed = true; releaseRequest(); mFps.invalidate(); mJank.cancel(); mScene.cancel(); mBand = -1; return;
        }
        mSensorFailed = false;
        const int next = decision.band;
        if (next != mBand) LOG(INFO) << "Game profile=" << selection.profile << " band=" << next
                                    << " temperature=" << *temp << " case=" << selection.mode->thermalCase;
        if (next != mBand) { mFps.invalidate(); mJank.cancel(); }
        mBand = next;
        const auto now = nowMs();
        if (!mGate.ready(now) && !mScene.active(now)) { releaseRequest(); return; }
        applyRequest(mScene.active(now) || mFps.active(now) || mJank.active(now) ?
                mScene.resources(now, *selection.mode, mBand, mFps.active(now), mJank.active(now)) : decision.resources, now);
    }
    void applyThreads() {
        const bool threadsChanged = mThreads.apply(mSession->pid, mSession->processStartTimeTicks, *mSelection.mode,
                                                           mSelection.threadPlacement, nowMs(), mPolicy.overlay.sampleMs);
        if (threadsChanged || mLoggedMode != mSelection.mode) {
            LOG(INFO) << "Game rule set tids_bound=" << mThreads.bound() << " tids_rt=" << mThreads.realtime()
                      << " perf_hint_pairs=" << mSelection.mode->perfHints.size();
            mLoggedMode = mSelection.mode;
        }
    }
    void applyCached() {
        const auto now = nowMs();
        if (!mSession || mVerifiedStart != mSession->processStartTimeTicks ||
            now - mVerifiedAt > 1500 || mSensorFailed ||
            (!mGate.ready(now) && !mScene.active(now))) { releaseRequest(); return; }
        applyRequest(mScene.resources(now, *mSelection.mode, mBand, mFps.active(now), mJank.active(now)), now);
    }
    void releaseRequest() { mRequestExpiry.set(0); mPerf.release(); }
    void applyRequest(const std::vector<int>& args, int64_t now) {
        if (mInterlock) { releaseRequest(); return; }
        mRequestExpiry.set(mScene.active(now) ? mScene.deadline() :
                mFps.active(now) ? mFps.deadline() : mJank.active(now) ? mJank.deadline() : 0);
        mPerf.apply(args);
    }

    // ---- Session listeners -------------------------------------------------------------------
    // mMutex held: queue the change; it is delivered after the lock is released.
    void emit(const GameSession& session, bool active) {
        SessionEvent event{session.userId, session.packageName, session.mode, active,
                           active && mRecorder.active()};
        std::lock_guard events(mEventMutex);
        mEvents.push_back(std::move(event));
    }
    bool hasEvents() {
        std::lock_guard events(mEventMutex);
        return !mEvents.empty();
    }
    // No lock may be held by the caller. The dispatch mutex keeps the order of events.
    void dispatchEvents() {
        std::lock_guard dispatch(mDispatchMutex);
        drainEvents();
    }
    // mDispatchMutex held.
    void drainEvents() {
        for (;;) {
            std::deque<SessionEvent> events;
            {
                std::lock_guard queue(mEventMutex);
                events.swap(mEvents);
            }
            if (events.empty()) return;
            std::vector<ListenerEntry> listeners;
            {
                std::lock_guard registry(mListenerMutex);
                listeners = mListeners;
            }
            for (const auto& event : events)
                for (const auto& entry : listeners)
                    if (entry.userId == event.userId)
                        static_cast<void>(entry.listener->onSessionChanged(
                                event.userId, event.packageName, event.mode, event.active, event.recording));
        }
    }
    void dropListener(uint64_t id) {
        std::lock_guard registry(mListenerMutex);
        mListeners.erase(std::remove_if(mListeners.begin(), mListeners.end(),
                [&](const ListenerEntry& entry) { return entry.id == id; }), mListeners.end());
    }
    // Registers once per binder; returns false when the listener cannot be linked or the table is full.
    bool addListener(int userId, const std::shared_ptr<gt::ISessionListener>& listener) {
        ndk::SpAIBinder binder = listener->asBinder();
        // Null inputs fail before the NDK creates a link (and therefore before onUnlinked).
        if (!binder.get() || !mListenerDeath) return false;
        uint64_t id = 0;
        {
            std::lock_guard registry(mListenerMutex);
            for (const auto& entry : mListeners)
                if (entry.binder.get() == binder.get()) return true;
            if (mListeners.size() >= static_cast<size_t>(kMaxListeners)) return false;
            id = ++mListenerId;
        }
        auto* cookie = new ListenerCookie{this, id};
        if (AIBinder_linkToDeath(binder.get(), mListenerDeath, cookie) != STATUS_OK) {
            // The NDK calls onUnlinked exactly once, including failed link attempts.
            return false;
        }
        // From here the link owns the cookie, which is freed when the link ends (death or unlink):
        // it is only an opaque handle for unlinking and is never dereferenced.
        bool duplicate = false;
        {
            std::lock_guard registry(mListenerMutex);
            for (const auto& entry : mListeners)
                if (entry.binder.get() == binder.get()) { duplicate = true; break; }
            if (!duplicate) mListeners.push_back({id, userId, listener, binder, cookie});
        }
        if (duplicate) {
            // A concurrent registration of the same binder won; drop the extra link.
            AIBinder_unlinkToDeath(binder.get(), mListenerDeath, cookie);
            return true;
        }
        // The link may have been broken before the entry existed.
        if (!AIBinder_isAlive(binder.get())) dropListener(id);
        return true;
    }
    void removeListener(const std::shared_ptr<gt::ISessionListener>& listener) {
        ndk::SpAIBinder binder = listener->asBinder();
        std::optional<ListenerEntry> removed;
        {
            std::lock_guard registry(mListenerMutex);
            for (auto it = mListeners.begin(); it != mListeners.end(); ++it)
                if (it->binder.get() == binder.get()) { removed = *it; mListeners.erase(it); break; }
        }
        if (removed) AIBinder_unlinkToDeath(removed->binder.get(), mListenerDeath, removed->cookie);
    }
    // Registers a listener and tells it about the running session. The dispatch mutex orders that first event
    // before any later one: queued events are delivered first, and later ones wait for the mutex.
    bool registerListener(int userId, const std::shared_ptr<gt::ISessionListener>& listener) {
        std::lock_guard dispatch(mDispatchMutex);
        drainEvents();
        const auto event = currentEvent(userId);
        if (!addListener(userId, listener)) return false;
        if (event)
            static_cast<void>(listener->onSessionChanged(event->userId, event->packageName, event->mode,
                                                         event->active, event->recording));
        return true;
    }
    // The running session of the user, if any. Takes mMutex; the caller holds only mDispatchMutex.
    std::optional<SessionEvent> currentEvent(int userId) {
        std::lock_guard lock(mMutex);
        if (!mSession || mSession->userId != userId) return {};
        return SessionEvent{mSession->userId, mSession->packageName, mSession->mode, true, mRecorder.active()};
    }

    // ---- Recording ---------------------------------------------------------------------------
    // After the session fields are set. mMutex held.
    void recordingHooks(const GameSession& session) {
        const auto now = nowMs();
        if (mRecorder.paused()) {
            if (mRecorder.resume(session.userId, session.packageName, session.pid, session.processStartTimeTicks, now))
                mCpuPrev = readCpuTimes();   // Utilization must not average over the pause.
            else finishRecord();
        }
        if (mRecorder.active()) return;
        bool armed = false;
        {
            std::lock_guard store(mStoreMutex);
            armed = mStore.armed(session.userId, session.packageName);
            // A new recording waits for a fresh app-side snapshot; never reuse another session's display intent.
            if (armed) mRecordingContext.erase({session.userId, session.packageName});
        }
        if (!armed) return;
        discoverProbes();
        mCpuPrev = readCpuTimes();
        RecordInfo info;
        info.userId = session.userId; info.package = session.packageName; info.mode = session.mode;
        info.epochMs = epochNowMs();
        for (int k = 0; k < kIdCandidates; ++k) {
            const std::string id = std::to_string(info.epochMs + k) + "_" + info.package;
            if (mRecorder.header(info.userId, id, nullptr)) info.before.push_back(id);
        }
        {
            std::lock_guard files(mFilesMutex);
            mRecorder.start(session.userId, session.packageName, session.pid, session.processStartTimeTicks,
                            session.mode, mView, now, info.epochMs);
            if (mRecorder.active()) mWriting = info;
        }
        if (mRecorder.active()) {
            LOG(INFO) << "Game recording started id=" << info.epochMs << "_" << info.package
                      << " user=" << info.userId << " mode=" << info.mode;
            mRecInfo = std::move(info);
        } else {
            LOG(WARNING) << "Game recording could not start for " << session.packageName;
        }
    }
    // mMutex held. The file work is quick; reading the result back happens in processFinished().
    void finishRecord() {
        {
            std::lock_guard files(mFilesMutex);
            mRecorder.finish(mLadders);
            mWriting.reset();
        }
        if (mRecInfo) {
            mFinished.push_back(std::move(*mRecInfo));
            mRecInfo.reset();
            mWake.notify_one();
        }
    }
    void discoverProbes() {
        mCurPaths = {dirOf(mPolicy.overlay.oppPaths[0]) + "/scaling_cur_freq",
                     dirOf(mPolicy.overlay.oppPaths[1]) + "/scaling_cur_freq",
                     dirOf(mPolicy.overlay.oppPaths[2]) + "/gpuclk"};
        mGpuBusyPath = dirOf(mPolicy.overlay.oppPaths[2]) + "/gpu_busy_percentage";
        mClusterMask = {cpuMask(read(dirOf(mPolicy.overlay.oppPaths[0]) + "/related_cpus")),
                        cpuMask(read(dirOf(mPolicy.overlay.oppPaths[1]) + "/related_cpus"))};
        mBatteryDir = "/sys/class/power_supply/battery";
        mSkinPath.clear(); mCpuZones.clear(); mGpuZones.clear();
        mProbesAt = nowMs();
        DIR* dir = opendir("/sys/class/thermal");
        if (!dir) return;
        while (auto entry = readdir(dir)) {
            if (std::string(entry->d_name).find("thermal_zone") != 0) continue;
            const auto path = std::string("/sys/class/thermal/") + entry->d_name;
            std::istringstream type(read(path + "/type"));
            std::string name; type >> name;
            if (name.empty()) continue;
            if (name == mPolicy.overlay.sensor) mSkinPath = path + "/temp";
            else if (name.compare(0, 4, "cpu-") == 0 && name.find("trip") == std::string::npos)
                mCpuZones.push_back(path + "/temp");
            else if (name.compare(0, 6, "gpuss-") == 0) mGpuZones.push_back(path + "/temp");
        }
        closedir(dir);
    }
    static int hottest(const std::vector<std::string>& zones) {
        std::optional<int> best;
        for (const auto& path : zones)
            if (const auto value = readMilliC(path)) best = best ? std::max(*best, *value) : *value;
        return best ? *best : kTemperatureUnavailable;
    }
    std::array<int, 2> clusterUtilization() {
        const auto now = readCpuTimes();
        std::array<int, 2> result{-1, -1};
        for (int c = 0; c < 2; ++c) {
            uint64_t busy = 0, total = 0;
            for (const auto& [cpu, times] : now) {
                if (cpu < 0 || cpu >= 64 || !((mClusterMask[c] >> cpu) & 1)) continue;
                const auto before = mCpuPrev.find(cpu);
                if (before == mCpuPrev.end() || times.total < before->second.total || times.busy < before->second.busy)
                    continue;
                busy += times.busy - before->second.busy;
                total += times.total - before->second.total;
            }
            if (total > 0) result[c] = std::clamp(static_cast<int>(std::lround(1000.0 * busy / total)), 0, 1000);
        }
        mCpuPrev = now;
        return result;
    }
    void recordSample() {
        const auto now = nowMs();
        if (mSkinPath.empty() && now - mProbesAt >= 10000) discoverProbes();
        Sample s;
        for (int r = 0; r < 3; ++r) {
            const auto value = readNumber(mCurPaths[r]);
            s.cur[r] = value && *value >= 0 && *value <= INT32_MAX ? static_cast<int>(*value) : 0;
        }
        const auto& args = mPerf.args();
        for (size_t i = 0; i + 1 < args.size(); i += 2) {
            if (args[i] == kGoldMax) s.cap[0] = args[i + 1] * 1000;
            else if (args[i] == kPrimeMax) s.cap[1] = args[i + 1] * 1000;
            else if (args[i] == kGpuMax) {
                const auto& table = mPolicy.tables()[2];
                if (args[i + 1] >= 0 && static_cast<size_t>(args[i + 1]) < table.size()) s.cap[2] = table[args[i + 1]];
            }
        }
        std::optional<int> skin;
        if (!mSkinPath.empty()) {
            skin = readMilliC(mSkinPath);
            if (!skin) mSkinPath.clear();   // Rediscovered after a pause, as zones can be renumbered.
        }
        s.skinMilliC = skin ? *skin : kTemperatureUnavailable;
        s.cpuMilliC = hottest(mCpuZones);
        s.gpuMilliC = hottest(mGpuZones);
        if (trimmed(read(mBatteryDir + "/status")) == "Discharging") {
            const auto current = readNumber(mBatteryDir + "/current_now");
            const auto voltage = readNumber(mBatteryDir + "/voltage_now");
            if (current && voltage) {
                const int64_t amps = *current < 0 ? -*current : *current;
                s.powerMw = static_cast<int>(amps * *voltage / 1000000000);
            }
        }
        if (const auto battery = readNumber(mBatteryDir + "/temp"); battery && *battery >= -200 && *battery <= 1250)
            s.batteryMilliC = static_cast<int>(*battery * 100);
        if (const auto capacity = readNumber(mBatteryDir + "/capacity"); capacity && *capacity >= 0 && *capacity <= 100)
            s.batteryPercent = static_cast<int>(*capacity);
        if (const auto busy = readNumber(mGpuBusyPath); busy && *busy >= 0 && *busy <= 100)
            s.gpuBusyPercent = static_cast<int>(*busy);
        s.cpuUtilPermille = clusterUtilization();
        s.band = mBand;
        s.thermalCase = mCase;
        if (mGate.ready(now)) s.flags |= kFlagReady;
        if (mScene.active(now)) s.flags |= kFlagScene;
        if (mFps.active(now)) s.flags |= kFlagFpsBoost;
        if (mJank.active(now)) s.flags |= kFlagJankBoost;
        if (mSensorFailed) s.flags |= kFlagSensorFailed;
        s.sceneId = mScene.scene().value_or(-1);
        s.mode = mSession->mode;
        s.profileGeneration = mProfileGeneration;
        {
            std::lock_guard store(mStoreMutex);
            const auto context = mRecordingContext.find({mSession->userId, mSession->packageName});
            if (context != mRecordingContext.end()) {
                s.desiredTargetFps = context->second.desiredFps;
                s.renderScale = context->second.scale; s.refreshRate = context->second.refresh;
            }
        }
        mRecorder.sample(s, now);
        // The recorder pauses itself at its sample limit.
        if (mRecorder.paused()) {
            finishRecord();
            emit(*mSession, true);   // The recording flag changed.
        }
    }
    // Run thread, no session lock held: locate the file that was just written and close any sensitivity probe.
    // Finished files are replaced atomically, so reading them needs no file lock.
    void processFinished(const RecordInfo& info) {
        std::lock_guard heavy(mHeavyMutex);
        std::optional<RecordHeader> next;
        for (int k = 0; k < kIdCandidates && !next; ++k) {
            const std::string id = std::to_string(info.epochMs + k) + "_" + info.package;
            if (std::find(info.before.begin(), info.before.end(), id) != info.before.end()) continue;
            RecordHeader header;
            if (mRecorder.header(info.userId, id, &header)) next = std::move(header);
        }
        if (!next) {
            LOG(INFO) << "Game recording discarded package=" << info.package;
            return;
        }
        LOG(INFO) << "Game recording finished id=" << next->id << " samples=" << next->sampleCount
                  << " end=" << next->endReason;
        if (next->summary.mixed || next->summary.validSamples < kMinValidSamples) return;
        const Key key{info.userId, next->package, next->mode};
        std::optional<TuningRecord> stored;
        int64_t armedAt = 0;
        {
            std::lock_guard store(mStoreMutex);
            stored = mStore.find(info.userId, next->package, next->mode);
            const auto since = mProbeSince.find(key);
            if (since != mProbeSince.end()) armedAt = since->second;
        }
        if (!stored || !stored->pendingProbe) return;
        const ProbeRec probe = *stored->pendingProbe;
        // A record that started before the tuning was stored says nothing about it; wait for the next one.
        if (armedAt > 0 && next->startEpochMs < armedAt) return;
        // The interlock acted during this record, so it measured something else; wait for a clean one.
        if (info.tainted) return;
        // The probe closes only with a record measured under exactly the stored tuning, with no interlock acting.
        // The arm time is restored from the tuning after a daemon restart.
        const bool attributable = armedAt > 0 && next->profile.tuned &&
                sameConfiguration(next->profile, resolveWith(stored, next->package, next->mode, true).view);
        std::optional<RecordHeader> baseline;
        if (attributable && validRecordId(probe.baseline)) {
            RecordHeader header;
            if (mRecorder.header(info.userId, probe.baseline, &header) && header.package == next->package &&
                header.mode == next->mode && header.id != next->id) baseline = std::move(header);
        }
        std::lock_guard store(mStoreMutex);
        auto current = mStore.find(info.userId, next->package, next->mode);
        if (!current || !current->pendingProbe || current->pendingProbe->resource != probe.resource ||
            current->pendingProbe->deltaSteps != probe.deltaSteps || current->pendingProbe->baseline != probe.baseline ||
            current->pendingProbe->armedAtEpochMs != probe.armedAtEpochMs)
            return;
        if (attributable && baseline && baseline->rangeRevision == probe.baselineRevision &&
            compatibleProbe(baseline->summary, next->summary) && probe.analysisVersion == kAnalysisVersion &&
            baseline->summary.cause.analysisTargetFps == probe.analysisTargetFps &&
            baseline->summary.cause.diagnosis == probe.diagnosis &&
            compatibleDisplay(probe.renderScale, probe.refreshRate, baseline->summary.renderScale, baseline->summary.refreshRate)) {
            const Sensitivity result = closeProbe(probe.resource, probe.deltaSteps, baseline->summary, next->summary);
            SensitivityEntry entry;
            entry.package = next->package; entry.mode = next->mode;
            entry.resource = result.resource; entry.deltaSteps = result.deltaSteps;
            entry.dFloor5 = result.dFloor5; entry.dTail95 = result.dTail95;
            entry.dPowerMw = result.dPowerMw; entry.dSkinPeakC = result.dSkinPeakC;
            entry.baselineId = baseline->id; entry.resultId = next->id;
            entry.baselineRevision = baseline->rangeRevision; entry.resultRevision = next->rangeRevision;
            entry.analysisTargetFps = probe.analysisTargetFps; entry.diagnosis = probe.diagnosis; entry.analysisVersion = kAnalysisVersion;
            entry.renderScale = probe.renderScale; entry.refreshRate = probe.refreshRate;
            std::string error;
            if (!mStore.addSensitivity(info.userId, entry, &error)) {
                LOG(WARNING) << "Cannot store sensitivity for " << next->package << ": " << error;
                return;   // Keep the probe pending and try again with the next record.
            }
            LOG(INFO) << "Game sensitivity stored package=" << next->package << " resource=" << probe.resource
                      << " delta=" << probe.deltaSteps;
        } else {
            LOG(INFO) << "Game sensitivity probe dropped package=" << next->package;
        }
        current->pendingProbe.reset();
        std::string error;
        if (!mStore.put(info.userId, *current, &error)) {
            LOG(WARNING) << "Cannot clear probe of " << next->package << ": " << error;
            return;
        }
        mProbeSince.erase(key);
    }
    enum class ProbeSource { kNone, kProposal, kCarried };
    // mStoreMutex held. A tuning equal to the last autoTune proposal inherits that proposal's probe. This only
    // fills in the record; commitProbe() applies the bookkeeping once the store accepted it.
    ProbeSource armProbe(int userId, TuningRecord& record, const std::optional<TuningRecord>& existing) {
        record.pendingProbe.reset();
        const auto proposal = mProposals.find(Key{userId, record.package, record.mode});
        if (proposal != mProposals.end() && sameTuning(proposal->second.tuning, record)) {
            record.pendingProbe = proposal->second.probe;
            record.pendingProbe->armedAtEpochMs = epochNowMs();
            return ProbeSource::kProposal;
        }
        if (existing && existing->pendingProbe && sameTuning(*existing, record)) {
            record.pendingProbe = existing->pendingProbe;
            return ProbeSource::kCarried;
        }
        return ProbeSource::kNone;
    }
    // mStoreMutex held, after a successful put.
    void commitProbe(int userId, const TuningRecord& record, ProbeSource source) {
        const Key key{userId, record.package, record.mode};
        if (source == ProbeSource::kProposal) {
            mProbeSince[key] = record.pendingProbe->armedAtEpochMs;
            mProposals.erase(key);
        } else if (source == ProbeSource::kNone) {
            mProbeSince.erase(key);
        }
    }

    RequestExpiry mRequestExpiry;
    Policy mPolicy;
    Selection mSelection{nullptr, {}, true};
    Perf mPerf;
    Threads mThreads;
    std::mutex mMutex;                 // Lock order: mMutex -> mStoreMutex -> mFilesMutex.
    std::condition_variable mWake;
    std::optional<GameSession> mSession;
    ndk::SpAIBinder mClient;
    AIBinder_DeathRecipient* mDeath;
    int64_t mGeneration = 0;
    int64_t mVerifiedStart = 0, mVerifiedAt = 0;
    int64_t mNextSample = 0;
    bool mWasReady = false;
    SessionGate mGate;
    FpsController mFps;
    SceneController mScene;
    JankController mJank;
    int mSignalTarget = 0;   // Resolved frame rate signal of the last report; 0 before the first, -1 when none.
    int64_t mFpsLogTime = nowMs();
    uint64_t mFpsSamples = 0, mFallbackSamples = 0, mFpsBoosts = 0, mJankBoosts = 0;
    const Mode* mLoggedMode = nullptr;
    std::string mSensor;
    int mCase = -1, mBand = -1;
    bool mSensorFailed = false;

    // Tuning state (mMutex).
    std::optional<Mode> mTunedMode;    // Backs mSelection.mode while a tuning is applied.
    int mProfileGeneration = 0;
    int mProfileMode = 0;              // Retained with mView across a pause/resume.
    int mStockCase = 0;
    bool mInterlock = false, mBatteryInterlock = false;
    ProfileView mView;                 // The effective profile of the session.
    Ladders mLadders;

    // Stored tunings, armed games and sensitivity history (mStoreMutex).
    TuningStore mStore;
    std::mutex mStoreMutex;
    struct RecordingContext { int desiredFps = -1; float scale = -1, refresh = -1; };
    std::map<std::pair<int, std::string>, RecordingContext> mRecordingContext;
    std::map<Key, PendingProposal> mProposals;
    std::map<Key, int64_t> mProbeSince;   // Epoch ms at which a proposal-backed tuning was stored.

    // Recording. mRecorder's session state follows mMutex; its files follow mFilesMutex.
    Recorder mRecorder;
    std::mutex mFilesMutex;               // Serializes start/finish/remove of record files.
    std::mutex mHeavyMutex;               // One heavy IGameTuning call at a time; taken before any other lock.
    std::optional<RecordInfo> mWriting;   // mFilesMutex: the record being written.
    std::optional<RecordInfo> mRecInfo;   // mMutex: the same record, for the finish step.
    std::deque<RecordInfo> mFinished;     // mMutex.
    std::array<std::string, 3> mCurPaths;
    std::string mGpuBusyPath, mSkinPath, mBatteryDir = "/sys/class/power_supply/battery";
    std::vector<std::string> mCpuZones, mGpuZones;
    std::array<uint64_t, 2> mClusterMask{};
    std::map<int, CpuTimes> mCpuPrev;
    int64_t mProbesAt = 0;

    // Listeners.
    AIBinder_DeathRecipient* mListenerDeath;
    std::mutex mListenerMutex;
    std::vector<ListenerEntry> mListeners;
    uint64_t mListenerId = 0;
    std::mutex mEventMutex;
    std::deque<SessionEvent> mEvents;
    std::mutex mDispatchMutex;         // Never held together with mMutex.
};

class TuningBinder final : public gt::BnGameTuning {
  public:
    explicit TuningBinder(std::shared_ptr<Service> core) : mCore(std::move(core)) {}

    ndk::ScopedAStatus getLimits(gt::PolicyLimits* out) override {
        if (!caller()) return denied();
        const auto runtime = mCore->mPolicy.archiveRuntime();
        const auto& policy = runtime ? *runtime : mCore->mPolicy;
        out->goldKHz = policy.ladder(0);
        out->primeKHz = policy.ladder(1);
        out->gpuHz = policy.ladder(2);
        out->goldCores = policy.cores(0);
        out->primeCores = policy.cores(1);
        out->thermalCases = {100, 200, 300, 400};
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getProfileSources(std::vector<gt::ProfileSource>* out) override {
        if (!caller()) return denied();
        std::lock_guard heavy(mCore->mHeavyMutex);
        const bool wasReady = mCore->mPolicy.archiveReady();
        std::string archiveError;
        if (!mCore->mPolicy.retryBuiltinArchive(&archiveError)) LOG(WARNING) << archiveError;
        const auto& policy = mCore->mPolicy;
        if (!wasReady && policy.archiveReady()) {
            std::lock_guard lock(mCore->mMutex);
            if (auto runtime = policy.archiveRuntime())
                for (int r = 0; r < 3; ++r) mCore->mLadders[r] = runtime->ladder(r);
            if (mCore->mSession) mCore->select(mCore->mSession->userId, mCore->mSession->packageName, mCore->mSession->mode);
        }
        gt::ProfileSource marker;
        marker.profileKey = policy.archiveMarker();
        out->push_back(std::move(marker));
        for (const auto& source : policy.sources) {
            const auto games = policy.games.find(source.key);
            if (games == policy.games.end()) continue;
            gt::ProfileSource item;
            item.profileKey = source.key;
            item.displayName = source.name;
            item.packages = source.packages;
            for (int base = kBaseBalanced; base <= kBaseMaximum; ++base)
                if (games->second.count(baseModeName(base))) item.baseModes.push_back(base);
            out->push_back(std::move(item));
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getProfile(int32_t userId, const std::string& packageName, int32_t mode, bool tuned,
                                  gt::PolicyProfile* out) override {
        if (!callerFor(userId)) return denied();
        if (!validPackage(packageName) || mode < 1 || mode > 3) return invalid("Invalid package or mode");
        const auto resolved = mCore->resolveStored(userId, packageName, mode, tuned);
        *out = toProfile(resolved.view);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getSourceProfile(const std::string& profileKey, int32_t baseMode,
                                        gt::PolicyProfile* out) override {
        if (!caller()) return denied();
        const char* name = baseModeName(baseMode);
        if (!name) return invalid("Invalid base mode");
        const auto& policy = mCore->mPolicy;
        const std::string prefix = "@builtin/history/";
        if (profileKey.rfind(prefix, 0) == 0) {
            try { *out = toProfile(archiveSourceProfile(policy, profileKey, baseMode)); }
            catch (const std::exception& e) { return invalid(e.what()); }
            return ndk::ScopedAStatus::ok();
        }
        if (auto entry = policy.archived(profileKey)) {
            *out = toProfile(view(policy, entry->mode, profileKey, baseMode, false,
                    entry->placement ? kThreadsOwn : kThreadsOff));
            return ndk::ScopedAStatus::ok();
        }
        const auto runtime = policy.archiveRuntime();
        const auto& profiles = runtime ? runtime->games : policy.games;
        const auto games = profiles.find(profileKey);
        if (games == profiles.end()) return notFound("Unknown profile");
        const auto stock = games->second.find(name);
        if (stock == games->second.end()) return notFound("Profile has no such mode");
        *out = toProfile(view(mCore->mPolicy, stock->second, profileKey, baseMode, false, kThreadsOwn));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getTunings(int32_t userId, std::vector<gt::GameTuningConfig>* out) override {
        if (!callerFor(userId)) return denied();
        std::vector<TuningRecord> records;
        {
            std::lock_guard store(mCore->mStoreMutex);
            records = mCore->mStore.all(userId);
        }
        for (const auto& record : records) out->push_back(toConfig(userId, record));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus previewTuning(const gt::GameTuningConfig& tuning, gt::PolicyProfile* out) override {
        if (!callerFor(tuning.userId)) return denied();
        TuningRecord record;
        std::string error;
        if (!fromConfig(tuning, &record, &error) || !normalize(mCore->mPolicy, record, &error)) return invalid(error);
        const auto resolved = mCore->resolveWith(record, record.package, record.mode, true);
        if (!resolved.error.empty() && resolved.error != "disabled") return invalid(resolved.error);
        *out = toProfile(resolved.view);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setTuning(const gt::GameTuningConfig& tuning, gt::GameTuningConfig* out) override {
        if (!callerFor(tuning.userId)) return denied();
        TuningRecord record;
        std::string error;
        if (!fromConfig(tuning, &record, &error) || !normalize(mCore->mPolicy, record, &error)) return invalid(error);
        const auto check = mCore->resolveWith(record, record.package, record.mode, true);
        if (!check.error.empty() && check.error != "disabled") return invalid(check.error);
        std::optional<TuningRecord> stored;
        {
            std::lock_guard store(mCore->mStoreMutex);
            const auto existing = mCore->mStore.find(tuning.userId, record.package, record.mode);
            const auto probeSource = mCore->armProbe(tuning.userId, record, existing);
            if (!mCore->mStore.put(tuning.userId, record, &error)) return storage(error);
            mCore->commitProbe(tuning.userId, record, probeSource);
            stored = mCore->mStore.find(tuning.userId, record.package, record.mode);
        }
        *out = toConfig(tuning.userId, stored ? *stored : record);
        // The store lock is released; only now take the session lock.
        mCore->reapply(tuning.userId, record.package, record.mode);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus clearTuning(int32_t userId, const std::string& packageName, int32_t mode) override {
        if (!callerFor(userId)) return denied();
        if (!validPackage(packageName) || mode < 1 || mode > 3) return invalid("Invalid package or mode");
        {
            std::lock_guard store(mCore->mStoreMutex);
            if (mCore->mStore.find(userId, packageName, mode) && !mCore->mStore.erase(userId, packageName, mode))
                return storage("Cannot remove the tuning");
            const Service::Key key{userId, packageName, mode};
            mCore->mProposals.erase(key);
            mCore->mProbeSince.erase(key);
        }
        mCore->reapply(userId, packageName, mode);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setRecordingArmed(int32_t userId, const std::string& packageName, bool armed) override {
        if (!callerFor(userId)) return denied();
        if (!validPackage(packageName)) return invalid("Invalid package");
        std::string error;
        std::lock_guard store(mCore->mStoreMutex);
        if (!mCore->mStore.setArmed(userId, packageName, armed, &error)) return storage(error);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getArmedPackages(int32_t userId, std::vector<std::string>* out) override {
        if (!callerFor(userId)) return denied();
        std::lock_guard store(mCore->mStoreMutex);
        *out = mCore->mStore.armedPackages(userId);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getRecordings(int32_t userId, const std::string& packageName,
                                     std::vector<gt::RecordingInfo>* out) override {
        if (!callerFor(userId)) return denied();
        if (!packageName.empty() && !validPackage(packageName)) return invalid("Invalid package");
        std::lock_guard heavy(mCore->mHeavyMutex);
        const auto headers = mCore->mRecorder.list(userId, packageName);
        for (const auto& header : headers) out->push_back(toInfo(header));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getRecordingSamples(int32_t userId, const std::string& id, int32_t offset,
                                           std::vector<gt::RecordingSample>* out) override {
        if (!callerFor(userId)) return denied();
        if (offset < 0 || !validRecordId(id)) return invalid("Invalid record id or offset");
        std::vector<Sample> samples;
        std::lock_guard heavy(mCore->mHeavyMutex);
        if (!mCore->mRecorder.read(userId, id, &samples)) return notFound("No such record");
        const std::vector<int> targets = sampleTargets(samples);
        for (size_t i = static_cast<size_t>(offset); i < samples.size() && i < static_cast<size_t>(offset) + kSampleChunk; ++i)
            out->push_back(toSample(samples[i], targets[i]));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus deleteRecording(int32_t userId, const std::string& id) override {
        if (!callerFor(userId)) return denied();
        if (!validRecordId(id)) return invalid("Invalid record id");
        std::lock_guard heavy(mCore->mHeavyMutex);
        std::lock_guard files(mCore->mFilesMutex);
        if (mCore->mWriting && mCore->mWriting->userId == userId) {
            const auto& writing = *mCore->mWriting;
            for (int k = 0; k < kIdCandidates; ++k)
                if (id == std::to_string(writing.epochMs + k) + "_" + writing.package &&
                    std::find(writing.before.begin(), writing.before.end(), id) == writing.before.end())
                    return invalid("The record is being written");
        }
        if (!mCore->mRecorder.remove(userId, id)) return notFound("No such record");
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus recommend(int32_t userId, const std::string& id, int32_t goal,
                                 std::vector<gt::Recommendation>* out) override {
        if (!callerFor(userId)) return denied();
        if (!validRecordId(id)) return invalid("Invalid record id");
        std::lock_guard heavy(mCore->mHeavyMutex);
        std::vector<Sample> samples;
        if (!mCore->mRecorder.read(userId, id, &samples)) return notFound("No such record");
        const std::vector<RecordHeader> all = mCore->mRecorder.list(userId, "");
        const auto found = std::find_if(all.begin(), all.end(), [&](const RecordHeader& h) { return h.id == id; });
        if (found == all.end()) return notFound("No such record");
        const RecordHeader header = *found;
        if (header.summary.mixed) return analysisError(kMixed);
        std::vector<TuningRecord> tunings;
        {
            std::lock_guard store(mCore->mStoreMutex);
            tunings = mCore->mStore.all(userId);
        }
        // Candidates are built without any lock.
        const Policy& policy = mCore->mPolicy;
        std::vector<Candidate> candidates;
        std::vector<RecordEvidence> evidence;
        for (const auto& other : all)
            evidence.push_back({other.package, other.mode, other.profile, other.summary});
        for (const auto& source : policy.sources) {
            const auto games = policy.games.find(source.key);
            if (games == policy.games.end()) continue;
            for (int base = kBaseBalanced; base <= kBaseMaximum; ++base) {
                if (!games->second.count(baseModeName(base))) continue;
                TuningRecord record;
                record.package = header.package; record.mode = header.mode; record.enabled = true;
                record.baseProfile = source.key; record.baseMode = base;
                Mode mode;
                Selection selection{nullptr, {}, true};
                if (!build(policy, record, &mode, &selection, nullptr)) continue;
                Candidate candidate;
                candidate.profileKey = source.key; candidate.baseMode = base;
                candidate.profile = view(policy, mode, source.key, base, false, record.threadRules);
                candidates.push_back(std::move(candidate));
            }
        }
        for (const auto& stored : tunings) {
            if (!stored.enabled || (stored.package == header.package && (!header.summary.cause.confirmed || stored.mode != header.mode))) continue;
            TuningRecord record = stored;
            record.package = header.package; record.mode = header.mode;
            record.pendingProbe.reset();
            Mode mode;
            Selection selection{nullptr, {}, true};
            if (!build(policy, record, &mode, &selection, nullptr)) continue;
            Candidate candidate;
            candidate.baseMode = stored.baseMode;
            candidate.tunedPackage = stored.package; candidate.tunedMode = stored.mode;
            candidate.profile = view(policy, mode, record.baseProfile, record.baseMode, true, record.threadRules);
            candidate.profile.variantId = stored.variantId;
            candidate.profile.variantName = stored.variantName;
            candidates.push_back(std::move(candidate));
        }
        int reasons = 0, error = kOk;
        const auto ranked = gamepolicy::recommend(samples, header.profile, candidates, mCore->mLadders, goal,
                                                  &reasons, &error, evidence);
        if (error != kOk) return analysisError(error);
        for (const auto& item : ranked) {
            gt::Recommendation r;
            r.profileKey = item.candidate.profileKey; r.baseMode = item.candidate.baseMode;
            r.tunedPackage = item.candidate.tunedPackage; r.tunedMode = item.candidate.tunedMode;
            r.score = static_cast<float>(item.score); r.reasons = reasons;
            r.directions.assign(item.directions.begin(), item.directions.end());
            r.profile = toProfile(item.candidate.profile);
            r.rangeRevision = header.rangeRevision; r.diagnosis = header.summary.cause.diagnosis;
            r.ceilingFps = header.summary.cause.ceilingFps; r.confirmedCap = header.summary.cause.confirmed;
            r.analysisVersion = kAnalysisVersion;
            out->push_back(std::move(r));
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus autoTune(int32_t userId, const std::string& id, int32_t goal,
                                gt::TuningProposal* out) override {
        if (!callerFor(userId)) return denied();
        if (!validRecordId(id)) return invalid("Invalid record id");
        std::lock_guard heavy(mCore->mHeavyMutex);
        std::vector<Sample> samples;
        RecordHeader header;
        if (!mCore->mRecorder.header(userId, id, &header) || !mCore->mRecorder.read(userId, id, &samples))
            return notFound("No such record");
        if (header.summary.mixed) return analysisError(kMixed);
        TuningRecord current;
        std::vector<Sensitivity> history;
        {
            std::lock_guard store(mCore->mStoreMutex);
            const auto stored = mCore->mStore.find(userId, header.package, header.mode);
            if (stored && stored->enabled) current = *stored;
            else {
                current.package = header.package; current.mode = header.mode; current.enabled = true;
                current.baseProfile = header.profile.profileKey; current.baseMode = header.profile.baseMode;
            }
            for (const auto& entry : mCore->mStore.sensitivity(userId, header.package, header.mode)) {
                RecordHeader baseHeader, resultHeader;
                if (entry.baselineId.empty() || entry.resultId.empty() || entry.analysisVersion != kAnalysisVersion ||
                    !mCore->mRecorder.header(userId, entry.baselineId, &baseHeader) ||
                    !mCore->mRecorder.header(userId, entry.resultId, &resultHeader) ||
                    baseHeader.rangeRevision != entry.baselineRevision || resultHeader.rangeRevision != entry.resultRevision ||
                    !compatibleProbe(baseHeader.summary, resultHeader.summary) ||
                    !compatibleProbe(baseHeader.summary, header.summary) ||
                    !compatibleDisplay(entry.renderScale, entry.refreshRate, header.summary.renderScale, header.summary.refreshRate) ||
                    entry.analysisTargetFps != header.summary.cause.analysisTargetFps ||
                    entry.diagnosis != header.summary.cause.diagnosis) continue;
                Sensitivity s;
                s.resource = entry.resource; s.deltaSteps = entry.deltaSteps;
                s.dFloor5 = entry.dFloor5; s.dTail95 = entry.dTail95;
                s.dPowerMw = entry.dPowerMw; s.dSkinPeakC = entry.dSkinPeakC;
                history.push_back(s);
            }
        }
        current.pendingProbe.reset();
        Proposal proposal;
        int error = kOk;
        if (!gamepolicy::autoTune(mCore->mPolicy, samples, header.profile, current, goal, history, &proposal, &error))
            return analysisError(error == kOk ? kNotFound : error);
        out->tuning = toConfig(userId, proposal.tuning);
        for (const auto& change : proposal.changes) {
            gt::TuningChange c;
            c.kind = change.kind; c.resource = change.resource; c.before = change.before; c.after = change.after;
            out->changes.push_back(c);
        }
        out->reasons = proposal.reasons;
        out->preview = toProfile(proposal.preview);
        out->probeResource = proposal.probeResource;
        out->probeDeltaSteps = proposal.probeDeltaSteps;
        out->rangeRevision = header.rangeRevision; out->diagnosis = header.summary.cause.diagnosis;
        out->ceilingFps = header.summary.cause.ceilingFps; out->confirmedCap = header.summary.cause.confirmed;
        out->analysisVersion = kAnalysisVersion;
        // The app applies the proposal unchanged; setTuning recognizes it and records the probe.
        if (proposal.probeResource >= 0) rememberProposal(userId, proposal, id);
        else {
            std::lock_guard store(mCore->mStoreMutex);
            mCore->mProposals.erase(Service::Key{userId, header.package, header.mode});
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setRecordingRange(int32_t userId, const std::string& id, int32_t start,
                                         int32_t end, int64_t revision, gt::RecordingInfo* out) override {
        if (!callerFor(userId)) return denied();
        if (!validRecordId(id)) return invalid("Invalid record id");
        std::lock_guard heavy(mCore->mHeavyMutex);
        std::lock_guard store(mCore->mStoreMutex);
        std::lock_guard files(mCore->mFilesMutex);
        RecordHeader header;
        if (!mCore->mRecorder.header(userId, id, &header)) return notFound("No such record");
        if (header.rangeRevision != revision) return invalid("Stale range revision");
        std::vector<Sample> raw;
        if (!mCore->mRecorder.raw(userId, id, &raw)) return notFound("No such record");
        if (!normalizeKeepRange(raw, header.originalDurationMs, &start, &end)) return invalid("Invalid range");
        if (start == header.keepStartMs && end == header.keepEndMs) {
            const int result = mCore->mRecorder.keepRange(userId, id, start, end, revision, mCore->mLadders, &header);
            if (result) return storage("Cannot sync range revision " + std::to_string(header.rangeRevision));
            *out = toInfo(header); return ndk::ScopedAStatus::ok();
        }
        std::string error;
        if (!mCore->mStore.invalidateEvidence(userId, header.package, header.mode, id, &error)) return storage(error);
        const Service::Key key{userId, header.package, header.mode};
        mCore->mProposals.erase(key); mCore->mProbeSince.erase(key);
        const int result = mCore->mRecorder.keepRange(userId, id, start, end, revision, mCore->mLadders, &header);
        if (result == 1) return notFound("No such record");
        if (result == 2) return invalid("Invalid range");
        if (result == 3) return invalid("Stale range revision");
        if (result == 5) return storage("Cannot sync range revision " + std::to_string(header.rangeRevision));
        if (result) return storage("Cannot save range");
        *out = toInfo(header);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus recordingPage(int32_t userId, const std::string& id, int32_t offset, int64_t revision,
                                     bool raw, std::vector<gt::RecordingSample>* out) {
        if (!callerFor(userId)) return denied();
        if (offset < 0 || !validRecordId(id)) return invalid("Invalid record id or offset");
        std::lock_guard heavy(mCore->mHeavyMutex);
        RecordHeader header; std::vector<Sample> samples;
        if (!mCore->mRecorder.header(userId, id, &header)) return notFound("No such record");
        if (header.rangeRevision != revision) return invalid("Stale range revision");
        if (!(raw ? mCore->mRecorder.raw(userId, id, &samples) : mCore->mRecorder.read(userId, id, &samples)))
            return notFound("No such record");
        const auto targets = sampleTargets(samples);
        for (size_t i = static_cast<size_t>(offset); i < samples.size() && i < static_cast<size_t>(offset) + kSampleChunk; ++i)
            out->push_back(toSample(samples[i], targets[i]));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getEffectiveRecordingSamples(int32_t userId, const std::string& id, int32_t offset,
                                                    int64_t revision, std::vector<gt::RecordingSample>* out) override {
        return recordingPage(userId, id, offset, revision, false, out);
    }
    ndk::ScopedAStatus getRawRecordingSamples(int32_t userId, const std::string& id, int32_t offset,
                                              int64_t revision, std::vector<gt::RecordingSample>* out) override {
        return recordingPage(userId, id, offset, revision, true, out);
    }
    ndk::ScopedAStatus setRecordingContext(int32_t userId, const std::string& pkg, int32_t fps,
                                           float scale, float refresh) override {
        if (!callerFor(userId)) return denied();
        if (!validPackage(pkg) || fps < -1 || fps > 1000 || !std::isfinite(scale) || !std::isfinite(refresh) ||
            (scale != -1 && (scale <= 0 || scale > 1)) || (refresh != -1 && (refresh <= 0 || refresh > 1000)))
            return invalid("Invalid recording context");
        std::lock_guard store(mCore->mStoreMutex);
        mCore->mRecordingContext[{userId, pkg}] = {fps, scale, refresh};
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus invalidateDisplayProbe(int32_t userId, const std::string& pkg, int32_t mode) override {
        if (!callerFor(userId)) return denied();
        if (!validPackage(pkg) || (mode < 1 || mode > 3)) return invalid("Invalid package or mode");
        std::lock_guard store(mCore->mStoreMutex);
        const Service::Key key{userId, pkg, mode};
        mCore->mProposals.erase(key); mCore->mProbeSince.erase(key);
        auto tuning = mCore->mStore.find(userId, pkg, mode);
        if (tuning && tuning->pendingProbe) {
            tuning->pendingProbe.reset(); std::string error;
            if (!mCore->mStore.put(userId, *tuning, &error)) return storage(error);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus registerSessionListener(const std::shared_ptr<gt::ISessionListener>& listener) override {
        int userId = 0;
        if (!caller(&userId)) return denied();
        if (!listener) return invalid("No listener");
        if (!mCore->registerListener(userId, listener)) return storage("Cannot register the listener");
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus unregisterSessionListener(const std::shared_ptr<gt::ISessionListener>& listener) override {
        if (!caller()) return denied();
        if (listener) mCore->removeListener(listener);
        return ndk::ScopedAStatus::ok();
    }

  private:
    // Remembers an autoTune proposal so that the matching setTuning can record the probe.
    void rememberProposal(int userId, const Proposal& proposal, const std::string& baseline) {
        RecordHeader header;
        if (!mCore->mRecorder.header(userId, baseline, &header) ||
            !compatibleDisplay(header.summary.renderScale, header.summary.refreshRate, header.summary.renderScale, header.summary.refreshRate)) return;
        TuningRecord tuning = proposal.tuning;
        std::string error;
        if (!normalize(mCore->mPolicy, tuning, &error)) return;
        tuning.pendingProbe.reset();
        std::lock_guard store(mCore->mStoreMutex);
        if (mCore->mProposals.size() >= kMaxProposals) mCore->mProposals.erase(mCore->mProposals.begin());
        cacheProposal(mCore->mProposals, userId, std::move(tuning),
                      ProbeRec{proposal.probeResource, proposal.probeDeltaSteps, baseline, 0,
                               header.rangeRevision, header.summary.cause.analysisTargetFps, header.summary.cause.diagnosis, kAnalysisVersion,
                               header.summary.renderScale, header.summary.refreshRate});
    }
    static bool caller(int* userId = nullptr) {
        const uid_t uid = AIBinder_getCallingUid();
        if (uid % 100000 != 1000) return false;
        if (userId) *userId = static_cast<int>(uid / 100000);
        return true;
    }
    static bool callerFor(int userId) {
        int user = 0;
        return caller(&user) && user == userId;
    }
    static ndk::ScopedAStatus denied() { return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY); }
    static ndk::ScopedAStatus invalid(const std::string& message) {
        return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(gt::IGameTuning::ERROR_INVALID, message.c_str());
    }
    static ndk::ScopedAStatus notFound(const std::string& message) {
        return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(gt::IGameTuning::ERROR_NOT_FOUND, message.c_str());
    }
    static ndk::ScopedAStatus storage(const std::string& message) {
        return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(gt::IGameTuning::ERROR_STORAGE, message.c_str());
    }
    static ndk::ScopedAStatus analysisError(int error) {
        switch (error) {
            case kNotEnoughData:
                return ndk::ScopedAStatus::fromServiceSpecificError(gt::IGameTuning::ERROR_NOT_ENOUGH_DATA);
            case kMixed:
                return ndk::ScopedAStatus::fromServiceSpecificError(gt::IGameTuning::ERROR_MIXED);
            default:
                return ndk::ScopedAStatus::fromServiceSpecificError(gt::IGameTuning::ERROR_NOT_FOUND);
        }
    }
    std::shared_ptr<Service> mCore;
};
} // namespace gamepolicy

int main(int argc, char** argv) {
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    (void)argc;
    gamepolicy::Threads threads([](const std::string& message) { LOG(WARNING) << message; },
                                [](const std::string& message) { LOG(INFO) << message; });
    threads.recover();
    // Reset even when configuration loading fails after a previous daemon crash.
    if (android::base::GetProperty("vendor.lenovo.game_thermal_case", "") != "0") {
        android::base::SetProperty("vendor.lenovo.game_thermal_case", "0");
    }
    gamepolicy::Policy policy;
    std::string error;
    if (!policy.load("/vendor/etc/gameperfconfig.xml", "/vendor/etc/game-policy.xml", &error)) {
        LOG(ERROR) << "Cannot load game policy: " << error; return 1;
    }
    gamepolicy::Opps opps;
    for (int r = 0; r < 3; ++r) {
        std::istringstream input(gamepolicy::read(policy.overlay.oppPaths[r]));
        int f; while (input >> f) opps[r].push_back(f);
    }
    std::array<int, 2> coreCounts{};
    uint64_t present = 0;
    bool presentKnown = true;
    for (int r = 0; r < 2; ++r) {
        const auto& path = policy.overlay.oppPaths[r];
        const auto related = gamepolicy::read(path.substr(0, path.rfind('/')) + "/related_cpus");
        coreCounts[r] = gamepolicy::coreCount(related);
        if (!coreCounts[r]) LOG(WARNING) << "Missing CPU topology for resource " << r;
        const uint64_t mask = gamepolicy::cpuMask(related);
        if (mask) present |= mask; else presentKnown = false;
    }
    // Without both cluster masks, custom thread masks cannot be validated against real CPUs.
    policy.resolve(opps, coreCounts, presentKnown ? present : 0);
    if (!policy.updateBuiltinArchive("/data/vendor/gamepolicy", &error))
        LOG(WARNING) << "Cannot capture built-in policy: " << error;
    for (const auto& diagnostic : policy.diagnostics) LOG(WARNING) << diagnostic;
    LOG(INFO) << "Loaded game policy " << policy.version << ", " << policy.games.size()
              << " packages, " << policy.sceneLists.size() << " scene metadata profiles";
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    auto service = ndk::SharedRefBase::make<gamepolicy::Service>(std::move(policy), std::move(threads));
    service->loadState();
    const std::string name = std::string(gamepolicy::Service::descriptor) + "/default";
    if (AServiceManager_addService(service->asBinder().get(), name.c_str()) != STATUS_OK) return 1;
    auto tuning = ndk::SharedRefBase::make<gamepolicy::TuningBinder>(service);
    const std::string tuningName = std::string(gamepolicy::TuningBinder::descriptor) + "/default";
    // Session control keeps working when the tuning interface cannot be published.
    if (AServiceManager_addService(tuning->asBinder().get(), tuningName.c_str()) != STATUS_OK)
        LOG(ERROR) << "Cannot register " << tuningName;
    ABinderProcess_startThreadPool();
    service->run();
    return 0;
}
