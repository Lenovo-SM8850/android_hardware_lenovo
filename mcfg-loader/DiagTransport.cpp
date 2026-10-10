// SPDX-License-Identifier: Apache-2.0
#include "DiagTransport.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <thread>

extern char** environ;

namespace mcfg {
namespace {
constexpr int kRequestPipe = 3, kReplyPipe = 4;
using Deadline = std::chrono::steady_clock::time_point;
void transfer(int fd, void* bytes, size_t size, bool writing, Deadline deadline) {
    auto* data = static_cast<uint8_t*>(bytes);
    while (size) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
        require(left > 0, "diag send/timeout");
        pollfd ready{fd, short(writing ? POLLOUT : POLLIN), 0};
        int result = poll(&ready, 1, int(left));
        if (result < 0 && errno == EINTR) continue;
        require(result > 0 && (ready.revents & ready.events), "diag send/timeout");
        ssize_t count = writing ? write(fd, data, size) : read(fd, data, size);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        require(count > 0, "diag send/timeout");
        data += count;
        size -= count;
    }
}
void header(int fd, uint32_t& value, bool writing, Deadline deadline) {
    uint8_t bytes[4];
    if (writing)
        for (int i = 0; i < 4; ++i) bytes[i] = (value >> (8 * i)) & 255;
    transfer(fd, bytes, sizeof(bytes), writing, deadline);
    if (!writing) {
        value = 0;
        for (int i = 0; i < 4; ++i) value |= uint32_t(bytes[i]) << (8 * i);
    }
}
Bytes receive(int fd, Deadline deadline) {
    uint32_t size = 0;
    header(fd, size, false, deadline);
    bool error = size & 0x80000000u;
    size &= 0x7fffffffu;
    require(size <= (error ? 128u : 4096u), "diag send/timeout");
    Bytes bytes(size);
    transfer(fd, bytes.data(), bytes.size(), false, deadline);
    if (error) throw std::runtime_error(std::string(bytes.begin(), bytes.end()));
    return bytes;
}
void reply(const Bytes& bytes, bool error = false) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint32_t size = bytes.size() | (error ? 0x80000000u : 0);
    header(kReplyPipe, size, true, deadline);
    transfer(kReplyPipe, const_cast<uint8_t*>(bytes.data()), bytes.size(), true, deadline);
}
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
DiagTransport::DiagTransport(std::function<bool()> modemReady, std::function<bool()> contextAllowed)
    : modemReady_(std::move(modemReady)), contextAllowed_(std::move(contextAllowed)) {}
