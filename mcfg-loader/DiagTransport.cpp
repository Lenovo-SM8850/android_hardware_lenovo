// SPDX-License-Identifier: Apache-2.0
#include "DiagTransport.h"
#include <dlfcn.h>
#include <chrono>

namespace mcfg {
namespace {
template <typename T>
T symbol(void* library, const char* name) {
    auto pointer = reinterpret_cast<T>(dlsym(library, name));
    require(pointer != nullptr, "libdiag ABI");
    return pointer;
}
uint16_t crc(const Bytes& bytes) {
    uint16_t value = 0xffff;
    for (auto byte : bytes) {
        value ^= byte;
        for (int i = 0; i < 8; ++i) value = (value >> 1) ^ ((value & 1) ? 0x8408 : 0);
    }
    return value;
}
}  // namespace
DiagTransport::DiagTransport() {
    library_ = dlopen("libdiag.so", RTLD_NOW | RTLD_LOCAL);
    require(library_, "libdiag unavailable");
    try {
        auto init = symbol<unsigned char (*)(uint8_t*)>(library_, "Diag_LSM_Init");
        deinit_ = symbol<unsigned char (*)()>(library_, "Diag_LSM_DeInit");
        auto registration = symbol<void (*)(int (*)(unsigned char*, int, void*), void*)>(
                library_, "diag_register_callback");
        auto logging = symbol<void (*)(int, char*)>(library_, "diag_switch_logging");
        send_ = symbol<int (*)(int, unsigned char*, int)>(library_, "diag_callback_send_data");
        require(init(nullptr) != 0, "libdiag init");
        registration(callback, this);
        logging(6, nullptr);  // CALLBACK_MODE; no USB composition/HDLC changes.
    } catch (...) {
        dlclose(library_);
        library_ = nullptr;
        throw;
    }
}
DiagTransport::~DiagTransport() {
    if (library_) {
        deinit_();
        dlclose(library_);
    }
}
std::vector<Bytes> DiagTransport::decodeCallback(const Bytes& bytes) {
    require(!bytes.empty() && bytes.size() <= 65536, "callback bounds");
    std::vector<Bytes> packets;
    // Version-1 non-HDLC envelope: delimiter, version, LE length, packet, delimiter.
    if (bytes.size() >= 5 && bytes[0] == 0x7e && bytes[1] == 1) {
        size_t pos = 0;
        while (pos < bytes.size()) {
            require(bytes.size() - pos >= 5 && bytes[pos] == 0x7e && bytes[pos + 1] == 1,
                    "diag envelope");
            size_t size = bytes[pos + 2] | (bytes[pos + 3] << 8);
            require(size <= 4096 && pos + size + 5 <= bytes.size() && bytes[pos + size + 4] == 0x7e,
                    "diag envelope length");
            packets.emplace_back(bytes.begin() + pos + 4, bytes.begin() + pos + 4 + size);
            pos += size + 5;
        }
    } else if (bytes.back() == 0x7e) {
        Bytes frame;
        bool escaped = false;
        for (auto byte : bytes) {
            if (byte == 0x7e) {
                if (!frame.empty()) {
                    require(!escaped && frame.size() >= 3 && crc(frame) == 0xf0b8, "diag HDLC CRC");
                    frame.resize(frame.size() - 2);
                    packets.push_back(std::move(frame));
                    frame.clear();
                }
            } else if (escaped) {
                frame.push_back(byte ^ 0x20);
                escaped = false;
            } else if (byte == 0x7d)
                escaped = true;
            else
                frame.push_back(byte);
            require(frame.size() <= 4098, "diag frame bounds");
        }
        require(frame.empty() && !escaped, "diag partial frame");
    } else {
        // Some libdiag revisions deliver an already decoded single response.
        require(bytes.size() <= 4096 && bytes[0] == 0x4b, "diag raw framing");
        packets.push_back(bytes);
    }
    return packets;
}
int DiagTransport::callback(unsigned char* bytes, int length, void* context) {
    if (!bytes || length <= 0 || length > 65536 || !context) return 0;
    auto* self = static_cast<DiagTransport*>(context);
    try {
        for (const auto& packet : decodeCallback(Bytes(bytes, bytes + length))) {
            std::lock_guard lock(self->responseMutex_);
            if (self->waiting_ && self->response_.empty() && packet.size() >= 4 &&
                packet[0] == 0x4b && packet[1] == 0x13 &&
                (packet[2] | (packet[3] << 8)) == self->command_) {
                self->response_ = packet;
                self->condition_.notify_one();
            }
        }
    } catch (...) { /* Unrelated logs or invalid frames cannot authorize an EFS operation. */
    }
    return 0;
}
Bytes DiagTransport::exchange(const Bytes& request) {
    std::lock_guard serial(requestMutex_);
    require(request.size() >= 4 && request.size() <= 1100 && request[0] == 0x4b &&
                    request[1] == 0x13,
            "diag request bounds");
    std::unique_lock responseLock(responseMutex_);
    require(!poisoned_, "diag transport poisoned after timeout");
    command_ = request[2] | (request[3] << 8);
    response_.clear();
    waiting_ = true;
    responseLock.unlock();
    int result = send_(0, const_cast<unsigned char*>(request.data()), request.size());
    responseLock.lock();
    if (result != 0 || !condition_.wait_for(responseLock, std::chrono::seconds(3),
                                            [&] { return !response_.empty(); })) {
        waiting_ = false;
        poisoned_ = true;
        throw std::runtime_error("diag send/timeout");
    }
    waiting_ = false;
    return response_;
}
}  // namespace mcfg
