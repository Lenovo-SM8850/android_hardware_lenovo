// SPDX-License-Identifier: Apache-2.0
#include "Policy.h"
#include "Threads.h"
#include <aidl/io/github/miner7222/gamepolicy/BnGamePolicy.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

using aidl::io::github::miner7222::gamepolicy::BnGamePolicy;
using aidl::io::github::miner7222::gamepolicy::GameSession;
using namespace std::chrono_literals;
namespace gamepolicy {
namespace {
std::string read(const std::string& path) {
    std::string data;
    if (!android::base::ReadFileToString(path, &data)) return {};
    return data;
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
        mHandle = 0; mArgs.clear();
    }
    void apply(const std::vector<int>& args) {
        if (args != mArgs) release();
        if (args.empty() || !mAcquire || !mRelease) return;
        auto writable = args;
        // Renew every sample; even a crash cannot leave a persistent frequency lock.
        const int handle = mAcquire(mHandle, 3000, writable.data(), writable.size());
        if (handle <= 0) { LOG(ERROR) << "perf_lock_acq failed: " << handle; release(); return; }
        mHandle = handle; mArgs = args;
    }
  private:
    using Acquire = int (*)(int, int, int*, int);
    using Release = int (*)(int);
    void* mLibrary = nullptr;
    Acquire mAcquire = nullptr;
    Release mRelease = nullptr;
    int mHandle = 0;
    std::vector<int> mArgs;
};
} // namespace

class Service final : public BnGamePolicy {
  public:
    Service(Policy policy, Threads threads) : mPolicy(std::move(policy)), mThreads(std::move(threads)) {
        mDeath = AIBinder_DeathRecipient_new([](void* cookie) {
            auto self = static_cast<Service*>(cookie);
            std::lock_guard lock(self->mMutex);
            self->clear();
            self->mClient = nullptr;
            self->mGeneration = 0;
        });
        // The service outlives its death recipient, so unlinking needs no cookie cleanup.
        AIBinder_DeathRecipient_setOnUnlinked(mDeath, [](void*) {});
        mCase = android::base::GetIntProperty("vendor.lenovo.game_thermal_case", -1);
        setCase(0);
    }
    ndk::ScopedAStatus setSession(const GameSession& session, const ndk::SpAIBinder& client) override {
        if (AIBinder_getCallingUid() != 1000) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        if (!client.get() || session.userId < 0 || session.uid < 0 || session.pid <= 0 ||
            session.uid / 100000 != session.userId || session.packageName.empty() || session.generation <= 0 ||
            session.processStartTimeTicks <= 0)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
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
        if (!same) {
            clear(false, !sameProcess);
            if (!sameProcess) mSessionStart = std::chrono::steady_clock::now();
            LOG(INFO) << "Game session uid=" << session.uid << " pid=" << session.pid
                      << " package=" << session.packageName << " mode=" << session.mode;
        }
        mSession = session;
        // Sample before the first request. Mode updates must not reuse a preceding band.
        update();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus stopSession(int64_t generation, const ndk::SpAIBinder& client) override {
        if (AIBinder_getCallingUid() != 1000) return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
        std::lock_guard lock(mMutex);
        if (client.get() == mClient.get() && generation > mGeneration) { mGeneration = generation; clear(); }
        return ndk::ScopedAStatus::ok();
    }
    void run() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(mPolicy.overlay.sampleMs));
            std::lock_guard lock(mMutex);
            update();
        }
    }
  private:
    bool adopt(const ndk::SpAIBinder& client) {
        if (client.get() == mClient.get()) return true;
        // Only one authority exists. Do not replace a live token with another caller.
        if (mClient.get()) return false;
        if (AIBinder_linkToDeath(client.get(), mDeath, this) != STATUS_OK) return false;
        mClient = client; mGeneration = 0; return true;
    }
    void clear(bool resetCase = true, bool resetThreads = true) {
        mPerf.release(); mSession.reset(); mBand = -1;
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
        if (!mSession) { mThreads.clear(); setCase(0); return; }
        const auto start = identity(mSession->pid, mSession->uid);
        if (!start || *start != mSession->processStartTimeTicks) { LOG(INFO) << "Game process exited"; clear(); return; }
        const auto selection = mPolicy.select(mSession->packageName, mSession->mode);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - mSessionStart).count();
        const bool threadsChanged = mThreads.apply(mSession->pid, *start, *selection.mode,
                                                  selection.threadPlacement, elapsed);
        if (threadsChanged || mLoggedMode != selection.mode) {
            LOG(INFO) << "Game rule set tids_bound=" << mThreads.bound() << " tids_rt=" << mThreads.realtime()
                      << " perf_hint_pairs=" << selection.mode->perfHints.size();
            mLoggedMode = selection.mode;
        }
        const auto before = std::chrono::steady_clock::now();
        auto temp = temperature();
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - before).count();
        const auto decision = Policy::evaluate(*selection.mode, temp, age, mBand, mPolicy.overlay.hysteresis);
        setCase(decision.thermalCase);
        if (!temp || age > 1500) {
            if (!mSensorFailed) LOG(WARNING) << "Invalid/stale game temperature; releasing frequency request";
            mSensorFailed = true; mPerf.release(); mBand = -1; return;
        }
        mSensorFailed = false;
        const int next = decision.band;
        if (next != mBand) LOG(INFO) << "Game profile=" << selection.profile << " band=" << next
                                    << " temperature=" << *temp << " case=" << selection.mode->thermalCase;
        mBand = next;
        mPerf.apply(decision.resources);
    }
    Policy mPolicy;
    Perf mPerf;
    Threads mThreads;
    std::mutex mMutex;
    std::optional<GameSession> mSession;
    ndk::SpAIBinder mClient;
    AIBinder_DeathRecipient* mDeath;
    int64_t mGeneration = 0;
    std::chrono::steady_clock::time_point mSessionStart;
    const Mode* mLoggedMode = nullptr;
    std::string mSensor;
    int mCase = -1, mBand = -1;
    bool mSensorFailed = false;
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
    policy.resolve(opps);
    for (const auto& diagnostic : policy.diagnostics) LOG(WARNING) << diagnostic;
    LOG(INFO) << "Loaded game policy " << policy.version << ", " << policy.games.size() << " packages";
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    auto service = ndk::SharedRefBase::make<gamepolicy::Service>(std::move(policy), std::move(threads));
    const std::string name = std::string(gamepolicy::Service::descriptor) + "/default";
    if (AServiceManager_addService(service->asBinder().get(), name.c_str()) != STATUS_OK) return 1;
    ABinderProcess_startThreadPool();
    service->run();
    return 0;
}
