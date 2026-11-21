// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <string_view>
#include "pica_metal_draw.h"

namespace Cara {

// Internal marker on prepared pipeline selections and encoded draws. Public
// catalog keys continue to identify the original packed shader ABI.
inline constexpr auto MetalRawIndexMode = static_cast<PicaMetalVertexMode>(3);
inline constexpr std::size_t MetalRawIndexRow = 256;
inline constexpr std::size_t MetalRawIndexHeader = 2;

inline bool UseMetalRawIndices() {
    static const bool enabled = [] {
        const char* option = std::getenv("CARA_METAL_RAW_INDICES");
        return option && std::string_view(option) == "1";
    }();
    return enabled;
}

// The raw-index constants shader reads metadata from the existing 256-byte
// layout uniform. Register zero still owns the low three element-count bits.
inline bool UseMetalRawIndexConstants() {
    static const bool enabled = [] {
        const char* option = std::getenv("CARA_METAL_RAW_INDEX_CONSTANTS");
        return option && std::string_view(option) == "1";
    }();
    return enabled;
}

} // namespace Cara
