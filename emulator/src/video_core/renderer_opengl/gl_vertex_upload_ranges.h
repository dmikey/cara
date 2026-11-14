// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OpenGL {

// Packed vertex inputs have at most one source range per shader input register.
// Keep that bounded scratch work off the heap; callers retain all address and
// transfer-size checks and fall back if the capacity is ever exceeded.
template <std::size_t Capacity>
class BoundedVertexUploadRanges {
public:
    struct Range {
        std::uint64_t begin, end;
        std::size_t destination;
    };

    bool Add(std::uint64_t begin, std::uint64_t end) {
        if (count == Capacity || end <= begin) return false;
        ranges[count++] = {begin, end, 0};
        return true;
    }

    void SortAndMerge() {
        std::sort(begin(), end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
        std::size_t merged = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (merged && ranges[i].begin <= ranges[merged - 1].end) {
                ranges[merged - 1].end = std::max(ranges[merged - 1].end, ranges[i].end);
            } else {
                ranges[merged++] = ranges[i];
            }
        }
        count = merged;
    }

    Range* begin() { return ranges.data(); }
    Range* end() { return ranges.data() + count; }
    const Range* begin() const { return ranges.data(); }
    const Range* end() const { return ranges.data() + count; }
    std::size_t Size() const { return count; }

    // Call only when the prefix and every payload range will be overwritten.
    // This leaves the same zero-filled alignment/tail bytes as clearing the
    // whole allocation, without writing the payload twice.
    bool ClearPadding(void* destination, std::size_t bytes, std::size_t prefix) const {
        if (prefix > bytes) return false;
        auto* data = static_cast<std::uint8_t*>(destination);
        std::size_t cursor = prefix;
        for (const auto& range : *this) {
            if (range.destination < cursor || range.destination > bytes ||
                range.end < range.begin || range.end-range.begin > bytes-range.destination)
                return false;
            std::memset(data+cursor, 0, range.destination-cursor);
            cursor = range.destination + static_cast<std::size_t>(range.end-range.begin);
        }
        std::memset(data+cursor, 0, bytes-cursor);
        return true;
    }

private:
    static_assert(Capacity > 0);
    std::array<Range, Capacity> ranges;
    std::size_t count = 0;
};

} // namespace OpenGL
