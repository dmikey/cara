// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>

namespace Cara {

struct LCDPresentationRect {
    int x, y, width, height;
};

// Both rectangles are in OpenGL's lower-left coordinates within the 400×480
// composited game framebuffer. The lower screen has 40 pixels of side padding.
constexpr std::array LCDPresentationRects{
    LCDPresentationRect{0, 240, 400, 240},
    LCDPresentationRect{40, 0, 320, 240},
};

constexpr int LCDUpscaleFactor(long preference) {
    return preference == 2 || preference == 4 ? static_cast<int>(preference) : 1;
}

constexpr bool FinalScaleIsIntegral(int width, int height, int fit_width, int fit_height) {
    return fit_width >= width && fit_height >= height && fit_width % width == 0 &&
           fit_height % height == 0 && fit_width / width == fit_height / height;
}

} // namespace Cara
