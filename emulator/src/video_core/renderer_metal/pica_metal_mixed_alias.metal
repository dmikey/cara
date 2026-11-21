// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

struct AliasUniforms {
    uint rgb_base;
    uint rgba_base;
    uint target_base;
    uint split;
    uint rgb_width;
    uint rgba_width;
};

inline uint morton8(uint x, uint y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) |
           ((x & 4u) << 2) | ((y & 4u) << 3);
}

inline uint2 linear_coordinate(uint index, uint width) {
    return uint2(index % width, index / width);
}

inline float rgb_component(float4 first, float4 second, uint component) {
    return component < 3 ? first[2 - component] : second[5 - component];
}

inline float guest_byte(texture2d<float, access::read> rgb,
                        texture2d<float, access::read> rgba, constant AliasUniforms& args,
                        uint address) {
    if (address < args.split) {
        const uint offset = address - args.rgb_base;
        const uint pixel = offset / 3;
        return rgb.read(linear_coordinate(pixel, args.rgb_width))[2 - offset % 3];
    }
    const uint offset = address - args.rgba_base;
    const uint pixel = offset / 4;
    return rgba.read(linear_coordinate(pixel, args.rgba_width))[3 - offset % 4];
}

kernel void cara_pica_mixed_linear_alias(
    texture2d<float, access::read> rgb [[texture(0)]],
    texture2d<float, access::read> rgba [[texture(1)]],
    texture2d<float, access::write> output [[texture(2)]],
    constant AliasUniforms& args [[buffer(0)]], uint2 xy [[thread_position_in_grid]]) {
    const uint guest_y = output.get_height() - 1 - xy.y;
    const uint tile = (guest_y / 8) * (output.get_width() / 8) + xy.x / 8;
    const uint address =
        args.target_base + 4 * (tile * 64 + morton8(xy.x & 7u, guest_y & 7u));

    if (address + 3 < args.split) {
        const uint offset = address - args.rgb_base;
        const uint first_pixel = offset / 3;
        const uint first_component = offset % 3;
        const float4 first = rgb.read(linear_coordinate(first_pixel, args.rgb_width));
        const float4 second = rgb.read(linear_coordinate(first_pixel + 1, args.rgb_width));
        output.write(float4(rgb_component(first, second, first_component + 3),
                            rgb_component(first, second, first_component + 2),
                            rgb_component(first, second, first_component + 1),
                            rgb_component(first, second, first_component)), xy);
        return;
    }
    if (address >= args.split && (address - args.rgba_base) % 4 == 0) {
        const uint pixel = (address - args.rgba_base) / 4;
        output.write(rgba.read(linear_coordinate(pixel, args.rgba_width)), xy);
        return;
    }

    // Four guest bytes make one output pixel. They may span RGB triplets
    // or the owner split. No alpha is synthesized for the raw byte alias.
    const float4 color = float4(guest_byte(rgb, rgba, args, address + 3),
                                guest_byte(rgb, rgba, args, address + 2),
                                guest_byte(rgb, rgba, args, address + 1),
                                guest_byte(rgb, rgba, args, address));
    output.write(color, xy);
}
