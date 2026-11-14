// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version
#pragma once

#include <algorithm>
#include <cstdint>

namespace OpenGL::SurfaceRemapLayout {
struct Span {
    std::uint64_t begin = 0, end = 0;
    explicit operator bool() const { return begin < end; }
};

// Choose a contiguous guest interval that is exactly one destination rectangle.
// Tiled views require whole 8x8 tiles; linear views require whole pixels. Never
// expand an ownership interval to a pixel/tile boundary.
inline Span RectangleSpan(std::uint64_t base, std::uint64_t row_bytes,
                          std::uint64_t unit_bytes, Span owned) {
    if (!row_bytes || !unit_bytes || row_bytes % unit_bytes || owned.begin < base ||
        owned.end <= owned.begin) return {};
    const auto begin = (owned.begin - base + unit_bytes - 1) / unit_bytes * unit_bytes;
    const auto end = (owned.end - base) / unit_bytes * unit_bytes;
    if (begin >= end) return {};
    const auto full_begin = (begin + row_bytes - 1) / row_bytes * row_bytes;
    const auto full_end = end / row_bytes * row_bytes;
    if (full_begin < full_end) return {base + full_begin, base + full_end};
    // At most two partial rows remain. Pick the larger; ties use the first.
    const auto first_end = std::min(end, (begin / row_bytes + 1) * row_bytes);
    if (end - std::max(begin, full_end) > first_end - begin)
        return {base + full_end, base + end};
    return {base + begin, base + first_end};
}
} // namespace OpenGL::SurfaceRemapLayout
