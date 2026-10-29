// SPDX-License-Identifier: Apache-2.0
#include "Threads.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace gamepolicy {
namespace {
constexpr size_t kMaxTasks = 256;
constexpr size_t kMaxState = 512 * 1024;
constexpr int kJournalVersion = 2;
std::string proc(int pid) { return "/proc/" + std::to_string(pid); }
std::string taskPath(int pid, int tid) { return proc(pid) + "/task/" + std::to_string(tid); }
int64_t startTime(const std::string& path) {
    std::ifstream in(path + "/stat");
    std::string line, field;
    if (!std::getline(in, line)) return 0;
    const auto end = line.rfind(')');
    if (end == std::string::npos) return 0;
    std::istringstream fields(line.substr(end + 1));
    for (int i = 3; i <= 22; ++i) {
        if (!(fields >> field) || (i == 3 && (field == "Z" || field == "X"))) return 0;
    }
    int64_t ticks = 0;
    auto result = std::from_chars(field.data(), field.data() + field.size(), ticks);
    return result.ec == std::errc{} && result.ptr == field.data() + field.size() && ticks > 0 ? ticks : 0;
}
cpu_set_t possibleAffinity() {
    cpu_set_t cpus; CPU_ZERO(&cpus);
    std::ifstream in("/sys/devices/system/cpu/possible");
    std::string list, part;
    if (!(in >> list)) return cpus;
    std::istringstream parts(list);
    while (std::getline(parts, part, ',')) {
        const auto dash = part.find('-');
        try {
            const int first = std::stoi(part), last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
            if (first < 0 || last < first) { CPU_ZERO(&cpus); return cpus; }
            for (int i = first; i <= std::min(last, CPU_SETSIZE - 1); ++i) CPU_SET(i, &cpus);
        } catch (const std::exception&) { CPU_ZERO(&cpus); return cpus; }
    }
    return cpus;
}
uint64_t possibleCpus() {
    const auto cpus = possibleAffinity();
    uint64_t bits = 0;
    for (int i = 0; i < 64; ++i) if (CPU_ISSET(i, &cpus)) bits |= uint64_t{1} << i;
    return bits;
}
} // namespace

bool bindMatches(const std::string& comm, const std::string& name) {
    return !name.empty() && comm.find(name) != std::string::npos;
}
bool schedulerMatches(const std::string& comm, const std::string& name) {
    return !name.empty() && comm.compare(0, name.size(), name) == 0;
}
bool sameThreadRules(const Mode& a, const Mode& b) {
    return a.bindCore.size() == b.bindCore.size() &&
        a.schedulers.size() == b.schedulers.size() &&
        std::equal(a.bindCore.begin(), a.bindCore.end(), b.bindCore.begin(), [](const auto& x, const auto& y) {
            return x.name == y.name && x.mask == y.mask;
        }) && std::equal(a.schedulers.begin(), a.schedulers.end(), b.schedulers.begin(), [](const auto& x, const auto& y) {
            return x.name == y.name && x.priority == y.priority && x.enabled == y.enabled;
        });
}
bool samePlacement(const Placement& a, const Placement& b) {
    return a.task.tid == b.task.tid && a.task.start == b.task.start && a.task.comm == b.task.comm &&
        a.mask == b.mask && a.priority == b.priority;
}
std::vector<Placement> placements(const std::vector<Task>& tasks, const Mode& mode, uint64_t possible) {
    std::vector<Placement> result;
    for (const auto& task : tasks) {
        Placement placement{task};
        for (const auto& rule : mode.bindCore) if (bindMatches(task.comm, rule.name)) {
            placement.mask = rule.mask & possible;
            break;
        }
        // Scheduling rules are issued in order; the last matching request wins.
        for (const auto& rule : mode.schedulers) if (rule.enabled && schedulerMatches(task.comm, rule.name))
            placement.priority = rule.priority;
        if (placement.mask || placement.priority) result.push_back(placement);
    }
    return result;
}

