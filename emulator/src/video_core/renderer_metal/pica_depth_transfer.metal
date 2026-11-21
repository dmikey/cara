// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
#include "depth_conversion.h"

using namespace metal;

struct DepthTransfer {
    uint width;
    uint height;
    uint x;
    uint y;
    uint packed_stride;
    uint depth_stride;
    uint stencil_stride;
    uint depth_bits;
};

kernel void unpack_pica_depth(device const uchar4* packed [[buffer(0)]],
                              device uint* depth [[buffer(1)]],
                              device uchar* stencil [[buffer(2)]],
                              device atomic_uint& errors [[buffer(3)]],
                              constant DepthTransfer& layout [[buffer(4)]],
                              uint2 cell [[thread_position_in_grid]]) {
    if (cell.x >= layout.width || cell.y >= layout.height)
        return;
    cell += uint2(layout.x, layout.y);
    const uchar4 in = packed[cell.y * layout.packed_stride + cell.x];
    const uint raw = (uint(in.y) << 16u) | (uint(in.z) << 8u) | uint(in.w);
    if (raw >= (1u << layout.depth_bits)) {
        atomic_fetch_or_explicit(&errors, 1u, memory_order_relaxed);
        return;
    }

    depth[cell.y * layout.depth_stride + cell.x] = PicaMetalDepth::Normalize(raw, layout.depth_bits);
    stencil[cell.y * layout.stencil_stride + cell.x] = in.x;
}

kernel void pack_pica_depth(device uchar4* packed [[buffer(0)]],
                            device const uint* depth [[buffer(1)]],
                            device const uchar* stencil [[buffer(2)]],
                            device atomic_uint& errors [[buffer(3)]],
                            constant DepthTransfer& layout [[buffer(4)]],
                            uint2 cell [[thread_position_in_grid]]) {
    if (cell.x >= layout.width || cell.y >= layout.height)
        return;
    cell += uint2(layout.x, layout.y);
    const uint value = depth[cell.y * layout.depth_stride + cell.x];
    if (!PicaMetalDepth::IsValidDepth(value)) {
        atomic_fetch_or_explicit(&errors, 2u, memory_order_relaxed);
        return;
    }

    const uint raw = PicaMetalDepth::Quantize(value, layout.depth_bits);
    packed[cell.y * layout.packed_stride + cell.x] =
        uchar4(stencil[cell.y * layout.stencil_stride + cell.x], uchar(raw >> 16u),
               uchar(raw >> 8u), uchar(raw));
}
