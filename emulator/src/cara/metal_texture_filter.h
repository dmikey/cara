// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Cara {
enum class MetalTextureFilter { Game, Nearest, Linear };

constexpr MetalTextureFilter ParseMetalTextureFilter(int value) {
    return value == 1 ? MetalTextureFilter::Nearest :
           value == 2 ? MetalTextureFilter::Linear : MetalTextureFilter::Game;
}
} // namespace Cara