std::string encodeThreadJournal(const ThreadJournal& journal) {
    std::ostringstream out;
    out << kJournalVersion << ' ' << journal.bootId << ' ' << journal.pid << ' ' << journal.start << '\n';
    for (const auto& original : journal.originals) {
        const auto& task = original.placement.task;
        out << task.tid << ' ' << task.start << ' ' << original.policy << ' ' << original.priority << ' '
            << original.bound << ' ' << original.realtime << ' ';
        for (int i = 0; i < CPU_SETSIZE; ++i) out << (CPU_ISSET(i, &original.affinity) ? '1' : '0');
        out << '\n';
    }
    return out.str();
}
JournalStatus decodeThreadJournal(const std::string& data, ThreadJournal* journal) {
    *journal = {};
    if (data.size() > kMaxState) return JournalStatus::Invalid;
    std::istringstream input(data);
    int version;
    if (!(input >> version)) return JournalStatus::Invalid;
    if (version != kJournalVersion) return JournalStatus::UnsupportedVersion;
    ThreadJournal parsed;
    if (!(input >> parsed.bootId >> parsed.pid >> parsed.start) || parsed.pid < 0 || parsed.start < 0 ||
        parsed.bootId.size() != 36 || parsed.bootId.find_first_not_of("0123456789abcdef-") != std::string::npos)
        return JournalStatus::Invalid;
    input >> std::ws;
    while (!input.eof()) {
        ThreadOriginal original;
        auto& task = original.placement.task;
        std::string bits;
        int bound, realtime;
        if (parsed.originals.size() == kMaxTasks ||
            !(input >> task.tid >> task.start >> original.policy >> original.priority >> bound >> realtime >> bits) ||
            task.tid <= 0 || task.start <= 0 || bits.size() != CPU_SETSIZE ||
            bits.find_first_not_of("01") != std::string::npos || (bound != 0 && bound != 1) ||
            (realtime != 0 && realtime != 1) || original.priority < 0 || original.priority > 99 || original.policy < 0)
            return JournalStatus::Invalid;
        original.bound = bound; original.realtime = realtime;
        CPU_ZERO(&original.affinity);
        for (int i = 0; i < CPU_SETSIZE; ++i) if (bits[i] == '1') CPU_SET(i, &original.affinity);
        parsed.originals.push_back(original);
        input >> std::ws;
    }
    *journal = std::move(parsed);
    return JournalStatus::Valid;
}
int restoreThreadState(const ThreadOriginal& original, const cpu_set_t& possible,
                       const std::function<int(const cpu_set_t&)>& affinity,
                       const std::function<int(int, int)>& scheduler, bool* fallback) {
    *fallback = false;
    int error = 0;
    if (original.bound) {
        int result = affinity(original.affinity);
        if (result == EINVAL) {
            *fallback = true;
            result = affinity(possible);
        }
        if (result != ESRCH) error = result;
    }
    if (original.realtime) {
        const int result = scheduler(original.policy, original.priority);
        if (result && result != ESRCH) error = result;
    }
    return error;
}

