// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>
#include <memory>
#include <ostream>
#include "EfsClient.h"

namespace mcfg {
struct Status {
    std::string state = "disabled", profile, modemVersion, token, error, sourceName,
                sourceSha256, nextProfile, step;
    bool enabled = false, restartAttempted = false;
};
class TransactionLock {
  public:
    explicit TransactionLock(const std::string& directory);
    ~TransactionLock();
    TransactionLock(const TransactionLock&) = delete;
    TransactionLock& operator=(const TransactionLock&) = delete;
    const std::string& directory() const { return directory_; }

  private:
    std::string directory_;
    int fd_ = -1;
};
class Transaction {
  public:
    Transaction(ProfileStore& store, EfsTransport& efs, std::string directory,
                std::function<std::string()> modemVersion, std::function<bool()> writePolicy,
                std::function<bool()> safe, std::shared_ptr<TransactionLock> lock = {},
                std::function<int64_t()> clock = {});
    ~Transaction();
    const Status& status() const { return status_; }
    // 7 means exactly 07; 0 includes absent or malformed controls.
    int autoselect(bool refresh = false);
    void enable(bool enabled);
    void evaluate(const std::vector<std::string>& homePlmns);
    void apply(const std::vector<std::string>& homePlmns, const std::string& token);
    void restore(const std::string& token);
    // The Binder service also triggers the vendor restart after a successful claim.
    bool claimRestart(const std::string& token);
    void resumeContext(const std::vector<std::string>& homePlmns);
    void complete(const std::string& token);
    void recover();
    void probe();
    bool dumpReadOnly(std::ostream& output);

  private:
    bool committedSourceMatches(const std::string& profile) const;
    Snapshot snapshot(const std::set<std::string>& paths);
    void verify(const Snapshot& wanted, const char* step = "readback");
    void mutate(const Snapshot& wanted);
    void execute(const Snapshot& wanted, const std::string& profile, const std::string& token,
                 const std::string& nextProfile = "", bool baseline = false);
    void save();
    void setState(const std::string& state);
    void gate(bool checkSafety = true);
    void fail(const char* error);
    bool pending() const;
    ProfileStore& store_;
    EfsTransport& efs_;
    std::string path_;
    std::function<std::string()> modemVersion_;
    std::function<bool()> writePolicy_, safe_;
    std::function<int64_t()> clock_;
    bool baselineReset_ = false;
    int autoselect_ = -1;
    int64_t autoselectUntil_ = 0;
    Status status_;
    Json::Value journal_;
    std::shared_ptr<TransactionLock> lock_;
};
}  // namespace mcfg
