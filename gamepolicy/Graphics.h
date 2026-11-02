// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <cstdint>
#include <set>
#include <string>

namespace gamepolicy {
struct GraphicsOptions {
    std::string textureFiltering, anisotropicFiltering, mipmapLodBias, msaaLimit;
    bool isDefault() const;
};
bool validGraphicsOptions(const GraphicsOptions&);
std::string encodeGraphicsProfile(const GraphicsOptions&);

class GraphicsBackend {
  public:
    virtual ~GraphicsBackend() = default;
    virtual bool supported() = 0;
    // Empty bytes remove only this package's profile. Success means the setter accepted the call.
    virtual bool apply(const std::string& package, const std::string& bytes) = 0;
};

// The service serializes access. Only session changes select the foreground user's HAL files.
class GraphicsStore {
  public:
    explicit GraphicsStore(std::string dir = "/data/vendor/gamepolicy/graphics");
    bool load();
    GraphicsOptions get(int user, const std::string& package) const;
    bool set(int user, const std::string& package, const GraphicsOptions&, GraphicsBackend&);
    bool activate(int user, GraphicsBackend&);
    bool reapply(GraphicsBackend&);
    bool retry(int64_t nowMs, GraphicsBackend&);
    bool pending() const { return mPending; }
    int64_t retryAt() const { return mRetryAt; }
  private:
    using Key = std::pair<int, std::string>;
    bool save(int user, const std::map<Key, GraphicsOptions>& records, const std::set<std::string>& managed);
    std::string mDir;
    int mForegroundUser = 0;
    bool mPending = false, mForegroundDirty = false;
    int64_t mRetryAt = 0;
    int mRetryDelayMs = 1000;
    std::map<Key, GraphicsOptions> mRecords;
    // Retain deletion tombstones so an unavailable HAL cannot strand an old override.
    std::set<std::string> mManaged;
};
} // namespace gamepolicy
