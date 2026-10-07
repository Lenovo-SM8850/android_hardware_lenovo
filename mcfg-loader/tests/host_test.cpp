// SPDX-License-Identifier: Apache-2.0
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "MbnParser.h"
#include "Transaction.h"

using namespace mcfg;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    require(ok, message);
}
void throws(const std::function<void()>& action, const char* message) {
    bool caught = false;
    try {
        action();
    } catch (...) {
        caught = true;
    }
    check(caught, message);
}
class FakeEfs final : public EfsTransport {
  public:
    Snapshot files;
    int writes = 0, removes = 0, failAt = -1;
    bool failForever = false, corruptOnce = false, parentMissing = false;
    std::string journal;
    std::vector<std::pair<std::string, Bytes>> written;
    std::function<void(int)> afterWrite;
    std::optional<Bytes> read(const std::string& path) override {
        if (corruptOnce && writes > 0 && path != kControl) {
            corruptOnce = false;
            return Bytes{0xee};
        }
        return files.count(path) ? files.at(path) : std::optional<Bytes>{};
    }
    void write(const std::string& path, const Bytes& bytes,
               const std::function<void()>& checkpoint) override {
        if (checkpoint) checkpoint();
        // Exercise the actual serialized WAL, not an in-memory state flag.
        const auto wal = parseJson(readFile(journal, 40 * 1024 * 1024));
        check(wal.isMember("before") && wal.isMember("wanted") && !wal["sha256"].asString().empty(),
              "WAL before every modem write");
        files[path] = bytes;
        ++writes;
        written.emplace_back(path, bytes);
        if (afterWrite) afterWrite(writes);
        if (writes == failAt || (failForever && writes >= failAt))
            throw std::runtime_error("injected partial write");
    }
    void remove(const std::string& path, const std::function<void()>& checkpoint) override {
        if (checkpoint) checkpoint();
        ++removes;
        files[path] = {};
    }
    void checkParent(const std::string&) override {
        require(!parentMissing, "injected missing parent");
    }
};
class ScriptedWire final : public PacketTransport {
  public:
    std::vector<Bytes> replies, requests;
    Bytes exchange(const Bytes& request) override {
        requests.push_back(request);
        require(!replies.empty(), "unexpected packet");
        auto out = replies.front();
        replies.erase(replies.begin());
        return out;
    }
};
void put(Bytes& bytes, uint32_t value) {
    for (int n = 0; n < 4; ++n) bytes.push_back((value >> (8 * n)) & 255);
}
Bytes packet(int command, std::initializer_list<uint32_t> fields) {
    Bytes out{0x4b, 0x13, uint8_t(command), 0};
    for (auto f : fields) put(out, f);
    return out;
}
Bytes hello() {
    return packet(0, {1, 1024, 1, 1024, 1, 1024, 1, 1, 1, 0xffffffff});
}
Bytes stat(bool directory, uint32_t size = 1, uint32_t error = 0) {
    return packet(16, {error, directory ? 0040000u : 0100000u, size, 1, 0, 0, 0});
}
void wireTests() {
    const std::string path = "/data/a";
    ScriptedWire wire;
    wire.replies = {hello(), stat(false), packet(2, {9, 0}), packet(4, {9, 0, 1, 0})};
    wire.replies.back().push_back(7);
    wire.replies.push_back(packet(3, {0}));
    EfsClient efs(wire, {path}, true);
    check(efs.read(path) == std::optional<Bytes>(Bytes{7}), "EFS read codec");
    check(wire.requests[2][4] == 0, "probe OPEN read-only");
    auto requests = wire.requests.size();
    throws([&] { efs.write(path, Bytes{1}); }, "probe refuses write");
    check(requests == wire.requests.size(), "probe emits no write packets");
    throws([&] { efs.read("/secret"); }, "unapproved read path");
    ScriptedWire missing;
    missing.replies = {hello(), stat(false, 0, 2)};
    EfsClient absent(missing, {path}, true);
    check(!absent.read(path), "ENOENT recorded absent");
    ScriptedWire error;
    error.replies = {hello(), stat(false, 0, 13)};
    EfsClient denied(error, {path}, true);
    throws([&] { denied.read(path); }, "EACCES never treated as absent");
    ScriptedWire shortWrite;
    shortWrite.replies = {
            hello(),       stat(true), stat(false), packet(2, {9, 0}), packet(5, {9, 0, 0, 0}),
            packet(3, {0})};
    EfsClient writable(shortWrite, {path}, false);
    throws([&] { writable.write(path, Bytes{8}); }, "short write rejected");
    check(shortWrite.requests.back()[2] == 3, "close on failed write");
    ScriptedWire interrupted;
    interrupted.replies = {
            hello(),       stat(true), stat(false), packet(2, {9, 0}), packet(5, {9, 0, 1024, 0}),
            packet(3, {0})};
    EfsClient chunked(interrupted, {path}, false);
    int checkpoints = 0;
    throws(
            [&] {
                chunked.write(path, Bytes(2048, 8), [&] {
                    if (++checkpoints == 3) throw std::runtime_error("incoming call");
                });
            },
            "lease revoked between chunks");
    check(interrupted.requests.size() == 6 && interrupted.requests.back()[2] == 3,
          "second chunk not sent; handle closed");
    for (auto bad : {packet(4, {8, 0, 1, 0}), packet(4, {9, 5, 1, 0}), packet(4, {9, 0, 2, 0}),
                     packet(4, {9, 0, 1, 13})}) {
        ScriptedWire invalid;
        invalid.replies = {hello(), stat(false), packet(2, {9, 0}), bad, packet(3, {0})};
        EfsClient invalidEfs(invalid, {path}, true);
        throws([&] { invalidEfs.read(path); }, "bad READ identity/offset/count/errno");
        check(invalid.requests.back()[2] == 3, "close on failed read");
    }
    ScriptedWire badHeader;
    auto wrong = hello();
    wrong[1] = 62;
    badHeader.replies = {wrong};
    throws([&] { EfsClient reject(badHeader, {path}, true); }, "alternate EFS subsystem rejected");
    const Bytes raw{0x4b, 0x13, 3, 0, 0, 0, 0, 0};
    check(DiagTransport::decodeCallback(raw) == std::vector<Bytes>{raw}, "raw callback");
    Bytes framed{0x7e, 1, uint8_t(raw.size()), 0};
    framed.insert(framed.end(), raw.begin(), raw.end());
    framed.push_back(0x7e);
    check(DiagTransport::decodeCallback(framed) == std::vector<Bytes>{raw}, "non-HDLC envelope");
    framed[2]++;
    throws([&] { DiagTransport::decodeCallback(framed); }, "envelope length");
    uint16_t crc = 0xffff;
    for (auto b : raw) {
        crc ^= b;
        for (int n = 0; n < 8; ++n) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
    }
    crc ^= 0xffff;
    Bytes hdlc = raw;
    hdlc.push_back(crc & 255);
    hdlc.push_back(crc >> 8);
    hdlc.push_back(0x7e);
    check(DiagTransport::decodeCallback(hdlc) == std::vector<Bytes>{raw}, "HDLC CRC");
    hdlc[0] ^= 1;
    throws([&] { DiagTransport::decodeCallback(hdlc); }, "HDLC CRC corruption");
}
Snapshot base(const ProfileStore& store) {
    Snapshot out;
    for (const auto& path : store.paths()) out[path] = Bytes{0x17};
    out[*store.paths().begin()] = {};
    out[kControl] = Bytes{7};
    return out;
}
void checksum(Json::Value& value) {
    value.removeMember("sha256");
    value["sha256"] = sha256(jsonBytes(value));
}
}  // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string(argv[1]) == "--parse") {
            fs::create_directories(argv[4]);
            ProfileStore store(argv[2], argv[4]);
            auto bytes = readFile(argv[3], 8 * 1024 * 1024);
            auto parsed = parseMbn(bytes, store.paths());
            auto id = store.importMbn(bytes, "sample.mbn", "45006");
            check(store.select({"45006"}).profile == id && store.select({"450006"}).profile == id,
                  "import precedence and carrier aliases");
            check(store.select({"45006", "45005"}).state == "conflict",
                  "import mixed-carrier refusal");
            ProfileStore persisted(argv[2], argv[4]);
            check(persisted.load(id) == parsed, "import persistence/integrity");
            persisted.deleteImported(id);
            check(persisted.select({"45006"}).profile == "Korea/LGU/Commercial",
                  "delete falls back to bundled");
            durableWrite(argv[5], encodeSnapshot(parsed));
            return 0;
        }
        require(argc == 3, "usage: host_test payload-root scratch-root");
        const std::string root = argv[1], scratch = argv[2];
        fs::create_directories(scratch);
        const std::string fixture = scratch + "/profiles";
        fs::create_directories(fixture);
        for (const auto& item : fs::directory_iterator(root))
            if (item.path().extension() == ".pack" || item.path().filename() == "profiles.json")
                fs::copy_file(item.path(), fs::path(fixture) / item.path().filename(),
                              fs::copy_options::overwrite_existing);
        ProfileStore packaged(fixture);
        const auto index = parseJson(readFile(fixture + "/profiles.json", 2 * 1024 * 1024));
        for (const auto& item : index["profiles"])
            check(packaged.load(item["id"].asString()).size() == item["entries"].asUInt(),
                  "all archive digests/entries");
        check(packaged.select({"45006"}).profile == "Korea/LGU/Commercial", "five-digit LGU");
        check(packaged.select({"450006"}).profile == "Korea/LGU/Commercial",
              "six-digit LGU with zero");
        check(packaged.select({"45006", "450006"}).state == "checking", "same-profile DSDS");
        for (const auto& homes : std::vector<std::vector<std::string>>{
                     {"45006", "45005"}, {"45006", "99999"}, {"45005", "45008"}})
            check(packaged.select(homes).state == "conflict", "mixed/unknown DSDS refusal");
        check(packaged.select({"45008"}).profile == "Korea/KT/Commercial_KT_LTE", "KT home match");
        check(packaged.select({"4500006"}).state == "waiting_for_identity",
              "no numeric normalization");
        check(packaged.select({}).state == "waiting_for_identity", "no SIM");
        check(packaged.select({"45007"}).state == "unsupported", "no broad 450 match");
        // Ambiguous variants are refused even if an approved index has duplicates.
        auto ambiguousIndex = index;
        ambiguousIndex["profiles"][0]["home_plmns"].append("45006");
        durableWrite(fixture + "/profiles.json", ambiguousIndex);
        ProfileStore ambiguous(fixture);
        check(ambiguous.select({"45006"}).state == "conflict", "ambiguous carrier variants");
        durableWrite(fixture + "/profiles.json", index);
        const auto baseline = base(packaged);
        ProfileStore store(fixture);
        const std::string token(32, 'a');
        int scenario = 0;
        auto setup = [&] {
            std::string dir = scratch + "/case" + std::to_string(++scenario);
            fs::create_directories(dir);
            return dir;
        };
        auto run = [&](const std::function<void(Transaction&, FakeEfs&, std::string&, bool&, bool&,
                                                const std::string&)>& test) {
            const auto dir = setup();
            FakeEfs efs;
            efs.files = baseline;
            efs.journal = dir + "/journal.json";
            std::string generation = "g1";
            bool policy = true, safe = true;
            Transaction txn(
                    store, efs, dir, [&] { return generation; }, [&] { return policy; },
                    [&] { return safe; });
            check(txn.status().enabled, "Automatic on from first boot");
            txn.enable(true);
            test(txn, efs, generation, policy, safe, dir);
        };
        run([&](auto& txn, auto& efs, auto&, auto&, auto& safe, const auto&) {
            txn.apply({"45006"}, token);
            efs.files[store.load("Korea/LGU/Commercial").begin()->first] = Bytes{0xee};
            auto beforeOff = efs.files;
            int writes = efs.writes;
            txn.enable(false);
            safe = false;
            throws([&] { txn.restore(token); }, "unsafe switch-off deferred");
            check(efs.writes == writes, "unsafe switch-off zero writes");
            safe = true;
            txn.restore(token);
            beforeOff[kControl] = Bytes{7};
            check(efs.files == beforeOff && efs.writes == writes + 1,
                  "switch-off supersedes drift with only 07");
            check(txn.claimRestart(token), "switch-off restart claim while disabled");
            txn.complete(token);
            check(txn.status().state == "disabled", "switch-off completion while disabled");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            txn.restore(token);
            txn.enable(true);
            txn.evaluate({"45006"});
            check(txn.status().state == "checking" && txn.status().enabled,
                  "reenable supersedes pending 07 restore");
            txn.apply({"45006"}, token);
            check(efs.files[kControl] == std::optional<Bytes>(Bytes{0}),
                  "reenable immediately reapplies 00");
            check(txn.claimRestart(token), "only selected profile claims restart after reenable");
            txn.complete(token);
        });
        run([&](auto& txn, auto& efs, auto& generation, auto& policy, auto& safe, const auto&) {
            (void)generation;
            (void)policy;
            (void)safe;
            throws([&] { txn.apply({"45006", "45005"}, token); }, "conflict no apply");
            check(efs.writes == 0, "conflict zero writes");
            txn.apply({"45006"}, token);
            check(txn.status().state == "restart_pending", "verified pending restart");
            check(efs.written.front() == std::make_pair(std::string(kControl), Bytes{0}),
                  "first write is autoselection 00");
            throws([&] { txn.resumeContext({"45006", "45005"}); }, "pending mixed SIM refusal");
            throws([&] { txn.resumeContext({"45005"}); }, "pending changed carrier refusal");
            txn.resumeContext({"45006", "450006"});
            txn.enable(false);
            throws([&] { txn.resumeContext({"45006"}); }, "disabled pending resume refusal");
            txn.enable(true);
            check(txn.claimRestart(token), "first restart");
            check(!txn.claimRestart(token), "one restart cap");
            throws([&] { txn.complete(std::string(32, 'b')); }, "wrong completion token");
            txn.complete(token);
            check(txn.status().state == "committed", "commit after recheck");
            int writes = efs.writes;
            txn.apply({"45006"}, token);
            check(efs.writes == writes && txn.status().state == "no_change",
                  "unchanged boot read-only");
            auto lgu = store.load("Korea/LGU/Commercial"), skt = store.load("Korea/SKT/Commercial");
            txn.apply({"45005"}, token);
            for (const auto& [path, bytes] : lgu)
                if (!skt.count(path)) {
                    (void)bytes;
                    check(efs.files[path] == baseline.at(path), "carrier switch old-only baseline");
                }
            txn.claimRestart(token);
            txn.complete(token);
            auto beforeOff = efs.files;
            int offWrites = efs.writes;
            txn.restore(token);
            check(efs.writes == offWrites + 1 &&
                          efs.written.back() == std::make_pair(std::string(kControl), Bytes{7}),
                  "switch-off writes only 07");
            beforeOff[kControl] = Bytes{7};
            check(efs.files == beforeOff, "switch-off preserves every other path");
            txn.claimRestart(token);
            txn.complete(token);
            check(efs.files[kControl] == std::optional<Bytes>(Bytes{7}) && !txn.status().enabled,
                  "restore only autoselection and disable");
            txn.enable(true);
            txn.apply({"45005"}, token);
            txn.claimRestart(token);
            txn.complete(token);
            auto selectedPath = skt.begin()->first;
            efs.files[selectedPath] = {};
            txn.apply({"45005"}, token);
            check(efs.files[selectedPath] == skt.at(selectedPath),
                  "managed drift repaired after modem wipe");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto& safe, const auto&) {
            txn.apply({"45006"}, token);
            efs.files[store.load("Korea/LGU/Commercial").begin()->first] = Bytes{0xee};
            auto beforeOff = efs.files;
            int writes = efs.writes;
            txn.enable(false);
            safe = false;
            throws([&] { txn.restore(token); }, "unsafe switch-off deferred");
            check(efs.writes == writes, "unsafe switch-off zero writes");
            safe = true;
            txn.restore(token);
            beforeOff[kControl] = Bytes{7};
            check(efs.files == beforeOff && efs.writes == writes + 1,
                  "switch-off supersedes drift with only 07");
            check(txn.claimRestart(token), "switch-off restart claim while disabled");
            txn.complete(token);
            check(txn.status().state == "disabled", "switch-off completion while disabled");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            txn.restore(token);
            txn.enable(true);
            txn.evaluate({"45006"});
            check(txn.status().state == "checking" && txn.status().enabled,
                  "reenable supersedes pending 07 restore");
            txn.apply({"45006"}, token);
            check(efs.files[kControl] == std::optional<Bytes>(Bytes{0}),
                  "reenable immediately reapplies 00");
            check(txn.claimRestart(token), "only selected profile claims restart after reenable");
            txn.complete(token);
        });
        run([&](auto& txn, auto& efs, auto& generation, auto& policy, auto& safe, const auto&) {
            generation = "g2";
            txn.apply({"45006"}, token);
            txn.claimRestart(token);
            txn.complete(token);
            check(efs.writes > 0, "modem version never gates writes");
            int written = efs.writes;
            generation = "g1";
            policy = false;
            throws([&] { txn.apply({"45006"}, token); }, "read-only policy");
            policy = true;
            safe = false;
            throws([&] { txn.apply({"45006"}, token); }, "unknown emergency/call context");
            check(efs.writes == written, "unsafe zero writes");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.files[kControl] = Bytes{0};
            txn.apply({"45006"}, token);
            check(efs.writes > 0, "baseline journal does not need firmware approval");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.parentMissing = true;
            throws([&] { txn.apply({"45006"}, token); }, "missing parent");
            check(efs.writes == 0, "no directory creation");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.failAt = 3;
            throws([&] { txn.apply({"45006"}, token); }, "injected apply failure");
            check(efs.files == baseline && txn.status().state == "error" && !txn.status().enabled,
                  "rollback verified and disabled");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.corruptOnce = true;
            throws([&] { txn.apply({"45006"}, token); }, "readback failure");
            check(efs.files == baseline && !txn.status().enabled, "readback failure rollback");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto& safe, const auto&) {
            efs.afterWrite = [&](int writes) {
                if (writes == 3) safe = false;
            };
            throws([&] { txn.apply({"45006"}, token); }, "incoming call revokes in-flight lease");
            check(efs.files == baseline && !txn.status().enabled,
                  "in-flight call verified rollback");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto& safe, const auto&) {
            txn.apply({"45006"}, token);
            safe = false;
            throws([&] { txn.claimRestart(token); }, "call defers restart");
            check(!txn.status().restartAttempted, "unsafe restart not consumed");
            safe = true;
            txn.claimRestart(token);
            efs.files[store.load("Korea/LGU/Commercial").begin()->first] = Bytes{0xee};
            throws([&] { txn.complete(token); }, "post-restart drift");
            check(efs.files == baseline && !txn.status().enabled, "post-restart readback rollback");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.failAt = 2;
            efs.failForever = true;
            throws([&] { txn.apply({"45006"}, token); }, "rollback transport failure");
            check(txn.status().state == "rollback_failed" && !txn.status().enabled,
                  "rollback failure latch");
            throws([&] { txn.enable(true); }, "latch refuses enable");
        });
        // Restart-phase crashes preserve the WAL; all incomplete mutation phases
        // restore the immediate pre-state, including control and absence.
        for (const auto& phase : {"staged", "applying", "readback_verified", "rechecking",
                                  "rolling_back", "restart_pending"}) {
            const auto dir = setup();
            FakeEfs efs;
            efs.files = baseline;
            efs.journal = dir + "/journal.json";
            {
                Transaction txn(
                        store, efs, dir, [] { return "g1"; }, [] { return true; },
                        [] { return true; });
                txn.enable(true);
                txn.apply({"45006"}, token);
            }
            auto wal = parseJson(readFile(efs.journal, 40 * 1024 * 1024));
            wal["state"] = phase;
            checksum(wal);
            durableWrite(efs.journal, wal);
            const auto partial = efs.files;
            {
                Transaction recovered(
                        store, efs, dir, [] { return "g1"; }, [] { return true; },
                        [] { return true; });
                if (std::string(phase) != "restart_pending") {
                    recovered.enable(false);
                    int writes = efs.writes;
                    recovered.recover();
                    check(efs.writes > writes && efs.files == baseline,
                          "interrupted disabled transaction recovers");
                    recovered.enable(true);
                }
                recovered.recover();
                if (std::string(phase) == "restart_pending") {
                    check(efs.files == partial && recovered.status().state == phase,
                          "restart pending retained");
                    recovered.resumeContext({"450006"});
                    check(recovered.claimRestart(token), "fresh process restart claim");
                    recovered.enable(false);
                    throws([&] { recovered.complete(token); }, "disabled completion refusal");
                    recovered.enable(true);
                    recovered.resumeContext({"45006"});
                    recovered.complete(token);
                    check(recovered.status().state == "committed",
                          "fresh process restart completion");
                } else
                    check(efs.files == baseline, "startup WAL recovery");
            }
        }
        const auto dir = setup();
        FakeEfs efs;
        efs.files = baseline;
        efs.journal = dir + "/journal.json";
        {
            Transaction txn(
                    store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
            txn.enable(true);
            txn.apply({"45006"}, token);
        }
        auto unfinished = parseJson(readFile(efs.journal, 40 * 1024 * 1024));
        unfinished["state"] = "applying";
        checksum(unfinished);
        durableWrite(efs.journal, unfinished);
        {
            Transaction unsafe(
                    store, efs, dir, [] { return "g1"; }, [] { return true; },
                    [] { return false; });
            int writes = efs.writes;
            throws([&] { unsafe.recover(); }, "unsafe startup recovery deferred");
            unsafe.probe();
            check(writes == efs.writes, "unsafe startup probe read-only");
        }
        {
            Transaction quarantined(
                    store, efs, dir, [] { return "g2"; }, [] { return true; }, [] { return true; });
            quarantined.recover();
            check(efs.files == baseline, "WAL recovery independent of modem version");
        }
        durableWrite(efs.journal, unfinished);
        {
            Transaction readonly(
                    store, efs, dir, [] { return "g1"; }, [] { return false; },
                    [] { return true; });
            int writes = efs.writes;
            throws([&] { readonly.recover(); }, "read-only recovery deferred");
            readonly.probe();
            check(writes == efs.writes, "probe no writes with WAL");
        }
        auto corrupt = parseJson(readFile(efs.journal, 40 * 1024 * 1024));
        corrupt["baseline"][kControl] = "09";
        durableWrite(efs.journal, corrupt);
        throws(
                [&] {
                    Transaction invalid(
                            store, efs, dir, [] { return "g1"; }, [] { return true; },
                            [] { return true; });
                },
                "journal checksum corruption");
        // Reject a corrupted compressed stream before any modem mutation.
        auto archive = fixture + "/profiles.pack";
        auto blob = readFile(archive, 8 * 1024 * 1024);
        blob[8] ^= 1;
        {
            FILE* f = fopen(archive.c_str(), "wb");
            require(f, "fixture open");
            fwrite(blob.data(), 1, blob.size(), f);
            fclose(f);
        }
        throws([&] { ProfileStore invalid(fixture); }, "pack corruption");
        wireTests();
        std::cout << "PASS: " << checks
                  << " checks; all 430 archives, selection, EFS codec, "
                     "WAL/readback/rollback/recovery/restart\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
