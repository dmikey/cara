// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
#include "depth_conversion.h"
using namespace metal;

struct ImportLayout {
    uint width, height, depth_stride, stencil_stride, depth_bits;
};

kernel void detile_unpack_depth_import(device const uchar4* guest [[buffer(0)]],
                                       device uint* depth [[buffer(1)]],
                                       device uchar* stencil [[buffer(2)]],
                                       device atomic_uint& errors [[buffer(3)]],
                                       constant ImportLayout& layout [[buffer(4)]],
                                       uint2 pixel [[thread_position_in_grid]]) {
    if (pixel.x >= layout.width || pixel.y >= layout.height)
        return;
    const uint guest_y = layout.height - 1u - pixel.y;
    const uint tile = (guest_y / 8u) * (layout.width / 8u) + pixel.x / 8u;
    uint morton = 0;
    for (uint bit = 0; bit < 3; ++bit) {
        morton |= ((pixel.x >> bit) & 1u) << (2u * bit);
        morton |= ((guest_y >> bit) & 1u) << (2u * bit + 1u);
    }
    const uchar4 value = guest[tile * 64u + morton];
    const uint raw = uint(value.x) | (uint(value.y) << 8u) | (uint(value.z) << 16u);
    if (raw >= (1u << layout.depth_bits)) {
        atomic_fetch_or_explicit(&errors, 1u, memory_order_relaxed);
        return;
    }
    depth[pixel.y * layout.depth_stride + pixel.x] = PicaMetalDepth::Normalize(raw, layout.depth_bits);
    stencil[pixel.y * layout.stencil_stride + pixel.x] = value.w;
}