void DiagTransport::loadLibrary() {
    library_ = dlopen("libdiag.so", RTLD_NOW | RTLD_LOCAL);
    require(library_, "libdiag unavailable");
    try {
        init_ = symbol<unsigned char (*)(uint8_t*)>(library_, "Diag_LSM_Init");
        deinit_ = symbol<unsigned char (*)()>(library_, "Diag_LSM_DeInit");
        registration_ = symbol<void (*)(int (*)(unsigned char*, int, void*), void*)>(
                library_, "diag_register_callback");
        logging_ = symbol<void (*)(int, char*)>(library_, "diag_switch_logging");
        send_ = symbol<int (*)(int, unsigned char*, int)>(library_, "diag_callback_send_data");
    } catch (...) {
        dlclose(library_);
        library_ = nullptr;
        throw;
    }
}
DiagTransport::~DiagTransport() {
    disconnect();
}
void DiagTransport::disconnect() {
    {
        std::lock_guard lock(responseMutex_);
        waiting_ = false;
        response_.clear();
    }
    // The stock reader detaches after signalling DeInit, so dlclose can race
    // its epilogue. Process death safely closes the router and resets all globals.
    // Close the pipes first: a worker blocked on its request pipe then sees EOF
    // and exits even if the kill is refused, so the wait below cannot hang.
    if (requestFd_ >= 0) close(requestFd_);
    if (replyFd_ >= 0) close(replyFd_);
    requestFd_ = replyFd_ = -1;
    if (child_ > 0) {
        kill(child_, SIGKILL);
        while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
        child_ = -1;
    }
    initialized_ = false;
}
void DiagTransport::invalidate() {
    std::lock_guard serial(requestMutex_);
    disconnect();
}
void DiagTransport::open() {
    // Exec gives every modem session fresh libdiag globals, including its mode cache.
    int pipes[4] = {-1, -1, -1, -1};
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "libdiag init");
    try {
        int pair[2];
        for (int i = 0; i < 4; i += 2) {
            require(pipe2(pair, O_CLOEXEC) == 0, "libdiag init");
            // All originals are above the protocol descriptors, avoiding dup2
            // collisions even when stdin/stdout/stderr were closed by the caller.
            pipes[i] = fcntl(pair[0], F_DUPFD_CLOEXEC, 5);
            pipes[i + 1] = fcntl(pair[1], F_DUPFD_CLOEXEC, 5);
            close(pair[0]); close(pair[1]);
            require(pipes[i] >= 0 && pipes[i + 1] >= 0, "libdiag init");
        }
        require(posix_spawn_file_actions_adddup2(&actions, pipes[0], kRequestPipe) == 0 &&
                        posix_spawn_file_actions_adddup2(&actions, pipes[3], kReplyPipe) == 0,
                "libdiag init");
        for (int fd : pipes)
            require(posix_spawn_file_actions_addclose(&actions, fd) == 0, "libdiag init");
        char executable[] = "/proc/self/exe", mode[] = "--diag-session";
        char* argv[] = {executable, mode, nullptr};
        require(posix_spawn(&child_, executable, &actions, nullptr, argv, environ) == 0,
                "libdiag init");
        requestFd_ = pipes[1]; pipes[1] = -1;
        replyFd_ = pipes[2]; pipes[2] = -1;
        require(fcntl(requestFd_, F_SETFL, O_NONBLOCK) == 0 &&
                        fcntl(replyFd_, F_SETFL, O_NONBLOCK) == 0, "libdiag init");
        require(receive(replyFd_, std::chrono::steady_clock::now() +
                        std::chrono::seconds(3)).empty(), "libdiag init");
    } catch (...) {
        for (int fd : pipes) if (fd >= 0) close(fd);
        posix_spawn_file_actions_destroy(&actions);
        disconnect();
        throw;
    }
    for (int fd : pipes) if (fd >= 0) close(fd);
    posix_spawn_file_actions_destroy(&actions);
    initialized_ = true;
}
int DiagTransport::runSession() {
    // This mode owns only its inherited pipes and diag connection, never Binder,
    // the transaction journal or restart authorization. Exit without dlclose.
    // Keep the callback context and its mutexes alive through error reporting;
    // libdiag's reader can still deliver a late packet until _exit kills it.
    DiagTransport diag;
    try {
#ifdef __ANDROID__
        require(getuid() == 0 || getuid() == 1001, "CLI caller must be root or radio");
#endif
        struct stat input{}, output{};
        require(fstat(kRequestPipe, &input) == 0 && S_ISFIFO(input.st_mode) &&
                        fstat(kReplyPipe, &output) == 0 && S_ISFIFO(output.st_mode), "libdiag init");
        require(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() != 1, "libdiag init");
        signal(SIGPIPE, SIG_IGN);
        diag.loadLibrary();
        require(diag.init_(nullptr) != 0, "libdiag init");
        diag.registration_(callback, &diag);
        diag.logging_(6, nullptr);  // One real CALLBACK_MODE switch in a fresh process.
        reply({});
        for (;;) {
            pollfd ready{kRequestPipe, POLLIN, 0};
            int result;
            do { result = poll(&ready, 1, -1); } while (result < 0 && errno == EINTR);
            require(result > 0 && (ready.revents & POLLIN), "diag send/timeout");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            uint32_t size = 0;
            header(kRequestPipe, size, false, deadline);
            require(size >= 4 && size <= 1100, "diag request bounds");
            Bytes request(size);
            transfer(kRequestPipe, request.data(), size, false, deadline);
            require(request[0] == 0x4b && request[1] == 0x13, "diag request bounds");
            reply(diag.callbackExchange(request));
        }
    } catch (const std::exception& error) {
        try {
            std::string message(error.what());
            message.resize(std::min<size_t>(message.size(), 128));
            reply(Bytes(message.begin(), message.end()), true);
        } catch (...) {}
    }
    _exit(1);
}
void DiagTransport::awaitReady() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (int attempt = 0; attempt < 3; ++attempt) {
        // A context lease is authorization, not a signal to wait for: renewal
        // may need this very Binder thread. Only the independently readable
        // restart-helper state may keep this call waiting.
        require(contextAllowed_(), "diag context lease unavailable");
        while (!modemReady_()) {
            disconnect();
            require(contextAllowed_(), "diag context lease unavailable");
            require(std::chrono::steady_clock::now() < deadline, "modem readiness timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(contextAllowed_(), "diag context lease unavailable");
        if (initialized_) return;
        try {
            open();
            // Radio ON can precede the diag/EFS endpoint. Probe without writes
            // before allowing any request in the newly opened session.
            Bytes hello{0x4b, 0x13, 0, 0};
            const auto put = [&](uint32_t value) {
                for (int i = 0; i < 4; ++i) hello.push_back((value >> (8 * i)) & 255);
            };
            for (int i = 0; i < 6; ++i) put(0x100000);
            for (int i = 0; i < 3; ++i) put(1);
            put(0xffffffff);
            const auto reply = exchangeOnce(hello);
            require(reply.size() == 44 && reply[28] == 1 && reply[29] == 0 &&
                            reply[30] == 0 && reply[31] == 0, "EFS readiness HELLO version");
            require(contextAllowed_(), "diag context lease unavailable");
            return;
        } catch (...) {
            disconnect();
            if (attempt == 2 || std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(500 << attempt));
        }
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
    if (!contextAllowed_()) {
        disconnect();
        throw std::runtime_error("diag context lease unavailable");
    }
    if (initialized_ && !modemReady_()) {
        // A READ/WRITE/CLOSE may carry an fd opened in this session. Do not
        // reconnect underneath it and send that fd to a different modem epoch.
        disconnect();
        throw std::runtime_error("diag transport lost during modem restart");
    }
    try {
        awaitReady();
        return exchangeOnce(request);
    } catch (...) {
        // Never replay a timed-out request: OPEN(O_TRUNC)/WRITE may have landed.
        // The transaction journals an error and retries with verified readback.
        disconnect();
        throw;
    }
}
Bytes DiagTransport::exchangeOnce(const Bytes& request) {
    // An exited worker must become an error, not a fatal SIGPIPE in the service.
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    require(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0, "diag send/timeout");
    sigpending(&pending);
    const bool alreadyPending = sigismember(&pending, SIGPIPE);
    const auto restore = [&] {
        if (!alreadyPending) {
            timespec immediate{};
            while (sigtimedwait(&blocked, nullptr, &immediate) < 0 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    };
    try {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        uint32_t size = request.size();
        header(requestFd_, size, true, deadline);
        transfer(requestFd_, const_cast<uint8_t*>(request.data()), request.size(), true, deadline);
        auto response = receive(replyFd_, deadline);
        require(response.size() >= 4 && response[0] == request[0] && response[1] == request[1] &&
                        response[2] == request[2] && response[3] == request[3], "diag send/timeout");
        restore();
        return response;
    } catch (...) {
        restore();
        throw;
    }
}
Bytes DiagTransport::callbackExchange(const Bytes& request) {
    std::unique_lock responseLock(responseMutex_);
    command_ = request[2] | (request[3] << 8);
    response_.clear();
    waiting_ = true;
    responseLock.unlock();
    int result = send_(0, const_cast<unsigned char*>(request.data()), request.size());
    responseLock.lock();
    if (result != 0 || !condition_.wait_for(responseLock, std::chrono::seconds(3),
                                            [&] { return !response_.empty(); })) {
        waiting_ = false;
        throw std::runtime_error("diag send/timeout");
    }
    waiting_ = false;
    return response_;
}
}  // namespace mcfg
