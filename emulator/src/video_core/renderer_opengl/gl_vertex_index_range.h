// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace OpenGL {
struct VertexIndexRange {
    std::uint32_t minimum, maximum;
};

// Read live indices on every call. Narrow reductions avoid widening every lane
// to u32; only the final range is widened. This is not a memory-content cache.
template <typename Index>
VertexIndexRange AnalyzeVertexIndexRange(const Index* indices, std::uint32_t count) {
    static_assert(std::is_same_v<Index, std::uint8_t> || std::is_same_v<Index, std::uint16_t>);
    // Preserve AnalyzeVertexArray's legacy empty-index result exactly.
    if (!count) return {0xFFFF, 0};
    Index minimum=std::numeric_limits<Index>::max(),maximum=0;
    for (std::uint32_t i=0;i<count;++i) {
        minimum=std::min(minimum,indices[i]);
        maximum=std::max(maximum,indices[i]);
    }
    return {minimum,maximum};
}
} // namespace OpenGL
