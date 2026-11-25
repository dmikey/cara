// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Common::ImageRows {
// GL_RGBA/GL_UNSIGNED_BYTE is already canonical RGBA. Reverse the rows,
// without interpreting the bytes of a pixel as BGRA or swapping channels.
inline bool FlipCanonicalRGBA(std::span<std::uint32_t> pixels, std::size_t width,
                              std::size_t height) {
    if (!width || !height || height > std::numeric_limits<std::size_t>::max()/width ||
        pixels.size() != width*height)
        return false;
    for (std::size_t y=0;y<height/2;++y) {
        auto first = pixels.begin()+y*width;
        std::swap_ranges(first, first+width, pixels.begin()+(height-1-y)*width);
    }
    return true;
}
}
