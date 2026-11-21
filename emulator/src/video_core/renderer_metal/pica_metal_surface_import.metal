// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

static uint InterleaveWithinTile(uint x, uint y) {
    uint offset = 0;
    for (uint bit = 0; bit < 3; ++bit) {
        offset |= ((x >> bit) & 1u) << (2u * bit);
        offset |= ((y >> bit) & 1u) << (2u * bit + 1u);
    }
    return offset;
}

// Dense tiled D24S8 guest pixels are [Dlo,Dmid,Dhi,S]. The retained depth
// unpack pipeline consumes [S,Dhi,Dmid,Dlo] in Cara's host row order.
kernel void DetilePicaDepth24Stencil8(device const uchar4* guest [[buffer(0)]],
                                     device uchar4* packed [[buffer(1)]],
                                     constant uint2& size [[buffer(2)]],
                                     uint2 position [[thread_position_in_grid]]) {
    if (any(position >= size))
        return;

    const uint guest_y = size.y - 1u - position.y;
    const uint tile = (guest_y / 8u) * (size.x / 8u) + position.x / 8u;
    const uint index = tile * 64u + InterleaveWithinTile(position.x, guest_y);
    packed[position.y * size.x + position.x] = guest[index].wzyx;
}

kernel void DetilePicaDepth24(device const uchar* guest [[buffer(0)]],
                             device uchar4* packed [[buffer(1)]],
                             constant uint2& size [[buffer(2)]],
                             uint2 position [[thread_position_in_grid]]) {
    if (any(position >= size))
        return;

    const uint guest_y = size.y - 1u - position.y;
    const uint tile = (guest_y / 8u) * (size.x / 8u) + position.x / 8u;
    const uint offset = 3u * (tile * 64u + InterleaveWithinTile(position.x, guest_y));
    packed[position.y * size.x + position.x] =
        uchar4(0, guest[offset + 2u], guest[offset + 1u], guest[offset]);
}

// Merge import stages only a rectangle whose underlying guest tiles are
// wholly RAM-owned. The output uses local rows for aligned depth/stencil blits.
struct GuestDepthRectangle {
    uint surface_width, surface_height;
    uint x, y, width, height;
};

kernel void DetilePicaDepth24Stencil8Rect(device const uchar4* guest [[buffer(0)]],
                                         device uchar4* packed [[buffer(1)]],
                                         constant GuestDepthRectangle& rect [[buffer(2)]],
                                         uint2 local [[thread_position_in_grid]]) {
    if (local.x >= rect.width || local.y >= rect.height)
        return;

    const uint x = rect.x + local.x;
    const uint guest_y = rect.surface_height - 1u - rect.y - local.y;
    const uint tile = (guest_y / 8u) * (rect.surface_width / 8u) + x / 8u;
    const uint index = tile * 64u + InterleaveWithinTile(x, guest_y);
    packed[local.y * rect.width + local.x] = guest[index].wzyx;
}
