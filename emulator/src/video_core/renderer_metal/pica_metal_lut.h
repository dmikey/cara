// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <memory>
#include "common/common_types.h"
#include "video_core/regs_lighting.h"

namespace Pica {
struct State;
}

namespace Cara {

// Lighting, fog, and procedural textures share the RG32F sampler ABI.
struct PicaMetalLutData {
    static constexpr unsigned width = 1024;
    static constexpr unsigned entries_per_sampler = 256;
    static constexpr unsigned fog_entries = 128;
    static constexpr unsigned fog_offset =
        Pica::LightingRegs::NumLightingSampler * entries_per_sampler;
    static constexpr unsigned rows = (fog_offset + fog_entries + width - 1) / width;
    static constexpr unsigned noise_offset = fog_offset + fog_entries;
    static constexpr unsigned color_map_offset = noise_offset + 128;
    static constexpr unsigned alpha_map_offset = color_map_offset + 128;
    // Each RGBA entry uses two adjacent RG texels.
    static constexpr unsigned color_offset = (alpha_map_offset + 128) / 2;
    static constexpr unsigned diff_offset = color_offset + 256;
    static constexpr unsigned procedural_rows = (2 * (diff_offset + 256) + width - 1) / width;
    u64 generation = 0;
    std::array<std::array<float, 2>, width * procedural_rows> texels{};
};

std::shared_ptr<const PicaMetalLutData> SnapshotPicaMetalLuts(const Pica::State& state,
                                                            u64 generation, bool procedural);

} // namespace Cara