Threads::Threads(Diagnostic diagnostic, Diagnostic restored, std::string path)
    : mDiagnostic(std::move(diagnostic)), mRestored(std::move(restored)), mPath(std::move(path)) {
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    boot >> mBootId;
    if (mBootId.size() != 36 || mBootId.find_first_not_of("0123456789abcdef-") != std::string::npos) {
        mReady = false;
        warn("Cannot read boot identity; thread changes disabled");
    }
}
void Threads::warn(const std::string& message) {
    if (mWarnings.insert(message).second) mDiagnostic(message);
}
bool Threads::alive(const Task& task) const {
    return mPid > 0 && mPid != getpid() && mStart > 0 && startTime(proc(mPid)) == mStart &&
        task.tid > 0 && task.start > 0 && startTime(taskPath(mPid, task.tid)) == task.start;
}
bool Threads::save() {
    ++mJournalWrites;
    const auto data = encodeThreadJournal({mBootId, mPid, mStart, mOriginals});
    const auto split = mPath.rfind('/');
    const auto directory = mPath.substr(0, split), file = mPath.substr(split + 1), temporary = file + ".tmp";
    int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) { warn("Cannot open thread recovery directory"); return false; }
    int fd = openat(dir, temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool ok = fd >= 0 && data.size() <= kMaxState;
    size_t offset = 0;
    while (ok && offset < data.size()) {
        ssize_t written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) ok = false;
        else offset += written;
    }
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (ok) ok = renameat(dir, temporary.c_str(), dir, file.c_str()) == 0 && fsync(dir) == 0;
    close(dir);
    if (!ok) warn("Cannot persist thread originals; thread changes disabled");
    return ok;
}
bool Threads::restore(const Original& original) {
    const auto& task = original.placement.task;
    if (!alive(task)) return true;
    // Revalidate membership before each numeric-TID syscall, including renamed tasks.
    bool fallback;
    const bool ok = restoreThreadState(original, possibleAffinity(), [&](const cpu_set_t& affinity) {
        if (!alive(task)) return ESRCH;
        return sched_setaffinity(task.tid, sizeof(cpu_set_t), &affinity) < 0 ? errno : 0;
    }, [&](int policy, int priority) {
        if (!alive(task)) return ESRCH;
        sched_param param{}; param.sched_priority = priority;
        return sched_setscheduler(task.tid, policy, &param) < 0 ? errno : 0;
    }, &fallback) == 0;
    if (fallback) warn("Original affinity outside current cpuset; restoring possible CPUs");
    if (!ok) warn("Cannot restore thread originals; retaining recovery entry");
    return ok;
}
void Threads::recover() {
    if (!mReady) return;
    struct stat st{};
    if (lstat(mPath.c_str(), &st) < 0 && errno == ENOENT) return;
    mReady = false;
    if (!S_ISREG(st.st_mode) || st.st_size > static_cast<off_t>(kMaxState)) {
        warn("Invalid thread recovery state; thread changes disabled"); return;
    }
    std::ifstream input(mPath);
    const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ThreadJournal journal;
    const auto status = decodeThreadJournal(data, &journal);
    if (status == JournalStatus::UnsupportedVersion) {
        warn("Unsupported thread recovery version; discarding state");
        mPid = 0; mStart = 0; mOriginals.clear();
        mReady = save();
        return;
    }
    if (status != JournalStatus::Valid) {
        warn("Invalid thread recovery entry; thread changes disabled"); return;
    }
    if (journal.bootId != mBootId) {
        mPid = 0; mStart = 0;
        mReady = save();
        return;
    }
    mPid = journal.pid; mStart = journal.start;
    mOriginals = std::move(journal.originals);
    mReady = true;
    clear();
}
bool Threads::clear() {
    if (mOriginals.empty()) {
        if (mJournalDirty) { mReady = save(); if (mReady) mJournalDirty = false; }
        mRules.reset(); mPid = 0; mStart = 0;
        return false;
    }
    const auto before = mOriginals.size();
    mOriginals.erase(std::remove_if(mOriginals.begin(), mOriginals.end(), [&](const auto& original) {
        return restore(original);
    }), mOriginals.end());
    mRules.reset();
    if (mOriginals.empty()) { mPid = 0; mStart = 0; }
    const bool changed = before != mOriginals.size();
    if (changed) mRestored("Thread overrides cleared; remaining=" + std::to_string(mOriginals.size()));
    if (changed || mJournalDirty) {
        mReady = save();
        if (mReady) mJournalDirty = false;
    }
    return changed;
}
bool Threads::apply(int pid, int64_t start, const Mode& mode, bool enabled, int64_t now, int sampleMs) {
    if (!mReady) return clear();
    bool changed = false;
    if (mPid != pid || mStart != start || !enabled || (!mRules && !mOriginals.empty()) ||
        (mRules && !sameThreadRules(*mRules, mode))) {
        if (!mOriginals.empty()) changed = clear();
        if (!mReady || !mOriginals.empty()) return changed;
        mRules.reset();
    }
    const bool hasRules = !mode.bindCore.empty() || std::any_of(mode.schedulers.begin(), mode.schedulers.end(),
            [](const auto& rule) { return rule.enabled; });
    if (!enabled || (!hasRules && mOriginals.empty()) || pid <= 0 || pid == getpid()) return changed;
    if (!mRules) { mRules = mode; mWarnings.clear(); mCadence.reset(now); changed = true; }
    if (!mCadence.due(now, sampleMs) || startTime(proc(pid)) != start) return changed;
    mPid = pid; mStart = start;
    DIR* dir = opendir((proc(pid) + "/task").c_str());
    if (!dir) return changed;
    ++mScans;
    std::vector<Task> tasks;
    while (auto entry = readdir(dir)) {
        std::string name = entry->d_name;
        int tid = 0;
        auto parsed = std::from_chars(name.data(), name.data() + name.size(), tid);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || tid <= 0) continue;
        const auto path = taskPath(pid, tid);
        Task task{tid, startTime(path), {}};
        std::ifstream comm(path + "/comm");
        if (task.start && std::getline(comm, task.comm) && alive(task)) tasks.push_back(task);
    }
    closedir(dir);
    // Possible CPUs are fixed for this boot. Retry only an unavailable/invalid table.
    if (!mode.bindCore.empty() && !mPossible) mPossible = possibleCpus();
    const auto desired = placements(tasks, mode, mPossible);
    bool removed = false;
    // These originals were already durable before this pass. Restore retiring
    // entries first, including recycled TIDs, so replacements can be placed now.
    for (auto it = mOriginals.begin(); it != mOriginals.end();) {
        const auto match = std::find_if(desired.begin(), desired.end(), [&](const auto& p) {
            return samePlacement(p, it->placement);
        });
        if (match == desired.end() && restore(*it)) { it = mOriginals.erase(it); removed = true; }
        else ++it;
    }
    if (removed) {
        changed = true; mJournalDirty = true;
        mRestored("Thread overrides restored for changed tasks");
    }
    const auto priorSize = mOriginals.size();
    std::vector<Placement> additions;
    for (const auto& placement : desired) {
        auto found = std::find_if(mOriginals.begin(), mOriginals.end(), [&](const auto& o) { return o.placement.task.tid == placement.task.tid; });
        if (found != mOriginals.end()) continue;
        if (mOriginals.size() >= kMaxTasks) { warn("Thread tracking limit reached (256)"); break; }
        Original original{}; original.placement = placement;
        const int tid = placement.task.tid;
        sched_param param{};
        if (!alive(placement.task) || sched_getaffinity(tid, sizeof(cpu_set_t), &original.affinity) < 0 ||
            (original.policy = sched_getscheduler(tid)) < 0 || sched_getparam(tid, &param) < 0) continue;
        original.priority = param.sched_priority;
        const int policy = original.policy & ~SCHED_RESET_ON_FORK;
        if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR &&
            policy != SCHED_BATCH && policy != SCHED_IDLE) {
            warn("Unrestorable scheduling policy; skipping thread"); continue;
        }
        if (!alive(placement.task)) continue;
        // Journal intent before either syscall. A crash before a call can only
        // restore an unchanged value; successful calls always have a durable flag.
        original.bound = placement.mask != 0;
        original.realtime = placement.priority != 0;
        mOriginals.push_back(original);
        additions.push_back(placement);
    }
    // One durable batch for removals and all new originals, before applying any
    // new overrides. Each retiring original was write-ahead logged on its own pass.
    if (mJournalDirty || !additions.empty()) {
        if (!save()) { mOriginals.resize(priorSize); mReady = false; return changed; }
        mJournalDirty = false;
    }
    for (const auto& placement : additions) {
        auto found = std::find_if(mOriginals.begin(), mOriginals.end(), [&](const auto& o) {
            return samePlacement(o.placement, placement);
        });
        if (found == mOriginals.end()) continue;
        auto& tracked = *found;
        const bool boundIntent = tracked.bound, realtimeIntent = tracked.realtime;
        const int tid = placement.task.tid;
        sched_param param{};
        tracked.bound = false; tracked.realtime = false;
        if (placement.mask && alive(placement.task)) {
            cpu_set_t affinity; CPU_ZERO(&affinity);
            for (int cpu = 0; cpu < 64; ++cpu) if (placement.mask & (uint64_t{1} << cpu)) CPU_SET(cpu, &affinity);
            tracked.bound = sched_setaffinity(tid, sizeof(cpu_set_t), &affinity) == 0;
            if (!tracked.bound && errno != ESRCH) warn("Cannot apply thread affinity");
        }
        if (placement.priority && alive(placement.task)) {
            param.sched_priority = std::clamp(placement.priority, 1, 99);
            tracked.realtime = sched_setscheduler(tid, SCHED_FIFO, &param) == 0;
            if (!tracked.realtime && errno != ESRCH) warn("Cannot apply FIFO scheduling");
        }
        changed = true;
        // Coalesce cleared intent flags into the next changed pass. Until then,
        // crash recovery may harmlessly restore an unchanged original value.
        if (tracked.bound != boundIntent || tracked.realtime != realtimeIntent) mJournalDirty = true;
    }
    return changed;
}
size_t Threads::bound() const {
    return std::count_if(mOriginals.begin(), mOriginals.end(), [](const auto& o) { return o.bound; });
}
size_t Threads::realtime() const {
    return std::count_if(mOriginals.begin(), mOriginals.end(), [](const auto& o) { return o.realtime; });
}
} // namespace gamepolicy
