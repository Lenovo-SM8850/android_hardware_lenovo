// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <sys/types.h>
#include "ProfileStore.h"

namespace mcfg {
class PacketTransport {
  public:
    virtual ~PacketTransport() = default;
    virtual Bytes exchange(const Bytes& request) = 0;
    virtual bool connected() const { return true; }
};
// Qualcomm callback ABI declarations are independently expressed, not copied
// implementation code. Lenovo's shipped libdiag chooses its own router endpoint.
class DiagTransport final : public PacketTransport {
  public:
    explicit DiagTransport(std::function<bool()> modemReady = [] { return true; },
                           std::function<bool()> contextAllowed = [] { return true; });
    ~DiagTransport() override;
    Bytes exchange(const Bytes& request) override;
    bool connected() const override { return initialized_; }
    void invalidate();
    static std::vector<Bytes> decodeCallback(const Bytes& bytes);
    // Private pipe worker entry point, dispatched before Binder startup.
    static int runSession();

  private:
    static int callback(unsigned char* bytes, int length, void* context);
    void disconnect();
    void open();
    void loadLibrary();
    Bytes callbackExchange(const Bytes& request);
    void awaitReady();
    Bytes exchangeOnce(const Bytes& request);
    void* library_ = nullptr;
    pid_t child_ = -1;
    int requestFd_ = -1, replyFd_ = -1;
    int (*send_)(int, unsigned char*, int) = nullptr;
    unsigned char (*deinit_)() = nullptr;
    unsigned char (*init_)(uint8_t*) = nullptr;
    void (*registration_)(int (*)(unsigned char*, int, void*), void*) = nullptr;
    void (*logging_)(int, char*) = nullptr;
    std::function<bool()> modemReady_, contextAllowed_;
    std::mutex requestMutex_, responseMutex_;
    std::condition_variable condition_;
    Bytes response_;
    uint16_t command_ = 0;
    bool waiting_ = false, initialized_ = false;
};
}  // namespace mcfg
