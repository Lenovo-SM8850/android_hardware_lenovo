// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
namespace {
struct FakeFile {
    char path[256]{};
    unsigned char data[65536]{};
    int length = 0;
};
struct FakeState {
    std::atomic<int> fault{0}, metrics[10]{};
    FakeFile files[86];
};
FakeState& fakeState() {
    static auto* state = [] {
        int fd = open(std::getenv("MCFG_FAKE_STATE"), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0 || ftruncate(fd, sizeof(FakeState)) != 0) std::abort();
        void* mapped = mmap(nullptr, sizeof(FakeState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) std::abort();
        return static_cast<FakeState*>(mapped);
    }();
    return *state;
}
void setFakeFile(const char* path, const unsigned char* bytes, int length) {
    if (std::strlen(path) >= 256 || length < 0 || length > 65536) std::abort();
    for (auto& file : fakeState().files) {
        if (file.path[0] && std::strcmp(file.path, path)) continue;
        std::strcpy(file.path, path);
        file.length = length;
        if (length) std::memcpy(file.data, bytes, length);
        return;
    }
    std::abort();
}
}
#ifdef MCFG_FAKE_DIAG
// A host-only libdiag ABI double. Production DiagTransport still dlopens,
// initializes, registers callbacks, waits, deinitializes and reopens normally.
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace {
using Data = std::vector<unsigned char>;
int (*receiver)(unsigned char*, int, void*) = nullptr;
void* receiverContext;
std::map<std::string, Data> files;
std::set<std::string> directories;
std::map<int, std::string> handles;
auto& fault = fakeState().fault;
auto& metrics = fakeState().metrics;
int nextFd = 9, loggingMode = 1, routerMode = 1;
uint32_t field(const unsigned char* bytes, int offset) {
    return bytes[offset] | (uint32_t(bytes[offset + 1]) << 8) |
            (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}
void put(Data& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back((value >> (8 * i)) & 255);
}
}
extern "C" {
void mcfg_test_fault(int value) { fault = value; }
int mcfg_test_metric(int index) { return metrics[index]; }
void mcfg_test_file(const char* path, const unsigned char* bytes, int length) {
    setFakeFile(path, bytes, length);
    files[path] = Data(bytes, bytes + length);
    std::string parent(path);
    while ((parent = parent.substr(0, parent.find_last_of('/'))).size() > 0)
        directories.insert(parent);
}
unsigned char Diag_LSM_Init(uint8_t*) {
    ++metrics[0];
    if (fault == 3) return 0;
    routerMode = 1;  // A newly connected client is back in the router default mode.
    files.clear(); directories.clear();
    for (const auto& file : fakeState().files) {
        if (!file.path[0]) continue;
        files[file.path] = Data(file.data, file.data + file.length);
        std::string parent(file.path);
        while ((parent = parent.substr(0, parent.find_last_of('/'))).size() > 0)
            directories.insert(parent);
    }
    handles.clear();
    nextFd = 9;
    return 1;
}
unsigned char Diag_LSM_DeInit() {
    ++metrics[1];
    receiver = nullptr;
    handles.clear();
    return 1;
}
void diag_register_callback(int (*callback)(unsigned char*, int, void*), void* context) {
    receiver = callback;
    receiverContext = context;
}
void diag_switch_logging(int mode, char*) {
    if (mode == loggingMode) {
        ++metrics[8];  // "no actual logging switch required", even after DeInit.
        return;
    }
    loggingMode = routerMode = mode;
    ++metrics[7];  // "wrote switch log pkt ret:32" / "logging switched".
}
int diag_callback_send_data(int, unsigned char* request, int length) {
    ++metrics[2];
    if (length < 4 || !receiver) return -1;
    if (routerMode != 6) return 0;  // Accepted, but no callback reaches this client.
    const int command = request[2] | (request[3] << 8);
    if (command == 0) ++metrics[5];
    if (fault == 4) return -1;
    if ((fault == 1 && command == 5) || (fault == 2 && command == 0)
            || (fault == 5 && command == 4)) {
        fault = 0;
        return 0;  // Accepted with no callback: exercise the real timeout.
    }
    Data response{0x4b, 0x13, uint8_t(command), uint8_t(command >> 8)};
    if (command == 0) {
        for (int i = 0; i < 6; ++i) put(response, 0x100000);
        for (int i = 0; i < 3; ++i) put(response, 1);
        put(response, 0xffffffff);
    } else if (command == 16 || command == 15) {
        const std::string path(reinterpret_cast<char*>(request + 4));
        put(response, files.count(path) || directories.count(path) ? 0 : 2);
        put(response, directories.count(path) ? 0040000 : 0100600);
        if (command == 15) { put(response, files[path].size()); put(response, 1); }
        for (int i = 0; i < 3; ++i) put(response, 0);
    } else if (command == 2) {
        ++metrics[6];
        const std::string path(reinterpret_cast<char*>(request + 12));
        if (field(request, 4) != 0) {
            files[path].clear();
            setFakeFile(path.c_str(), nullptr, 0);
        }
        handles[nextFd] = path;
        put(response, nextFd++);
        put(response, 0);
    } else if (command == 3) {
        ++metrics[4];
        const auto removed = handles.erase(field(request, 4));
        put(response, removed ? 0 : 9);
    } else if (command == 4 || command == 5) {
        const auto fd = field(request, 4);
        const auto offset = field(request, command == 4 ? 12 : 8);
        auto& data = files[handles.at(fd)];
        uint32_t count;
        if (command == 4) {
            count = std::min<uint32_t>(field(request, 8), data.size() - offset);
        } else {
            ++metrics[3];
            count = length - 12;
            data.resize(offset + count);
            std::copy(request + 12, request + length, data.begin() + offset);
            setFakeFile(handles.at(fd).c_str(), data.data(), data.size());
        }
        put(response, fd); put(response, offset); put(response, count); put(response, 0);
        if (command == 4) response.insert(response.end(), data.begin() + offset,
                                          data.begin() + offset + count);
    } else return -1;
    ++metrics[9];
    receiver(response.data(), response.size(), receiverContext);
    return 0;
}
}
#else
#include <filesystem>
#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>
#include <zlib.h>
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <thread>
#include "MbnParser.h"
#include "Transaction.h"
#include "Recovery.h"
#include "EfsDiagnostics.h"

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
void throwsContaining(const std::function<void()>& action,
                      std::initializer_list<std::string> fragments) {
    try {
        action();
    } catch (const std::exception& error) {
        for (const auto& fragment : fragments)
            check(std::string(error.what()).find(fragment) != std::string::npos,
                  "exception retains EFS diagnostic context");
        return;
    }
    check(false, "expected diagnostic exception");
}
class FakeEfs final : public EfsTransport {
  public:
    Snapshot files;
    int writes = 0, removes = 0, reads = 0, failAt = -1;
    bool failForever = false, corruptOnce = false, parentMissing = false;
    std::string journal;
    std::string failReadPath;
    std::set<std::string> forbiddenPaths;
    std::vector<std::pair<std::string, Bytes>> written;
    std::function<void(int)> afterWrite;
    std::optional<Bytes> read(const std::string& path) override {
        ++reads;
        require(!forbiddenPaths.count(path), "forbidden EFS read forbidden");
        if (path == failReadPath) throw std::runtime_error("injected READ errno=13");
        if (corruptOnce && writes > 0 && path != kControl) {
            corruptOnce = false;
            return Bytes{0xee};
        }
        return files.count(path) ? files.at(path) : std::optional<Bytes>{};
    }
    void write(const std::string& path, const Bytes& bytes,
               const std::function<void()>& checkpoint) override {
        require(!forbiddenPaths.count(path), "forbidden EFS write forbidden");
        if (checkpoint) checkpoint();
        // Exercise the actual serialized WAL, not an in-memory state flag.
        const auto wal = parseJson(readFile(journal, 40 * 1024 * 1024));
        check(wal["state"] == "applying" && !wal["token"].asString().empty() &&
                      !wal["sha256"].asString().empty(), "state WAL before every modem write");
        for (const auto& name : {"baseline", "before", "wanted", "managed"})
            check(!wal.isMember(name), "state WAL never contains EFS snapshots");
        files[path] = bytes;
        ++writes;
        written.emplace_back(path, bytes);
        if (afterWrite) afterWrite(writes);
        if (writes == failAt || (failForever && writes >= failAt))
            throw std::runtime_error("injected partial write");
    }
    void remove(const std::string& path, const std::function<void()>& checkpoint) override {
        require(!forbiddenPaths.count(path), "forbidden EFS unlink forbidden");
        if (checkpoint) checkpoint();
        ++removes;
        files[path] = {};
    }
    void checkParent(const std::string& path) override {
        require(!forbiddenPaths.count(path), "forbidden EFS parent check forbidden");
        require(!parentMissing, "injected missing parent");
    }
};
class ForwardEfs final : public EfsTransport {
  public:
    ForwardEfs(FakeEfs& efs, bool readOnly) : efs_(efs), readOnly_(readOnly) {}
    std::optional<Bytes> read(const std::string& path) override { return efs_.read(path); }
    void checkParent(const std::string& path) override { efs_.checkParent(path); }
    void write(const std::string& path, const Bytes& bytes,
               const std::function<void()>& checkpoint) override {
        require(!readOnly_, "read-only recovery transport write");
        efs_.write(path, bytes, checkpoint);
    }
    void remove(const std::string& path, const std::function<void()>& checkpoint) override {
        require(!readOnly_, "read-only recovery transport unlink");
        efs_.remove(path, checkpoint);
    }

  private:
    FakeEfs& efs_;
    bool readOnly_;
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
Bytes lstat(bool directory, uint32_t error = 0) {
    return packet(16, {error, directory ? 0040000u : 0100000u, 0, 0, 0});
}
Bytes stat(uint32_t size = 1) {
    return packet(15, {0, 0100000u, size, 1, 0, 0, 0});
}
void wireTests() {
    const std::string path = "/data/a";
    ScriptedWire wire;
    wire.replies = {hello(), lstat(false), stat(), packet(2, {9, 0}), packet(4, {9, 0, 1, 0})};
    wire.replies.back().push_back(7);
    wire.replies.push_back(packet(3, {0}));
    EfsClient efs(wire, {path}, true);
    check(efs.read(path) == std::optional<Bytes>(Bytes{7}), "EFS read codec");
    check(wire.requests[1][2] == 16 && wire.requests[2][2] == 15, "LSTAT type, STAT size");
    check(wire.requests[3][4] == 0, "probe OPEN read-only");
    auto requests = wire.requests.size();
    throws([&] { efs.write(path, Bytes{1}); }, "probe refuses write");
    check(requests == wire.requests.size(), "probe emits no write packets");
    throws([&] { efs.read("/secret"); }, "unapproved read path");
    ScriptedWire item;
    item.replies = {hello(), packet(16, {0, 0160644u, 0, 0, 0}),
                    packet(15, {0, 0160644u, 1, 1, 0, 0, 0}), packet(2, {9, 0}),
                    packet(4, {9, 0, 1, 0})};
    item.replies.back().push_back(5);
    item.replies.push_back(packet(3, {0}));
    EfsClient itemReader(item, {path}, true);
    check(itemReader.read(path) == std::optional<Bytes>(Bytes{5}), "EFS item file read");
    ScriptedWire itemWrite;
    itemWrite.replies = {hello(), lstat(true), packet(16, {0, 0160644u, 0, 0, 0}),
                         packet(2, {9, 0}), packet(5, {9, 0, 1, 0}), packet(3, {0})};
    EfsClient itemWriter(itemWrite, {path}, false);
    itemWriter.write(path, Bytes{6});
    check(itemWrite.requests[3][8] == 0xa4 && itemWrite.requests[3][9] == 0xe1,
          "EFS item file mode kept on OPEN");
    ScriptedWire linked;
    linked.replies = {hello(), packet(16, {0, 0120777u, 0, 0, 0}),
                      packet(15, {0, 0160644u, 1, 1, 0, 0, 0}), packet(2, {9, 0}),
                      packet(4, {9, 0, 1, 0})};
    linked.replies.back().push_back(4);
    linked.replies.push_back(packet(3, {0}));
    EfsClient linkReader(linked, {path}, true);
    check(linkReader.read(path) == std::optional<Bytes>(Bytes{4}), "EFS symlink followed to item");
    ScriptedWire dangling;
    dangling.replies = {hello(), packet(16, {0, 0120777u, 0, 0, 0}),
                        packet(15, {2, 0, 0, 0, 0, 0, 0})};
    EfsClient danglingReader(dangling, {path}, true);
    throws([&] { danglingReader.read(path); }, "dangling EFS symlink rejected");
    ScriptedWire linkWrite;
    linkWrite.replies = {hello(), lstat(true), packet(16, {0, 0120777u, 0, 0, 0}),
                         packet(15, {0, 0160644u, 1, 1, 0, 0, 0}), packet(2, {9, 0}),
                         packet(5, {9, 0, 1, 0}), packet(3, {0})};
    EfsClient linkWriter(linkWrite, {path}, false);
    linkWriter.write(path, Bytes{3});
    check(linkWrite.requests[4][8] == 0xa4 && linkWrite.requests[4][9] == 0xe1,
          "EFS symlink write keeps the target mode");
    ScriptedWire missing;
    missing.replies = {hello(), lstat(false, 2)};
    EfsClient absent(missing, {path}, true);
    check(!absent.read(path), "ENOENT recorded absent");
    ScriptedWire error;
    error.replies = {hello(), lstat(false, 13)};
    EfsClient denied(error, {path}, true);
    throws([&] { denied.read(path); }, "EACCES never treated as absent");
    ScriptedWire diagnostic;
    diagnostic.replies = {hello(), lstat(false, 13)};
    EfsClient diagnosticReader(diagnostic, {path}, true);
    throwsContaining([&] { diagnosticReader.read(path); },
                     {"path=/data/a", "step=read", "LSTAT", "errno=13"});
    ScriptedWire shortWrite;
    shortWrite.replies = {
            hello(),       lstat(true), lstat(false), packet(2, {9, 0}), packet(5, {9, 0, 0, 0}),
            packet(3, {0})};
    EfsClient writable(shortWrite, {path}, false);
    throws([&] { writable.write(path, Bytes{8}); }, "short write rejected");
    check(shortWrite.requests.back()[2] == 3, "close on failed write");
    ScriptedWire interrupted;
    interrupted.replies = {
            hello(),       lstat(true), lstat(false), packet(2, {9, 0}), packet(5, {9, 0, 1024, 0}),
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
        invalid.replies = {hello(), lstat(false), stat(), packet(2, {9, 0}), bad, packet(3, {0})};
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
void transportRecoveryTests(ProfileStore& store, const std::string& scratch) {
    const auto fault = [](int value) { fakeState().fault = value; };
    const auto metric = [](int index) { return fakeState().metrics[index].load(); };
    const auto file = setFakeFile;
    for (const auto& path : store.paths()) {
        const Bytes data{0x17};
        file(path.c_str(), data.data(), data.size());
    }
    const Bytes automatic{7};
    file(kControl, automatic.data(), automatic.size());
    const std::string dir = scratch + "/transport-restart";
    fs::create_directories(dir);
    std::atomic<bool> modemUp{true};
    std::atomic<bool> contextFresh{true};
    bool dropOnOpen = false;
    int seenOpen = 0;
    DiagTransport diag([&] { return modemUp.load() && (!dropOnOpen || metric(6) == seenOpen); },
                       [&] { return contextFresh.load(); });
    EfsClient efs(diag, store.paths(), false);
    int64_t clock = 1;
    Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; },
                    [] { return true; }, {}, [&] { return clock; });
    txn.enable(true);
    const std::string token(32, 'a');
    txn.apply({"45006"}, token);
    txn.claimRestart(token);
    diag.invalidate();
    txn.complete(token);
    txn.apply({"45008"}, token);
    check(txn.status().profile.empty() && txn.status().nextProfile == "Korea/KT/Commercial_KT_LTE",
          "LGU to KT first writes 07 before restart");
    txn.claimRestart(token);
    diag.invalidate();
    modemUp = false;
    const int sends = metric(2), writes = metric(3);
    for (int i = 0; i < 360; ++i)
        check(txn.autoselect() == -1, "restart status unknown without EFS");
    check(metric(2) == sends, "status polls emit no EFS while restarting");
    // Completion already has the coordinator's radio witness, but the actual
    // modem/diag endpoint can still lag it. Immediate P must wait, not write.
    txn.complete(token);
    bool quietWhileDown = false;
    std::thread recover([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        quietWhileDown = metric(2) == sends && metric(3) == writes;
        modemUp = true;
    });
    fault(2);  // First readiness HELLO still times out after modem-up.
    const auto started = std::chrono::steady_clock::now();
    try { txn.apply({"45008"}, token); }
    catch (...) { recover.join(); throw; }
    recover.join();
    check(quietWhileDown, "no EFS packets or writes while modem is down");
    check(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(250),
          "immediate KT transaction waited for modem-up");
    check(metric(3) > writes && txn.status().state == "restart_pending",
          "KT applies after modem-up and successful EFS probe");
    int expectedWrites = 1; // Control 00, followed by each profile chunk once.
    for (const auto& [path, bytes] : store.load("Korea/KT/Commercial_KT_LTE")) {
        (void)path;
        expectedWrites += (bytes->size() + 1023) / 1024;
    }
    check(metric(3) == writes + expectedWrites, "readiness retries never duplicate KT writes");
    check(metric(7) == metric(0) && metric(8) == 0 && metric(9) > 0,
          "every exec session makes a real logging switch and reaches the callback after restart");
    for (const auto& [path, bytes] : store.load("Korea/KT/Commercial_KT_LTE"))
        check(efs.read(path) == bytes, "KT exact bytes after readiness gate");
    txn.claimRestart(token);
    diag.invalidate();
    txn.complete(token);
    check(txn.status().state == "committed" && !txn.status().sourceName.empty(),
          "KT committed with source after its restart");
    check(efs.read(kControl) == std::optional<Bytes>(Bytes{0}), "warm current EFS session");
    txn.enable(true); // Explicit fresh apply exercises the transport fault, not the no-op guard.
    fault(1);  // A real EFS WRITE timeout ends the transaction without replay.
    const int closes = metric(4), opens = metric(0);
    throws([&] { txn.apply({"45008"}, token); }, "in-flight transport loss reports error");
    check(txn.status().state == "error" && txn.status().token.empty(),
          "transport timeout journaled as retryable error");
    check(metric(0) == opens && metric(4) == closes && !diag.connected(),
          "failed WRITE cleanup does not reopen or close an old modem fd");
    txn.apply({"45008"}, token);
    check(metric(0) > opens && txn.status().state == "restart_pending",
          "next operation reopens and successfully reapplies");
    txn.claimRestart(token);
    diag.invalidate();
    txn.complete(token);
    fault(5);
    throws([&] { txn.autoselect(true); }, "status timeout");
    const int pollSends = metric(2);
    for (int i = 0; i < 360; ++i)
        check(txn.autoselect() == -1, "down status stays unknown during failure cache");
    check(metric(2) == pollSends, "failed status polls do not storm EFS");
    clock += 30'000;
    check(txn.autoselect() == 0, "status resumes after failure-cache backoff");
    const int cachedSends = metric(2);
    clock += 59'999;
    check(txn.autoselect() == 0 && metric(2) == cachedSends, "successful cache maximum age");
    ++clock;
    check(txn.autoselect() == 0 && metric(2) > cachedSends, "expired status refresh");
    check(txn.autoselect(true) == 0, "explicit autoselect refresh");
    diag.invalidate();
    fault(3);
    const int failedOpens = metric(0);
    throws([&] { efs.read(kControl); }, "bounded reconnect attempts");
    check(metric(0) == failedOpens + 3, "reconnect retries are bounded");
    fault(0);
    check(efs.read(kControl) == std::optional<Bytes>(Bytes{0}), "init failure recovers next operation");
    seenOpen = metric(6);
    dropOnOpen = true;
    const int beforeDrop = metric(2), closedBeforeDrop = metric(4);
    throws([&] { efs.read(kControl); }, "modem-down between OPEN and READ aborts old session");
    check(metric(2) == beforeDrop + 3 && metric(4) == closedBeforeDrop && !diag.connected(),
          "old-session fd is never READ or CLOSEd after modem-down");
    dropOnOpen = false;
    check(efs.read(kControl) == std::optional<Bytes>(Bytes{0}), "fresh operation recovers after radio loss");
    // Model one Binder thread: updateContext cannot run until apply returns.
    // The old combined readiness callback waited 30 seconds for this renewal.
    // A stale lease must fail before any EFS request, not wait for queued IPC.
    diag.invalidate(); // Restart/process recovery needs a newly opened session.
    contextFresh = false;
    const int staleSends = metric(2), staleWrites = metric(3);
    const auto staleEntry = std::chrono::steady_clock::now();
    throws([&] { txn.apply({"45006"}, token); }, "stale lease rejects carrier restore");
    check(std::chrono::steady_clock::now() - staleEntry < std::chrono::seconds(1),
          "single Binder thread never waits for context renewal IPC");
    check(metric(2) == staleSends && metric(3) == staleWrites && !diag.connected(),
          "stale lease sends no EFS and discards the old session");
    check(txn.status().state == "error" && txn.status().profile == "Korea/KT/Commercial_KT_LTE"
                  && txn.status().nextProfile == "Korea/LGU/Commercial",
          "stale carrier restore remains journaled and retryable");
    contextFresh = true; // Only now can the queued renewal run on that thread.
    txn.apply({"45006"}, token);
    check(txn.status().profile.empty() && txn.status().state == "restart_pending"
                  && efs.read(kControl) == std::optional<Bytes>(Bytes{7}),
          "renewed lease permits verified 07 carrier restore");
    txn.claimRestart(token);
    diag.invalidate();
    txn.complete(token);
    txn.apply({"45006"}, token);
    txn.claimRestart(token);
    diag.invalidate();
    txn.complete(token);
    check(txn.status().state == "committed" && txn.status().profile == "Korea/LGU/Commercial",
          "single-threaded queued renewal recovers KT to LGU");
    txn.enable(false);
    txn.restore(token);
    check(!txn.status().enabled && txn.status().profile.empty()
                  && efs.read(kControl) == std::optional<Bytes>(Bytes{7}),
          "OFF verifies 07 and clears the stale applied profile");
    txn.claimRestart(token);
    diag.invalidate();
    txn.complete(token);
    check(txn.status().state == "disabled" && txn.status().profile.empty(),
          "OFF completes in Default after the control restart");
    check(dlopen("libdiag.so", RTLD_NOW | RTLD_NOLOAD) == nullptr,
          "long-lived parent never loads libdiag");
    std::cout << "PASS: single Binder thread stale lease fails promptly; queued renewal restores 07 and applies LGU; OFF verifies 07\n";
    // run_host_tests.py checks that only the first identical diagnostic is emitted.
    for (int i = 0; i < 360; ++i) logEfsError("host repeated transport error");
    std::cout << "PASS: exec reconnect resets libdiag cache; real logging switch and HELLO callback on each session\n";
    std::cout << "PASS: modem-down wait, EFS readiness timeout/reopen, KT apply, status cache and bounded retries\n";
}
void checksum(Json::Value& value) {
    value.removeMember("sha256");
    value["sha256"] = sha256(jsonBytes(value));
}
Bytes mbnFixture(const Snapshot& entries) {
    Bytes body{'M', 'C', 'F', 'G', 0, 0, 1, 0};
    auto number = [&](uint32_t value, int count) {
        for (int n = 0; n < count; ++n) body.push_back((value >> (8 * n)) & 255);
    };
    number(entries.size() + 1, 4);
    number(0, 4);
    number(4995, 2);
    number(0, 2);
    for (const auto& item : entries) {
        const auto& path = item.first;
        Bytes data{7};
        data.insert(data.end(), item.second->begin(), item.second->end());
        while ((16 + path.size() + 1 + data.size()) % 4) data.push_back(0);
        number(16 + path.size() + 1 + data.size(), 4);
        number(2, 1);
        number(0, 3);
        number(1, 2);
        number(path.size() + 1, 2);
        body.insert(body.end(), path.begin(), path.end());
        body.push_back(0);
        number(2, 2);
        number(data.size(), 2);
        body.insert(body.end(), data.begin(), data.end());
    }
    number(12, 4);
    number(10, 2);
    number(0, 2);
    number(0xa1, 2);
    number(0, 2);
    number(16, 4);
    Bytes mbn(148, 0);
    auto field = [&](size_t offset, uint32_t value, int count) {
        for (int n = 0; n < count; ++n) mbn[offset + n] = (value >> (8 * n)) & 255;
    };
    mbn[0] = 0x7f;
    mbn[1] = 'E'; mbn[2] = 'L'; mbn[3] = 'F';
    mbn[4] = 1; mbn[5] = 1; mbn[6] = 1;
    field(28, 52, 4);
    field(42, 32, 2);
    field(44, 3, 2);
    field(116, 1, 4);
    field(120, 148, 4);
    field(132, body.size(), 4);
    field(144, 4, 4);
    mbn.insert(mbn.end(), body.begin(), body.end());
    return mbn;
}
void managedPathTests(ProfileStore& store, const std::string& root, const std::string& scratch) {
    const auto lgu = store.load("Korea/LGU/Commercial");
    check(store.paths().size() == 85 && lgu.size() == 52, "all profile paths are managed");
    const std::set<std::string> formerlyExcluded{
        "/Data_Profiles/Profile1", "/Data_Profiles/Profile2", "/Data_Profiles/Profile3",
        "/Data_Profiles/Profile4", "/Data_Profiles/Profile9",
        "/nv/item_files/ims/qp_ims_xcap_private_config_item",
        "/nv/item_files/gps/cgps/sm/gnss_lpp_enable",
        "/nv/item_files/ims/qp_ims_plani_config",
        "/nv/item_files/ims/qp_ims_service_enablement_config",
        "/nv/item_files/modem/mmode/mode_pref"};
    for (const auto& path : formerlyExcluded)
        check(isManagedPath(path) && store.paths().count(path), "formerly excluded paths are managed");
    check(isManagedPath("/Data_Profiles/future/nested"), "there is no volatile-path exclusion");
    Snapshot source = lgu;
    source["/Data_Profiles/Profile9"] = Bytes{0xaa};
    check(filterManagedSnapshot(source) == source &&
                  decodeSnapshot(encodeSnapshot(source), store.paths()) == source,
          "profile and snapshot codecs keep all items");
    throws([&] { decodeSnapshot(encodeSnapshot(Snapshot{{"/unapproved", Bytes{1}}}), store.paths()); },
           "immutable allowlist still rejects unknown paths");
    const auto importsDir = scratch + "/all-items-imports";
    fs::create_directories(importsDir);
    ProfileStore imported(root, importsDir);
    const auto mbn = mbnFixture(source);
    const auto parsed = parseMbn(mbn, store.paths());
    check(parsed.size() == source.size(), "MBN parser retains every allowlisted item");
    for (const auto& item : source) check(parsed.count(item.first), "MBN item path is preserved");
    const auto id = imported.importMbn(mbn, "all-items.mbn", "45006");
    check(imported.load(id) == parsed, "new MBN imports preserve all profile items");
    ProfileStore reloaded(root, importsDir);
    check(reloaded.load(id) == parsed, "all imported items persist");
}
void importedChoiceTests(const std::string& root, const std::string& scratch) {
    const auto directory = scratch + "/import-choices";
    fs::create_directories(directory);
    ProfileStore store(root, directory);
    const std::string bundled = "Korea/LGU/Commercial";
    auto source = store.load(bundled);
    const auto mbn1 = mbnFixture(source);
    source.begin()->second = Bytes{0x12, 0x34};
    const auto mbn2 = mbnFixture(source);
    const auto first = store.importMbn(mbn1, "first.mbn", "45006");
    const auto second = store.importMbn(mbn2, "second.mbn", "45006");
    check(first == "imported/" + sha256(mbn1) && second == "imported/" + sha256(mbn2)
                  && first != second && store.profiles().size() == 432,
          "different source MBNs add imports for the same carrier");
    check(store.importMbn(mbn1, "renamed.mbn", "45006") == first && store.profiles().size() == 432,
          "reimporting a source is idempotent");
    check(store.select({"45006"}).profile == bundled && store.chosenImported("45006").empty(),
          "multiple matching imports neither auto-select nor conflict");
    throws([&] { store.chooseImported("45008", first); }, "choice must match SIM carrier");
    store.chooseImported("45006", first);
    check(store.select({"45006"}).profile == first && store.select({"450006"}).profile == first,
          "explicit choice covers the bundled carrier aliases");
    check(store.select({"45006", "45005"}).state == "conflict", "mixed carriers still conflict");
    {
        ProfileStore loaded(root, directory);
        check(loaded.chosenImported("450006") == first && loaded.load(second) == parseMbn(mbn2, store.paths()),
              "choice and all import bytes survive reload");
    }
    FakeEfs efs;
    efs.files = base(store);
    efs.journal = directory + "/journal.json";
    Transaction txn(store, efs, directory, [] { return "g1"; }, [] { return true; }, [] { return true; });
    txn.enable(true);
    const std::string token(32, 'a');
    txn.apply({"45006"}, token);
    check(txn.status().profile == first && txn.status().state == "restart_pending"
                  && efs.files.at(kControl) == std::optional<Bytes>(Bytes{0}),
          "chosen import uses the normal overlay transaction after reset restart");
    for (const auto& [path, bytes] : store.load(first))
        check(efs.files.at(path) == bytes, "chosen import exact readback bytes");
    txn.claimRestart(token);
    const int reads = efs.reads;
    txn.complete(token);
    check(txn.status().state == "committed" && efs.reads > reads,
          "chosen import completes only after final restart readback");
    const int committedWrites = efs.writes;
    const auto committedJournal = readFile(efs.journal, 40 * 1024 * 1024);
    txn.evaluate({"45006"});
    txn.apply({"45006"}, token);
    check(efs.writes == committedWrites && txn.status().state == "committed" &&
                  readFile(efs.journal, 40 * 1024 * 1024) == committedJournal,
          "same chosen committed import does not reset or restart");
    store.chooseImported("45006", second);
    txn.apply({"45006"}, token);
    check(txn.status().profile.empty() && txn.status().nextProfile == second,
          "choosing another import requires the same reset restart");
    txn.claimRestart(token); txn.complete(token);
    txn.apply({"45006"}, token); txn.claimRestart(token);
    const auto changedPath = store.load(second).begin()->first;
    efs.files[changedPath] = Bytes{0xee};
    throws([&] { txn.complete(token); }, "post-restart import drift is an error");
    check(txn.status().state == "error", "failed import readback stays retryable");
    txn.apply({"45006"}, token);
    check(txn.status().profile.empty(), "readback retry resets the modem first");
    txn.claimRestart(token); txn.complete(token);
    txn.apply({"45006"}, token); txn.claimRestart(token); txn.complete(token);
    check(txn.status().state == "committed" && txn.status().profile == second,
          "chosen import error retries without reboot");
    store.chooseImported("45006", "");
    check(store.select({"45006"}).profile == bundled && store.chosenImported("450006").empty(),
          "unchoose restores bundled carrier aliases");
    store.chooseImported("45006", second);
    store.deleteImported(second);
    check(store.select({"45006"}).profile == bundled && store.profiles().size() == 431
                  && store.load(first) == parseMbn(mbn1, store.paths()),
          "deleting chosen import falls back to bundled while preserving others");
    {
        ProfileStore loaded(root, directory);
        check(loaded.select({"450006"}).profile == bundled, "delete and fallback persist together");
    }
    txn.apply({"45006"}, token);
    check(txn.status().profile.empty() && txn.status().nextProfile == bundled,
          "deleted applied import transitions through reset to bundled");
    txn.claimRestart(token); txn.complete(token);
    txn.apply({"45006"}, token); txn.claimRestart(token); txn.complete(token);
    check(txn.status().profile == bundled, "bundled fallback reuses the existing transaction");

    auto profile = store.metadata(first);
    const std::string oldId = "imported/" + sha256(jsonBytes(profile["home_plmns"]));
    profile["id"] = oldId;
    profile.removeMember("legacy_ids");
    Json::Value legacy;
    legacy["schema_version"] = 1;
    legacy["profiles"] = Json::Value(Json::arrayValue);
    legacy["profiles"].append(profile);
    checksum(legacy);
    const auto legacyDir = scratch + "/legacy-imports";
    fs::create_directories(legacyDir);
    durableWrite(legacyDir + "/imports.json", legacy);
    {
        ProfileStore migrated(root, legacyDir);
        check(migrated.load(oldId) == store.load(first) && migrated.metadata(oldId)["id"] == first,
              "legacy IDs resolve to migrated source identity without losing bytes");
        check(migrated.importMbn(mbn1, "same.mbn", "45006") == first && migrated.profiles().size() == 431,
              "reimport of a migrated source remains idempotent");
        migrated.chooseImported("45006", oldId);
    }
    {
        ProfileStore migrated(root, legacyDir);
        check(migrated.select({"45006"}).profile == first && migrated.load(oldId) == store.load(first),
              "migrated aliases and choice survive a second loader restart");
        const auto saved = parseJson(readFile(legacyDir + "/imports.json", 40 * 1024 * 1024));
        check(saved["schema_version"] == 2 && saved["profiles"][0]["id"] == first,
              "source IDs are rewritten durably in imports.json");
        migrated.deleteImported(oldId);
        check(migrated.select({"45006"}).profile == bundled, "delete accepts a legacy alias");
    }
    auto staleChoice = parseJson(readFile(legacyDir + "/imports.json", 40 * 1024 * 1024));
    staleChoice["choices"]["45006"] = "imported/" + std::string(64, 'a');
    checksum(staleChoice);
    durableWrite(legacyDir + "/imports.json", staleChoice);
    {
        ProfileStore loaded(root, legacyDir);
        check(loaded.select({"45006"}).profile == bundled,
              "a choice whose source no longer exists falls back to bundled");
    }
    const auto capDir = scratch + "/import-cap";
    fs::create_directories(capDir);
    ProfileStore capped(root, capDir);
    Bytes original;
    for (int i = 0; i < 16; ++i) {
        source.begin()->second = Bytes{uint8_t(i)};
        auto bytes = mbnFixture(source);
        if (i == 0) original = bytes;
        capped.importMbn(bytes, "cap.mbn", "45006");
    }
    source.begin()->second = Bytes{16};
    throws([&] { capped.importMbn(mbnFixture(source), "overflow.mbn", "45006"); }, "16 import cap");
    check(capped.profiles().size() == 446 && capped.importMbn(original, "same.mbn", "45006")
                  == "imported/" + sha256(original), "reimport remains allowed at the cap");
    std::cout << "PASS: multiple imports per carrier, durable choice, reset/apply/readback, deletion fallback and legacy migration\n";
}
void baselineResetTests(ProfileStore& store, const std::string& scratch) {
    const std::string token(32, 'd');
    int scenario = 0;
    const auto setup = [&] {
        const auto dir = scratch + "/baseline-reset" + std::to_string(++scenario);
        fs::create_directories(dir);
        return dir;
    };
    const auto saved = [](const FakeEfs& efs) {
        return parseJson(readFile(efs.journal, 40 * 1024 * 1024));
    };
    for (const int schema : {1, 2}) {
        for (const int priorUse : {0, 1, 2, 3, 4}) {
            const auto dir = setup();
            FakeEfs efs;
            efs.files = base(store);
            efs.files[kControl] = Bytes{0};
            efs.journal = dir + "/journal.json";
            const std::string profile = "Korea/LGU/Commercial";
            Json::Value legacy;
            legacy["schema_version"] = schema;
            legacy["enabled"] = priorUse == 1 || priorUse == 2;
            legacy["profile"] = priorUse == 1 || priorUse == 3 ? profile : "";
            legacy["state"] = priorUse == 1 || priorUse == 4 ? "committed"
                                      : priorUse == 2 ? "waiting_for_identity" : "disabled";
            if (priorUse == 1) {
                const auto metadata = store.metadata(profile);
                legacy["source_name"] = metadata["source_name"];
                legacy["source_sha256"] = metadata["source_sha256"];
            }
            checksum(legacy);
            durableWrite(efs.journal, legacy);
            const auto before = readFile(efs.journal, 40 * 1024 * 1024);
            Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
            check(efs.reads == 0 && readFile(efs.journal, 40 * 1024 * 1024) == before,
                  "legacy baseline migration neither reads EFS nor saves during construction");
            if (priorUse == 1) {
                txn.apply({"45006"}, token);
                check(efs.writes == 0 && txn.status().state == "committed",
                      "legacy enabled committed profile remains a no-op after update");
            }
            txn.enable(true);
            check(saved(efs)["baselineReset"].asBool() == (priorUse != 0),
                  "next save persists baseline migration from legacy prior use");
            txn.apply({"45006"}, token);
            if (priorUse == 0) {
                check(efs.writes == 1 && efs.files[kControl] == Bytes{7}
                              && txn.status().state == "baseline_restart_pending"
                              && !saved(efs)["baselineReset"].asBool(),
                      "legacy disabled unused journal resets baseline on first enable");
                txn.claimRestart(token);
                txn.complete(token);
                txn.apply({"45006"}, token);
            } else {
                for (const auto& write : efs.written)
                    check(write != std::make_pair(std::string(kControl), Bytes{7}),
                          "legacy prior use skips baseline 07 write and restart");
            }
            check(txn.status().state == "restart_pending" && txn.status().profile == profile
                          && efs.files[kControl] == Bytes{0} && saved(efs)["baselineReset"].asBool(),
                  "legacy migration continues through the normal overlay apply");
        }
    }
    std::cout << "PASS: legacy prior-use journals skip baseline; disabled unused journals reset on first enable\n";
    for (const auto& control : std::vector<std::optional<Bytes>>{
                 Bytes{0}, Bytes{7}, Bytes{9}, Bytes{7, 0}, std::nullopt}) {
        const auto dir = setup();
        FakeEfs efs;
        efs.files = base(store);
        efs.journal = dir + "/journal.json";
        {
            Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
            check(!txn.status().enabled && txn.status().state == "disabled" && efs.reads == 0,
                  "no journal defaults OFF without EFS access");
            txn.evaluate({"45006"});
            throws([&] { txn.apply({"45006"}, token); }, "fresh install cannot apply while disabled");
            check(efs.writes == 0, "disabled fresh install never writes");
            check(txn.autoselect() == 7, "prime automatic status cache");
            efs.files[kControl] = control;
            txn.enable(true);
            check(efs.writes == 0, "enable waits for a safe supported apply context");
            txn.apply({"45006"}, token);
            if (control != std::optional<Bytes>(Bytes{7})) {
                auto expected = base(store);
                expected[kControl] = Bytes{7};
                check(efs.files == expected && efs.writes == 1 && txn.status().profile.empty()
                              && txn.status().state == "baseline_restart_pending"
                              && !saved(efs)["baselineReset"].asBool(),
                      "fresh control read writes only 07 before baseline restart");
                throws([&] { txn.apply({"45006"}, token); }, "overlay waits for baseline recovery");
                throws([&] { txn.complete(token); }, "baseline cannot complete without restart claim");
                check(txn.claimRestart(token), "baseline requests existing modem restart path");
                const int reads = efs.reads;
                check(txn.autoselect(true) == -1 && efs.reads == reads,
                      "baseline restart suppresses even refreshed EFS polling");
                txn.resumeContext({"45006"});
                txn.complete(token);
                check(saved(efs)["baselineReset"].asBool(), "baseline done only after recovery");
                txn.apply({"45006"}, token);
            } else {
                check(saved(efs)["baselineReset"].asBool(), "07 marks baseline done immediately");
                for (const auto& write : efs.written)
                    check(write != std::make_pair(std::string(kControl), Bytes{7}),
                          "already 07 skips baseline write and restart");
            }
            check(txn.status().state == "restart_pending" && efs.files[kControl] == Bytes{0},
                  "normal overlay ends with 00 and its normal restart");
            for (const auto& [path, bytes] : store.load("Korea/LGU/Commercial"))
                check(efs.files.at(path) == bytes, "baseline continuation verifies every profile item");
            txn.claimRestart(token);
            txn.complete(token);
        }
        {
            Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
            check(txn.status().enabled, "journal retains saved ON choice");
            txn.enable(false);
            const int writes = efs.writes;
            txn.enable(true);
            txn.apply({"45006"}, token);
            check(txn.status().state == "restart_pending" && efs.writes > writes
                          && efs.written[writes] == std::make_pair(std::string(kControl), Bytes{0}),
                  "second enable skips baseline even with control 00 after process restart");
        }
        {
            Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
            txn.enable(false);
        }
        {
            Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
            check(!txn.status().enabled, "journal retains saved OFF choice");
        }
    }
    for (const std::string phase : {"staged", "applying", "readback_verified", "baseline_restart_pending"}) {
        for (const bool claimed : {false, true}) {
            if (claimed && phase != "baseline_restart_pending") continue;
            const auto dir = setup();
            FakeEfs efs;
            efs.files = base(store);
            efs.files[kControl] = Bytes{0};
            efs.journal = dir + "/journal.json";
            {
                Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
                txn.enable(true);
                txn.apply({"45006"}, token);
                if (claimed) txn.claimRestart(token);
            }
            auto wal = saved(efs);
            wal["state"] = phase;
            checksum(wal);
            durableWrite(efs.journal, wal);
            {
                Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
                const int writes = efs.writes;
                txn.recover();
                if (phase == "staged" || phase == "applying") {
                    check(txn.status().state == "error", "partial baseline follows retry recovery");
                    txn.enable(true);
                    txn.apply({"45006"}, token);
                    check(efs.writes == writes + 1, "interrupted 07 still retries reset before overlay");
                } else
                    check(efs.writes == writes, "verified baseline resumes without repeating 07");
                check(txn.status().state == "baseline_restart_pending" && !saved(efs)["baselineReset"].asBool(),
                      "baseline pending and unfinished flag survive crash");
                txn.resumeContext({"45006"});
                check(txn.claimRestart(token) == !claimed, "baseline restart claim is durable at most once");
                txn.complete(token);
                txn.apply({"45006"}, token);
                check(txn.status().profile == "Korea/LGU/Commercial" && efs.files[kControl] == Bytes{0},
                      "crashed baseline resumes profile only after restart completion");
            }
        }
    }
    for (const bool policyFailure : {false, true}) {
        const auto dir = setup();
        FakeEfs efs;
        efs.files = base(store);
        efs.files[kControl] = Bytes{0};
        efs.journal = dir + "/journal.json";
        bool allowed = false;
        Transaction txn(store, efs, dir, [] { return "g1"; },
                        [&] { return !policyFailure || allowed; }, [&] { return policyFailure || allowed; });
        txn.enable(true);
        throws([&] { txn.apply({"45006"}, token); }, "existing policy/safety gates baseline");
        check(efs.writes == 0 && !saved(efs)["baselineReset"].asBool(), "gated baseline writes nothing");
        allowed = true;
        efs.failAt = 1;
        throws([&] { txn.apply({"45006"}, token); }, "baseline write failure remains retryable");
        check(txn.status().state == "error" && !saved(efs)["baselineReset"].asBool(),
              "failed baseline never marks completion");
        efs.failAt = -1;
        txn.apply({"45006"}, token);
        check(txn.status().state == "baseline_restart_pending", "baseline failure retries 07 restart");
        txn.enable(false);
        txn.restore(token);
        check(!txn.status().enabled && txn.status().state == "restart_pending", "OFF supersedes baseline reset");
    }
    {
        const auto dir = setup();
        FakeEfs efs;
        efs.files = base(store);
        efs.files[kControl] = Bytes{0};
        efs.journal = dir + "/journal.json";
        Transaction txn(store, efs, dir, [] { return "g1"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        efs.afterWrite = [&](int) { efs.files[kControl] = Bytes{9}; };
        throws([&] { txn.apply({"45006"}, token); }, "baseline 07 readback mismatch");
        check(txn.status().state == "error" && efs.writes == 1 && !saved(efs)["baselineReset"].asBool(),
              "baseline readback failure leaves completion unset");
        throws([&] { txn.claimRestart(token); }, "unverified baseline cannot claim restart");
        efs.afterWrite = {};
        txn.apply({"45006"}, token);
        check(txn.status().state == "baseline_restart_pending" && efs.files[kControl] == Bytes{7},
              "baseline readback failure retries 07 and verification");
    }
    std::cout << "PASS: default OFF, first-enable baseline reset/07 skip, second enable and baseline crash recovery\n";
}
void controlRestartTests(ProfileStore& store, const std::string& scratch) {
    const std::string token(32, 'c');
    int scenario = 0;
    const auto dir = [&] {
        const auto path = scratch + "/control-restart" + std::to_string(++scenario);
        fs::create_directories(path);
        return path;
    };
    const auto journal = [](const FakeEfs& efs) {
        return parseJson(readFile(efs.journal, 40 * 1024 * 1024));
    };
    for (const auto& control : std::vector<std::optional<Bytes>>{
                 Bytes{0}, Bytes{7}, Bytes{9}, Bytes{7, 0}, std::nullopt}) {
        const auto path = dir();
        FakeEfs efs;
        efs.files = base(store);
        efs.files[kControl] = control;
        efs.journal = path + "/journal.json";
        Transaction txn(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        txn.apply({"45006"}, token);
        if (control != std::optional<Bytes>(Bytes{7})) {
            check(efs.writes == 1 && efs.written.front() == std::make_pair(std::string(kControl), Bytes{7})
                          && txn.status().state == "baseline_restart_pending", "first enable resets baseline");
            txn.claimRestart(token);
            txn.complete(token);
            efs.written.clear();
            txn.apply({"45006"}, token);
        }
        check(txn.status().profile == "Korea/LGU/Commercial" && txn.status().nextProfile.empty() &&
                      efs.written.front() == std::make_pair(std::string(kControl), Bytes{0}),
              "profile applies after any required first-enable baseline reset");
        for (const auto& item : store.load("Korea/LGU/Commercial"))
            check(efs.files.at(item.first) == item.second, "every profile item is written");
        check(txn.claimRestart(token) && !txn.claimRestart(token), "at most one claim per token");
        auto drift = efs.files;
        for (const auto& item : store.load("Korea/LGU/Commercial")) efs.files[item.first] = Bytes{0xee};
        efs.files[kControl] = Bytes{9};
        drift = efs.files;
        efs.forbiddenPaths = store.paths();
        efs.forbiddenPaths.insert(kControl);
        txn.complete(token);
        check(txn.status().state == "committed" && efs.files == drift,
              "completion never reads or reverts post-restart drift including control");
        check(journal(efs)["schema_version"] == 2 && journal(efs).isMember("updated_at_ms"),
              "state-only journal records progress and timestamp");
    }
    for (const auto& homes : std::vector<std::vector<std::string>>{{}, {"45007"}}) {
        const auto path = dir();
        FakeEfs efs;
        efs.files = base(store);
        efs.files[kControl] = Bytes{0};
        efs.journal = path + "/journal.json";
        const auto foreign = efs.files;
        Transaction txn(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        txn.evaluate(homes);
        txn.restore(token);
        auto expected = foreign;
        expected[kControl] = Bytes{7};
        check(efs.files == expected && efs.writes == 1 && txn.status().profile.empty(),
              "unsupported or explicit OFF without SIM writes only 07");
        txn.claimRestart(token);
        txn.resumeContext(homes);
        txn.complete(token);
        check(txn.status().state == "disabled" && txn.status().nextProfile.empty(),
              "control-only restore completes without a profile or baseline");
    }
    // Old snapshots are inert even if their entries were absent, malformed or missing.
    for (const auto& phase : {"committed", "restart_pending", "staged", "applying",
                              "readback_verified", "rolling_back", "rechecking", "rollback_failed"}) {
        const auto path = dir();
        FakeEfs efs;
        efs.files = base(store);
        efs.journal = path + "/journal.json";
        Json::Value old;
        old["schema_version"] = 1;
        old["state"] = phase;
        old["profile"] = "Korea/LGU/Commercial";
        old["enabled"] = true;
        old["token"] = token;
        old["restart_attempted"] = false;
        for (const auto& name : {"baseline", "before", "wanted", "managed"}) old[name] = "obsolete";
        checksum(old);
        durableWrite(efs.journal, old);
        bool safe = false, policy = true;
        Transaction txn(store, efs, path, [] { return "g2"; }, [&] { return policy; }, [&] { return safe; });
        txn.enable(true);
        check(txn.status().profile == "Korea/LGU/Commercial" && efs.writes == 0,
              "old journal keeps profile without reading EFS/snapshots");
        const auto state = std::string(phase);
        if (state == "staged" || state == "applying" || state == "readback_verified" || state == "rolling_back") {
            throws([&] { txn.recover(); }, "unsafe recovery is deferred");
            safe = true;
            policy = false;
            throws([&] { txn.recover(); }, "write policy still gates recovery");
            policy = true;
            txn.recover();
            check(txn.status().state == "error" && efs.writes == 0,
                  "interrupted legacy phases recover without rollback");
        } else if (state == "restart_pending") {
            safe = true;
            txn.resumeContext({"45006"});
            txn.claimRestart(token);
            txn.complete(token);
            check(txn.status().state == "committed", "legacy pending restart still completes");
        } else {
            check(txn.status().token.empty() || state == "committed", "obsolete failure/recheck becomes idle");
        }
        safe = true;
        txn.enable(true);
        txn.apply({"45005"}, token);
        check(efs.writes == 1 &&
                      txn.status().profile.empty() && txn.status().nextProfile == "Korea/SKT/Commercial",
              "migrated old profile switches through control-only 07");
        const auto saved = journal(efs);
        for (const auto& name : {"baseline", "before", "wanted", "managed", "normalization_pending"})
            check(!saved.isMember(name), "migration permanently drops snapshot/normalization fields");
    }
    for (const auto& phase : {"applying", "restart_pending", "error"}) {
        const auto path = dir();
        FakeEfs efs;
        efs.files = base(store);
        efs.journal = path + "/journal.json";
        Json::Value old;
        old["schema_version"] = 1;
        old["state"] = phase;
        old["profile"] = "";
        old["enabled"] = true;
        old["normalization_pending"] = true;
        old["token"] = token;
        old["before"] = Json::Value();
        checksum(old);
        durableWrite(efs.journal, old);
        Transaction txn(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        check(txn.status().state == "waiting_for_identity" && txn.status().token.empty(),
              "old normalization marker becomes idle");
        txn.apply({"45006"}, token);
        check(txn.status().profile == "Korea/LGU/Commercial", "old normalization applies directly");
    }
    for (const auto& phase : {"staged", "applying", "readback_verified"}) {
        const auto path = dir();
        FakeEfs efs;
        efs.files = base(store);
        efs.journal = path + "/journal.json";
        {
            Transaction txn(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
            txn.enable(true);
            txn.apply({"45006"}, token);
            txn.claimRestart(token);
            txn.complete(token);
            txn.apply({"45005"}, token);
        }
        auto interrupted = journal(efs);
        interrupted["state"] = phase;
        if (std::string(phase) != "readback_verified") interrupted["profile"] = "Korea/LGU/Commercial";
        checksum(interrupted);
        durableWrite(efs.journal, interrupted);
        Transaction txn(store, efs, path, [] { return "g2"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        const int writes = efs.writes;
        txn.recover();
        check(efs.writes == writes, "interrupted carrier reset never rolls back");
        if (std::string(phase) != "readback_verified") {
            check(txn.status().state == "error", "partial reset retries from the old applied profile");
            txn.apply({"45005"}, token);
            check(efs.writes == writes + 1 && txn.status().profile.empty(), "reset retry still writes only 07");
        }
        check(txn.status().state == "restart_pending", "verified reset recovery still waits for its restart");
        txn.claimRestart(token);
        txn.complete(token);
        txn.apply({"45005"}, token);
        check(txn.status().profile == "Korea/SKT/Commercial", "recovered reset applies P only after restart");
    }
    // Crash while resetting Q -> P: both the next phase and at-most-once claim survive.
    const auto path = dir();
    FakeEfs efs;
    efs.files = base(store);
    efs.journal = path + "/journal.json";
    {
        Transaction txn(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        txn.apply({"45006"}, token);
        txn.claimRestart(token);
        txn.complete(token);
        txn.apply({"45005"}, token);
        txn.claimRestart(token);
    }
    const auto reset = efs.files;
    {
        Transaction resumed(store, efs, path, [] { return "g2"; }, [] { return true; }, [] { return true; });
        check(resumed.status().profile.empty() && resumed.status().nextProfile == "Korea/SKT/Commercial",
              "reset phase survives process death");
        resumed.resumeContext({}); // Safe control-only recovery can change identity.
        check(!resumed.claimRestart(token), "restart claim survives process death");
        resumed.complete(token);
        check(efs.files == reset, "reset completion never reads or writes EFS");
        resumed.apply({"45005"}, token);
        check(resumed.status().profile == "Korea/SKT/Commercial" && resumed.status().nextProfile.empty(),
              "next pass applies P after the reset restart");
    }
}
void recoveryTests(ProfileStore& store, const std::string& scratch) {
    const auto path = scratch + "/readonly-cli";
    fs::create_directories(path);
    FakeEfs efs;
    efs.files = base(store);
    efs.journal = path + "/journal.json";
    {
        Transaction txn(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
        txn.enable(true);
        txn.apply({"45006"}, std::string(32, 'a'));
    }
    const auto bytes = readFile(efs.journal, 40 * 1024 * 1024);
    const auto files = efs.files;
    int sessions = 0;
    auto create = [&](bool readOnly) -> std::unique_ptr<EfsTransport> {
        check(readOnly, "CLI always opens read-only EFS");
        ++sessions;
        return std::make_unique<ForwardEfs>(efs, readOnly);
    };
    std::ostringstream out, errors;
    auto cli = [&](const std::string& mode, uid_t uid = 1001) {
        out.str(""); errors.str("");
        return runRecoveryCli(mode, uid, store, path, create, [] { return false; }, out, errors);
    };
    check(cli("--dump-readonly") == 0 && out.str().find("DUMP_OK") != std::string::npos,
          "radio CLI dump is available under a read-only policy");
    check(out.str().find("before=") == std::string::npos && out.str().find("baseline=") == std::string::npos &&
                  out.str().find("wanted=") == std::string::npos, "dump reports no obsolete snapshot comparisons");
    const auto dumped = out.str();
    check(std::count(dumped.begin(), dumped.end(), '\n') == int(store.paths().size() + 2),
          "dump covers every profile path and control");
    check(efs.files == files && readFile(efs.journal, 40 * 1024 * 1024) == bytes,
          "dump never writes EFS or journal");
    const int opened = sessions;
    check(cli("--recover-baseline", 0) == 1 && sessions == opened, "baseline recovery CLI is removed");
    check(cli("--dump-readonly", 2000) == 1 && sessions == opened, "shell cannot open CLI EFS");
    {
        Transaction live(store, efs, path, [] { return "g1"; }, [] { return true; }, [] { return true; });
        check(cli("--dump-readonly") == 1 && sessions == opened &&
                      errors.str().find("transaction locked") != std::string::npos,
              "live service mutex blocks a second DIAG session");
    }
    efs.failReadPath = *store.paths().begin();
    check(cli("--dump-readonly") == 1 && out.str().find("DUMP_FAIL") != std::string::npos &&
                  out.str().find("errno=13") != std::string::npos, "read-only dump reports path failures");
}

}  // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--diag-session") return DiagTransport::runSession();
    if (argc == 2 && std::string(argv[1]) == "--fake-diag-cache") {
        void* library = dlopen("libdiag.so", RTLD_NOW | RTLD_LOCAL);
        require(library, "host libdiag double available");
        const auto init = reinterpret_cast<unsigned char (*)(uint8_t*)>(dlsym(library, "Diag_LSM_Init"));
        const auto deinit = reinterpret_cast<unsigned char (*)()>(dlsym(library, "Diag_LSM_DeInit"));
        const auto logging = reinterpret_cast<void (*)(int, char*)>(dlsym(library, "diag_switch_logging"));
        const auto registration = reinterpret_cast<void (*)(int (*)(unsigned char*, int, void*), void*)>(
                dlsym(library, "diag_register_callback"));
        const auto send = reinterpret_cast<int (*)(int, unsigned char*, int)>(
                dlsym(library, "diag_callback_send_data"));
        require(init && deinit && logging && registration && send, "host libdiag ABI");
        int callbacks = 0;
        auto callback = [](unsigned char*, int, void* context) {
            ++*static_cast<int*>(context); return 0;
        };
        unsigned char request[] = {0x4b, 0x13, 0, 0};
        check(init(nullptr), "fake first init");
        registration(callback, &callbacks); logging(6, nullptr);
        check(send(0, request, sizeof(request)) == 0 && callbacks == 1, "fake fresh callback");
        check(deinit() && init(nullptr), "fake DeInit and reinit");
        registration(callback, &callbacks); logging(6, nullptr);
        check(send(0, request, sizeof(request)) == 0 && callbacks == 1 &&
                      fakeState().metrics[8] == 1, "fake skipped logging switch loses responses");
        std::cout << "PASS: cached CALLBACK_MODE skips switch after reinit and yields no callback" << std::endl;
        _exit(0);
    }
    try {
        if (argc == 6 && std::string(argv[1]) == "--parse") {
            fs::create_directories(argv[4]);
            ProfileStore store(argv[2], argv[4]);
            auto bytes = readFile(argv[3], 8 * 1024 * 1024);
            auto parsed = parseMbn(bytes, store.paths());
            auto id = store.importMbn(bytes, "sample.mbn", "45006");
            check(store.select({"45006"}).profile == "Korea/LGU/Commercial", "import does not auto-select");
            store.chooseImported("45006", id);
            check(store.select({"45006"}).profile == id && store.select({"450006"}).profile == id,
                  "explicit import choice and carrier aliases");
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
        const auto pack = readFile(fixture + "/profiles.pack", 8 * 1024 * 1024);
        for (const auto& item : index["profiles"]) {
            Bytes expanded(item["expanded_size"].asUInt());
            uLongf length = expanded.size();
            check(uncompress(expanded.data(), &length, pack.data() + item["offset"].asUInt64(),
                             item["compressed_size"].asUInt64()) == Z_OK && length == expanded.size(),
                  "test archive expansion");
            const auto manifest = parseJson(expanded);
            Snapshot expected;
            for (const auto& entry : manifest["entries"]) {
                const auto path = entry["path"].asString();
                expected[path] = unhex(entry["hex"].asString());
            }
            check(packaged.load(item["id"].asString()) == expected,
                  "all archive digests and exact filtered entries");
        }
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
        transportRecoveryTests(store, scratch);
        managedPathTests(store, fixture, scratch);
        importedChoiceTests(fixture, scratch);
        recoveryTests(store, scratch);
        baselineResetTests(store, scratch);
        controlRestartTests(store, scratch);
        const std::string token(32, 'a');
        int scenario = 0;
        auto setup = [&] {
            std::string dir = scratch + "/case" + std::to_string(++scenario);
            fs::create_directories(dir);
            return dir;
        };
        for (const std::string mismatch : {"", "source_name", "source_sha256"}) {
            const auto dir = setup();
            FakeEfs efs;
            efs.files = baseline;
            efs.journal = dir + "/journal.json";
            {
                Transaction txn(store, efs, dir, [] { return "g1"; },
                                [] { return true; }, [] { return true; });
                txn.enable(true);
                txn.apply({"45006"}, token);
                txn.claimRestart(token);
                txn.complete(token);
            }
            if (!mismatch.empty()) {
                auto saved = parseJson(readFile(efs.journal, 40 * 1024 * 1024));
                saved[mismatch] = mismatch == "source_name" ? "old.mbn" : std::string(64, 'f');
                saved.removeMember("sha256");
                saved["sha256"] = sha256(jsonBytes(saved));
                durableWrite(efs.journal, saved);
            }
            const auto before = readFile(efs.journal, 40 * 1024 * 1024);
            const int writes = efs.writes, reads = efs.reads;
            Transaction rebooted(store, efs, dir, [] { return "g2"; },
                                 [] { return true; }, [] { return true; });
            rebooted.evaluate({"45006"}); // Same path as Binder apply.
            rebooted.apply({"45006"}, std::string(32, 'b'));
            if (mismatch.empty()) {
                check(efs.writes == writes && efs.reads == reads &&
                              rebooted.status().state == "committed" && rebooted.status().token.empty() &&
                              readFile(efs.journal, 40 * 1024 * 1024) == before,
                      "reboot same committed source does no EFS IO, journal write or restart");
                throws([&] { rebooted.claimRestart(token); }, "no-op cannot claim another restart");
                rebooted.apply({"45005"}, token);
                check(efs.writes == writes + 1 && rebooted.status().state == "restart_pending",
                      "SIM change after reboot still resets and restarts");
            } else {
                check(efs.writes > writes && rebooted.status().state == "restart_pending",
                      "changed source identity still rewrites and restarts");
            }
        }
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
            check(!txn.status().enabled && txn.status().state == "disabled",
                  "Automatic off from first boot");
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
            check(txn.status().state == "restart_pending" && txn.status().enabled,
                  "reenable waits for pending 07 restart");
            txn.claimRestart(token);
            txn.complete(token);
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
            check(txn.status().state == "committed", "commit after radio recovery");
            int writes = efs.writes;
            const auto committedJournal = readFile(efs.journal, 40 * 1024 * 1024);
            txn.apply({"45006"}, token);
            check(efs.writes == writes && txn.status().state == "committed" &&
                          readFile(efs.journal, 40 * 1024 * 1024) == committedJournal,
                  "same committed profile trigger is a no-op");
            throws([&] { txn.claimRestart(token); }, "same committed profile cannot restart");
            auto skt = store.load("Korea/SKT/Commercial");
            const auto beforeReset = efs.files;
            int resetWrites = efs.writes;
            const auto beforeRemovalJournal = readFile(efs.journal, 40 * 1024 * 1024);
            txn.evaluate({});
            txn.apply({}, token);
            check(efs.files == beforeReset && efs.writes == resetWrites &&
                          txn.status().profile == "Korea/LGU/Commercial" && txn.status().state == "committed" &&
                          readFile(efs.journal, 40 * 1024 * 1024) == beforeRemovalJournal,
                  "SIM removal preserves profile/control/journal with no writes or state change");
            txn.apply({"45005"}, token);
            auto expectedReset = beforeReset;
            expectedReset[kControl] = Bytes{7};
            check(efs.files == expectedReset && efs.writes == resetWrites + 1 &&
                          txn.status().profile.empty() && txn.status().nextProfile == "Korea/SKT/Commercial",
                  "Q to P first writes only 07");
            throws([&] { txn.apply({"45005"}, token); }, "P waits for the reset restart");
            txn.claimRestart(token);
            txn.complete(token);
            txn.apply({"45005"}, token);
            for (const auto& item : skt)
                check(efs.files.at(item.first) == item.second, "P writes all items after automatic restart");
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
            txn.enable(true); // Explicit request invalidates committed proof and repairs drift.
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
            check(txn.status().state == "restart_pending" && txn.status().enabled,
                  "reenable waits for pending 07 restart");
            txn.claimRestart(token);
            txn.complete(token);
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
            txn.enable(true); // Exercise write gates on a fresh explicit attempt.
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
            check(efs.writes == 1 && txn.status().state == "baseline_restart_pending",
                  "foreign overlay resets baseline without adding firmware approval");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            txn.apply({"45006"}, token);
            txn.claimRestart(token);
            txn.complete(token);
            txn.enable(true);
            efs.writes = 0;
            efs.parentMissing = true;
            throwsContaining([&] { txn.apply({"45006"}, token); },
                             {"step=mutate", "path=/", "injected missing parent", "errno=unknown"});
            check(efs.writes == 0, "no directory creation");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.failAt = 3;
            throws([&] { txn.apply({"45006"}, token); }, "injected apply failure");
            check(efs.files != baseline && efs.writes == 3 && txn.status().state == "error"
                          && txn.status().enabled, "failed apply leaves partial writes and switch enabled");
            efs.failAt = -1;
            txn.apply({"45006"}, token);
            txn.claimRestart(token);
            txn.complete(token);
            check(txn.status().state == "committed", "failure then direct retry succeeds");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.corruptOnce = true;
            throws([&] { txn.apply({"45006"}, token); }, "readback failure");
            check(efs.files != baseline && txn.status().enabled && txn.status().state == "error",
                  "readback failure reports error without restoring EFS");
            txn.apply({"45006"}, token);
            check(txn.status().state == "restart_pending", "readback failure can retry");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.failAt = store.load("Korea/LGU/Commercial").size() + 1;
            throws([&] { txn.apply({"45006"}, token); }, "failure after final profile write");
            const int writes = efs.writes;
            efs.failAt = -1;
            txn.enable(true); // A new coordinator/switch trigger clears the displayed error.
            txn.apply({"45006"}, token);
            check(efs.writes > writes && txn.status().state == "restart_pending",
                  "fully written failed apply still retries and requests its restart");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.failAt = 4;
            throws([&] { txn.apply({"45006"}, token); }, "partial old carrier write");
            efs.failAt = -1;
            txn.apply({"45005"}, token);
            check(txn.status().profile.empty() && txn.status().nextProfile == "Korea/SKT/Commercial" &&
                          efs.files.at(kControl) == Bytes{7}, "failed Q also resets before switching to P");
            txn.claimRestart(token);
            txn.complete(token);
            txn.apply({"45005"}, token);
            check(txn.status().profile == "Korea/SKT/Commercial", "P retries after failed Q and reset restart");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto& safe, const auto&) {
            efs.afterWrite = [&](int writes) {
                if (writes == 3) safe = false;
            };
            throws([&] { txn.apply({"45006"}, token); }, "incoming call revokes in-flight lease");
            check(efs.files != baseline && efs.writes == 3 && txn.status().enabled,
                  "expired lease stops writes without automatic restoration");
            safe = true;
            efs.afterWrite = {};
            txn.apply({"45006"}, token);
            check(txn.status().state == "restart_pending", "fresh lease retries failed apply");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto& safe, const auto&) {
            txn.apply({"45006"}, token);
            safe = false;
            throws([&] { txn.claimRestart(token); }, "call defers restart");
            check(!txn.status().restartAttempted, "unsafe restart not consumed");
            safe = true;
            txn.claimRestart(token);
            efs.files[store.load("Korea/LGU/Commercial").begin()->first] = Bytes{0xee};
            const auto drifted = efs.files;
            txn.complete(token);
            check(efs.files == drifted && txn.status().state == "committed",
                  "post-restart drift completes without readback");
        });
        run([&](auto& txn, auto& efs, auto&, auto&, auto&, const auto&) {
            efs.failAt = 2;
            efs.failForever = true;
            throwsContaining([&] { txn.apply({"45006"}, token); },
                             {"step=mutate", "path=/", "injected partial write"});
            check(txn.status().state == "error" && txn.status().enabled && efs.writes == 2,
                  "persistent write failure never attempts rollback or latches");
            txn.enable(false);
            efs.failForever = false;
            txn.restore(token);
            check(txn.status().profile.empty() && efs.files.at(kControl) == Bytes{7},
                  "switch OFF works after a failed apply");
        });
        // Restart-phase crashes preserve the WAL; all incomplete mutation phases
        // retain partial EFS and become retryable without restoring the pre-state.
        for (const auto& phase : {"staged", "applying", "readback_verified",
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
                    check(efs.writes == writes && efs.files == partial
                                  && recovered.status().state == "error",
                          "interrupted disabled transaction recovers without EFS changes");
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
                    check(efs.files == partial, "startup WAL recovery leaves EFS untouched");
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
            check(efs.files != baseline && quarantined.status().state == "error",
                  "WAL recovery independent of modem version leaves EFS untouched");
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
        corrupt["error"] = "tampered";
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
                     "state-WAL/all-items/two-restart-switch/retry/migration\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
#endif  // MCFG_FAKE_DIAG
