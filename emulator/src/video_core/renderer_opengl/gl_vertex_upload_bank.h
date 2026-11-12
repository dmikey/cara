// Cara bounded vertex upload bank layout; SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <tuple>
#include <vector>

namespace OpenGL::IOSVertexUploadBank {
constexpr std::size_t FormatCount = 5;
constexpr std::size_t HeightClasses = 14;
constexpr std::size_t ClassCount = FormatCount * HeightClasses;
constexpr std::size_t LitPairFormat = 4;

constexpr std::size_t ClassIndex(std::size_t format, std::size_t height_class) {
    return format * HeightClasses + height_class;
}
constexpr std::size_t ReclaimIndex(std::size_t order) {
    return (order % FormatCount) * HeightClasses + order / FormatCount;
}
constexpr std::size_t AllocationBytes(std::size_t size_class, std::size_t width) {
    const auto format = size_class / HeightClasses;
    // RGBA32F/UI source, optional R16UI/EBO, or RG16UI + EBO.
    const std::size_t bytes_per_texel = 16 + (format == LitPairFormat ? 6 : format ? 2 : 0);
    return (std::size_t(1) << (size_class % HeightClasses)) * width * bytes_per_texel;
}

template <typename Entry>
struct Bank {
    std::array<std::vector<Entry>, ClassCount> sizes;
    std::array<std::size_t, ClassCount> used{};
    static_assert(std::tuple_size_v<decltype(sizes)> == std::tuple_size_v<decltype(used)>);
    std::size_t bytes = 0;
    std::size_t textures = 0;
};

template <typename Entry>
void Reclaim(Bank<Entry>& bank, std::size_t required_bytes, std::size_t budget,
             std::size_t maximum_entries, std::size_t width) {
    for (std::size_t order = ClassCount; order-- > 0 &&
         (bank.bytes + required_bytes > budget || bank.textures >= maximum_entries);) {
        const auto index = ReclaimIndex(order);
        auto& unused = bank.sizes[index];
        const auto bytes = AllocationBytes(index, width);
        while (unused.size() > bank.used[index] &&
               (bank.bytes + required_bytes > budget || bank.textures >= maximum_entries)) {
            assert(bank.bytes >= bytes && bank.textures > 0);
            unused.pop_back();
            bank.bytes -= bytes;
            --bank.textures;
        }
    }
}
} // namespace OpenGL::IOSVertexUploadBank
