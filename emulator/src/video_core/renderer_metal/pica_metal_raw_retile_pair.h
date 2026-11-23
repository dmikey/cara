// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Cara::MetalTransfer {

struct RawRetilePairLayout {
    std::uint64_t begin{};
    std::uint32_t row_bytes{}, rows{}, split_bytes{};
    std::array<std::uint32_t, 2> source_offsets{};

    bool Valid() const {
        return rows && split_bytes && split_bytes < row_bytes &&
               ((row_bytes | split_bytes | source_offsets[0] | source_offsets[1]) & 3u) == 0;
    }

    bool Matches(std::size_t span, std::uint64_t first, std::uint64_t last,
                 std::uint64_t from) const {
        const std::uint64_t row = span / 2;
        if (row >= rows)
            return false;
        const unsigned source = span % 2;
        const std::uint64_t byte = row * row_bytes;
        const auto expected = begin + byte + (source ? split_bytes : 0);
        const auto size = source ? row_bytes - split_bytes : split_bytes;
        return first == expected && last == first + size &&
               from == source_offsets[source] + byte;
    }
};

} // namespace Cara::MetalTransfer
