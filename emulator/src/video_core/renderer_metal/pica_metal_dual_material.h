// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <initializer_list>
#include <optional>

namespace Cara {

inline constexpr unsigned NoMaterialUnit = 3;
constexpr bool SupportedDualUnits(unsigned primary, unsigned secondary) {
    return (primary == 0 && secondary == 1) || (primary == 1 && secondary == 2);
}

struct DualSamplerRecipe {
    unsigned color_filter;
    unsigned wrap_s, wrap_t;
};

constexpr std::optional<DualSamplerRecipe> ResolveDualSampler(
    unsigned min, unsigned mag, unsigned s, unsigned t) {
    // Fixed explicit level clamp is exactly [0,0]. The caller has
    // established base level zero, a one-level complete image, zero bias,
    // no comparison/anisotropy, and normalized identity-swizzled RGBA8.
    // Requiring equal spatial filters removes the min/mag switch ambiguity.
    const bool nearest=min==0x2600 || min==0x2700 || min==0x2702;
    const bool linear=min==0x2601 || min==0x2701 || min==0x2703;
    if ((!nearest && !linear) || mag!=(nearest?0x2600u:0x2601u) ||
        (s!=0x2901 && s!=0x812f) || (t!=0x2901 && t!=0x812f))
        return std::nullopt;
    return DualSamplerRecipe{mag,s,t};
}

constexpr bool DistinctFragmentMaterialSlots(std::array<unsigned,3> textures,
                                             std::array<unsigned,3> samplers) {
    for (const auto bank : {textures,samplers}) {
        if (bank[0]>=16 || bank[1]>=16 || bank[2]>=16 ||
            bank[0]==bank[1] || bank[0]==bank[2] || bank[1]==bank[2])
            return false;
    }
    return true;
}

} // namespace Cara
