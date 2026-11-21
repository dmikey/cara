// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include "video_core/renderer_metal/pica_transfer_plan.h"

namespace Cara::MetalLCD {

enum class Format : std::uint32_t { RGBA8, RGB8, RGB565, RGB5A1, RGBA4 };

struct Scanout {
    MetalTransfer::Rows rows;
    std::uint32_t width, height;
    Format format;
};

// LCD memory has a byte stride and its dimensions are in before-rotation
// coordinates. Ignore row padding; bound every byte sampled by the decoder.
constexpr std::optional<Scanout> PlanScanout(std::uint32_t address, std::uint32_t width,
                                             std::uint32_t height, std::uint32_t stride,
                                             std::uint32_t format) {
    if (!address || !width || !height || format > 4)
        return std::nullopt;

    const auto bpp = format == 0 ? 4u : format == 1 ? 3u : 2u;
    const std::uint64_t row = std::uint64_t{width} * bpp;
    if (row > stride)
        return std::nullopt;
    // Largest supported end is a representable exclusive PAddr.
    const std::uint64_t size = std::uint64_t{height - 1} * stride + row;
    if (size > std::numeric_limits<std::uint32_t>::max() - address)
        return std::nullopt;

    return Scanout{{{address, address + static_cast<std::uint32_t>(size)},
                    static_cast<std::uint32_t>(row), stride, height},
                   width, height, static_cast<Format>(format)};
}

} // namespace Cara::MetalLCD
