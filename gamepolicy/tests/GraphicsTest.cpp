// SPDX-License-Identifier: Apache-2.0
#include "Tests.h"
#include "Graphics.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gamepolicy;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Backend final : public GraphicsBackend {
  public:
    bool present = true, accepts = true;
    int probes = 0;
    std::map<std::string, std::string> files;
    std::vector<std::string> calls;
    bool supported() override { ++probes; return present && accepts; }
    bool apply(const std::string& package, const std::string& bytes) override {
        calls.push_back(package);
        if (!supported()) return false;
        if (bytes.empty()) files.erase(package);
        else files[package] = bytes;
        return true;
    }
};
void same(const GraphicsOptions& a, const GraphicsOptions& b) {
    require(a.textureFiltering == b.textureFiltering && a.anisotropicFiltering == b.anisotropicFiltering &&
            a.mipmapLodBias == b.mipmapLodBias && a.msaaLimit == b.msaaLimit, "Graphics choices differ");
}
} // namespace

void graphicsTests(const char* tempDir) {
    const auto dir = std::filesystem::path(tempDir) / "graphics";
    const std::string package = "io.github.miner7222.testgame";
    const GraphicsOptions full{"Fastest", "16.0", "-4.0", "4"};
    require(validGraphicsOptions({}), "Default graphics rejected");
    require(validGraphicsOptions(full), "Graphics choices rejected");
    for (const auto& o : {GraphicsOptions{"Quality", "", "", ""}, {"", "3.0", "", ""},
                          {"", "", "0.0", ""}, {"", "", "-3.0", ""}, {"", "", "", "8"}})
        require(!validGraphicsOptions(o), "Invalid graphics accepted");
    require(encodeGraphicsProfile({}).empty(), "Defaults must remove the profile");
    std::ofstream(dir.string() + "-full.pb", std::ios::binary) << encodeGraphicsProfile(full);
    const GraphicsOptions balanced{"Balanced", "", "", ""};
    std::ofstream(dir.string() + "-balanced.pb", std::ios::binary) << encodeGraphicsProfile(balanced);
    // Emit the entire approved choice domain for independent descriptor verification.
    int n = 0;
    for (const auto& value : {"Balanced", "Fastest"})
        std::ofstream(dir.string() + "-choice-" + std::to_string(n++) + ".pb", std::ios::binary)
            << encodeGraphicsProfile({value, "", "", ""});
    for (const auto& value : {"1.0", "2.0", "4.0", "8.0", "16.0"})
        std::ofstream(dir.string() + "-choice-" + std::to_string(n++) + ".pb", std::ios::binary)
            << encodeGraphicsProfile({"", value, "", ""});
    for (const auto& value : {"-4.0", "-2.0", "-1.0", "1.0", "2.0", "4.0"})
        std::ofstream(dir.string() + "-choice-" + std::to_string(n++) + ".pb", std::ios::binary)
            << encodeGraphicsProfile({"", "", value, ""});
    for (const auto& value : {"1", "2", "4"})
        std::ofstream(dir.string() + "-choice-" + std::to_string(n++) + ".pb", std::ios::binary)
            << encodeGraphicsProfile({"", "", "", value});

    Backend backend;
    GraphicsStore store(dir.string());
    require(store.load(), "Missing graphics state should load");
    require(store.set(0, package, full, backend), "Graphics set failed");
    same(store.get(0, package), full);
    require(backend.files.at(package) == encodeGraphicsProfile(full), "Graphics upload mismatch");
    require(store.get(10, package).isDefault(), "Graphics users leaked");
    require(!store.set(-1, package, full, backend), "Negative graphics user accepted");
    require(!store.set(0, "../bad", full, backend), "Unsafe graphics package accepted");
    require(!store.set(0, package, {"Raw", "", "", ""}, backend), "Raw graphics value accepted");
    GraphicsStore loaded(dir.string());
    require(loaded.load(), "Graphics reload failed"); same(loaded.get(0, package), full);
    backend.files[package] = "different";
    require(loaded.reapply(backend) && backend.files.at(package) == encodeGraphicsProfile(full), "Changed profile not repaired");
    backend.files.clear();
    require(loaded.reapply(backend) && backend.files.count(package), "Missing profile not repaired");
    require(loaded.set(10, package, balanced, backend), "Second user graphics set failed");
    require(backend.files.at(package) == encodeGraphicsProfile(full), "Background user replaced foreground graphics");
    same(loaded.get(0, package), full);
    require(loaded.activate(10, backend) && backend.files.at(package) == encodeGraphicsProfile(balanced), "Foreground switch not applied");
    const size_t foregroundCalls = backend.calls.size();
    same(loaded.get(0, package), full);
    require(loaded.set(0, package, {}, backend), "Background default not stored");
    require(backend.calls.size() == foregroundCalls && backend.files.at(package) == encodeGraphicsProfile(balanced),
            "Background read or reset touched foreground graphics");
    require(loaded.set(0, package, full, backend) && backend.calls.size() == foregroundCalls,
            "Background set touched foreground graphics");
    GraphicsStore foregroundReload(dir.string());
    require(foregroundReload.load() && foregroundReload.reapply(backend) &&
            backend.files.at(package) == encodeGraphicsProfile(balanced), "Background set changed persisted foreground user");
    require(loaded.activate(0, backend) && backend.files.at(package) == encodeGraphicsProfile(full), "User graphics not restored");
    require(loaded.set(0, package, {}, backend) && !backend.files.count(package), "Default profile not deleted");
    GraphicsStore cleared(dir.string());
    require(cleared.load() && cleared.get(0, package).isDefault(), "Default deletion not stored");
    same(cleared.get(10, package), balanced);
    backend.files[package] = "stale";
    require(cleared.reapply(backend) && !backend.files.count(package), "Default deletion not reapplied");
    backend.present = false;
    require(!backend.supported(), "Missing service reported supported");
    require(!cleared.set(0, package, full, backend), "Missing service accepted upload");
    GraphicsStore retry(dir.string());
    require(retry.load(), "Desired graphics lost on unavailable service"); same(retry.get(0, package), full);
    backend.present = true; backend.accepts = false;
    require(!backend.supported() && !retry.reapply(backend), "Rejected setter reported supported");
    backend.accepts = true;
    require(retry.reapply(backend) && backend.files.at(package) == encodeGraphicsProfile(full), "Graphics retry failed");
    backend.present = false;
    require(!retry.set(0, package, {}, backend), "Unavailable deletion reported accepted");
    GraphicsStore pendingDelete(dir.string());
    require(pendingDelete.load(), "Pending deletion lost");
    backend.present = true;
    require(pendingDelete.reapply(backend) && !backend.files.count(package), "Pending deletion not retried");

    Backend lateBackend;
    lateBackend.present = false;
    GraphicsStore late((dir / "late-service").string());
    require(!late.set(0, package, full, lateBackend) && late.pending(), "Offline upload not pending");
    require(!late.retry(100, lateBackend) && late.retryAt() == 1100, "Initial retry backoff incorrect");
    const int probes = lateBackend.probes;
    const size_t lateCalls = lateBackend.calls.size();
    require(late.set(10, package, balanced, lateBackend) && lateBackend.calls.size() == lateCalls,
            "Offline background set touched HAL");
    lateBackend.present = true;
    require(!late.retry(1099, lateBackend) && lateBackend.probes == probes, "Retry ignored backoff");
    require(late.retry(1100, lateBackend) && !late.pending() &&
            lateBackend.files.at(package) == encodeGraphicsProfile(full), "Service appearance did not flush foreground upload");
    lateBackend.present = false;
    require(!late.set(0, package, {}, lateBackend) && !late.retry(2000, lateBackend), "Offline deletion not pending");
    lateBackend.present = true;
    require(late.retry(3000, lateBackend) && !late.pending() && !lateBackend.files.count(package),
            "Service appearance did not flush deletion");
    require(late.activate(10, lateBackend) && lateBackend.files.at(package) == encodeGraphicsProfile(balanced),
            "Deferred background choices not applied on foreground switch");
    std::cout << "GRAPHICS_FOREGROUND_RETRY_OK background-read-set-reset persisted-user late-service upload-delete backoff\n";
    const auto badDir = dir / "not-a-directory";
    std::ofstream(badDir) << "file";
    GraphicsStore unwritable(badDir.string());
    const size_t calls = backend.calls.size();
    require(!unwritable.set(0, package, full, backend) && backend.calls.size() == calls &&
            unwritable.get(0, package).isDefault(), "Failed storage must not mutate state or HAL");
    std::ofstream(dir / "graphics.xml") << "<graphics user='0'><options package='../bad'/></graphics>";
    GraphicsStore malformed(dir.string());
    require(!malformed.load(), "Malformed graphics state accepted");
    std::cout << "GRAPHICS_TESTS_OK encoding storage defaults reapply users failure-retry\n";
}
