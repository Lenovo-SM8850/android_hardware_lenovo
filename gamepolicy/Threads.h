// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Policy.h"
#include <algorithm>
#include <sched.h>
#include <functional>
#include <set>

namespace gamepolicy {
struct Task { int tid; int64_t start; std::string comm; };
struct Placement {
    Task task;
    uint64_t mask = 0;
    int priority = 0;
};
bool bindMatches(const std::string& comm, const std::string& name);
bool schedulerMatches(const std::string& comm, const std::string& name);
bool sameThreadRules(const Mode& a, const Mode& b);
bool samePlacement(const Placement& a, const Placement& b);
std::vector<Placement> placements(const std::vector<Task>& tasks, const Mode& mode, uint64_t possible);

struct ThreadOriginal {
    Placement placement;
    cpu_set_t affinity{};
    int policy = 0, priority = 0;
    bool bound = false, realtime = false;
};
struct ThreadJournal {
    std::string bootId;
    int pid = 0;
    int64_t start = 0;
    std::vector<ThreadOriginal> originals;
};
enum class JournalStatus { Valid, UnsupportedVersion, Invalid };
std::string encodeThreadJournal(const ThreadJournal& journal);
JournalStatus decodeThreadJournal(const std::string& data, ThreadJournal* journal);
// Callbacks return errno values, allowing restoration decisions to be tested
// without changing a host thread's affinity or scheduling policy.
int restoreThreadState(const ThreadOriginal& original, const cpu_set_t& possible,
                       const std::function<int(const cpu_set_t&)>& affinity,
                       const std::function<int(int, int)>& scheduler, bool* fallback);

class ThreadScanCadence {
  public:
    void reset(int64_t now) { mStart = now; mNext = now; }
    int64_t deadline() const { return mNext; }
    bool due(int64_t now, int sampleMs = 1000) {
        if (now < mNext) return false;
        mNext = now + (now - mStart < 10000 ? std::min(sampleMs, 5000) : 5000);
        return true;
    }
  private:
    int64_t mStart = 0, mNext = 0;
};

class Threads {
  public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit Threads(Diagnostic diagnostic, Diagnostic restored,
                     std::string path = "/data/vendor/gamepolicy/threads");
    // Recovery runs before policy loading, even if that loading subsequently fails.
    void recover();
    bool apply(int pid, int64_t start, const Mode& mode, bool enabled, int64_t now, int sampleMs = 1000);
    bool clear();
    size_t bound() const;
    size_t realtime() const;
    std::optional<int64_t> deadline() const {
        return mReady && mRules ? std::optional<int64_t>(mCadence.deadline()) : std::nullopt;
    }
    size_t scans() const { return mScans; }
    size_t journalWrites() const { return mJournalWrites; }
  private:
    using Original = ThreadOriginal;
    bool save();
    bool restore(const Original& original);
    bool alive(const Task& task) const;
    void warn(const std::string& message);
    Diagnostic mDiagnostic;
    Diagnostic mRestored;
    std::string mPath;
    std::string mBootId;
    std::set<std::string> mWarnings;
    int mPid = 0;
    int64_t mStart = 0;
    std::vector<Original> mOriginals;
    std::optional<Mode> mRules;
    bool mReady = true;
    ThreadScanCadence mCadence;
    size_t mScans = 0, mJournalWrites = 0;
    bool mJournalDirty = false;
    uint64_t mPossible = 0;
};
} // namespace gamepolicy
