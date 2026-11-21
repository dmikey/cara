// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>

#include "video_core/renderer_metal/pica_transfer_plan.h"

namespace Cara::MetalTransfer {

struct AliasLayout {
    GuestInterval bytes;
    std::uint32_t width;
    std::uint32_t height;
    constexpr bool operator==(const AliasLayout& other) const {
        return bytes.begin == other.bytes.begin && bytes.end == other.bytes.end &&
               width == other.width && height == other.height;
    }
};

struct MixedAliasPlan {
    AliasLayout rgb;
    AliasLayout rgba;
    AliasLayout tiled;
    std::uint32_t split;
    bool operator==(const MixedAliasPlan&) const = default;
};

inline constexpr std::optional<MixedAliasPlan> PlanMixedAlias(
    AliasLayout rgb, AliasLayout rgba, AliasLayout tiled, std::uint32_t split) {
    const auto exact_size = [](AliasLayout layout, std::uint32_t bpp) {
        // Bound dimensions before multiplying and keep all shader byte indices in u32.
        return layout.width && layout.height && layout.width <= 16384 && layout.height <= 16384 &&
               layout.bytes.begin && layout.bytes.begin < layout.bytes.end &&
               layout.bytes.Size() <= 16 * 1024 * 1024 &&
               std::uint64_t{layout.width} * layout.height * bpp == layout.bytes.Size();
    };
    if (!exact_size(rgb, 3) || !exact_size(rgba, 4) || !exact_size(tiled, 4) ||
        ((tiled.width | tiled.height) & 7u) ||
        !rgb.bytes.Contains({tiled.bytes.begin, split}) ||
        !rgba.bytes.Contains({split, tiled.bytes.end})) {
        return std::nullopt;
    }
    return MixedAliasPlan{rgb, rgba, tiled, split};
}

} // namespace Cara::MetalTransfer
