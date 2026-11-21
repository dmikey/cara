// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

// Source row numbering matches Cara's cache (GL row 0). Input is either
// RGBA8 [R,G,B,A] or packed GPU depth output [S,Dhi,Dmid,Dlo]. The PICA
// tiled layouts use [A,B,G,R] and [Dlo,Dmid,Dhi,S], respectively.
struct PicaTiledExport {
    uint width;
    uint height;
    uint source_row;
    uint tiles_per_row;
    uint begin;
    uint end;
};

static uint DrawMortonCoordinate(uint bits) {
    return (bits & 1u) | ((bits >> 1u) & 2u) | ((bits >> 2u) & 4u);
}

kernel void ExportPicaTiled32(device const uchar* canonical [[buffer(0)]],
                              device uchar* requested_guest_bytes [[buffer(1)]],
                              constant PicaTiledExport& layout [[buffer(2)]],
                              uint index [[thread_position_in_grid]]) {
    const uint first = layout.begin >> 2u;
    const uint last = (layout.end - 1u) >> 2u;
    if (index > last - first) {
        return;
    }
    const uint pixel = first + index;
    const uint tile = pixel >> 6u;
    const uint in_tile = pixel & 63u;
    const uint x = (tile % layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile);
    const uint y = (tile / layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile >> 1u);
    if (x >= layout.width || y >= layout.height) {
        return;
    }
    const ulong source = ulong(layout.height - 1u - y) * layout.source_row + ulong(x) * 4u;
    const uint destination = pixel * 4u;
    for (uint byte = 0; byte < 4u; ++byte) {
        const uint address = destination + byte;
        if (address >= layout.begin && address < layout.end) {
            requested_guest_bytes[address - layout.begin] = canonical[source + 3u - byte];
        }
    }
}

kernel void ExportPicaTiledDepth24(device const uchar* canonical [[buffer(0)]],
                                  device uchar* requested [[buffer(1)]],
                                  constant PicaTiledExport& layout [[buffer(2)]],
                                  uint index [[thread_position_in_grid]]) {
    const uint pixel = layout.begin / 3u + index;
    if (pixel > (layout.end - 1u) / 3u)
        return;
    const uint tile = pixel / 64u;
    const uint in_tile = pixel % 64u;
    const uint x = (tile % layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile);
    const uint y = (tile / layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile >> 1u);
    const ulong source = ulong(layout.height - 1u - y) * layout.source_row + ulong(x) * 4u;
    for (uint byte = 0; byte < 3u; ++byte) {
        const uint address = pixel * 3u + byte;
        if (address >= layout.begin && address < layout.end)
            requested[address - layout.begin] = canonical[source + 3u - byte];
    }
}

// Invert the same Morton mapping directly from a tracked RGBA8 attachment.
// UNORM8 conversion and the R/B byte order match the buffer export above.
kernel void ExportPicaTiledRGBA8(texture2d<float, access::read> source [[texture(0)]],
                                device uchar* requested [[buffer(0)]],
                                constant PicaTiledExport& layout [[buffer(1)]],
                                uint index [[thread_position_in_grid]]) {
    const uint first = layout.begin >> 2u;
    const uint last = (layout.end - 1u) >> 2u;
    if (index > last - first)
        return;
    const uint pixel = first + index;
    const uint tile = pixel >> 6u;
    const uint in_tile = pixel & 63u;
    const uint x = (tile % layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile);
    const uint y = (tile / layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile >> 1u);
    if (x >= layout.width || y >= layout.height)
        return;
    const auto packed = uchar4(rint(source.read(uint2(x, layout.height - 1u - y)) * 255.0f));
    const uint destination = pixel * 4u;
    for (uint byte = 0; byte < 4u; ++byte) {
        const uint address = destination + byte;
        if (address >= layout.begin && address < layout.end)
            requested[address - layout.begin] = packed[3u - byte];
    }
}

// Packed RGB565 shares its little-endian word representation with B5G6R5Unorm.
kernel void ExportPicaTiledRGB565(device const uchar* canonical [[buffer(0)]],
                                  device uchar* requested [[buffer(1)]],
                                  constant PicaTiledExport& layout [[buffer(2)]],
                                  uint index [[thread_position_in_grid]]) {
    const uint first = layout.begin / 2u;
    const uint last = (layout.end - 1u) / 2u;
    if (index > last - first)
        return;
    const uint pixel = first + index;
    const uint tile = pixel / 64u;
    const uint in_tile = pixel % 64u;
    const uint x = (tile % layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile);
    const uint y = (tile / layout.tiles_per_row) * 8u + DrawMortonCoordinate(in_tile >> 1u);
    if (x >= layout.width || y >= layout.height)
        return;

    const ulong source = ulong(layout.height - 1u - y) * layout.source_row + ulong(x) * 2u;
    const uint destination = pixel * 2u;
    for (uint byte = 0; byte < 2u; ++byte) {
        const uint address = destination + byte;
        if (address >= layout.begin && address < layout.end)
            requested[address - layout.begin] = canonical[source + byte];
    }
}

kernel void DetilePicaRgb565(device const uchar* source [[buffer(0)]],
                            device uchar* linear [[buffer(1)]],
                            constant uint* size [[buffer(2)]],
                            uint2 xy [[thread_position_in_grid]]) {
    const uint width = size[0];
    const uint height = size[1];
    if (xy.x >= width || xy.y >= height)
        return;

    const uint y = height - 1u - xy.y;
    const uint x = xy.x;
    const uint morton = (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) |
                        ((y & 2u) << 2u) | ((x & 4u) << 2u) | ((y & 4u) << 3u);
    const uint pixel = (y & ~7u) * width + (x & ~7u) * 8u + morton;
    const uint out = xy.y * size[2] + xy.x * 2u;
    linear[out] = source[pixel * 2u];
    linear[out + 1u] = source[pixel * 2u + 1u];
}
