// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gamepolicy {
using Opps = std::array<std::vector<int>, 3>; // Gold/Prime kHz, GPU Hz (descending).
struct Bounds { int floor = -1; int ceiling = -1; };
struct Band { double temperature; std::array<Bounds, 3> bounds; };
struct PerfHint { int opcode; int value; };
struct BindCore { std::string name; uint64_t mask; };
struct ThreadScheduler { std::string name; int priority; bool enabled; };
struct Mode {
    int thermalCase = 0;
    std::vector<Band> bands;
    std::vector<PerfHint> perfHints{};
    std::vector<BindCore> bindCore{};
    std::vector<ThreadScheduler> schedulers{};
    int delayMs = 0;
};
struct Alias { std::string profile; bool threadPlacement = true; };
struct Overlay {
    std::map<int, std::string> modes;
    std::map<std::string, Alias> aliases;
    std::array<int, 3> ceilings{};
    std::array<std::string, 3> oppPaths;
    std::string sensor;
    double hysteresis = 1;
    int sampleMs = 1000;
};
struct Selection { const Mode* mode; std::string profile; bool threadPlacement; };
struct Decision { int thermalCase = 0; int band = -1; std::vector<int> resources; };
class Policy {
  public:
    Overlay overlay;
    std::map<std::string, std::map<std::string, Mode>> games;
    std::vector<std::string> diagnostics;
    std::string version;
    bool load(const std::string& stock, const std::string& overrides, std::string* error);
    // Resolve every index once, before any session can request a resource.
    void resolve(const Opps& actual);
    Selection select(const std::string& package, int mode) const;
    bool aliasesValid(std::string* error) const;
    static size_t band(const Mode& mode, double temperature, int previous, double hysteresis);
    static std::vector<int> resources(const Band& band);
    static std::vector<int> resources(const Mode& mode, int band);
    static uint64_t mask(const std::string& text);
    static Decision evaluate(const Mode& mode, std::optional<double> temperature,
                             int64_t sampleAgeMs, int previous, double hysteresis);
  private:
    Opps mDeclared;
    bool mResolved = false;
};
} // namespace gamepolicy
