// SPDX-License-Identifier: Apache-2.0
#include "QspmClient.h"
#include <aidl/vendor/qti/qspmhal/IQspmhal.h>
#include <android/binder_manager.h>
#include <android-base/logging.h>
#include <cutils/ashmem.h>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace gamepolicy {
namespace {
namespace hal = aidl::vendor::qti::qspmhal;
static_assert(sizeof(hal::ParcelableMemory) == 0x30);
static_assert(sizeof(hal::ProfData) == 0x88);
class QspmClient final : public GraphicsBackend {
  public:
    bool supported() override {
        // Probe only a reserved broker filename; never remove a game's profile as a capability check.
        return send(".gamepolicy-graphics-probe", {});
    }
    bool apply(const std::string& package, const std::string& bytes) override {
        const bool ok = send(package + "-gpu", bytes);
        LOG(INFO) << "Graphics " << (bytes.empty() ? "remove" : "store") << " package=" << package
                  << " bytes=" << bytes.size() << " accepted=" << ok;
        return ok;
    }
  private:
    bool send(const std::string& name, const std::string& bytes) {
        const std::string service = std::string(hal::IQspmhal::descriptor) + "/default";
        ndk::SpAIBinder binder(AServiceManager_checkService(service.c_str()));
        if (!binder.get()) return false;
        auto client = hal::IQspmhal::fromBinder(binder);
        if (!client) return false;
        int32_t version = 0;
        std::string hash;
        if (!client->getInterfaceVersion(&version).isOk() || version != 1 ||
            !client->getInterfaceHash(&hash).isOk() || hash != "d43cd1309bd4375f541a73ac5fe6b77ad29ee729") return false;
        hal::ParcelableMemory memory;
        const size_t size = bytes.empty() ? 1 : bytes.size();
        const int fd = ashmem_create_region("gamepolicy-graphics", size);
        if (fd < 0) return false;
        memory.file.fd = ndk::ScopedFileDescriptor(fd);
        void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) return false;
        if (!bytes.empty()) std::memcpy(mapped, bytes.data(), bytes.size());
        if (munmap(mapped, size) != 0 || ashmem_set_prot_region(fd, PROT_READ) != 0) return false;
        memory.file.length = size; memory.file.prot = PROT_READ; memory.file.offset = 0;
        memory.name = "gamepolicy-graphics";
        const auto status = client->setAppProfile(memory, bytes.size(), name,
            bytes.empty() ? hal::Command::REMOVE : hal::Command::STORE);
        if (!status.isOk()) LOG(WARNING) << "Graphics setter rejected: " << status.getDescription();
        return status.isOk();
    }
};
} // namespace
std::unique_ptr<GraphicsBackend> makeQspmClient() { return std::make_unique<QspmClient>(); }
} // namespace gamepolicy
