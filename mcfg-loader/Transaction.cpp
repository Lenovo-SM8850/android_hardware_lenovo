// SPDX-License-Identifier: Apache-2.0
#include "Transaction.h"
#include "EfsDiagnostics.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>

namespace mcfg {
TransactionLock::TransactionLock(const std::string& directory) : directory_(directory) {
    fd_ = open((directory + "/lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(fd_ >= 0, "transaction lock open");
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("transaction locked");
    }
}
TransactionLock::~TransactionLock() {
    if (fd_ >= 0) close(fd_);
}
Transaction::Transaction(ProfileStore& store, EfsTransport& efs, std::string directory,
                         std::function<std::string()> modemVersion,
                         std::function<bool()> safe,
                         std::shared_ptr<TransactionLock> lock, std::function<int64_t()> clock)
    : store_(store),
      efs_(efs),
      path_(directory + "/journal.json"),
      modemVersion_(std::move(modemVersion)),
      safe_(std::move(safe)),
      clock_(clock ? std::move(clock) : [] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();
      }),
      lock_(lock ? std::move(lock) : std::make_shared<TransactionLock>(directory)) {
    require(lock_->directory() == directory, "transaction lock directory");
    try {
        if (access(path_.c_str(), F_OK) == 0) {
            journal_ = parseJson(readFile(path_, 40 * 1024 * 1024));
            const auto checksum = journal_["sha256"].asString();
            journal_.removeMember("sha256");
            require(sha256(jsonBytes(journal_)) == checksum, "journal checksum");
            require(journal_["schema_version"].asInt() == 1 ||
                            journal_["schema_version"].asInt() == 2, "journal schema");
            setState(journal_["state"].asString());
            status_.profile = journal_["profile"].asString();
            status_.modemVersion = journal_["baseband"].asString();
            status_.token = journal_["token"].asString();
            status_.sourceName = journal_["source_name"].asString();
            status_.sourceSha256 = journal_["source_sha256"].asString();
            status_.enabled = journal_["enabled"].asBool();
            // Legacy journals with prior use have already passed their first enable.
            baselineReset_ = journal_.isMember("baselineReset")
                                     ? journal_["baselineReset"].asBool()
                                     : status_.enabled || !status_.profile.empty() ||
                                               status_.state == "committed";
            status_.restartAttempted = journal_["restart_attempted"].asBool();
            status_.nextProfile = journal_["next_profile"].asString();
            status_.step = journal_["step"].asString();
            status_.error = journal_["error"].asString();
            if (status_.state == "rollback_failed" || journal_["normalization_pending"].asBool()) {
                // Legacy failure/normalization markers are idle; no EFS restoration.
                setState(status_.enabled ? "waiting_for_identity" : "disabled");
                status_.token.clear();
                status_.restartAttempted = false;
                status_.nextProfile.clear();
                status_.step.clear();
                status_.error.clear();
            }
            if (status_.state == "rechecking") {
                // Older code had already restarted before its removed drift check.
                setState(status_.enabled ? "committed" : "disabled");
                status_.token.clear();
                status_.restartAttempted = false;
            }
            if (status_.state == "quarantined")
                setState("waiting_for_identity");  // Migrate the old gate.
            const std::set<std::string> states{"disabled",
                                               "waiting_for_identity",
                                               "checking",
                                               "unsupported",
                                               "conflict",
                                               "no_change",
                                               "staged",
                                               "applying",
                                               "readback_verified",
                                               "restart_pending",
                                               "baseline_restart_pending",
                                               "rechecking",
                                               "committed",
                                               "rolling_back",
                                               "error",
                                               "probe_readonly"};
            require(states.count(status_.state), "journal state");
            // Legacy EFS snapshots are intentionally discarded without decoding.
            for (const auto& name : {"baseline", "before", "wanted", "managed"})
                journal_.removeMember(name);
            if (pending()) require(!status_.token.empty(), "incomplete journal");
        } else
            require(errno == ENOENT, "journal access");
    } catch (...) {
        lock_.reset();
        throw;
    }
}
Transaction::~Transaction() = default;
bool Transaction::pending() const {
    // Keep mutation and restart phases pending until recovery or completion.
    return status_.state == "staged" || status_.state == "applying" ||
           status_.state == "readback_verified" || status_.state == "restart_pending" ||
           status_.state == "baseline_restart_pending" ||
           status_.state == "rechecking" || status_.state == "rolling_back";
}
void Transaction::setState(const std::string& state) {
    if (state != status_.state)
        MCFG_INFO("Transaction state %s -> %s profile=%s", status_.state.c_str(),
                  state.c_str(), status_.profile.c_str());
    status_.state = state;
}
void Transaction::save() {
    // The journal records only transaction progress, never EFS payloads.
    journal_ = Json::Value(Json::objectValue);
    journal_["schema_version"] = 2;
    journal_["state"] = status_.state;
    journal_["profile"] = status_.profile;
    journal_["next_profile"] = status_.nextProfile;
    journal_["step"] = status_.step;
    journal_["baseband"] = status_.modemVersion;
    journal_["source_name"] = status_.sourceName;
    journal_["source_sha256"] = status_.sourceSha256;
    journal_["token"] = status_.token;
    journal_["enabled"] = status_.enabled;
    journal_["baselineReset"] = baselineReset_;
    journal_["restart_attempted"] = status_.restartAttempted;
    journal_["error"] = status_.error;
    journal_["updated_at_ms"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    journal_["sha256"] = sha256(jsonBytes(journal_));
    durableWrite(path_, journal_);
}
void Transaction::fail(const char* error) {
    MCFG_ERROR("Transaction failed: %s", error);
    status_.error = error;
    status_.token.clear();
    status_.restartAttempted = false;
    setState("error");
    save();
}
void Transaction::gate(bool checkSafety) {
    require(!checkSafety || safe_(), "call/emergency/radio/identity context unsafe or expired");
}
Snapshot Transaction::snapshot(const std::set<std::string>& paths) {
    Snapshot result;
    for (const auto& path : paths) {
        result[path] = efsStep(path, "snapshot", [&] {
            efs_.checkParent(path);
            return efs_.read(path);
        });
    }
    return result;
}
int Transaction::autoselect(bool refresh) {
    // No EFS polling during our own restart, even if a stale lease says radio ON.
    if ((status_.state == "restart_pending" || status_.state == "baseline_restart_pending") &&
        status_.restartAttempted) return -1;
    if (!refresh && clock_() < autoselectUntil_) return autoselect_;
    autoselect_ = -1;
    try {
        autoselect_ = snapshot({kControl}).at(kControl) == std::optional<Bytes>(Bytes{7}) ? 7 : 0;
        autoselectUntil_ = clock_() + 60'000;
        return autoselect_;
    } catch (...) {
        // Cache failures too: status may be polled by both the app and coordinator.
        autoselectUntil_ = clock_() + 30'000;
        throw;
    }
}
void Transaction::verify(const Snapshot& wanted, const char* step) {
    for (const auto& item : wanted) {
        const auto& path = item.first;
        const auto& bytes = item.second;
        efsStep(path, step, [&] {
            const bool drifted = efs_.read(path) != bytes;
            if (drifted) MCFG_ERROR("EFS drift step=%s path=%s", step, path.c_str());
            require(!drifted, "EFS readback mismatch errno=0");
        });
    }
}
void Transaction::mutate(const Snapshot& wanted) {
    const auto checkpoint = [&] { gate(); };
    const auto write = [&](const std::string& path, const std::optional<Bytes>& bytes) {
        if (path == kControl) {
            autoselect_ = -1;
            autoselectUntil_ = 0;
        }
        efsStep(path, "mutate", [&] {
            checkpoint();
            if (bytes) efs_.write(path, *bytes, checkpoint);
            else {
                efs_.checkParent(path);
                efs_.remove(path, checkpoint);
            }
        });
        // Verify each write immediately. A failure leaves all bytes as written;
        // Android/modem owners may change them again after the restart.
        verify(Snapshot{{path, bytes}});
        if (path == kControl) {
            autoselect_ = bytes == std::optional<Bytes>(Bytes{7}) ? 7 : 0;
            autoselectUntil_ = clock_() + 60'000;
        }
    };
    require(wanted.count(kControl), "missing saved control");
    if (wanted.size() == 1 && wanted.at(kControl) == std::optional<Bytes>(Bytes{7})) {
        write(kControl, Bytes{7});
        return;
    }
    // The auto-selection byte must be disabled before any overlay mutation.
    write(kControl, Bytes{0});
    for (const auto& [path, bytes] : wanted)
        if (path != kControl) write(path, bytes);
    if (wanted.at(kControl) != std::optional<Bytes>(Bytes{0}))
        write(kControl, wanted.at(kControl));
}
void Transaction::enable(bool enabled) {
    if (!enabled) {
        status_.enabled = false;
        if (!pending()) setState("disabled");
        save();
        return;
    }
    status_.enabled = true;
    if (!pending()) {
        setState("waiting_for_identity");
        status_.error.clear();
    }
    save();
}
void Transaction::evaluate(const std::vector<std::string>& homePlmns) {
    if (homePlmns.empty()) return;  // Temporary SIM removal preserves the applied state.
    if (status_.state == "restart_pending" && status_.enabled && !status_.profile.empty()) {
        const auto next = store_.select(homePlmns);
        if (next.state == "checking" && next.profile != status_.profile) {
            gate();
            // A changed carrier starts its own 07/restart before the next overlay.
            status_.token.clear();
            status_.restartAttempted = false;
            setState("checking");
            save();
        }
    }
    if (pending()) return;
    if (!status_.enabled) {
        setState("disabled");
        return;
    }
    auto selection = store_.select(homePlmns);
    // Binder apply evaluates first; retain the durable proof for its no-op guard.
    if (selection.state == "checking" && committedSourceMatches(selection.profile)) return;
    setState(selection.state);
    save();
}
bool Transaction::committedSourceMatches(const std::string& profile) const {
    if (!status_.enabled || pending() || status_.state != "committed" ||
        profile.empty() || status_.profile != profile || status_.sourceName.empty() ||
        status_.sourceSha256.empty()) return false;
    const auto metadata = store_.metadata(profile);
    return status_.sourceName == metadata["source_name"].asString() &&
           status_.sourceSha256 == metadata["source_sha256"].asString();
}
void Transaction::execute(const Snapshot& wanted, const std::string& profile,
                          const std::string& token, const std::string& nextProfile, bool baseline) {
    require(token.size() == 32 && token.find_first_not_of("0123456789abcdef") == std::string::npos,
            "transaction token");
    try {
        gate();
        status_.error.clear();
        status_.nextProfile = nextProfile;
        status_.step = baseline ? "baseline" : (profile.empty() ? "restore" : "overlay");
        // Track a partially written overlay too. For 07, retain the old profile
        // until the write succeeds so an interrupted carrier reset retries 07.
        if (!profile.empty()) status_.profile = profile;
        status_.token = token;
        status_.restartAttempted = false;
        setState("staged");
        save();
        setState("applying");
        save();
        mutate(wanted);
        status_.profile = profile;
        const auto metadata = store_.metadata(profile);
        status_.sourceName = metadata["source_name"].asString();
        status_.sourceSha256 = metadata["source_sha256"].asString();
        setState("readback_verified");
        save();
        setState(baseline ? "baseline_restart_pending" : "restart_pending");
        save();
    } catch (const std::exception& error) {
        fail(error.what());
        throw;
    }
}
void Transaction::apply(const std::vector<std::string>& homePlmns, const std::string& token) {
    if (homePlmns.empty()) return;
    try {
        require(status_.enabled && !pending(), "application disabled/pending");
        const auto selection = store_.select(homePlmns);
        require(selection.state == "checking", "identity conflict/unsupported");
        if (committedSourceMatches(selection.profile)) {
            MCFG_INFO("Carrier profile already committed; skipping apply profile=%s",
                      selection.profile.c_str());
            return;
        }
        status_.modemVersion = modemVersion_();  // Diagnostics only, never an approval gate.
        gate();
        if (!baselineReset_) {
            // An interrupted 07 write still needs its restart, even if 07 is now present.
            if (status_.step == "baseline" || autoselect(true) != 7) {
                execute(Snapshot{{kControl, Bytes{7}}}, "", token, selection.profile, true);
                return;
            }
            baselineReset_ = true;
            // An already automatic modem also satisfies an import's initial reset.
            if (status_.profile.empty()) status_.nextProfile = selection.profile;
            save();
        }
        if ((store_.metadata(selection.profile)["imported"].asBool() &&
                    status_.nextProfile != selection.profile) ||
            (!status_.profile.empty() && status_.profile != selection.profile) ||
            (status_.step == "restore" && !status_.nextProfile.empty())) {
            MCFG_INFO("Restoring automatic selection before carrier profile change");
            // The automatic MBN restores what it owns after the existing restart.
            // Overlay-only files it does not rewrite remain; no stock EFS exists
            // to identify or remove them.
            execute(Snapshot{{kControl, Bytes{7}}}, "", token, selection.profile);
            return;
        }
        auto wanted = store_.load(selection.profile);
        wanted[kControl] = Bytes{0};
        execute(wanted, selection.profile, token);
    } catch (const std::exception& error) {
        if (status_.state != "error" && !pending()) fail(error.what());
        throw;
    }
}
void Transaction::restore(const std::string& token) {
    try {
        gate();
        // Explicit OFF or an unsupported/no SIM supersedes a pending overlay.
        if (status_.state == "restart_pending" || status_.state == "baseline_restart_pending") {
            setState("disabled");
            status_.token.clear();
            save();
        }
        require(!pending(), "restore unavailable");
        status_.enabled = false;
        execute(Snapshot{{kControl, Bytes{7}}}, "", token);
    } catch (const std::exception& error) {
        if (status_.state != "error" && !pending()) fail(error.what());
        throw;
    }
}
bool Transaction::claimRestart(const std::string& token) {
    require((status_.enabled || status_.profile.empty()) &&
                    (status_.state == "restart_pending" || status_.state == "baseline_restart_pending") &&
                    token == status_.token,
            "restart token/state");
    gate();
    if (status_.restartAttempted) return false;
    // Durable at-most-once mark BEFORE the loader requests the vendor restart helper.
    status_.restartAttempted = true;
    autoselect_ = -1;
    autoselectUntil_ = 0;
    save();
    return true;
}
void Transaction::resumeContext(const std::vector<std::string>& homePlmns) {
    require((status_.enabled || status_.profile.empty()) &&
                    (status_.state == "restart_pending" || status_.state == "baseline_restart_pending"),
            "resume disabled/state");
    if (status_.profile.empty()) return;  // Restoring 07 is independent of carrier identity.
    const auto selection = store_.select(homePlmns);
    require(selection.state == "checking" &&
                    (status_.profile.empty() || selection.profile == status_.profile),
            "pending transaction identity conflict/changed");
}
void Transaction::complete(const std::string& token) {
    require((status_.enabled || status_.profile.empty()) &&
                    (status_.state == "restart_pending" || status_.state == "baseline_restart_pending") &&
                    status_.restartAttempted && token == status_.token,
            "completion token/state");
    gate();
    try {
        if (store_.metadata(status_.profile)["imported"].asBool()) {
            // User-chosen imports also need readback after their final restart.
            // Reuse the same verifier and EFS transport, including fresh HELLO.
            auto wanted = store_.load(status_.profile);
            wanted[kControl] = Bytes{0};
            verify(wanted);
        }
        if (status_.state == "baseline_restart_pending") baselineReset_ = true;
        // Radio recovery completes the transaction. Do not re-read an overlay
        // from the bundled automatic MBN that Android may legitimately rewrite.
        status_.token.clear();
        status_.step.clear();
        autoselect_ = -1;
        autoselectUntil_ = 0;
        setState(status_.enabled ? (status_.profile.empty() ? "waiting_for_identity" : "committed")
                                 : "disabled");
        status_.error.clear();
        save();
    } catch (const std::exception& error) {
        fail(error.what());
        throw;
    }
}
void Transaction::recover() {
    if (!pending() || status_.state == "restart_pending" ||
        status_.state == "baseline_restart_pending") return;
    gate();
    if (status_.state == "readback_verified" &&
        (status_.step == "restore" || status_.step == "baseline") && !status_.nextProfile.empty()) {
        // 07 was verified, but its restart must still precede the next overlay.
        setState(status_.step == "baseline" ? "baseline_restart_pending" : "restart_pending");
        save();
        return;
    }
    // A crashed writer may have changed only part of EFS. Keep it for a retry;
    // the recorded profile/next step determines whether another 07 reset is needed.
    fail("interrupted transaction; retry on the next trigger");
}
void Transaction::probe() {
    autoselectUntil_ = 0;  // Explicit refresh also invalidates the status cache.
    auto paths = store_.paths();
    paths.insert(kControl);
    const auto result = snapshot(paths);
    size_t existing = 0;
    for (const auto& [path, data] : result) {
        (void)path;
        if (data) ++existing;
    }
    // Only a snapshot digest/count is exposed; no EFS bytes or subscriber IDs.
    status_.error = "read-only EFS probe: " + std::to_string(existing) +
                    " present, snapshot sha256=" + sha256(jsonBytes(encodeSnapshot(result)));
    if (!pending()) setState("probe_readonly");
}
bool Transaction::dumpReadOnly(std::ostream& output) {
    auto paths = store_.paths();
    paths.insert(kControl);
    bool ok = true;
    for (const auto& path : paths) {
        try {
            const auto current = efsStep(path, "snapshot", [&] { return efs_.read(path); });
            output << path << " current=";
            if (current)
                output << "sha256:" << sha256(*current).substr(0, 16) << "/size:" << current->size();
            else
                output << "absent";
            output << '\n';
        } catch (const std::exception& error) {
            ok = false;
            output << path << " current=error " << error.what() << '\n';
        }
    }
    output << (ok ? "DUMP_OK\n" : "DUMP_FAIL\n");
    return ok;
}
}  // namespace mcfg
