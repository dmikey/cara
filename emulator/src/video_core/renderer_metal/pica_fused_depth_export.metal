// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
#include "depth_conversion.h"

using namespace metal;

struct FusedDepthExport {
    uint width;
    uint height;
    uint first_guest_row;
    uint row_count;
    uint depth_stride;
    uint stencil_stride;
    uint begin;
    uint end;
};

static uint MortonCoordinate(uint bits) {
    return (bits & 1u) | ((bits >> 1u) & 2u) | ((bits >> 2u) & 4u);
}

kernel void ExportCheckedPicaDepth(device const uint* depth [[buffer(0)]],
                                   device const uchar* stencil [[buffer(1)]],
                                   device uchar* requested [[buffer(2)]],
                                   device atomic_uint& errors [[buffer(3)]],
                                   constant FusedDepthExport& layout [[buffer(4)]],
                                   uint index [[thread_position_in_grid]]) {
    if (index >= layout.width * layout.row_count)
        return;
    // Cover every pixel in the copied rows, including pixels outside the
    // requested byte interval. The separate pack pass validates those too.
    const uint pixel = index + layout.width * layout.first_guest_row;
    const uint tile = pixel >> 6u;
    const uint tiles_per_row = layout.width >> 3u;
    const uint x = (tile % tiles_per_row) * 8u + MortonCoordinate(pixel & 63u);
    const uint guest_y = (tile / tiles_per_row) * 8u + MortonCoordinate((pixel & 63u) >> 1u);
    const uint y = layout.height - 1u - guest_y;
    const uint bits = depth[y * layout.depth_stride + x];
    if (!PicaMetalDepth::IsValidDepth(bits)) {
        atomic_fetch_or_explicit(&errors, 2u, memory_order_relaxed);
        return;
    }
    const uint destination = pixel * 4u;
    if (destination >= layout.end || destination + 4u <= layout.begin)
        return;
    const uint raw = PicaMetalDepth::Quantize(bits, 24u);
    const uchar4 bytes = uchar4(uchar(raw), uchar(raw >> 8u), uchar(raw >> 16u),
                               stencil[y * layout.stencil_stride + x]);
    for (uint byte = 0; byte < 4u; ++byte) {
        const uint address = destination + byte;
        if (address >= layout.begin && address < layout.end)
            requested[address - layout.begin] = bytes[byte];
    }
}
