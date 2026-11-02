// SPDX-License-Identifier: Apache-2.0
#include "Tests.h"
#include "Recorder.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/resource.h>
#ifdef GAMEPOLICY_IO_FAULT_TEST
#include <cstdarg>
#include <cerrno>
#include <fcntl.h>
namespace {
bool failDirectoryOpen = false, failDirectorySync = false;
}
extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_fsync(int);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (failDirectoryOpen && (flags & O_DIRECTORY)) { errno = EIO; return -1; }
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        const mode_t mode = va_arg(args, int); va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}
extern "C" int __wrap_fsync(int fd) {
    struct stat st;
    if (failDirectorySync && ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
void archiveIoFault(bool failOpen, bool failSync) {
    failDirectoryOpen = failOpen; failDirectorySync = failSync;
}
#endif

using namespace gamepolicy;
namespace {
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

const Ladders kLadders{std::vector<int>{1000, 2000, 3000}, std::vector<int>{1000, 2000, 3000},
                       std::vector<int>{100, 200, 300}};
constexpr int64_t kEpoch = 1793300000000;
constexpr const char* kGame = "com.example.game";

ProfileView profile() {
    ProfileView p;
    p.profileKey = kGame; p.baseMode = 2; p.tuned = true; p.thermalCase = 200; p.uclampMin = 5;
    p.gpuDefaultHz = 500000000; p.keepCoresOnline = true; p.frameBoostSteps = {2, 2, 2}; p.jankBoostSteps = {3, 3, 3};
    p.startDelayMs = 3000; p.threadRules = 1; p.threadRuleCount = 5; p.sceneCount = 2;
    p.steps.push_back({0, {Bounds{1000, 3000}, Bounds{-1, 2000}, Bounds{200, 300}}});
    p.steps.push_back({35.5, {Bounds{1000, 2000}, Bounds{-1, 1000}, Bounds{100, 200}}});
    p.boostLengthMs = 700; p.dropSensitivity = -1; p.dropLowerPermille = 600; p.dropUpperPermille = 980; p.hysteresisDeciC = 15; p.skinTargetMilliC = 42500; p.powerTargetMw = 6500;
    p.sceneMode = 1; p.variantId = "perf_120"; p.variantName = "Performance, 120 fps hot";
    p.scenes = {{1, true, 500, p.steps[0].units}, {2, false, 1000, p.steps[1].units}};
    p.threads = {{"Render,Thread", 0xC0, 95}, {"worker", 1ULL << 63, -1}};
    return p;
}
bool sameProfile(const ProfileView& a, const ProfileView& b) {
    if (a.profileKey != b.profileKey || a.baseMode != b.baseMode || a.tuned != b.tuned || a.thermalCase != b.thermalCase ||
        a.uclampMin != b.uclampMin || a.gpuDefaultHz != b.gpuDefaultHz || a.keepCoresOnline != b.keepCoresOnline ||
        a.frameBoostSteps != b.frameBoostSteps || a.jankBoostSteps != b.jankBoostSteps || a.startDelayMs != b.startDelayMs ||
        a.threadRules != b.threadRules || a.threadRuleCount != b.threadRuleCount || a.sceneCount != b.sceneCount ||
        a.boostLengthMs != b.boostLengthMs || a.dropSensitivity != b.dropSensitivity || a.dropLowerPermille != b.dropLowerPermille || a.dropUpperPermille != b.dropUpperPermille || a.hysteresisDeciC != b.hysteresisDeciC ||
        a.skinTargetMilliC != b.skinTargetMilliC || a.powerTargetMw != b.powerTargetMw || a.sceneMode != b.sceneMode ||
        a.variantId != b.variantId || a.variantName != b.variantName || a.steps.size() != b.steps.size()) return false;
    for (size_t i = 0; i < a.steps.size(); ++i) {
        if (std::fabs(a.steps[i].temperature - b.steps[i].temperature) > 1e-9) return false;
        for (int r = 0; r < 3; ++r)
            if (a.steps[i].units[r].floor != b.steps[i].units[r].floor || a.steps[i].units[r].ceiling != b.steps[i].units[r].ceiling)
                return false;
    }
    return true;
}
bool sameSample(const Sample& a, const Sample& b) {
    return a.timeMs == b.timeMs && a.fps == b.fps && a.fpsMin == b.fpsMin && a.jankFrames == b.jankFrames &&
           a.frames == b.frames && a.cur == b.cur && a.cap == b.cap && a.skinMilliC == b.skinMilliC &&
           a.cpuMilliC == b.cpuMilliC && a.gpuMilliC == b.gpuMilliC && a.powerMw == b.powerMw && a.band == b.band &&
           a.thermalCase == b.thermalCase && a.flags == b.flags && a.mode == b.mode &&
           a.profileGeneration == b.profileGeneration && a.sceneId == b.sceneId && a.maxFrameTimeUs == b.maxFrameTimeUs &&
           a.cpuUtilPermille == b.cpuUtilPermille && a.gpuBusyPercent == b.gpuBusyPercent &&
           a.batteryMilliC == b.batteryMilliC && a.batteryPercent == b.batteryPercent &&
           a.requestedFps == b.requestedFps && a.throttleFps == b.throttleFps;
}

// Feeds `count` seconds of synthetic data: one frame report and one sample per second.
struct Feeder {
    Recorder& recorder;
    int64_t startNow;
    int64_t now;
    int counter = 0;
    std::vector<Sample> expected;
    Feeder(Recorder& r, int64_t start) : recorder(r), startNow(start), now(start) {}
    void feed(int count, int64_t step = 1000) {
        for (int n = 0; n < count; ++n, ++counter) {
            now += step;
            const float fps = static_cast<float>(300 + counter % 100) / 10.f;
            const int maxFt = counter % 7 == 0 ? -1 : 10000 + counter;
            const float requested = counter % 5 == 0 ? -1.f : 29.97f + static_cast<float>(counter % 4) * 15.f;
            const float throttle = counter % 6 == 0 ? 45.5f : -1.f;
            recorder.frame(fps, counter % 3, 30 + counter % 5, maxFt, requested, throttle);
            require(recorder.due(now), "A sample must be due after one period");
            Sample in;
            in.cur = {1000 + counter % 3 * 1000, 2000, 100 + counter % 3 * 100};
            in.cap = {3000, counter % 2 ? -1 : 2000, 300};
            in.skinMilliC = 30000 + counter; in.cpuMilliC = counter % 4 ? 41000 + counter : kTemperatureUnavailable;
            in.gpuMilliC = 40000; in.powerMw = counter % 5 ? 3000 + counter : -1; in.band = counter % 3 - 1;
            in.thermalCase = 200; in.flags = kFlagReady | (counter % 2 ? kFlagScene : 0); in.mode = 1; in.profileGeneration = 1;
            in.sceneId = counter % 4 == 0 ? -1 : counter % 4; in.cpuUtilPermille = {counter % 1000, 999 - counter % 1000};
            in.gpuBusyPercent = counter % 101; in.batteryMilliC = 31000 + counter % 50; in.batteryPercent = 80 - counter / 100;
            recorder.sample(in, now);
            Sample out = in;
            out.timeMs = static_cast<int>(now - startNow);
            out.fps = out.fpsMin = fps;
            out.jankFrames = counter % 3; out.frames = 30 + counter % 5; out.maxFrameTimeUs = maxFt;
            out.requestedFps = requested; out.throttleFps = throttle;
            expected.push_back(out);
        }
    }
};

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::vector<std::string> names(const fs::path& dir, const std::string& suffix) {
    std::vector<std::string> out;
    if (!fs::exists(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}
void startSession(Recorder& r, int user, const std::string& pkg, int64_t now, int64_t epoch, int pid = 100, int64_t ticks = 555) {
    r.start(user, pkg, pid, ticks, 1, profile(), now, epoch);
    require(r.active() && !r.paused(), "Session starts active");
}

void lifecycle(const fs::path& root) {
    Recorder r(root.string());
    require(!r.active() && !r.paused() && !r.deadline() && !r.due(0) && !r.expired(0), "Idle recorder");
    r.finish(kLadders);   // No-op without a session.
    constexpr int64_t kStart = 1000000;
    startSession(r, 0, kGame, kStart, kEpoch);
    require(r.deadline() && *r.deadline() == kStart + 1000, "First sample 1 s after the start");
    require(!r.due(kStart + 999) && r.due(kStart + 1000), "Due exactly at the deadline");
    const fs::path dir = root / "u0";
    require(fs::exists(dir / (std::to_string(kEpoch) + "_" + kGame + ".rec.partial")), "Partial file created at start");
    {   // Some filesystems (for example a Windows mount) ignore modes; probe before asserting.
        const fs::path probe = root / "mode-probe";
        ::mkdir(probe.c_str(), 0700);
        const auto open = fs::perms::group_all | fs::perms::others_all;
        if ((fs::status(probe).permissions() & open) == fs::perms::none)
            require((fs::status(dir).permissions() & open) == fs::perms::none, "User directory is private");
    }

    Feeder feeder(r, kStart);
    feeder.feed(45);
    require(r.active(), "Still active after 45 samples");
    const std::string partial = slurp(dir / (std::to_string(kEpoch) + "_" + kGame + ".rec.partial"));
    require(std::count(partial.begin(), partial.end(), '\n') > 30, "Partial holds the flushed samples");
    r.pause(feeder.now, kEndStopped);
    require(r.paused() && !r.active() && r.deadline() && *r.deadline() == feeder.now + 60000, "Pause deadline is 60 s");
    require(!r.due(feeder.now + 5000) && !r.expired(feeder.now + 59999) && r.expired(feeder.now + 60000), "Pause expiry");
    r.frame(60, 0, 60);   // Ignored while paused.
    require(!r.resume(0, kGame, 101, 555, feeder.now + 1000) && !r.resume(0, kGame, 100, 556, feeder.now + 1000) &&
            !r.resume(0, "other.game", 100, 555, feeder.now + 1000) && !r.resume(1, kGame, 100, 555, feeder.now + 1000),
            "Resume needs the same user, package, pid and start ticks");
    require(r.paused(), "Failed resumes keep the pause");
    feeder.now += 30000;   // The gap shows as a jump in timeMs.
    require(r.resume(0, kGame, 100, 555, feeder.now) && r.active(), "Resume within 60 s");
    require(*r.deadline() == feeder.now + 1000 && !r.resume(0, kGame, 100, 555, feeder.now), "Cadence restarts after a resume");
    feeder.feed(40);
    r.pause(feeder.now, kEndExited);
    r.pause(feeder.now + 5, kEndStopped);   // A second pause keeps the first reason.
    r.finish(kLadders);
    require(!r.active() && !r.paused(), "Finished");

    const std::string id = std::to_string(kEpoch) + "_" + kGame;
    require(names(dir, ".rec") == std::vector<std::string>{id + ".rec"} && names(dir, ".partial").empty() && names(dir, ".tmp").empty(),
            "Exactly one record and no leftovers");
    const auto listed = r.list(0, "");
    require(listed.size() == 1 && listed[0].id == id, "list finds the record");
    const RecordHeader& h = listed[0];
    require(h.userId == 0 && h.package == kGame && h.mode == 1 && h.startEpochMs == kEpoch && h.sampleCount == 85 &&
            h.durationMs == 45000 + 30000 + 40000 && h.endReason == kEndExited, "Header fields");
    require(sameProfile(h.profile, profile()), "Profile round trip, including expert fields and variant");
    std::vector<Sample> samples;
    require(r.read(0, id, &samples) && samples.size() == 85, "read returns 85 rows");
    require(samples[44].timeMs == 45000 && samples[45].timeMs == 45000 + 30000 + 1000, "timeMs continues across the pause");
    for (size_t i = 0; i < samples.size(); ++i)
        require(sameSample(samples[i], feeder.expected[i]), "Row " + std::to_string(i) + " round trip");
    const Summary s = summarize(samples, kLadders);
    require(h.summary.validSamples == s.validSamples && h.summary.validSamples > 70 && h.summary.targetFps == s.targetFps &&
            h.summary.avgFps == s.avgFps && h.summary.hitRate == s.hitRate && h.summary.floor5Fps == s.floor5Fps &&
            h.summary.tail95 == s.tail95 && h.summary.jankPer1000 == s.jankPer1000 && h.summary.scenesSeen == s.scenesSeen &&
            h.summary.avgCur == s.avgCur && h.summary.pegged == s.pegged && h.summary.avgPowerMw == s.avgPowerMw &&
            h.summary.maxSkinC == s.maxSkinC && h.summary.mixed == s.mixed && h.summary.scenesSeen == std::vector<int>({1, 2, 3}),
            "Stored summary equals summarize() of the stored rows");
    RecordHeader one;
    require(r.header(0, id, &one) && one.id == id && one.sampleCount == 85 && one.package == kGame, "header()");
    require(r.list(0, "other.game").empty() && r.list(0, kGame).size() == 1 && r.list(3, "").empty(), "list filters");

    // The text format keeps the documented layout.
    const std::string text = slurp(dir / (id + ".rec"));
    require(text.rfind("game-policy-record 2\nuser=0\npackage=com.example.game\nmode=1\nstart=1793300000000\n", 0) == 0, "Record header lines");
    require(text.find("\nprofile=com.example.game,2,1,200,5,500000000,1,2;2;2,3;3;3,3000,1,5,2\n") != std::string::npos, "profile= encoding");
    require(text.find("\nstep=0,1000,3000,-1,2000,200,300\nstep=35.5,1000,2000,-1,1000,100,200\n") != std::string::npos, "step= encoding");
    require(text.find("\nend=2\nsamples\nt,fps,fpsMin,jank,frames,gold,prime,gpu,goldCap,primeCap,gpuCap,skin,cpu,gpu,power,band,case,flags,mode,gen,") != std::string::npos,
            "end= and column header");
    require(text.find("\n1000,30.0,30.0,0,30,1000,2000,100,3000,-1,300,30000,-273000,40000,-1,-1,200,1,1,1,") != std::string::npos ||
            text.find("\n1000,30.0,30.0,0,30,1000,2000,100,3000,2000,300,30000,-273000,40000,-1,-1,200,1,1,1,") != std::string::npos,
            "First row layout with one decimal fps");
    require(text.find("\nsummary=") != std::string::npos && text.back() == '\n', "summary= line");

    // remove
    require(r.remove(0, id) && !r.remove(0, id) && !r.header(0, id, nullptr) && r.list(0, "").empty(), "remove");
}

void cadenceAndFrames(const fs::path& root) {
    Recorder r(root.string());
    constexpr int64_t kStart = 5000000;
    startSession(r, 2, kGame, kStart, kEpoch + 1);
    auto take = [&](int64_t now) { Sample s; s.flags = kFlagReady; r.sample(s, now); };
    r.frame(60, 1, 60, 16000);
    r.frame(58, 2, 59, 25000);
    r.frame(std::numeric_limits<float>::quiet_NaN(), 9, 9, 99000);
    r.frame(std::numeric_limits<float>::infinity(), 9, 9, 99000);
    r.frame(-1, 9, 9, 99000);
    take(kStart + 1000);
    require(*r.deadline() == kStart + 2000, "Next due one period after the previous due time");
    take(kStart + 2300);   // 300 ms late: the schedule does not drift.
    require(*r.deadline() == kStart + 3000, "No drift after a short delay");
    take(kStart + 3000);
    require(*r.deadline() == kStart + 4000, "On time");
    take(kStart + 5500);   // More than one period missed: one sample, reschedule from now.
    require(*r.deadline() == kStart + 6500, "Reschedule from now after a missed period");
    require(!r.due(kStart + 6499) && r.due(kStart + 6500), "Due check after reschedule");
    r.pause(kStart + 5500, kEndStopped);
    r.finish(kLadders);   // 5.5 s: discarded.
    require(r.list(2, "").empty(), "Short record discarded");

    // Accumulator values land in the first sample; an empty second gives fps = -1.
    startSession(r, 2, kGame, kStart, kEpoch + 2);
    r.frame(60, 1, 60, 16000);
    r.frame(58.04f, 2, 59, 25000);
    take(kStart + 1000);
    take(kStart + 2000);
    for (int i = 3; i <= 30; ++i) { r.frame(40, 0, 40); take(kStart + 1000 * i); }
    r.pause(kStart + 30000, kEndStopped);
    r.finish(kLadders);
    const auto list = r.list(2, "");
    require(list.size() == 1, "Thirty seconds is kept");
    std::vector<Sample> rows;
    require(r.read(2, list[0].id, &rows) && rows.size() == 30, "30 rows");
    require(sameConfiguration(list[0].profile, profile()), "Full scene and thread configuration survives recording");
    require(rows[0].fps == 59.0f && rows[0].fpsMin == 58.0f && rows[0].jankFrames == 3 && rows[0].frames == 119 &&
            rows[0].maxFrameTimeUs == 25000, "Accumulated second: mean, min, sums, max");
    require(rows[1].fps == -1 && rows[1].fpsMin == -1 && rows[1].jankFrames == 0 && rows[1].frames == 0 &&
            rows[1].maxFrameTimeUs == -1, "Empty second");
    require(rows[2].fps == 40.0f && rows[2].maxFrameTimeUs == -1 && rows[29].timeMs == 30000, "Accumulator resets after each sample");
    // 29 samples (29 s) are discarded.
    startSession(r, 2, kGame, kStart, kEpoch + 3);
    for (int i = 1; i <= 29; ++i) take(kStart + 1000 * i);
    r.finish(kLadders);
    require(r.list(2, "").size() == 1 && names(root / "u2", ".partial").empty(), "29 s discarded with its partial file");
}

void checkpointFailures(const fs::path& root) {
    for (bool retry : {false, true}) {
        const fs::path dir = root / (retry ? "retry" : "crash");
        {
            Recorder r(dir.string());
            startSession(r, 0, kGame, 0, kEpoch);
            Feeder feeder(r, 0); feeder.feed(30);
            const fs::path partial = dir / "u0" / (std::to_string(kEpoch) + "_" + kGame + ".rec.partial");
            const std::string good = slurp(partial);
            struct rlimit original;
            require(::getrlimit(RLIMIT_FSIZE, &original) == 0, "Read file-size limit");
            auto oldSignal = std::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limited = original; limited.rlim_cur = good.size() + 13;
            require(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "Inject short checkpoint write");
            feeder.feed(30);   // Short write followed by EFBIG, exercising the real write failure path.
            const int restored = ::setrlimit(RLIMIT_FSIZE, &original);
            std::signal(SIGXFSZ, oldSignal);
            require(restored == 0, "Restore file-size limit");
            require(slurp(partial) == good, "Failed checkpoint leaves last good rows intact");
            if (retry) r.pause(feeder.now, kEndStopped);
        }
        Recorder recovered(dir.string()); recovered.recover(kLadders);
        const auto records = recovered.list(0, "");
        std::vector<Sample> rows;
        require(records.size() == 1 && recovered.read(0, records[0].id, &rows) && rows.size() == (retry ? 60u : 30u),
                "Failed checkpoint recovers whole rows, with or without retry");
        for (size_t i = 0; i < rows.size(); ++i)
            require(rows[i].timeMs == static_cast<int>((i + 1) * 1000), "Retry does not duplicate or split samples");
    }
}

void largeCounters(const fs::path& root) {
    Recorder r(root.string());
    startSession(r, 0, kGame, 0, kEpoch);
    for (int second = 1; second <= 30; ++second) {
        r.frame(60, INT32_MAX, INT32_MAX);
        r.frame(60, INT32_MAX, INT32_MAX);
        r.frame(60, 1, 1);
        Sample s; s.flags = kFlagReady; r.sample(s, second * 1000);
    }
    r.finish(kLadders);
    const auto records = r.list(0, ""); std::vector<Sample> rows;
    require(records.size() == 1 && r.read(0, records[0].id, &rows), "Large counters still produce readable recording");
    for (const auto& row : rows)
        require(row.frames == INT32_MAX && row.jankFrames == INT32_MAX, "Frame and jank counters saturate without overflow");
    require(records[0].summary.jankPercent == 100, "Saturated counters summarize correctly");
}

void crashRecovery(const fs::path& root) {
    const fs::path dir = root / "u4";
    {
        Recorder crashed(root.string()), crashedShort(root.string());
        startSession(crashed, 4, kGame, 7000000, kEpoch + 10);
        Feeder feeder(crashed, 7000000);
        feeder.feed(70);   // 60 rows reach the partial file (flush every 30 samples).
        startSession(crashedShort, 4, "short.game", 8000000, kEpoch + 11);
        Feeder small(crashedShort, 8000000);
        small.feed(25);
        // Dropped without finish(): the process "crashed".
    }
    const std::string id = std::to_string(kEpoch + 10) + "_" + kGame;
    require(names(dir, ".partial").size() == 2, "Two partial files after the crash");
    // A torn last line, a garbage file, and a leftover temp file.
    {
        std::ofstream out(dir / (id + ".rec.partial"), std::ios::app | std::ios::binary);
        out << "61000,31.0,31.0,0,3";
    }
    put(dir / "1793300999999_bad.game.rec.partial", "garbage\nnot a record\n");
    put(dir / "1793300999998_tmp.game.rec.tmp", "x");
    Recorder fresh(root.string());
    fresh.recover(kLadders);
    require(names(dir, ".partial").empty() && names(dir, ".tmp").empty(), "Recovery clears partial and temp files");
    require(names(dir, ".rec") == std::vector<std::string>{id + ".rec"}, "Only the long partial became a record");
    RecordHeader h;
    require(fresh.header(4, id, &h) && h.endReason == kEndInterrupted && h.sampleCount == 60 && h.durationMs == 60000 &&
            h.package == kGame && sameProfile(h.profile, profile()), "Recovered header end=4");
    std::vector<Sample> rows;
    require(fresh.read(4, id, &rows) && rows.size() == 60 && rows[59].timeMs == 60000, "Recovered rows");
    require(h.summary.validSamples == summarize(rows, kLadders).validSamples && h.summary.validSamples > 0, "Recovered summary");
    // Recovering twice changes nothing.
    fresh.recover(kLadders);
    require(names(dir, ".rec").size() == 1, "Recovery is idempotent");
    // A finished record next to its partial (crash between rename and unlink) keeps the finished one.
    const std::string finished = slurp(dir / (id + ".rec"));
    std::string stale = finished;
    stale.erase(stale.find("end=4\n"), 6);
    stale.erase(stale.find("summary="));
    put(dir / (id + ".rec.partial"), stale);
    fresh.recover(kLadders);
    require(names(dir, ".partial").empty() && slurp(dir / (id + ".rec")) == finished,
            "Stale partial next to a finished record is dropped");
}

void retention(const fs::path& root) {
    Recorder r(root.string());
    for (int i = 0; i < 21; ++i) {
        startSession(r, 5, kGame, 100000, kEpoch + 1000 + i);
        Feeder f(r, 100000);
        f.feed(30);
        r.pause(f.now, kEndStopped);
        r.finish(kLadders);
        require(r.list(5, "").size() == static_cast<size_t>(std::min(i + 1, 20)), "Retention count after record " + std::to_string(i));
    }
    const auto all = r.list(5, "");
    require(all.size() == 20 && all.front().startEpochMs == kEpoch + 1020 && all.back().startEpochMs == kEpoch + 1001,
            "The oldest record was deleted, newest first");
    require(r.list(5, kGame).size() == 20 && r.list(5, "x").empty(), "Package filter");
    // Other users keep their own quota.
    startSession(r, 6, kGame, 100000, kEpoch);
    Feeder f(r, 100000);
    f.feed(30);
    r.finish(kLadders);
    require(r.list(6, "").size() == 1 && r.list(5, "").size() == 20, "Per-user retention");
}

void sampleLimit(const fs::path& root) {
    Recorder r(root.string());
    startSession(r, 7, kGame, 0, kEpoch);
    for (int i = 1; i <= 7200; ++i) {
        Sample s; s.flags = kFlagReady;
        r.frame(60, 0, 60);
        r.sample(s, 1000LL * i);
    }
    require(r.active(), "Active after 7200 samples");
    r.frame(60, 0, 60);
    Sample s; s.flags = kFlagReady;
    r.sample(s, 7201000);
    require(r.paused() && !r.active(), "The 7201st sample call pauses");
    r.sample(s, 7202000);   // Ignored while paused.
    require(!r.resume(0 + 7, kGame, 100, 555, 7201500), "A full record cannot be resumed");
    r.finish(kLadders);
    const auto list = r.list(7, "");
    require(list.size() == 1 && list[0].sampleCount == 7200 && list[0].endReason == kEndLimit && list[0].durationMs == 7200000,
            "7200 rows with end=3");
    require(list[0].summary.validSamples == 7200 - 9, "Summary over 7200 rows");
    std::vector<Sample> rows;
    require(r.read(7, list[0].id, &rows) && rows.size() == 7200 && rows.back().timeMs == 7200000, "Read 7200 rows");
    require(fs::file_size(root / "u7" / (list[0].id + ".rec")) < 1000000, "A full record stays below 1 MB");
}

void ids(const fs::path& root) {
    Recorder r(root.string());
    startSession(r, 8, kGame, 0, kEpoch);
    Feeder f(r, 0);
    f.feed(30);
    r.finish(kLadders);
    const std::string good = std::to_string(kEpoch) + "_" + kGame;
    put(root / "u8" / "evil", "x");
    put(root / "secret.rec", "x");
    for (const std::string& bad : {std::string(), std::string("1"), std::string("_a"), std::string("1_"), std::string("a_b"),
                                  std::string("../secret"), std::string("../u8/") + good, std::string("1_a/b"),
                                  std::string("1_a b"), std::string("1_a\n"), std::string(good + ".rec"),
                                  std::string("1_a-b"), std::string(1, '\0'), std::string("..")}) {
        std::vector<Sample> rows;
        RecordHeader h;
        require(!r.read(8, bad, &rows) && !r.header(8, bad, &h) && !r.remove(8, bad), "Rejected id '" + bad + "'");
    }
    require(fs::exists(root / "secret.rec") && fs::exists(root / "u8" / "evil") && r.header(8, good, nullptr), "Rejected ids touch nothing");
    require(!r.read(-1, good, nullptr) && !r.read(9, good, nullptr) && !r.header(9, good, nullptr), "Wrong user");
    require(r.read(8, good, nullptr), "Valid id");
}

void oldFormat(const fs::path& root) {
    const std::string id = "1793300000001_com.old.game";
    put(root / "u9" / (id + ".rec"),
        "game-policy-record 1\nuser=9\npackage=com.old.game\nmode=2\nstart=1793300000001\n"
        "profile=com.old.game,1,0,200,-1,-1,0,,,0,1,5,0\nstep=0,1324800,1996800,-1,768000,726000000,726000000\n"
        "step=35,1324800,1900800,-1,768000,726000000,726000000\nend=1\nsamples\n"
        "t,fps,fpsMin,jank,frames,gold,prime,gpu,goldCap,primeCap,gpuCap,skin,cpu,gpu,power,band,case,flags,mode,gen\n"
        "10000,59.8,58.9,0,60,1996800,768000,726000000,1996800,768000,726000000,36125,48300,41200,-1,0,200,1,1,1\n"
        "11000,-1.0,-1.0,0,0,1996800,768000,726000000,-1,-1,-1,-273000,-273000,-273000,4100,1,200,1,1,1\n"
        "summary=2,59.8,59.8,59.8,59.8,60,1,1,0,36.125,36.125,48.3,41.2,-1,1996800,768000,726000000,0,1,1,0.5,0\n");
    Recorder r(root.string());
    RecordHeader h;
    std::vector<Sample> rows;
    require(r.header(9, id, &h) && r.read(9, id, &rows) && rows.size() == 2, "Old files stay readable");
    require(h.mode == 2 && h.endReason == kEndStopped && h.profile.steps.size() == 2 && h.profile.steps[1].temperature == 35 &&
            h.profile.boostLengthMs == 0 && h.profile.skinTargetMilliC == -1 && h.profile.variantId.empty(), "Old header defaults");
    require(rows[0].sceneId == -1 && rows[0].maxFrameTimeUs == -1 && rows[0].cpuUtilPermille == std::array<int, 2>({-1, -1}) &&
            rows[0].gpuBusyPercent == -1 && rows[0].batteryMilliC == kTemperatureUnavailable && rows[0].batteryPercent == -1 &&
            rows[0].fps == 59.8f && rows[0].cur[0] == 1996800 && rows[1].fps == -1 && rows[1].powerMw == 4100, "Old rows get defaults");
    require(h.summary.validSamples == 2 && h.summary.targetFps == 60 && h.summary.floor5Fps == 59.8f && h.summary.tail95 == -1 &&
            h.summary.pegged[1] == 1 && h.summary.throttled == 0.5f && h.summary.scenesSeen.empty() && !h.summary.mixed,
            "Old 22-field summary");
    // Version 1 records carry no frame rate signals: both read back as -1, and the steady ratio is derived.
    require(rows[0].requestedFps == -1 && rows[0].throttleFps == -1 && rows[1].requestedFps == -1 &&
            h.summary.floorRatio == -1 && std::fabs(steadyRatio(h.summary) - 59.8f / 60.f) < 1e-6, "Version 1 signals default to -1");
    const std::string id27 = "1793300000004_com.old.game";
    put(root / "u9" / (id27 + ".rec"),
        "game-policy-record 1\nuser=9\npackage=com.old.game\nmode=2\nstart=1793300000004\n"
        "profile=com.old.game,1,0,200,-1,-1,0,,,0,1,5,0\nstep=0,1324800,1996800,-1,768000,726000000,726000000\nend=1\nsamples\n"
        "t,fps,fpsMin,jank,frames,gold,prime,gpu,goldCap,primeCap,gpuCap,skin,cpu,gpu,power,band,case,flags,mode,gen,scene,maxFt,"
        "utilGold,utilPrime,gpuBusy,battMilliC,battPct\n"
        "10000,59.8,58.9,0,60,1996800,768000,726000000,1996800,768000,726000000,36125,48300,41200,-1,0,200,1,1,1,3,25000,100,200,50,31000,80\n"
        "summary=1,59.8,59.8,59.8,59.8,60,1,1,0,36.125,36.125,48.3,41.2,-1,1996800,768000,726000000,0,1,1,0,0,59.8,-1,0,\n");
    std::vector<Sample> rows27;
    require(r.read(9, id27, &rows27) && rows27.size() == 1 && rows27[0].sceneId == 3 && rows27[0].batteryPercent == 80 &&
            rows27[0].requestedFps == -1 && rows27[0].throttleFps == -1, "Version 1 rows with 27 columns");
    // A version 2 file needs all 29 columns.
    std::string v2 = slurp(root / "u9" / (id27 + ".rec"));
    v2.replace(0, 20, "game-policy-record 2");
    put(root / "u9" / "1793300000005_com.old.game.rec", v2);
    require(!r.read(9, "1793300000005_com.old.game", nullptr), "Version 2 rejects short rows");
    // A damaged record is skipped by list() and never read.
    put(root / "u9" / "1793300000002_com.bad.game.rec", "game-policy-record 1\nuser=9\n");
    put(root / "u9" / "1793300000003_com.bad.game.rec", slurp(root / "u9" / (id + ".rec")) + "extra\n");
    require(r.list(9, "").size() == 2 && !r.read(9, "1793300000002_com.bad.game", nullptr) &&
            !r.header(9, "1793300000003_com.bad.game", nullptr), "Damaged records are ignored");
}

void resumeConfiguration(const fs::path& root) {
    for (bool change : {false, true}) {
        Recorder r((root / (change ? "changed" : "unchanged")).string());
        const ProfileView before = profile();
        auto applied = before;
        int generation = nextProfileGeneration(0, 0, {}, 1, before);
        startSession(r, 0, kGame, 0, kEpoch);
        int64_t now = 0;
        for (int second = 1; second <= 150; ++second) {
            if (second == 76) {
                r.pause(now, kEndStopped);
                now += 10000;
                generation = nextProfileGeneration(generation, 1, applied, 1, applied);
                require(r.resume(0, kGame, 100, 555, now), "Same game process resumes the recording");
            }
            if (change && second == 100) {
                auto next = applied; ++next.steps[0].units[2].ceiling;
                generation = nextProfileGeneration(generation, 1, applied, 1, next);
                applied = std::move(next);
            }
            now += 1000;
            r.frame(60, 0, 60);
            Sample sample; sample.flags = kFlagReady; sample.mode = 1; sample.profileGeneration = generation;
            r.sample(sample, now);
        }
        r.finish(kLadders);
        const auto records = r.list(0, kGame);
        std::vector<Sample> samples;
        require(records.size() == 1 && records[0].sampleCount == 150 && r.read(0, records[0].id, &samples),
                "Pause/resume produces one complete recording");
        require(records[0].summary.validSamples >= kMinValidSamples && records[0].summary.mixed == change,
                "Only a real configuration change makes the resumed recording mixed");
        require(samples[74].profileGeneration == samples[75].profileGeneration &&
                samples.back().profileGeneration == (change ? 2 : 1), "Recorded generations reflect configuration changes");
    }
    std::cout << "RECORDER_RESUME_CONFIG_OK unchanged-clean changed-mixed\n";
}

void keepRanges(const fs::path& root) {
    Recorder r(root.string());
    startSession(r, 0, kGame, 0, kEpoch);
    Feeder f(r, 0); f.feed(150); r.finish(kLadders);
    auto h = r.list(0, "").front();
    const auto id = h.id;
    require(h.originalDurationMs == 150000 && h.keepEndMs == 150000 && h.rangeRevision == 0, "Legacy full bounds");
    require(r.keepRange(0, id, 20500, 120500, 0, kLadders, &h) == 0, "Keep range");
    require(h.keepStartMs == 21000 && h.keepEndMs == 120000 && h.durationMs == 99000 &&
            h.sampleCount == 100 && h.summary.validSamples == 100 && h.rangeRevision == 1, "Inclusive normalized range");
    std::vector<Sample> selected, raw;
    require(r.read(0, id, &selected) && r.raw(0, id, &raw) && raw.size() == 150 && selected.size() == 100 &&
            selected.front().timeMs == 21000 && selected.front().selectedGameplay, "Raw retained, original validity and gameplay provenance");
    require(r.keepRange(0, id, 0, 150000, 0, kLadders, &h) == 3, "Stale edit rejected");
    require(r.keepRange(0, id, 0, 9000, 1, kLadders, &h) == 2, "No usable selection rejected");
    require(r.keepRange(0, id, -1, 10000, 1, kLadders, &h) == 2 &&
            r.keepRange(0, id, 50000, 20000, 1, kLadders, &h) == 2 &&
            r.keepRange(1, id, 0, 150000, 1, kLadders, &h) == 1, "Invalid bounds/user");
    Recorder restarted(root.string()); restarted.recover(kLadders);
    require(restarted.header(0, id, &h) && h.rangeRevision == 1 && h.summary.validSamples == 100, "Range survives recovery");
    require(restarted.keepRange(0, id, 0, 150000, 1, kLadders, &h) == 0 && h.rangeRevision == 2 && h.sampleCount == 150,
            "Reset retains raw rows and advances revision");
    require(restarted.keepRange(0, id, 0, 150000, 2, kLadders, &h) == 0 && h.rangeRevision == 2, "Unchanged range is no-op");
    require(restarted.raw(0, id, &raw) && sameSample(raw[20], f.expected[20]), "Mutation never rewrites raw values");
    require(::chmod((root / "u0").c_str(), 0500) == 0, "Make range storage read-only");
    require(restarted.keepRange(0, id, 30000, 60000, 2, kLadders, &h) == 4, "Storage failure rejected");
    require(::chmod((root / "u0").c_str(), 0700) == 0, "Restore range storage");
    require(restarted.header(0, id, &h) && h.rangeRevision == 2 && h.sampleCount == 150, "Failed mutation retains previous full view");
#ifdef GAMEPOLICY_IO_FAULT_TEST
    for (bool openFailure : {true, false}) {
        const int64_t previous = h.rangeRevision;
        failDirectoryOpen = openFailure; failDirectorySync = !openFailure;
        require(restarted.keepRange(0, id, 30000, openFailure ? 60000 : 70000, previous, kLadders, &h) == 5,
                "Post-rename directory failure must not report success");
        require(h.rangeRevision == previous + 1 && h.keepStartMs == 30000, "Published revision reconciled after durability failure");
        RecordHeader visible;
        require(restarted.header(0, id, &visible) && visible.rangeRevision == h.rangeRevision &&
                visible.keepEndMs == h.keepEndMs, "Failed sync leaves a complete visible range");
        require(restarted.keepRange(0, id, 0, 150000, previous, kLadders, &visible) == 3,
                "Old revision rejected after published failure");
        require(restarted.keepRange(0, id, h.keepStartMs, h.keepEndMs, h.rangeRevision, kLadders, &visible) == 5,
                "No-op cannot hide an ongoing durability failure");
        failDirectoryOpen = failDirectorySync = false;
        require(restarted.keepRange(0, id, h.keepStartMs, h.keepEndMs, h.rangeRevision, kLadders, &visible) == 0 &&
                visible.rangeRevision == h.rangeRevision, "Retry sync completes durability without another revision");
    }
    std::cout << "F7_DIRECTORY_DURABILITY_PASS open fsync published-revision stale no-op retry\n";
#endif
    Recorder mixed((root / "mixed").string());
    startSession(mixed, 0, kGame, 0, kEpoch);
    for (int i = 1; i <= 150; ++i) {
        Sample sample; sample.flags = kFlagReady; sample.mode = 1; sample.profileGeneration = i <= 75 ? 1 : 2;
        sample.renderScale = .85f; sample.refreshRate = 120;
        mixed.frame(60, 0, 60); mixed.sample(sample, i * 1000);
    }
    mixed.finish(kLadders);
    auto originalMixed = mixed.list(0, "").front();
    require(originalMixed.summary.renderScale == .85f && originalMixed.summary.refreshRate == 120,
            "Full-view header preserves recorded display provenance");
    require(mixed.keepRange(0, originalMixed.id, 10000, 70000, 0, kLadders, &h) == 0 && h.summary.mixed,
            "Selected single generation cannot rescue original mixed provenance");
    require(h.summary.renderScale == .85f && h.summary.refreshRate == 120, "Effective header preserves display provenance");
    std::cout << "RECORDER_KEEP_RANGE_PASS revision reset raw stale bounds restart atomic-failure mixed\n";
}

void invalidStart(const fs::path& root) {
    Recorder r(root.string());
    r.start(0, "bad/package", 1, 1, 1, profile(), 0, kEpoch);
    require(!r.active(), "Invalid package never records");
    r.start(-1, kGame, 1, 1, 1, profile(), 0, kEpoch);
    require(!r.active(), "Invalid user never records");
    // The same start timestamp gets a unique id.
    startSession(r, 10, kGame, 0, kEpoch);
    Feeder a(r, 0);
    a.feed(30);
    r.pause(a.now, kEndStopped);
    r.finish(kLadders);
    startSession(r, 10, kGame, 0, kEpoch);
    Feeder b(r, 0);
    b.feed(30);
    r.finish(kLadders);
    const auto list = r.list(10, "");
    require(list.size() == 2 && list[0].startEpochMs == kEpoch + 1 && list[1].startEpochMs == kEpoch, "Colliding start times stay distinct");
}
} // namespace

void recorderTests(const char* tempDir) {
    const fs::path root = fs::path(tempDir) / "recorder-test";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    lifecycle(root / "lifecycle");
    cadenceAndFrames(root / "cadence");
    checkpointFailures(root / "checkpoint-failures");
    largeCounters(root / "large-counters");
    crashRecovery(root / "crash");
    retention(root / "retention");
    sampleLimit(root / "limit");
    ids(root / "ids");
    oldFormat(root / "old");
    resumeConfiguration(root / "resume-configuration");
    invalidStart(root / "invalid");
    keepRanges(root / "keep");
    std::cout << "recorder tests passed\n";
}
