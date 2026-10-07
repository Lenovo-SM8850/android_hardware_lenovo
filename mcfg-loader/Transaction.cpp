// SPDX-License-Identifier: Apache-2.0
#include "Transaction.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>

namespace mcfg {
Transaction::Transaction(ProfileStore& store, EfsTransport& efs, std::string directory,
                         std::function<std::string()> modemVersion,
                         std::function<bool()> writePolicy, std::function<bool()> safe)
    : store_(store),
      efs_(efs),
      path_(directory + "/journal.json"),
      modemVersion_(std::move(modemVersion)),
      writePolicy_(std::move(writePolicy)),
      safe_(std::move(safe)) {
    lockFd_ = open((directory + "/lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(lockFd_ >= 0, "transaction lock open");
    if (flock(lockFd_, LOCK_EX | LOCK_NB) != 0) {
        close(lockFd_);
        lockFd_ = -1;
        throw std::runtime_error("transaction locked");
    }
    try {
        if (access(path_.c_str(), F_OK) == 0) {
            journal_ = parseJson(readFile(path_, 40 * 1024 * 1024));
            const auto checksum = journal_["sha256"].asString();
            journal_.removeMember("sha256");
            require(sha256(jsonBytes(journal_)) == checksum, "journal checksum");
            require(journal_["schema_version"].asInt() == 1, "journal schema");
            status_.state = journal_["state"].asString();
            status_.profile = journal_["profile"].asString();
            status_.modemVersion = journal_["baseband"].asString();
            status_.token = journal_["token"].asString();
            status_.sourceName = journal_["source_name"].asString();
            status_.sourceSha256 = journal_["source_sha256"].asString();
            status_.enabled = journal_["enabled"].asBool();
            status_.restartAttempted = journal_["restart_attempted"].asBool();
            status_.error = journal_["error"].asString();
            if (status_.state == "quarantined")
                status_.state = "waiting_for_identity";  // Migrate the old gate.
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
                                               "rechecking",
                                               "committed",
                                               "rolling_back",
                                               "error",
                                               "rollback_failed",
                                               "probe_readonly"};
            require(states.count(status_.state), "journal state");
            for (const auto& name : {"baseline", "before", "wanted", "managed"}) {
                if (journal_.isMember(name)) {
                    const auto saved = decodeSnapshot(journal_[name], store_.paths());
                    require(saved.count(kControl), "journal missing control");
                }
            }
            if (pending()) {
                require(journal_.isMember("before") && journal_.isMember("wanted") &&
                                !status_.token.empty(),
                        "incomplete journal");
            }
        } else
            require(errno == ENOENT, "journal access");
    } catch (...) {
        close(lockFd_);
        lockFd_ = -1;
        throw;
    }
}
Transaction::~Transaction() {
    if (lockFd_ >= 0) close(lockFd_);
}
bool Transaction::pending() const {
    return status_.state == "staged" || status_.state == "applying" ||
           status_.state == "readback_verified" || status_.state == "restart_pending" ||
           status_.state == "rechecking" || status_.state == "rolling_back";
}
void Transaction::save() {
    journal_.removeMember("sha256");
    journal_["schema_version"] = 1;
    journal_["state"] = status_.state;
    journal_["profile"] = status_.profile;
    journal_.removeMember("generation");
    journal_["baseband"] = status_.modemVersion;
    journal_["source_name"] = status_.sourceName;
    journal_["source_sha256"] = status_.sourceSha256;
    journal_["token"] = status_.token;
    journal_["enabled"] = status_.enabled;
    journal_["restart_attempted"] = status_.restartAttempted;
    journal_["error"] = status_.error;
    journal_["sha256"] = sha256(jsonBytes(journal_));
    durableWrite(path_, journal_);
}
void Transaction::fail(const char* error) {
    status_.enabled = false;
    status_.error = error;
    status_.state = "error";
    save();
}
void Transaction::gate(bool checkSafety) {
    require(writePolicy_(), "writes disabled by build/probe policy");
    require(!checkSafety || safe_(), "call/emergency/radio/identity context unsafe or expired");
}
Snapshot Transaction::snapshot(const std::set<std::string>& paths) {
    Snapshot result;
    for (const auto& path : paths) {
        efs_.checkParent(path);
        result[path] = efs_.read(path);
    }
    return result;
}
void Transaction::verify(const Snapshot& wanted) {
    for (const auto& [path, bytes] : wanted)
        require(efs_.read(path) == bytes, "EFS readback mismatch");
}
void Transaction::mutate(const Snapshot& wanted, bool restoring) {
    const auto checkpoint = [&] { gate(!restoring); };
    if (wanted.size() == 1 && wanted.count(kControl) &&
        wanted.at(kControl) == std::optional<Bytes>(Bytes{7})) {
        gate(!restoring);
        efs_.write(kControl, Bytes{7}, checkpoint);
        verify(wanted);
        return;
    }
    // The auto-selection byte must be disabled before any overlay mutation.
    gate(!restoring);
    efs_.write(kControl, Bytes{0}, checkpoint);
    require(efs_.read(kControl) == std::optional<Bytes>(Bytes{0}), "control readback");
    for (const auto& [path, bytes] : wanted) {
        if (path == kControl) continue;
        gate(!restoring);
        if (bytes)
            efs_.write(path, *bytes, checkpoint);
        else
            efs_.remove(path, checkpoint);
    }
    gate(!restoring);
    require(wanted.count(kControl) && wanted.at(kControl), "missing saved control");
    if (restoring || *wanted.at(kControl) != Bytes{0})
        efs_.write(kControl, *wanted.at(kControl), checkpoint);
    verify(wanted);
}
void Transaction::enable(bool enabled) {
    if (!enabled) {
        status_.enabled = false;
        if (!pending() && status_.state != "rollback_failed") status_.state = "disabled";
        save();
        return;
    }
    require(status_.state != "rollback_failed", "rollback failure latched");
    status_.enabled = true;
    if (!pending()) {
        status_.state = "waiting_for_identity";
        status_.error.clear();
    }
    save();
}
void Transaction::evaluate(const std::vector<std::string>& homePlmns) {
    if (status_.state == "restart_pending" && status_.enabled) {
        const auto next = store_.select(homePlmns);
        if (next.state == "checking" && next.profile != status_.profile) {
            gate();
            rollback();
            enable(true);  // Supersede the old SIM before a single restart.
        }
    }
    if (pending() || status_.state == "rollback_failed") return;
    if (!status_.enabled) {
        status_.state = "disabled";
        return;
    }
    auto selection = store_.select(homePlmns);
    status_.state = selection.state;
    save();
}
void Transaction::execute(const Snapshot& wanted, const std::string& profile,
                          const std::string& token, bool force) {
    require(token.size() == 32 && token.find_first_not_of("0123456789abcdef") == std::string::npos,
            "transaction token");
    std::set<std::string> affected;
    for (const auto& [path, bytes] : wanted) {
        (void)bytes;
        affected.insert(path);
    }
    const auto before = snapshot(affected);
    gate();
    const auto metadata = store_.metadata(profile);
    if (before == wanted && !force) {
        status_.profile = profile;
        journal_["managed"] = encodeSnapshot(wanted);
        status_.sourceName = metadata["source_name"].asString();
        status_.sourceSha256 = metadata["source_sha256"].asString();
        status_.state = "no_change";
        status_.token.clear();
        save();
        return;
    }
    journal_["before"] = encodeSnapshot(before);
    journal_["wanted"] = encodeSnapshot(wanted);
    journal_["previous_profile"] = status_.profile;
    journal_["previous_source_name"] = status_.sourceName;
    journal_["previous_source_sha256"] = status_.sourceSha256;
    status_.profile = profile;
    status_.token = token;
    status_.restartAttempted = false;
    status_.sourceName = metadata["source_name"].asString();
    status_.sourceSha256 = metadata["source_sha256"].asString();
    status_.state = "staged";
    save();
    try {
        status_.state = "applying";
        save();
        mutate(wanted, false);
        status_.state = "readback_verified";
        save();
        status_.state = "restart_pending";
        save();
    } catch (...) {
        rollback();
        throw;
    }
}
void Transaction::apply(const std::vector<std::string>& homePlmns, const std::string& token) {
    require(status_.enabled && !pending() && status_.state != "rollback_failed",
            "application disabled/pending");
    const auto selection = store_.select(homePlmns);
    require(selection.state == "checking", "identity conflict/unsupported");
    status_.modemVersion = modemVersion_();  // Diagnostics only, never an approval gate.
    gate();
    Snapshot wanted = store_.load(selection.profile);
    wanted[kControl] = Bytes{0};
    if (!journal_.isMember("baseline")) {
        auto paths = store_.paths();
        paths.insert(kControl);
        const auto baseline = snapshot(paths);
        journal_["baseline"] = encodeSnapshot(baseline);
        save();
    }
    const auto baseline = decodeSnapshot(journal_["baseline"], store_.paths());
    if (journal_.isMember("managed")) {
        for (const auto& [path, bytes] : decodeSnapshot(journal_["managed"], store_.paths())) {
            (void)bytes;
            if (!wanted.count(path)) {
                require(baseline.count(path), "baseline missing old path");
                wanted[path] = baseline.at(path);
            }
        }
    }
    execute(wanted, selection.profile, token);
}
void Transaction::restore(const std::string& token) {
    gate();
    // A switch-off supersedes a verified overlay awaiting restart. Preserve its
    // managed paths for the next carrier switch, but restart only the 07 restore.
    if (status_.state == "restart_pending") {
        // Profile drift must not prevent the user's control-only restore. The
        // new transaction snapshots/readbacks just 07; retain old managed paths.
        if (!status_.profile.empty()) journal_["managed"] = journal_["wanted"];
        status_.state = "disabled";
        status_.token.clear();
        save();
    }
    require(!pending() && status_.state != "rollback_failed", "restore unavailable");
    status_.enabled = false;
    execute(Snapshot{{kControl, Bytes{7}}}, "", token, true);
}
void Transaction::rollback() {
    try {
        gate(false);
        status_.state = "rolling_back";
        save();
        mutate(decodeSnapshot(journal_["before"], store_.paths()), true);
        status_.profile = journal_["previous_profile"].asString();
        status_.token.clear();
        status_.sourceName = journal_["previous_source_name"].asString();
        status_.sourceSha256 = journal_["previous_source_sha256"].asString();
        fail("transaction rolled back and verified; enable explicitly to retry");
    } catch (...) {
        status_.state = "rollback_failed";
        status_.enabled = false;
        status_.error = "restoration failed; manual recovery required";
        save();
        throw;
    }
}
bool Transaction::claimRestart(const std::string& token) {
    require((status_.enabled || status_.profile.empty()) && status_.state == "restart_pending" &&
                    token == status_.token,
            "restart token/state");
    gate();
    if (status_.restartAttempted) return false;
    // Durable at-most-once mark BEFORE the coordinator calls rebootModem().
    status_.restartAttempted = true;
    save();
    return true;
}
void Transaction::resumeContext(const std::vector<std::string>& homePlmns) {
    require((status_.enabled || status_.profile.empty()) && status_.state == "restart_pending",
            "resume disabled/state");
    if (status_.profile.empty()) return;  // Restoring 07 is independent of carrier identity.
    const auto selection = store_.select(homePlmns);
    require(selection.state == "checking" &&
                    (status_.profile.empty() || selection.profile == status_.profile),
            "pending transaction identity conflict/changed");
}
void Transaction::complete(const std::string& token) {
    require((status_.enabled || status_.profile.empty()) && status_.state == "restart_pending" &&
                    status_.restartAttempted && token == status_.token,
            "completion token/state");
    gate();
    try {
        status_.state = "rechecking";
        save();
        verify(decodeSnapshot(journal_["wanted"], store_.paths()));
        if (!status_.profile.empty()) journal_["managed"] = journal_["wanted"];
        // Retain the saved bytes even if a final fsync fails. The committed
        // phase makes these inert; the next transaction replaces them.
        status_.token.clear();
        status_.state = status_.enabled ? "committed" : "disabled";
        status_.error.clear();
        save();
    } catch (...) {
        rollback();
        throw;
    }
}
void Transaction::recover() {
    if (!pending()) return;
    if (status_.state == "restart_pending") return;
    // Startup recovery is itself a modem mutation. Wait for a fresh known-safe
    // coordinator lease. Recover an interrupted switch-off as well as an apply.
    gate();
    rollback();
}
void Transaction::probe() {
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
    if (!pending()) status_.state = "probe_readonly";
}
}  // namespace mcfg
