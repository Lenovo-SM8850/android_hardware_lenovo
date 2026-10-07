// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <condition_variable>
#include <mutex>
#include "ProfileStore.h"

namespace mcfg {
class PacketTransport {
  public:
    virtual ~PacketTransport() = default;
    virtual Bytes exchange(const Bytes& request) = 0;
};
// Qualcomm callback ABI declarations are independently expressed, not copied
// implementation code. Lenovo's shipped libdiag chooses its own router endpoint.
class DiagTransport final : public PacketTransport {
  public:
    DiagTransport();
    ~DiagTransport() override;
    Bytes exchange(const Bytes& request) override;
    static std::vector<Bytes> decodeCallback(const Bytes& bytes);

  private:
    static int callback(unsigned char* bytes, int length, void* context);
    void* library_ = nullptr;
    int (*send_)(int, unsigned char*, int) = nullptr;
    unsigned char (*deinit_)() = nullptr;
    std::mutex requestMutex_, responseMutex_;
    std::condition_variable condition_;
    Bytes response_;
    uint16_t command_ = 0;
    bool waiting_ = false, poisoned_ = false;
};
}  // namespace mcfg
