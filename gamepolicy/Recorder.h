// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Analysis.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gamepolicy {
struct RecordHeader {
    std::string id; int userId = 0; std::string package; int mode = 1;
    int64_t startEpochMs = 0; int durationMs = 0; int sampleCount = 0; int endReason = 0;
    ProfileView profile; Summary summary;
};
enum EndReason { kEndStopped = 1, kEndExited = 2, kEndLimit = 3, kEndInterrupted = 4 };

// Not thread-safe and without threads of its own; the service guards it with its own locks.
class Recorder {
  public:
    static constexpr size_t kMaxSamples = 7200, kMaxRecords = 20;
    static constexpr int kMinDurationMs = 30000, kPauseMs = 60000, kPeriodMs = 1000, kFlushEvery = 30;

    explicit Recorder(std::string root = "/data/vendor/gamepolicy/records");
    void recover(const Ladders&);                 // Finalize every *.rec.partial (end=4), delete unparsable ones.
    // Session lifecycle; the caller holds its own lock. A previous session must be finished first.
    void start(int user, const std::string& pkg, int pid, int64_t startTicks, int mode,
               const ProfileView& profile, int64_t nowMs, int64_t epochMs);
    bool resume(int user, const std::string& pkg, int pid, int64_t startTicks, int64_t nowMs); // True if paused match within 60 s.
    void pause(int64_t nowMs, int reason);         // Keeps the record for 60 s.
    void finish(const Ladders&);                   // Writes .rec (or discards when duration < 30 s), applies retention.
    bool active() const; bool paused() const;
    std::optional<int64_t> deadline() const;       // Next 1 s sample while active, pause expiry while paused.
    bool due(int64_t nowMs) const;                 // Sample due.
    bool expired(int64_t nowMs) const;             // Pause expired.
    // Accumulate for the current second; the frame rate signals of the last report win, -1 when none.
    void frame(float fps, int jankFrames, int frames, int maxFrameTimeUs = -1, float requestedFps = -1, float throttleFps = -1);
    void sample(Sample sample, int64_t nowMs);     // Fills timeMs and the frame fields from the accumulator.
    // Queries read finished .rec files only, which are replaced atomically, so they need no lock against
    // start/finish; the caller serializes remove() and finish() with its file mutex.
    std::vector<RecordHeader> list(int user, const std::string& pkg) const;     // Newest first, "" = all.
    bool read(int user, const std::string& id, std::vector<Sample>* samples) const;
    bool remove(int user, const std::string& id);
    bool header(int user, const std::string& id, RecordHeader* out) const;

  private:
    struct Session {
        int user = 0; std::string package; int pid = 0; int64_t startTicks = 0; int mode = 1;
        ProfileView profile; int64_t startNowMs = 0; int64_t startEpochMs = 0; std::string id;
        std::vector<Sample> samples; size_t flushed = 0;
        bool paused = false; int64_t pausedAt = 0; int endReason = 0;
        int64_t nextDue = 0;
        double fpsSum = 0; int fpsCount = 0; float fpsMin = -1; int jank = 0, frames = 0, maxFrameTimeUs = -1;
        float requestedFps = -1, throttleFps = -1;
    };
    void flush(Session& session);
    void resetSecond(Session& session);
    void retain(int user);
    std::string mRoot;
    std::optional<Session> mSession;
};
} // namespace gamepolicy
