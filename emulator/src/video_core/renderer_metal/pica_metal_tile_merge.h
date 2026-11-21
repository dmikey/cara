// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Cara {

struct PicaTileInterval {
    std::uint32_t begin, end;
};

struct PicaGuestTilePatch {
    // Exact guest bytes relative to the original layout and the matching
    // Metal rectangle in Cara's inverted host row convention.
    PicaTileInterval bytes;
    std::uint32_t x, y, width, height;
};

inline std::optional<std::vector<PicaGuestTilePatch>> PlanPicaGuestTilePatches(
    std::uint32_t width, std::uint32_t height, std::uint32_t bytes,
    std::span<const PicaTileInterval> native) {
    constexpr std::uint32_t tile = 8 * 8 * 4;
    constexpr std::size_t max_ranges = 64;
    if (!width || !height || (width & 7) || (height & 7) || !bytes || bytes % tile ||
        std::uint64_t{width / 8} * (height / 8) != bytes / tile || native.empty() ||
        native.size() > max_ranges)
        return std::nullopt;

    std::vector<PicaGuestTilePatch> patches;
    const auto tiles_in_row = width / 8;
    const auto append_gap = [&](std::uint32_t begin, std::uint32_t end) {
        if (begin == end)
            return true;
        if (begin % tile || end % tile)
            return false;
        auto first = begin / tile;
        const auto last = end / tile;
        while (first < last) {
            const auto row = first / tiles_in_row;
            const auto x = first % tiles_in_row;
            const auto count = last - first;
            const auto all_rows = x == 0 ? count / tiles_in_row : 0;
            const auto cols = all_rows ? tiles_in_row
                             : count < tiles_in_row - x ? count : tiles_in_row - x;
            const auto rows = all_rows ? all_rows : 1;
            const auto next = first + cols * rows;
            patches.push_back({{first * tile, next * tile}, x * 8,
                               height - (row + rows) * 8, cols * 8, rows * 8});
            first = next;
        }
        return true;
    };

    std::uint32_t cursor = 0;
    for (const auto& range : native) {
        if (range.begin < cursor || range.end <= range.begin || range.end > bytes ||
            !append_gap(cursor, range.begin))
            return std::nullopt;
        cursor = range.end;
    }
    if (!append_gap(cursor, bytes) || patches.empty())
        return std::nullopt;
    return patches;
}

} // namespace Cara
