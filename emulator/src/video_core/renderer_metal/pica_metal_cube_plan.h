// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "video_core/renderer_metal/pica_transfer_plan.h"

namespace Cara::MetalTransfer {

struct CubeRegisters {
    // Face order is PICA and Metal +X, -X, +Y, -Y, +Z, -Z.
    std::array<std::uint32_t, 6> addresses;
    std::uint32_t width, height, bytes_per_tile;
    std::uint32_t min_level, max_level;
    std::int32_t lod_bias;
};

struct CubeFacePlan {
    std::array<GuestInterval, 8> mips{};
    GuestInterval bytes{};
};

struct CubePlan {
    std::array<CubeFacePlan, 6> faces{};
    std::uint32_t width = 0, levels = 0;
};

inline constexpr std::optional<CubePlan> PlanCube(
    const CubeRegisters& regs, GuestInterval color, GuestInterval depth,
    std::optional<GuestInterval> other_material = std::nullopt) {
    if (regs.width < 8 || regs.width > 1024 || regs.height != regs.width ||
        (regs.width & (regs.width - 1)) || regs.max_level >= 8 ||
        regs.min_level > regs.max_level || regs.lod_bias ||
        (regs.width >> regs.max_level) < 8 || !regs.bytes_per_tile ||
        regs.bytes_per_tile > 8 * 8 * 4)
        return std::nullopt;

    CubePlan plan{};
    plan.width = regs.width;
    plan.levels = regs.max_level + 1;
    const auto compatible_reads = [](GuestInterval a, GuestInterval b) {
        return !a.Overlaps(b) || (a.begin == b.begin && a.end == b.end);
    };
    for (std::size_t face = 0; face < plan.faces.size(); ++face) {
        auto& out = plan.faces[face];
        auto cursor = regs.addresses[face];
        if (!cursor)
            return std::nullopt;
        for (std::uint32_t level = 0; level < plan.levels; ++level) {
            const auto tiles = (plan.width >> level) / 8;
            const auto bytes = std::uint64_t{tiles} * tiles * regs.bytes_per_tile;
            const auto range = AlignedGuestInterval(cursor, bytes);
            if (!range)
                return std::nullopt;
            out.mips[level] = *range;
            cursor = range->end;
        }
        out.bytes = {regs.addresses[face], cursor};
        if (out.bytes.Overlaps(color) || out.bytes.Overlaps(depth) ||
            (other_material && !compatible_reads(*other_material, out.bytes)))
            return std::nullopt;
        for (std::size_t previous = 0; previous < face; ++previous) {
            if (!compatible_reads(plan.faces[previous].bytes, out.bytes))
                return std::nullopt;
        }
    }
    return plan;
}

} // namespace Cara::MetalTransfer
