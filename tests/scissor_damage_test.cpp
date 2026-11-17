// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>
#include <iostream>
#include "video_core/renderer_opengl/gl_scissor_damage.h"

using OpenGL::ScissorDamage::Rect;
using OpenGL::ScissorDamage::NarrowInclude;
static bool Contains(Rect r, std::int64_t x, std::int64_t y) {
    return x >= r.left && x < r.right && y >= r.bottom && y < r.top;
}
static bool Same(Rect a, Rect b) {
    return a.left == b.left && a.right == b.right && a.bottom == b.bottom && a.top == b.top;
}
int main() {
    std::uint64_t checks = 0;
    for (unsigned scale : {1u, 2u, 3u, 4u}) {
        const auto s = static_cast<int>(scale);
        for (int ox : {0, 3, 8}) for (int oy : {0, 5, 16}) {
            Rect current{ox * s, oy * s, (ox + 9) * s, (oy + 7) * s};
            for (int x1 = -2; x1 <= 11; ++x1) for (int x2 : {-2, 0, 3, 8, 11}) {
                for (int y1 : {-2, 0, 4, 8}) for (int y2 : {-2, 0, 4, 8}) {
                    // Construct actual uniforms from independent inclusive guest endpoints.
                    Rect bounds{(ox + x1) * s, (oy + y1) * s,
                                (ox + x2 + 1) * s, (oy + y2 + 1) * s};
                    const auto result = NarrowInclude(current, bounds, 3, scale, false);
                    assert(result.eligible);
                    for (int y = current.bottom; y < current.top; ++y)
                        for (int x = current.left; x < current.right; ++x) {
                            // Independent shader predicate: floating sample centres and
                            // the inclusive guest endpoints, not rectangle intersection.
                            const float fx = (float(x) + 0.5f) / scale - ox;
                            const float fy = (float(y) + 0.5f) / scale - oy;
                            const bool kept = fx >= x1 && fx < x2 + 1 &&
                                              fy >= y1 && fy < y2 + 1;
                            assert(Contains(result.rect, x, y) == kept);
                            ++checks;
                        }
                    for (unsigned mode : {0u, 1u, 2u, 4u})
                        assert(Same(NarrowInclude(current, bounds, mode, scale, false).rect, current));
                    assert(Same(NarrowInclude(current, bounds, 3, scale, true).rect, current));
                }
            }
        }
    }
    const Rect original{0, 0, 16, 16};
    assert(!NarrowInclude(original, original, 3, 0, false).eligible);
    assert(!NarrowInclude(original, {1, 0, 16, 16}, 3, 2, false).eligible);
    assert(!NarrowInclude({-1, 0, 16, 16}, original, 3, 1, false).eligible);
    assert(!NarrowInclude({0, 0, (1LL << 23) + 1, 16}, original, 3, 1, false).eligible);
    assert(!NarrowInclude(original, {INT64_MIN, 0, INT64_MAX, 16}, 3, 1, false).eligible);
    // At >=2^23 a half-pixel centre can round across an integer shader bound.
    // A former2^24 limit would falsely exclude this shader-visible pixel.
    const float rounded_centre = static_cast<float>(8388609) + 0.5f;
    assert(rounded_centre >= static_cast<float>(8388610));
    assert(!NarrowInclude({8388608, 0, 8388611, 1}, {8388610, 0, 8388611, 1},
                          3, 1, false).eligible);
    const auto exact_edge = NarrowInclude({8388607, 0, 8388608, 1},
                                          {8388607, 0, 8388608, 1}, 3, 1, false);
    assert(exact_edge.eligible && Same(exact_edge.rect, {8388607, 0, 8388608, 1}));
    assert(!NarrowInclude(original, {3, 7, 4, 8}, 3, 1, false, true).eligible);
    assert(Same(NarrowInclude(original, {3, 7, 4, 8}, 3, 1, false, true).rect, original));
    const auto one = NarrowInclude(original, {3, 7, 4, 8}, 3, 1, false);
    assert(one.eligible && Same(one.rect, {3, 7, 4, 8}));
    std::cout << "INCLUDE damage pixel checks " << checks << " PASS\n";
}
