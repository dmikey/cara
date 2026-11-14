// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#include "../emulator/src/video_core/renderer_opengl/gl_surface_remap_layout.h"
#include <cassert>
#include <iostream>
using namespace OpenGL::SurfaceRemapLayout;
int main() {
    // Exhaust small spans, including partial pixels/tiles and two-row seams.
    for (unsigned unit : {3u, 4u, 192u, 256u}) {
        const unsigned row = unit * 5;
        const unsigned base = 17;
        for (unsigned begin = 0; begin < 3 * row; begin += unit == 3 ? 1 : 7) {
            for (unsigned length = 1; length < 3 * row; length += unit == 3 ? 1 : 11) {
                const auto span = RectangleSpan(base, row, unit,
                                                {base + begin, base + begin + length});
                const auto aligned_begin = (begin + unit - 1) / unit * unit;
                const auto aligned_end = (begin + length) / unit * unit;
                assert(bool(span) == (aligned_begin < aligned_end));
                if (!span) continue;
                assert(span.begin >= base + begin && span.end <= base + begin + length);
                const auto a = span.begin - base, b = span.end - base;
                assert(a % unit == 0 && b % unit == 0);
                assert((a % row == 0 && b % row == 0) || a / row == (b - 1) / row);
                // Removing the result must make strict forward progress.
                assert(b > a);
            }
        }
    }
    assert(!RectangleSpan(100, 960, 3, {99, 102}));
    assert(!RectangleSpan(0, 960, 3, {1, 2}));
    const auto rgb = RectangleSpan(0, 7680, 256, {0, 159744});
    assert(rgb && rgb.begin == 0 && rgb.end == 153600);
    std::cout << "Surface remap ownership rectangle tests passed\n";
}
