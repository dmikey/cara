// SPDX-License-Identifier: GPL-2.0-or-later
// Real SurfaceParams conversion, with an independent per-pixel Morton-byte oracle.
#include <cassert>
#include <cstdint>
#include <iostream>
#include "video_core/renderer_opengl/gl_surface_params.h"
#include "video_core/renderer_opengl/gl_scissor_damage.h"

static unsigned Morton(unsigned x, unsigned y) {
    unsigned result = 0;
    for (unsigned b = 0; b < 3; ++b) {
        result |= ((x >> b) & 1) << (2 * b);
        result |= ((y >> b) & 1) << (2 * b + 1);
    }
    return result;
}
int main() {
    using namespace OpenGL;
    std::uint64_t checked = 0, cases = 0;
    for (auto format : {SurfaceParams::PixelFormat::RGBA8, SurfaceParams::PixelFormat::RGB8,
                        SurfaceParams::PixelFormat::RGB565, SurfaceParams::PixelFormat::D16,
                        SurfaceParams::PixelFormat::D24, SurfaceParams::PixelFormat::D24S8})
    for (unsigned width : {16u, 24u, 40u}) for (unsigned height : {16u, 24u}) {
        SurfaceParams p{};
        p.addr = 0x100000; p.width = p.stride = width; p.height = height;
        p.pixel_format = format; p.is_tiled = true; p.res_scale = 1; p.UpdateParams();
        const unsigned bpp = p.GetFormatBpp() / 8;
        for (int offset : {0, 3, 8}) for (unsigned scale : {1u, 2u, 3u}) {
            const auto s = static_cast<int>(scale);
            ScissorDamage::Rect current{offset * s, offset * s,
                                       static_cast<int>(width - 1) * s,
                                       static_cast<int>(height - 1) * s};
            const auto old = p.GetSubRectInterval({static_cast<u32>(current.left / s),
                static_cast<u32>(current.top / s), static_cast<u32>(current.right / s),
                static_cast<u32>(current.bottom / s)});
            for (int left : {0, 1, 7, 8, 9, 15}) for (int bottom : {0, 1, 7, 8, 9})
            for (int right : {0, 7, 8, 15, 40}) for (int top : {0, 7, 8, 15, 24}) {
                const ScissorDamage::Rect bounds{left * s, bottom * s,
                                                (right + 1) * s, (top + 1) * s};
                const auto result = ScissorDamage::NarrowInclude(current, bounds, 3, scale, false);
                assert(result.eligible);
                const auto interval = p.GetSubRectInterval({
                    static_cast<u32>(result.rect.left / s), static_cast<u32>(result.rect.top / s),
                    static_cast<u32>(result.rect.right / s), static_cast<u32>(result.rect.bottom / s)});
                const bool empty = result.rect.left == result.rect.right || result.rect.bottom == result.rect.top;
                if (empty) assert(boost::icl::length(interval) == 0);
                else assert(boost::icl::first(interval) >= boost::icl::first(old) &&
                            boost::icl::last_next(interval) <= boost::icl::last_next(old));
                for (unsigned y = offset; y < height - 1; ++y)
                for (unsigned x = offset; x < width - 1; ++x) {
                    // Integer guest pixel centres obey the generated shader's
                    // inclusive-register predicate; no rectangle helper is used here.
                    const bool written = int(x) >= left && int(x) <= right &&
                                         int(y) >= bottom && int(y) <= top;
                    if (!written) continue;
                    assert(!empty);
                    const auto memory_y = height - 1 - y;
                    const auto tile = (memory_y / 8) * (width / 8) + x / 8;
                    const auto physical = p.addr + (tile * 64 + Morton(x % 8, memory_y % 8)) * bpp;
                    for (unsigned byte = 0; byte < bpp; ++byte) {
                        assert(physical + byte >= boost::icl::first(interval));
                        assert(physical + byte < boost::icl::last_next(interval));
                        ++checked;
                    }
                }
                ++cases;
            }
        }
    }
    std::cout << "INCLUDE tiled damage " << cases << " cases / " << checked
              << " written-byte coverage checks PASS\n";
}
