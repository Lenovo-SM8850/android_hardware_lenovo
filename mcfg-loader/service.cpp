// SPDX-License-Identifier: Apache-2.0
#include <aidl/vendor/lenovo/hardware/mcfg/BnMcfgLoader.h>
#include <aidl/vendor/lenovo/hardware/mcfg/IMcfgCallback.h>
#include <android-base/properties.h>
#include <android/binder_ibinder.h>
#include <android/binder_ibinder_platform.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include "Transaction.h"
#include "Recovery.h"
#include "EfsDiagnostics.h"
#include "Log.h"
#include "FirmwareGeneration.h"

namespace {
using namespace mcfg;
namespace ipc = aidl::vendor::lenovo::hardware::mcfg;
constexpr char kRoot[] = "/vendor/etc/mcfg-loader";
constexpr char kData[] = "/data/vendor/mcfg-loader";
int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}
// Journal construction must not open DIAG just to expose a persisted failure.
class JournalOnlyEfs final : public EfsTransport {
  public:
    std::optional<Bytes> read(const std::string&) override {
        throw std::runtime_error("journal-only EFS read");
    }
    void checkParent(const std::string&) override {
        throw std::runtime_error("journal-only EFS parent check");
    }
    void write(const std::string&, const Bytes&, const std::function<void()>&) override {
        throw std::runtime_error("journal-only EFS write");
    }
    void remove(const std::string&, const std::function<void()>&) override {
        throw std::runtime_error("journal-only EFS unlink");
    }
};
class Loader final : public ipc::BnMcfgLoader {
  public:
    Loader() : lock_(std::make_shared<TransactionLock>(kData)), store_(kRoot, kData) {
        initialTransaction_ = std::make_unique<Transaction>(
                store_, journalEfs_, kData, [] { return ""; }, [] { return false; }, lock_,
                std::function<int64_t()>{}, [&] { return firmware_.get(); });
        initialStatus_ = initialTransaction_->status();
    }
    ndk::ScopedAStatus getStatus(ipc::McfgStatus* out) override {
        return run("getStatus", [&] { *out = status(); });
    }
    ndk::ScopedAStatus getProfiles(std::vector<ipc::ProfileEntry>* out) override {
        return run("getProfiles", [&] {
            out->clear();
            for (const auto& p : store_.profiles()) {
                ipc::ProfileEntry entry;
                entry.id = p["id"].asString();
                entry.archiveSha256 = p["archive_sha256"].asString();
                entry.imported = p["imported"].asBool();
                entry.sourceName = p["source_name"].asString();
                entry.sourceSha256 =
                        p[entry.imported ? "source_sha256" : "source_mbn_sha256"].asString();
                entry.entryCount = entry.imported ? p["entries"].size() : p["entries"].asInt();
                for (const auto& m : p["home_plmns"]) {
                    const auto home = m.asString();
                    entry.homePlmns.push_back(home);
                    if (entry.imported && store_.chosenImported(home) == entry.id)
                        entry.chosenHomePlmns.push_back(home);
                }
                out->push_back(entry);
            }
        });
    }
    ndk::ScopedAStatus updateContext(const std::vector<std::string>& home,
                                     const std::string& baseband, bool safe) override {
        if (!authorized()) return denied("updateContext");
        try {
            requireContext(home);
            require(baseband.size() <= 256, "baseband bounds");
        } catch (const std::exception& error) {
            MCFG_ERROR("IPC updateContext failed: %s", error.what());
            safe_ = false;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        // This method never waits behind the EFS transaction mutex. A Binder
        // thread can revoke the lease while another thread is writing.
        {
            std::lock_guard lock(contextMutex_);
            if (home != home_ || baseband != baseband_) {
                safe = false;
                home_ = home;
                baseband_ = baseband;
                ++epoch_;
            }
            safe_ = safe;
            lease_ = now();
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setEnabled(bool enabled, ipc::McfgStatus* out) override {
        if (!authorized()) return denied("setEnabled");
        return run("setEnabled", [&] {
            std::vector<std::string> home;
            {
                std::lock_guard lock(contextMutex_);
                home = home_;
            }
            transaction().enable(enabled, home);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus apply(const std::string& token, ipc::McfgStatus* out) override {
        return run("apply", [&] {
            std::vector<std::string> home;
            {
                std::lock_guard lock(contextMutex_);
                home = home_;
                applyingEpoch_ = epoch_.load();
            }
            if (!home.empty()) transaction().recover();
            transaction().evaluate(home);
            transaction().apply(home, token);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus restore(const std::string& token, ipc::McfgStatus* out) override {
        return run("restore", [&] {
            applyingEpoch_ = epoch_.load();
            transaction().recover();
            transaction().restore(token);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus claimRestart(const std::string& token, bool* out) override {
        return run("claimRestart", [&] {
            resumeContext();
            *out = transaction().claimRestart(token);
            // The frozen V1 claim also requests init's restricted oneshot helper.
            if (*out) {
                diag_->invalidate();
                const bool requested = android::base::SetProperty("vendor.lenovo.mcfg.restart", "0") &&
                                android::base::SetProperty("vendor.lenovo.mcfg.restart", "1");
                if (!requested) transaction().restartFailed("vendor modem restart request failed");
                require(requested, "vendor modem restart request failed");
            }
            notify(status());
        });
    }
    ndk::ScopedAStatus recover(ipc::McfgStatus* out) override {
        return run("recover", [&] {
            {
                std::lock_guard lock(contextMutex_);
                applyingEpoch_ = epoch_.load();
            }
            transaction().recover();
            // The coordinator calls recover for a claimed restart only after
            // its bounded recovery wait expires without a down/up witness.
            transaction().restartFailed("modem recovery not observed; explicit Apply required");
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus complete(const std::string& token, ipc::McfgStatus* out) override {
        return run("complete", [&] {
            resumeContext();
            transaction().complete(token);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus probeReadOnly(ipc::McfgStatus* out) override {
        return run("probeReadOnly", [&] {
            transaction().probe();
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus setCallback(const std::shared_ptr<ipc::IMcfgCallback>& callback) override {
        return run("setCallback", [&] { callback_ = callback; });
    }
    ndk::ScopedAStatus importProfile(const ndk::ScopedFileDescriptor& fd,
                                     const std::string& filename, const std::string& home,
                                     std::string* out) override {
        return run("importProfile", [&] {
            require((transaction_ ? transaction_->status() : initialStatus_).token.empty(),
                    "import deferred during transaction");
            struct stat st{};
            require(fstat(fd.get(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
                            st.st_size <= 8 * 1024 * 1024,
                    "import file type/size");
            Bytes bytes(st.st_size);
            size_t offset = 0;
            while (offset < bytes.size()) {
                const auto n =
                        pread(fd.get(), bytes.data() + offset, bytes.size() - offset, offset);
                if (n < 0 && errno == EINTR) continue;
                require(n > 0, "import short read");
                offset += n;
            }
            *out = store_.importMbn(bytes, filename, home);
            notify(status());
        });
    }
    ndk::ScopedAStatus deleteImportedProfile(const std::string& id) override {
        return run("deleteImportedProfile", [&] {
            require((transaction_ ? transaction_->status() : initialStatus_).token.empty(),
                    "deletion deferred during transaction");
            store_.deleteImported(id);
            notify(status());
        });
    }

    ndk::ScopedAStatus chooseImportedProfile(const std::string& home, const std::string& id,
                                             ipc::McfgStatus* out) override {
        return run("chooseImportedProfile", [&] {
            require((transaction_ ? transaction_->status() : initialStatus_).token.empty(),
                    "selection deferred during transaction");
            store_.chooseImported(home, id);
            *out = status();
            notify(*out);
        });
    }

  private:
    static ndk::ScopedAStatus denied(const char* operation) {
        MCFG_ERROR("IPC %s failed: unauthorized caller", operation);
        return ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
    }
    static bool authorized() {
        if (AIBinder_getCallingUid() != 1000 || !AIBinder_getCallingSid) return false;
        const char* sid = AIBinder_getCallingSid();
        return sid && std::string(sid) == "u:r:lenovo_mcfg_coordinator:s0";
    }
    static void requireContext(const std::vector<std::string>& home) {
        require(home.size() <= 2, "subscription count");
        for (const auto& value : home) require(value.size() <= 6, "identity bounds");
    }
    bool safe() const { return safe_ && now() - lease_ < 5000 && applyingEpoch_ == epoch_; }
    void resumeContext() {
        auto& txn = transaction();
        std::lock_guard lock(contextMutex_);
        txn.resumeContext(home_);
        // After an SSR or process restart, accept a fresh stable lease only for
        // the same selected overlay, or any safe identity for a control-only restore.
        applyingEpoch_ = epoch_.load();
    }
    std::string baseband() {
        std::lock_guard lock(contextMutex_);
        return baseband_;
    }
    Transaction& transaction() {
        if (!transaction_) {
            diag_ = std::make_unique<DiagTransport>(
                    [] {
                        // This signal can change without a Binder call to us.
                        return android::base::GetProperty(
                                       "init.svc.vendor.lenovo.mcfg-restart", "stopped") != "running";
                    },
                    [&] { return safe_ && now() - lease_ < 5000; });
            efs_ = std::make_unique<EfsClient>(*diag_, store_.paths(), false);
            transaction_ = std::make_unique<Transaction>(
                    store_, *efs_, kData, [&] { return baseband(); },
                    [&] { return safe(); }, lock_, std::function<int64_t()>{},
                    [&] { return firmware_.get(); });
        }
        return *transaction_;
    }
    ipc::McfgStatus status() {
        ipc::McfgStatus out;
        const auto& s = transaction_ ? transaction_->status() : initialStatus_;
        out.state = s.state;
        out.profileId = s.profile;
        // OFF does not read firmware metadata. Status only projects the cached
        // generation; no journal transition or transaction starts here.
        const std::string generation = s.enabled ? firmware_.get() : "";
        out.firmwareGeneration = baseband();
        out.transactionToken = s.token;
        out.sourceName = s.sourceName;
        out.sourceSha256 = s.sourceSha256;
        out.enabled = s.enabled;
        out.restartAttempted = s.restartAttempted;
        out.nextProfileId = s.nextProfile;
        // Unknown until a fresh coordinator lease permits opening DIAG. Reading
        // only the control cannot mistake an absent/malformed byte for automatic.
        if (safe_ && now() - lease_ < 5000 &&
            !((s.state == "restart_pending" || s.state == "baseline_restart_pending") &&
              s.restartAttempted) &&
            (transaction_ || now() >= autoselectRetryAt_)) {
            try {
                // Also throttle cold-start HELLO failures before a Transaction exists.
                autoselectRetryAt_ = now() + 30'000;
                out.autoselect = transaction().autoselect();
            } catch (const std::exception& error) {
                logEfsError(std::string("Autoselect status unavailable: ") + error.what());
            }
        }
        out.detail = s.error;
        std::vector<std::string> home;
        {
            std::lock_guard lock(contextMutex_);
            home = home_;
        }
        auto selection = store_.select(home);
        out.candidateProfileId = selection.profile;
        if (out.enabled && out.transactionToken.empty() && selection.state == "checking") {
            // The journal-only view also retains the durable failed-key gate.
            // Constructing EfsClient performs HELLO, which must stay inside the
            // lease-guarded status probe above or an actual transaction call.
            auto& journal = transaction_ ? *transaction_ : *initialTransaction_;
            if (journal.firmwareRepairBlocked(selection.profile))
                out.state = "firmware_repair_blocked";
            else if (out.state == "firmware_repair_blocked")
                out.state = "checking";  // A different generation/source has its own budget.
            else if (out.state == "committed" &&
                    (generation.empty() || s.appliedFirmware != generation))
                out.state = "checking";
        }
        // Enabling resets the transaction to waiting_for_identity. A loaded SIM
        // can already select a profile while the coordinator waits for safety;
        // reporting the transaction's old gate incorrectly suggests no SIM.
        if (out.enabled && out.state == "waiting_for_identity" &&
            out.transactionToken.empty() && selection.state == "checking")
            out.state = selection.state;
        if (out.enabled && out.state != "error" &&
            out.transactionToken.empty() &&
            selection.state != "checking" && !home.empty())
            out.state = selection.state;
        if (out.enabled && out.state == "restart_pending" && !out.profileId.empty() &&
            selection.state == "unsupported")
            out.state = selection.state;  // Allow control-only restore to supersede the overlay.
        out.firmwareGeneration = baseband();  // The V1 field is diagnostics only.
        out.writesAllowed = true;  // Kept for the frozen V1 interface.
        if (!out.transactionToken.empty() && !out.profileId.empty() &&
            selection.state != "checking")
            out.detail = "Carrier profile selection: " + selection.state +
                         "; no writes or restart until resolved";
        if (!detail_.empty()) out.detail = detail_;
        return out;
    }
    void notify(const ipc::McfgStatus& status) {
        if (callback_) {
            auto result = callback_->onStatus(status);
            if (!result.isOk()) MCFG_ERROR("IPC onStatus failed: %s", result.getMessage());
        }
    }
    template <typename F>
    ndk::ScopedAStatus run(const char* name, F operation) {
        if (!authorized()) return denied(name);
        std::lock_guard lock(mutex_);
        try {
            detail_.clear();
            operation();
            return ndk::ScopedAStatus::ok();
        } catch (const std::exception& e) {
            logEfsError(std::string("IPC ") + name + " failed: " + e.what());
            safe_ = false;
            detail_ = e.what();
            // Parser/input failures must not disable an otherwise healthy loader.
            notify(status());
            return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(1, e.what());
        }
    }
    std::shared_ptr<TransactionLock> lock_;
    ProfileStore store_;
    JournalOnlyEfs journalEfs_;
    std::unique_ptr<Transaction> initialTransaction_;
    Status initialStatus_;
    FirmwareGeneration firmware_;
    std::unique_ptr<DiagTransport> diag_;
    std::unique_ptr<EfsClient> efs_;
    std::unique_ptr<Transaction> transaction_;
    std::mutex mutex_, contextMutex_;
    std::vector<std::string> home_;
    std::string baseband_;
    std::atomic<bool> safe_{false};
    std::atomic<int64_t> lease_{0}, epoch_{0}, applyingEpoch_{-1};
    std::string detail_;
    int64_t autoselectRetryAt_ = 0;
    std::shared_ptr<ipc::IMcfgCallback> callback_;
};
}  // namespace
int main(int argc, char** argv) {
    const std::string mode = argc == 2 ? argv[1] : "";
    if (mode == "--diag-session") return DiagTransport::runSession();
    try {
        if (mode == "--probe-readonly" || mode == "--dump-readonly") {
            const uid_t caller = getuid();
            require(caller == 0 || caller == 1001, "CLI caller must be root or radio");
            // Root commands use the service UID so new lock/journal files stay readable by radio.
            if (caller == 0) {
                const gid_t groups[] = {1001, 1000};
                require(setgroups(2, groups) == 0 && setgid(1001) == 0 && setuid(1001) == 0,
                        "CLI radio credentials");
            }
            ProfileStore store(kRoot);
            if (mode != "--probe-readonly") {
                std::unique_ptr<DiagTransport> diag;
                return runRecoveryCli(mode, caller, store, kData,
                                      [&](bool readOnly) -> std::unique_ptr<EfsTransport> {
                                          diag = std::make_unique<DiagTransport>();
                                          return std::make_unique<EfsClient>(*diag, store.paths(), readOnly);
                                      }, std::cout, std::cerr);
            }
            TransactionLock lock(kData);
            DiagTransport diag;
            EfsClient efs(diag, store.paths(), true);
            auto paths = store.paths();
            paths.insert(kControl);
            Snapshot snapshot;
            for (const auto& path : paths) {
                snapshot[path] = efsStep(path, "snapshot", [&] {
                    efs.checkParent(path);
                    return efs.read(path);
                });
            }
            std::cout << "read-only transport/EFS PASS snapshot_sha256="
                      << sha256(jsonBytes(encodeSnapshot(snapshot))) << '\n';
            return 0;
        }
        require(argc == 1, "unsupported argument");
        ABinderProcess_setThreadPoolMaxThreadCount(4);
        // Setting the limit alone starts no workers. Context updates must run
        // while serialized EFS calls are probing/writing, including revocation.
        ABinderProcess_startThreadPool();
        auto service = ndk::SharedRefBase::make<Loader>();
        require(AIBinder_setRequestingSid && AIBinder_getCallingSid, "binder SID API unavailable");
        const auto binder = service->asBinder();
        AIBinder_setRequestingSid(binder.get(), true);
        auto name = std::string(ipc::IMcfgLoader::descriptor) + "/default";
        require(AServiceManager_addService(binder.get(), name.c_str()) == STATUS_OK,
                "binder registration");
        ABinderProcess_joinThreadPool();
    } catch (const std::exception& e) {
        MCFG_ERROR("Loader main failed: %s", e.what());
        std::cerr << "mcfg-loader: " << e.what() << '\n';
        if (mode == "--dump-readonly") std::cout << "DUMP_FAIL\n";
        return 1;
    }
    return 0;
}
