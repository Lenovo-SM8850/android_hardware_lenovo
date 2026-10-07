// SPDX-License-Identifier: Apache-2.0
#include <aidl/vendor/lenovo/hardware/mcfg/BnMcfgLoader.h>
#include <aidl/vendor/lenovo/hardware/mcfg/IMcfgCallback.h>
#include <android-base/properties.h>
#include <android/binder_ibinder.h>
#include <android/binder_ibinder_platform.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <selinux/selinux.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include "Transaction.h"

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
bool writePolicy() {
    return security_getenforce() == 1;
}
class Loader final : public ipc::BnMcfgLoader {
  public:
    Loader() : store_(kRoot, kData) {}
    ndk::ScopedAStatus getStatus(ipc::McfgStatus* out) override {
        return run([&] { *out = status(); });
    }
    ndk::ScopedAStatus getProfiles(std::vector<ipc::ProfileEntry>* out) override {
        return run([&] {
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
                for (const auto& m : p["home_plmns"]) entry.homePlmns.push_back(m.asString());
                out->push_back(entry);
            }
        });
    }
    ndk::ScopedAStatus updateContext(const std::vector<std::string>& home,
                                     const std::string& baseband, bool safe) override {
        if (!authorized()) return denied();
        try {
            requireContext(home);
            require(baseband.size() <= 256, "baseband bounds");
        } catch (...) {
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
        if (!authorized()) return denied();
        return run([&] {
            transaction().enable(enabled);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus apply(const std::string& token, ipc::McfgStatus* out) override {
        return run([&] {
            std::vector<std::string> home;
            {
                std::lock_guard lock(contextMutex_);
                home = home_;
                applyingEpoch_ = epoch_.load();
            }
            transaction().recover();
            transaction().evaluate(home);
            transaction().apply(home, token);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus restore(const std::string& token, ipc::McfgStatus* out) override {
        return run([&] {
            applyingEpoch_ = epoch_.load();
            transaction().recover();
            transaction().restore(token);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus claimRestart(const std::string& token, bool* out) override {
        return run([&] {
            resumeContext();
            *out = transaction().claimRestart(token);
            notify(status());
        });
    }
    ndk::ScopedAStatus complete(const std::string& token, ipc::McfgStatus* out) override {
        return run([&] {
            resumeContext();
            transaction().complete(token);
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus probeReadOnly(ipc::McfgStatus* out) override {
        return run([&] {
            transaction().probe();
            *out = status();
            notify(*out);
        });
    }
    ndk::ScopedAStatus setCallback(const std::shared_ptr<ipc::IMcfgCallback>& callback) override {
        return run([&] { callback_ = callback; });
    }
    ndk::ScopedAStatus importProfile(const ndk::ScopedFileDescriptor& fd,
                                     const std::string& filename, const std::string& home,
                                     std::string* out) override {
        return run([&] {
            require(!transaction_ || transaction_->status().token.empty(),
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
        return run([&] {
            require(!transaction_ || transaction_->status().token.empty(),
                    "deletion deferred during transaction");
            store_.deleteImported(id);
            notify(status());
        });
    }

  private:
    static ndk::ScopedAStatus denied() {
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
        // the same selected profile. A mixed or changed SIM cannot resume it.
        applyingEpoch_ = epoch_.load();
    }
    std::string baseband() {
        std::lock_guard lock(contextMutex_);
        return baseband_;
    }
    Transaction& transaction() {
        if (!transaction_) {
            diag_ = std::make_unique<DiagTransport>();
            efs_ = std::make_unique<EfsClient>(*diag_, store_.paths(), false);
            transaction_ = std::make_unique<Transaction>(
                    store_, *efs_, kData, [&] { return baseband(); }, writePolicy,
                    [&] { return safe(); });
        }
        return *transaction_;
    }
    ipc::McfgStatus status() {
        ipc::McfgStatus out;
        if (transaction_) {
            const auto& s = transaction_->status();
            out.state = s.state;
            out.profileId = s.profile;
            out.firmwareGeneration = baseband();
            out.transactionToken = s.token;
            out.sourceName = s.sourceName;
            out.sourceSha256 = s.sourceSha256;
            out.enabled = s.enabled;
            out.restartAttempted = s.restartAttempted;
            out.detail = s.error;
        } else {
            out.state = "waiting_for_identity";
            out.enabled = true;
        }
        std::vector<std::string> home;
        {
            std::lock_guard lock(contextMutex_);
            home = home_;
        }
        auto selection = store_.select(home);
        out.candidateProfileId = selection.profile;
        // Enabling resets the transaction to waiting_for_identity. A loaded SIM
        // can already select a profile while the coordinator waits for safety;
        // reporting the transaction's old gate incorrectly suggests no SIM.
        if (out.enabled && out.state == "waiting_for_identity" &&
            out.transactionToken.empty() && selection.state == "checking")
            out.state = selection.state;
        if (out.enabled && out.state != "rollback_failed" && out.transactionToken.empty() &&
            selection.state != "checking")
            out.state = selection.state;
        out.firmwareGeneration = baseband();  // The V1 field is diagnostics only.
        out.writesAllowed = writePolicy();
        if (!out.transactionToken.empty() && !out.profileId.empty() &&
            selection.state != "checking")
            out.detail = "Carrier profile selection: " + selection.state +
                         "; no writes or restart until resolved";
        if (!detail_.empty()) out.detail = detail_;
        return out;
    }
    void notify(const ipc::McfgStatus& status) {
        if (callback_) callback_->onStatus(status);
    }
    template <typename F>
    ndk::ScopedAStatus run(F operation) {
        if (!authorized()) return denied();
        std::lock_guard lock(mutex_);
        try {
            detail_.clear();
            operation();
            return ndk::ScopedAStatus::ok();
        } catch (const std::exception& e) {
            safe_ = false;
            detail_ = e.what();
            // Parser/input failures must not disable an otherwise healthy loader.
            notify(status());
            return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(1, e.what());
        }
    }
    ProfileStore store_;
    std::unique_ptr<DiagTransport> diag_;
    std::unique_ptr<EfsClient> efs_;
    std::unique_ptr<Transaction> transaction_;
    std::mutex mutex_, contextMutex_;
    std::vector<std::string> home_;
    std::string baseband_;
    std::atomic<bool> safe_{false};
    std::atomic<int64_t> lease_{0}, epoch_{0}, applyingEpoch_{-1};
    std::string detail_;
    std::shared_ptr<ipc::IMcfgCallback> callback_;
};
}  // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--probe-readonly") {
            require(getuid() == 0 || getuid() == 1001 || getuid() == 1000, "probe caller");
            ProfileStore store(kRoot);
            DiagTransport diag;
            EfsClient efs(diag, store.paths(), true);
            auto paths = store.paths();
            paths.insert(kControl);
            Snapshot snapshot;
            for (const auto& path : paths) {
                efs.checkParent(path);
                snapshot[path] = efs.read(path);
            }
            std::cout << "read-only transport/EFS PASS snapshot_sha256="
                      << sha256(jsonBytes(encodeSnapshot(snapshot))) << '\n';
            return 0;
        }
        require(argc == 1, "unsupported argument");
        ABinderProcess_setThreadPoolMaxThreadCount(4);
        auto service = ndk::SharedRefBase::make<Loader>();
        require(AIBinder_setRequestingSid && AIBinder_getCallingSid, "binder SID API unavailable");
        const auto binder = service->asBinder();
        AIBinder_setRequestingSid(binder.get(), true);
        auto name = std::string(ipc::IMcfgLoader::descriptor) + "/default";
        require(AServiceManager_addService(binder.get(), name.c_str()) == STATUS_OK,
                "binder registration");
        ABinderProcess_joinThreadPool();
    } catch (const std::exception& e) {
        std::cerr << "mcfg-loader: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
