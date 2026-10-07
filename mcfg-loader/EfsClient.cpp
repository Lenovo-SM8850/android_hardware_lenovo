// SPDX-License-Identifier: Apache-2.0
#include "EfsClient.h"
#include <algorithm>
#include <stdexcept>

namespace mcfg {
namespace {
void put(Bytes& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back((value >> (8 * i)) & 255);
}
uint32_t get(const Bytes& bytes, size_t pos) {
    require(pos + 4 <= bytes.size(), "EFS field bounds");
    return bytes[pos] | (uint32_t(bytes[pos + 1]) << 8) | (uint32_t(bytes[pos + 2]) << 16) |
           (uint32_t(bytes[pos + 3]) << 24);
}
Bytes pathBytes(const std::string& path) {
    Bytes bytes(path.begin(), path.end());
    bytes.push_back(0);
    return bytes;
}
void error(const char* operation, uint32_t code) {
    if (code != 0)
        throw std::runtime_error(std::string("EFS ") + operation +
                                 " errno=" + std::to_string(code));
}
}  // namespace
EfsClient::EfsClient(PacketTransport& transport, std::set<std::string> allow, bool readOnly)
    : transport_(transport), allow_(std::move(allow)), readOnly_(readOnly) {
    allow_.insert(kControl);
    Bytes hello;
    for (int i = 0; i < 6; ++i) put(hello, 0x100000);
    for (int i = 0; i < 3; ++i) put(hello, 1);
    put(hello, 0xffffffff);
    const auto reply = command(0, hello, 44);
    require(reply.size() == 44 && get(reply, 28) == 1, "EFS HELLO version");
}
Bytes EfsClient::command(uint16_t cmd, const Bytes& body, size_t minimum) {
    Bytes packet{0x4b, 0x13, uint8_t(cmd), uint8_t(cmd >> 8)};
    packet.insert(packet.end(), body.begin(), body.end());
    auto reply = transport_.exchange(packet);
    require(reply.size() >= minimum && reply.size() <= 4096 && reply[0] == 0x4b &&
                    reply[1] == 0x13 && reply[2] == uint8_t(cmd) && reply[3] == uint8_t(cmd >> 8),
            "EFS reply identity/length");
    return reply;
}
void EfsClient::pathAllowed(const std::string& path) const {
    require(allow_.count(path), "EFS path not approved");
}
Bytes EfsClient::stat(const std::string& path) {
    auto reply = command(16, pathBytes(path), 32);  // LSTAT: reject symlinks.
    require(reply.size() == 32, "EFS LSTAT length");
    return reply;
}
void EfsClient::checkParent(const std::string& path) {
    pathAllowed(path);
    auto reply = stat(path.substr(0, path.find_last_of('/')));
    error("parent LSTAT", get(reply, 4));
    require(get(reply, 4) == 0 && (get(reply, 8) & 0170000) == 0040000,
            "EFS parent absent/not directory");
}
int EfsClient::open(const std::string& path, bool writing) {
    Bytes body;
    put(body, writing ? 01101 : 0);
    put(body, 0600);
    const auto name = pathBytes(path);
    body.insert(body.end(), name.begin(), name.end());
    auto reply = command(2, body, 12);
    error("OPEN", get(reply, 8));
    require(reply.size() == 12 && get(reply, 8) == 0 && get(reply, 4) <= INT32_MAX,
            "EFS OPEN errno/fd");
    return get(reply, 4);
}
void EfsClient::close(int fd) {
    Bytes body;
    put(body, fd);
    auto reply = command(3, body, 8);
    error("CLOSE", get(reply, 4));
    require(reply.size() == 8 && get(reply, 4) == 0, "EFS CLOSE errno");
}
std::optional<Bytes> EfsClient::read(const std::string& path) {
    pathAllowed(path);
    const auto info = stat(path);
    if (get(info, 4) == 2) return {};
    error("LSTAT", get(info, 4));
    require(get(info, 4) == 0 && (get(info, 8) & 0170000) == 0100000 && get(info, 12) <= 65536,
            "EFS LSTAT errno/type/size");
    int fd = open(path, false);
    Bytes bytes;
    try {
        while (true) {
            Bytes body;
            put(body, fd);
            put(body, 1024);
            put(body, bytes.size());
            auto reply = command(4, body, 20);
            auto count = get(reply, 12);
            error("READ", get(reply, 16));
            require(get(reply, 4) == uint32_t(fd) && get(reply, 8) == bytes.size() &&
                            get(reply, 16) == 0 && count <= 1024 && reply.size() == count + 20 &&
                            bytes.size() + count <= 65536,
                    "EFS READ errno/fd/offset/count");
            bytes.insert(bytes.end(), reply.begin() + 20, reply.end());
            if (count < 1024) break;
        }
        require(bytes.size() == get(info, 12), "EFS changed during read");
    } catch (...) {
        try {
            close(fd);
        } catch (...) {
        }
        throw;
    }
    close(fd);
    return bytes;
}
void EfsClient::write(const std::string& path, const Bytes& bytes,
                      const std::function<void()>& checkpoint) {
    pathAllowed(path);
    require(!readOnly_ && bytes.size() <= 65536, "EFS writes disabled/bounds");
    checkParent(path);
    const auto info = stat(path);
    require(get(info, 4) == 2 || (get(info, 4) == 0 && (get(info, 8) & 0170000) == 0100000),
            "EFS write target type");
    if (checkpoint) checkpoint();  // OPEN(O_TRUNC) is already a mutation.
    int fd = open(path, true);
    try {
        for (size_t offset = 0; offset < bytes.size(); offset += 1024) {
            if (checkpoint) checkpoint();
            size_t count = std::min(size_t(1024), bytes.size() - offset);
            Bytes body;
            put(body, fd);
            put(body, offset);
            body.insert(body.end(), bytes.begin() + offset, bytes.begin() + offset + count);
            auto reply = command(5, body, 20);
            error("WRITE", get(reply, 16));
            require(reply.size() == 20 && get(reply, 4) == uint32_t(fd) &&
                            get(reply, 8) == offset && get(reply, 12) == count &&
                            get(reply, 16) == 0,
                    "EFS WRITE errno/fd/offset/short count");
        }
    } catch (...) {
        try {
            close(fd);
        } catch (...) {
        }
        throw;
    }
    close(fd);
}
void EfsClient::remove(const std::string& path, const std::function<void()>& checkpoint) {
    pathAllowed(path);
    require(!readOnly_, "EFS writes disabled");
    const auto info = stat(path);
    if (get(info, 4) == 2) return;
    error("unlink LSTAT", get(info, 4));
    require(get(info, 4) == 0 && (get(info, 8) & 0170000) == 0100000, "EFS unlink target type");
    if (checkpoint) checkpoint();
    auto reply = command(8, pathBytes(path), 8);
    error("UNLINK", get(reply, 4));
    require(reply.size() == 8 && get(reply, 4) == 0, "EFS UNLINK errno");
}
}  // namespace mcfg
