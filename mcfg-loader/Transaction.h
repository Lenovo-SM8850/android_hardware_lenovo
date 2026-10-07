// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>
#include "EfsClient.h"

namespace mcfg {
struct Status {
    std::string state = "waiting_for_identity", profile, modemVersion, token, error, sourceName,
                sourceSha256;
    bool enabled = true, restartAttempted = false;
};
class Transaction {
  public:
    Transaction(ProfileStore& store, EfsTransport& efs, std::string directory,
                std::function<std::string()> modemVersion, std::function<bool()> writePolicy,
                std::function<bool()> safe);
    ~Transaction();
    const Status& status() const { return status_; }
    void enable(bool enabled);
    void evaluate(const std::vector<std::string>& homePlmns);
    void apply(const std::vector<std::string>& homePlmns, const std::string& token);
    void restore(const std::string& token);
    bool claimRestart(const std::string& token);
    void resumeContext(const std::vector<std::string>& homePlmns);
    void complete(const std::string& token);
    void recover();
    void probe();

  private:
    Snapshot snapshot(const std::set<std::string>& paths);
    void verify(const Snapshot& wanted);
    void mutate(const Snapshot& wanted, bool rollback);
    void execute(const Snapshot& wanted, const std::string& profile, const std::string& token,
                 bool force = false);
    void rollback();
    void save();
    void gate(bool checkSafety = true);
    void fail(const char* error);
    bool pending() const;
    ProfileStore& store_;
    EfsTransport& efs_;
    std::string path_;
    std::function<std::string()> modemVersion_;
    std::function<bool()> writePolicy_, safe_;
    Status status_;
    Json::Value journal_;
    int lockFd_ = -1;
};
}  // namespace mcfg
