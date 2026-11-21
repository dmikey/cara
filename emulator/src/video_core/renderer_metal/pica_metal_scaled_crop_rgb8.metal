// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

// Tiled ABGR input to linear BGR output. Both are guest byte layouts.
kernel void cara_pica_display_scaled_crop_rgb8(
    const device uchar* source [[buffer(0)]], device uchar* destination [[buffer(1)]],
    constant uint* args [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    const uint input_width = args[0];
    const uint output_width = args[4];
    const uint output_height = args[5];
    if (xy.x >= output_width || xy.y >= output_height)
        return;

    const uint x = 2u * xy.x;
    const uint y = xy.y;
    const uint morton = (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) |
                        ((y & 2u) << 2u) | ((x & 4u) << 2u) | ((y & 4u) << 3u);
    const uint first = 4u * ((y & ~7u) * input_width + (x & ~7u) * 8u + morton);
    const uint output_y = args[2] ? output_height - 1u - y : y;
    const uint to = 3u * (output_y * output_width + xy.x);
    for (uint channel = 0; channel < 3u; ++channel) {
        // x is even, so the matching pixel is the next Morton texel.
        const uint byte = first + 1u + channel;
        destination[to + channel] = uchar((uint(source[byte]) + source[byte + 4u]) >> 1u);
    }
}

// Reinterpret a linear guest ABGR8 span as tiled guest BGR8 without filtering.
kernel void cara_pica_display_linear_to_tiled_rgb8(
    const device uchar* source [[buffer(0)]], device uchar* destination [[buffer(1)]],
    constant uint* args [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    const uint width = args[0];
    const uint height = args[5];
    if (xy.x >= width || xy.y >= height)
        return;
    const uint x = xy.x;
    const uint y = args[2] ? height - 1u - xy.y : xy.y;
    const uint morton = (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) |
                        ((y & 2u) << 2u) | ((x & 4u) << 2u) | ((y & 4u) << 3u);
    const uint from = 4u * (xy.y * width + x);
    const uint to = 3u * ((y & ~7u) * width + (x & ~7u) * 8u + morton);
    for (uint channel = 0; channel < 3u; ++channel)
        destination[to + channel] = source[from + 1u + channel];
}

// Exact fusion of ExportPicaTiledRGBA8 followed by the scaled/cropped raw
// transfer. Quantize each texel BEFORE the integer box filter: float averaging
// would round half values differently from the existing guest byte path.
kernel void cara_pica_display_texture_scaled_crop(
    texture2d<float, access::read> source [[texture(0)]],
    device uchar* destination [[buffer(0)]], constant uint4& args [[buffer(1)]],
    uint2 xy [[thread_position_in_grid]]) {
    const uint width=args.x, height=args.y, bpp=args.z;
    if (xy.x>=width || xy.y>=height) return;
    const uint source_y=source.get_height()-1u-xy.y;
    const uint3 first=uint3(rint(source.read(uint2(2u*xy.x,source_y)).rgb*255.0f));
    const uint3 second=uint3(rint(source.read(uint2(2u*xy.x+1u,source_y)).rgb*255.0f));
    const uint3 sum=first+second;
    const uint output_y=args.w ? height-1u-xy.y : xy.y;
    const uint to=bpp*(output_y*width+xy.x);
    if (bpp==3u) {
        destination[to]=uchar(sum.b>>1u);
        destination[to+1u]=uchar(sum.g>>1u);
        destination[to+2u]=uchar(sum.r>>1u);
    } else {
        const uint packed=((sum.r>>4u)<<11u)|((sum.g>>3u)<<5u)|(sum.b>>4u);
        destination[to]=uchar(packed);
        destination[to+1u]=uchar(packed>>8u);
    }
}
