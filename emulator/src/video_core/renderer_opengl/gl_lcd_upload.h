// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace OpenGL::LcdUpload {
// GPU::Regs::PixelFormat register values. The renderer asserts this mapping.
// Packed 16-bit words retain their bit layout for the matching GL packed type.
inline bool Stage(const std::uint8_t* source, std::size_t source_size, unsigned format,
                  std::size_t width, std::size_t height, std::size_t stride,
                  std::vector<std::uint8_t>& output) {
    if (format > 4 || !source || width == 0 || height == 0) {
        return false;
    }
    const std::size_t bpp = format == 0 ? 4 : format == 1 ? 3 : 2;
    if (width > std::numeric_limits<std::size_t>::max() / bpp) {
        return false;
    }
    const std::size_t row_size = width * bpp;
    if (stride < row_size || height > std::numeric_limits<std::size_t>::max() / row_size ||
        (height - 1) > (std::numeric_limits<std::size_t>::max() - row_size) / stride ||
        source_size < (height - 1) * stride + row_size) {
        return false;
    }
    output.resize(row_size * height);
    for (std::size_t y = 0; y < height; ++y) {
        const auto* row = source + y * stride;
        auto* destination = output.data() + y * row_size;
        for (std::size_t x = 0; x < width; ++x) {
            const auto* pixel = row + x * bpp;
            auto* packed = destination + x * bpp;
            if (bpp == 2) {
                // Guest memory is LE; GL_UNSIGNED_SHORT_* consumes a native word.
                const std::uint16_t word = pixel[0] | (std::uint16_t(pixel[1]) << 8);
                std::memcpy(packed, &word, sizeof(word));
            } else {
                // Guest RGBA8 is ABGR in memory; RGB8 is BGR.
                for (std::size_t c = 0; c < bpp; ++c) {
                    packed[c] = pixel[bpp - 1 - c];
                }
            }
        }
    }
    return true;
}
} // namespace OpenGL::LcdUpload
