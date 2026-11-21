// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Cara {

inline constexpr std::string_view RuntimeMetalUserClip = "#define CARA_METAL_USER_CLIP 1\n";

enum class RuntimeMetalVertexMode { Packed, DecodedIndexed, StageIn };

inline constexpr unsigned RuntimeMetalUnusedSlot = std::numeric_limits<unsigned>::max();

struct RuntimeMetalShaderPair {
    std::string vertex, fragment;
    std::array<unsigned, 3> units = {3, 3, 3};
    // LUT texture, LUT sampler, material texture, material sampler, common, lighting.
    std::array<unsigned, 6> slots{};
    unsigned secondary_texture = RuntimeMetalUnusedSlot;
    unsigned secondary_sampler = RuntimeMetalUnusedSlot;
    unsigned tertiary_texture = RuntimeMetalUnusedSlot;
    unsigned tertiary_sampler = RuntimeMetalUnusedSlot;
};

// Translate the full matched PICA stages into Metal Shading Language. The
// caller owns the Metal device and validates the compiled pipeline bindings.
std::optional<RuntimeMetalShaderPair> TranslateRuntimeMetalShaders(
    std::string_view vs, std::string_view fs, RuntimeMetalVertexMode mode,
    bool raw_indices, bool raw_index_constants, std::string& error);

} // namespace Cara
