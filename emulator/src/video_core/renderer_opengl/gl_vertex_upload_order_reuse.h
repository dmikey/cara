// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace OpenGL::IOSVertexUploadOrderReuse {
struct Result {
    std::size_t selected = 0;
    std::size_t candidates = 0;
    std::size_t full_comparisons = 0;
    bool input_equal = false;
    bool index_equal = false;
};
inline bool Equal(const std::vector<std::uint8_t>& shadow, const void* bytes,
                  std::size_t size, std::size_t& full_comparisons, bool filter_edges = true) {
    if (!size || shadow.size() != size) return false;
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    if (filter_edges) {
        const auto edge = std::min<std::size_t>(16, size);
        if (std::memcmp(shadow.data(), data, edge) ||
            std::memcmp(shadow.data() + size - edge, data + size - edge, edge)) return false;
    }
    ++full_comparisons;
    return std::memcmp(shadow.data(), data, size) == 0;
}
// Read-only bounded search. Caller owns class/format partitioning and supplies
// eligibility (matching extent and live resources). Prefix [0, first) is never
// inspected or selected. A successful alternate requires BOTH exact payloads.
template <typename Entry, typename Eligible>
Result Find(const std::vector<Entry>& pool, std::size_t first, std::size_t lookahead,
            const void* input, std::size_t input_size, const void* index,
            std::size_t index_size, Eligible eligible) {
    Result result; result.selected = first;
    if (first >= pool.size()) return result;
    const auto compare = [&](std::size_t position, bool both) {
        if (!eligible(pool[position])) return false;
        ++result.candidates;
        // The ordinal entry is usually an exact hit. Compare it directly as the
        // original upload-reuse path did; edge filters only help the search of
        // alternates, where mismatches are expected.
        const bool a = Equal(pool[position].input_shadow, input, input_size,
                             result.full_comparisons, both);
        // The initial entry needs both booleans even if input differs, because
        // unchanged indices must still skip their original upload on a miss.
        const bool b = (!both || a) && Equal(pool[position].index_shadow, index, index_size,
                                           result.full_comparisons, both);
        if (position == first) { result.input_equal = a; result.index_equal = b; }
        if (!a || !b) return false;
        result.selected = position; result.input_equal = true; result.index_equal = true;
        return true;
    };
    if (compare(first, false)) return result;
    const auto count = std::min(lookahead, pool.size() - first - 1);
    for (std::size_t offset = 1; offset <= count; ++offset)
        if (compare(first + offset, true)) break;
    return result;
}
} // namespace OpenGL::IOSVertexUploadOrderReuse
