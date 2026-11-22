// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

vertex float4 cara_scale_vertex(uint id [[vertex_id]]) {
    return float4(id == 1 ? 3.f : -1.f, id == 2 ? 3.f : -1.f, 0.f, 1.f);
}

struct ScaleOutput {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
    uint stencil [[stencil]];
};

// The direction is encoded by the destination dimensions: nearest expansion
// seeds every subpixel; the reverse pass boxes color and picks one coupled
// depth/stencil sample so guest mask and depth always come from the same pixel.
fragment ScaleOutput cara_scale_fragment(float4 location [[position]],
    texture2d<float, access::read> color [[texture(0)]],
    depth2d<float, access::read> depth [[texture(1)]],
    texture2d<uint, access::read> stencil [[texture(2)]],
    constant uint* layout [[buffer(0)]]) {
    const uint2 target(layout[0],layout[1]);
    const uint2 xy = uint2(location.xy);
    const uint2 extent(color.get_width(), color.get_height());
    const uint2 start = xy * extent / target;
    const uint2 samples = max(extent / target, 1u);
    const uint2 center = start + samples / 2u;
    float4 sum = 0.0f;
    for (uint y = 0; y < samples.y; ++y)
        for (uint x = 0; x < samples.x; ++x)
            sum += color.read(start + uint2(x,y));
    return {sum / float(samples.x * samples.y), layout[2] ? depth.read(center) : 0.f, layout[2] ? stencil.read(center).r : 0u};
}

// Preserve subpixel samples across a proven RGBA8/RGB8 linear LCD transfer.
// Rows match the native transfer before the LCD compositor rotates them.
kernel void cara_scale_scanout(texture2d<float,access::read> src [[texture(0)]],
    texture2d<float,access::write> dst [[texture(1)]], constant uint4& flags [[buffer(0)]],
    uint2 xy [[thread_position_in_grid]]) {
    if (xy.x >= dst.get_width() || xy.y >= dst.get_height()) return;
    uint y = flags.z + (flags.x ? dst.get_height() - 1u - xy.y : xy.y);
    uint dx = flags.w;
    uint x = dx*xy.x;
    float4 value = src.read(uint2(x,y));
    if (dx == 2) value = .5f*(value + src.read(uint2(x+1u,y)));
    if (flags.y == 3) value.a = 1.f;
    dst.write(value,xy);
}

inline uint3 scale_pack_rgb565(float3 value) {
    return (uint3(round(value * 255.f)) * uint3(31, 63, 31) + 127u) / 255u;
}

// Write the guest-visible RGB565 bytes and capture the four enhanced
// subpixels before the next draw can reuse the source surface.
kernel void cara_scale_scanout_rgb565(texture2d<float,access::read> hi [[texture(0)]],
    texture2d<float,access::write> dst [[texture(1)]], texture2d<float,access::read> native [[texture(2)]],
    constant uint4& flags [[buffer(0)]], device ushort* guest [[buffer(1)]], uint2 xy [[thread_position_in_grid]]) {
    if (xy.x >= dst.get_width() || xy.y >= dst.get_height()) return;
    const uint2 source(xy.x, flags.z + (flags.x ? dst.get_height() - 1u - xy.y : xy.y));
    const auto high = scale_pack_rgb565(hi.read(source).rgb);
    dst.write(float4(float3(high) / float3(31,63,31), 1.f), xy);
    if (((xy.x | xy.y) & 1u) == 0u) {
        const uint3 rgb = scale_pack_rgb565(native.read(source / 2u).rgb);
        guest[(xy.y / 2u) * (dst.get_width() / 2u) + xy.x / 2u] =
            ushort((rgb.r << 11u) | (rgb.g << 5u) | rgb.b);
    }
}
