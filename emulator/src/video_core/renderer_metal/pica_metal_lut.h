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

// The packed shader ABI samples one RG32F atlas containing lighting and fog.
struct PicaMetalLutData {
    static constexpr unsigned width = 1024;
    static constexpr unsigned entries_per_sampler = 256;
    static constexpr unsigned fog_entries = 128;
    static constexpr unsigned fog_offset =
        Pica::LightingRegs::NumLightingSampler * entries_per_sampler;
    static constexpr unsigned rows = (fog_offset + fog_entries + width - 1) / width;
    u64 generation = 0;
    std::array<std::array<float, 2>, width * rows> texels{};
};

std::shared_ptr<const PicaMetalLutData> SnapshotPicaMetalLuts(const Pica::State& state,
                                                            u64 generation);

} // namespace Cara
