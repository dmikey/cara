// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace OpenGL::ScissorDamage {
// Window coordinates, unlike Common::Rectangle's left/top/right/bottom order.
struct Rect {
    std::int64_t left = 0, bottom = 0, right = 0, top = 0;
};
struct Result {
    Rect rect;
    bool eligible = false;
};

// Shared geometry only. The caller decides whether this intersection is used
// for host clipping or conservative ownership; no GL or cache state is changed.
inline Result IntersectInclude(Rect current, Rect shader_bounds, std::uint32_t mode,
                               std::uint32_t scale, bool shadow) {
    if (mode != 3 || shadow || !scale || current.left < 0 || current.bottom < 0 ||
        current.right < current.left || current.top < current.bottom ||
        current.right > (1LL << 23) || current.top > (1LL << 23))
        return {current, false};
    // The shader compares integer uniforms converted to binary32. Framebuffer
    // coordinates in this range have exact half-pixel centres and integer boundaries. Guard
    // malformed bounds and preserve the existing footprint rather than guessing.
    for (const auto coordinate :
         {shader_bounds.left, shader_bounds.bottom, shader_bounds.right, shader_bounds.top}) {
        if (coordinate < std::numeric_limits<std::int32_t>::min() ||
            coordinate > std::numeric_limits<std::int32_t>::max() || coordinate % scale)
            return {current, false};
    }
    for (const auto coordinate : {current.left, current.bottom, current.right, current.top}) {
        if (coordinate % scale)
            return {current, false};
    }
    Rect result{
        std::max(current.left, shader_bounds.left), std::max(current.bottom, shader_bounds.bottom),
        std::min(current.right, shader_bounds.right), std::min(current.top, shader_bounds.top)};
    if (result.right <= result.left || result.top <= result.bottom)
        result = {current.left, current.bottom, current.left, current.bottom};
    return {result, true};
}
// Dirty-only wrapper: potential stencil writes retain the full original
// footprint even when the separate host-rendering experiment is enabled.
inline Result NarrowInclude(Rect current, Rect shader_bounds, std::uint32_t mode,
                            std::uint32_t scale, bool shadow, bool stencil_write_possible = false) {
    if (stencil_write_possible)
        return {current, false};
    return IntersectInclude(current, shader_bounds, mode, scale, shadow);
}
} // namespace OpenGL::ScissorDamage
