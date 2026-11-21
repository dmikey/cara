// Derived from Cara's Pica::Texture::LookupTexelInTile and SampleETC1Subtile.
// Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

constant uint pica_format [[function_constant(0)]];
constant bool disable_alpha [[function_constant(1)]];

struct DecodeRegion {
    uint height;
    uint tile_row_bytes;
    uint2 origin;
    uint2 size;
};

constant uint tile_bytes[] = {256, 192, 128, 128, 128, 128, 128, 64, 64, 64, 32, 32, 32, 64};
constant int2 etc_modifiers[] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}
};

static uint SpreadTileCoordinate(uint coordinate) {
    const uint bits = coordinate & 7u;
    return (bits & 1u) | ((bits & 2u) << 1u) | ((bits & 4u) << 2u);
}

static uchar Expand4(uint value) { return uchar((value << 4u) | value); }
static uchar Expand5(uint value) { return uchar((value << 3u) | (value >> 2u)); }
static uchar Expand6(uint value) { return uchar((value << 2u) | (value >> 4u)); }
static uint Read16(device const uchar* p) { return uint(p[0]) | (uint(p[1]) << 8u); }

static uchar Nibble(device const uchar* data, uint offset) {
    const auto bits = data[offset / 2u];
    return Expand4((bits >> ((offset & 1u) * 4u)) & 15u);
}

static uchar EtcComponent(uint bits, bool differential, bool second) {
    if (!differential)
        return Expand4(second ? bits & 15u : bits >> 4u);
    const int base = int(bits >> 3u);
    const int delta = int((bits & 7u) ^ 4u) - 4;
    // Match the existing u8 input to Convert5To8 even for noncanonical bits.
    return Expand5(uchar(second ? base + delta : base));
}

static uchar4 DecodeEtc(device const uchar* tile, uint x, uint y) {
    const bool has_alpha = pica_format == 13u;
    const uint subtile = (x / 4u) + 2u * (y / 4u);
    const auto data = tile + subtile * (has_alpha ? 16u : 8u);
    const uint tx = x & 3u;
    const uint ty = y & 3u;
    const uint texel = 4u * tx + ty;
    const uchar alpha = (!has_alpha || disable_alpha) ? 255u : Nibble(data, texel);
    const auto color = data + (has_alpha ? 8u : 0u);
    const uint control = color[4];
    const bool second = ((control & 1u) ? ty : tx) >= 2u;
    const bool differential = (control & 2u) != 0;
    const uint table = (control >> (second ? 2u : 5u)) & 7u;
    const uint select = (Read16(color) >> texel) & 1u;
    const int sign = ((Read16(color + 2) >> texel) & 1u) ? -1 : 1;
    const int modifier = sign * etc_modifiers[table][select];
    const int3 rgb = int3(EtcComponent(color[7], differential, second),
                          EtcComponent(color[6], differential, second),
                          EtcComponent(color[5], differential, second)) + modifier;
    return uchar4(uchar3(clamp(rgb, int3(0), int3(255))), alpha);
}

static uchar4 DecodeTexel(device const uchar* tile, uint x, uint y) {
    const uint z = SpreadTileCoordinate(x) | (SpreadTileCoordinate(y) << 1u);
    if (pica_format == 0u) {
        const auto data = tile + z * 4u;
        return uchar4(data[3], data[2], data[1], disable_alpha ? 255u : data[0]);
    }
    if (pica_format == 1u) {
        const auto data = tile + z * 3u;
        return uchar4(data[2], data[1], data[0], 255u);
    }
    if (pica_format == 2u) {
        const uint bits = Read16(tile + z * 2u);
        return uchar4(Expand5(bits >> 11u), Expand5((bits >> 6u) & 31u),
                      Expand5((bits >> 1u) & 31u), disable_alpha || (bits & 1u) ? 255u : 0u);
    }
    if (pica_format == 3u) {
        const uint bits = Read16(tile + z * 2u);
        return uchar4(Expand5(bits >> 11u), Expand6((bits >> 5u) & 63u),
                      Expand5(bits & 31u), 255u);
    }
    if (pica_format == 4u) {
        const uint bits = Read16(tile + z * 2u);
        return uchar4(Expand4(bits >> 12u), Expand4((bits >> 8u) & 15u),
                      Expand4((bits >> 4u) & 15u), disable_alpha ? 255u : Expand4(bits & 15u));
    }
    if (pica_format == 5u || pica_format == 9u) {
        const auto data = tile + z * (pica_format == 5u ? 2u : 1u);
        const uchar intensity = pica_format == 5u ? data[1] : Expand4(data[0] >> 4u);
        const uchar alpha = pica_format == 5u ? data[0] : Expand4(data[0] & 15u);
        return disable_alpha ? uchar4(intensity, alpha, 0u, 255u)
                             : uchar4(intensity, intensity, intensity, alpha);
    }
    if (pica_format == 6u)
        return uchar4(tile[z * 2u + 1u], tile[z * 2u], 0u, 255u);
    if (pica_format == 7u || pica_format == 10u) {
        const uchar intensity = pica_format == 7u ? tile[z] : Nibble(tile, z);
        return uchar4(intensity, intensity, intensity, 255u);
    }
    if (pica_format == 8u || pica_format == 11u) {
        const uchar alpha = pica_format == 8u ? tile[z] : Nibble(tile, z);
        return disable_alpha ? uchar4(alpha, alpha, alpha, 255u) : uchar4(0u, 0u, 0u, alpha);
    }
    return DecodeEtc(tile, x, y);
}

// Require a supported format, checked source span and CPU-authoritative tiled
// bytes before encoding. The destination follows the existing cache's row order.
kernel void DecodePicaTexture(device const uchar* tiled [[buffer(0)]],
                              constant DecodeRegion& region [[buffer(1)]],
                              texture2d<float, access::write> rgba [[texture(0)]],
                              uint2 index [[thread_position_in_grid]]) {
    if (pica_format >= 14u || any(index >= region.size))
        return;
    const uint2 dst = region.origin + index;
    const uint guest_y = region.height - 1u - dst.y;
    const ulong offset = ulong(guest_y / 8u) * region.tile_row_bytes +
                         ulong(dst.x / 8u) * tile_bytes[pica_format];
    const uchar4 pixel = DecodeTexel(tiled + offset, dst.x & 7u, guest_y & 7u);
    rgba.write(float4(pixel) / 255.0f, dst);
}
