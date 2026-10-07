// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>
#include "DiagTransport.h"

namespace mcfg {
class EfsTransport {
  public:
    virtual ~EfsTransport() = default;
    virtual std::optional<Bytes> read(const std::string& path) = 0;
    virtual void write(const std::string& path, const Bytes& bytes,
                       const std::function<void()>& checkpoint = {}) = 0;
    virtual void remove(const std::string& path, const std::function<void()>& checkpoint = {}) = 0;
    virtual void checkParent(const std::string& path) = 0;
};
class EfsClient final : public EfsTransport {
  public:
    EfsClient(PacketTransport& transport, std::set<std::string> allow, bool readOnly);
    std::optional<Bytes> read(const std::string& path) override;
    void write(const std::string& path, const Bytes& bytes,
               const std::function<void()>& checkpoint = {}) override;
    void remove(const std::string& path, const std::function<void()>& checkpoint = {}) override;
    void checkParent(const std::string& path) override;

  private:
    Bytes command(uint16_t command, const Bytes& body, size_t minimum);
    Bytes stat(const std::string& path);
    int open(const std::string& path, bool write);
    void close(int fd);
    void pathAllowed(const std::string& path) const;
    PacketTransport& transport_;
    std::set<std::string> allow_;
    bool readOnly_;
};
}  // namespace mcfg
