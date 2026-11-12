// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/common_types.h"

namespace OpenGL::ColorReadback {
// The source renderbuffers already quantize these channels to 4/5/6/1 bits.
// Their normalized 8-bit readback expands those levels; shifting recovers the
// exact original integer without introducing a second rounding step.
constexpr u16 PackRGB565(const u8* rgba) {
    return ((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3);
}
constexpr u16 PackRGB5A1(const u8* rgba) {
    return ((rgba[0] >> 3) << 11) | ((rgba[1] >> 3) << 6) |
           ((rgba[2] >> 3) << 1) | (rgba[3] >> 7);
}
constexpr u16 PackRGBA4(const u8* rgba) {
    return ((rgba[0] >> 4) << 12) | ((rgba[1] >> 4) << 8) |
           ((rgba[2] >> 4) << 4) | (rgba[3] >> 4);
}
} // namespace OpenGL::ColorReadback
