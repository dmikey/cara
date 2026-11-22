// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace Cara {

enum class RuntimeTevMode { Off, Adaptive, Burst, Always };

inline RuntimeTevMode ParseRuntimeTevMode(const char* value) {
    const std::string_view option = value ? value : "";
    if (option == "0") return RuntimeTevMode::Off;
    if (option == "1") return RuntimeTevMode::Always;
    if (option == "burst") return RuntimeTevMode::Burst;
    return RuntimeTevMode::Adaptive;
}

// Source identities are interned by this pipeline owner. A lone fixed-state
// change or repeated use of one fragment is not a burst of shader programs.
class PicaMetalTevAdmission {
public:
    bool Observe(const char* vertex, const char* fragment, bool dual, std::uint64_t refresh) {
        auto family = std::find_if(families.begin(), families.end(), [&](const auto& candidate) {
            return candidate.vertex == vertex && candidate.dual == dual;
        });
        if (family == families.end()) {
            family = std::min_element(families.begin(), families.end(), [](const auto& a, const auto& b) {
                return a.last < b.last;
            });
            *family = {};
            family->vertex = vertex;
            family->dual = dual;
            family->start = refresh;
        }
        if (refresh < family->last || refresh - family->last > Window ||
            (family->count < Minimum && refresh - family->start > Window)) {
            family->count = 0;
            family->start = refresh;
        }
        family->last = refresh;
        if (family->count == Minimum)
            return true;
        const auto end = family->fragments.begin() + family->count;
        if (std::find(family->fragments.begin(), end, fragment) == end)
            family->fragments[family->count++] = fragment;
        return family->count == Minimum;
    }

    // Count distinct eligible fragment sources across packed vertex programs.
    // Each texture layout has its own generic shader. Expire at the next refresh.
    bool ObserveBurst(const char* fragment, bool dual, std::uint64_t refresh) {
        auto& burst = bursts[dual];
        if (burst.refresh != refresh) {
            burst.refresh = refresh;
            burst.count = 0;
        }
        if (burst.count < burst.fragments.size()) {
            const auto end = burst.fragments.begin() + burst.count;
            if (std::find(burst.fragments.begin(), end, fragment) == end)
                burst.fragments[burst.count++] = fragment;
        }
        return burst.count == burst.fragments.size();
    }

private:
    struct Burst {
        std::uint64_t refresh{};
        std::array<const char*, 3> fragments{};
        unsigned count{};
    };
    std::array<Burst, 2> bursts{};
    static constexpr unsigned Minimum = 6;
    static constexpr unsigned Window = 32;
    struct Family {
        const char* vertex = nullptr;
        std::array<const char*, Minimum> fragments{};
        unsigned count{};
        bool dual{};
        std::uint64_t start{}, last{};
    };
    std::array<Family, 32> families{};
};

} // namespace Cara
