// SPDX-License-Identifier: GPL-2.0-or-later
#include <metal_stdlib>
using namespace metal;

struct ScanoutArgs {
    uint width, height, stride, format;
    ulong offset;
};

kernel void DecodePicaLinearLCD(device const uchar* guest [[buffer(0)]],
                                constant ScanoutArgs& plan [[buffer(1)]],
                                texture2d<float, access::write> output [[texture(0)]],
                                uint2 coordinate [[thread_position_in_grid]]) {
    if (coordinate.x >= plan.width || coordinate.y >= plan.height)
        return;

    const uint bpp = plan.format == 0 ? 4u : plan.format == 1 ? 3u : 2u;
    const device uchar* pixel = guest + plan.offset + ulong(coordinate.y) * plan.stride +
                               ulong(coordinate.x) * bpp;
    float4 color;
    if (plan.format < 2u) {
        color = float4(pixel[bpp - 1u], pixel[bpp - 2u], pixel[bpp - 3u],
                       plan.format == 0u ? pixel[0] : 255u) / 255.0f;
    } else {
        const uint word = pixel[0] | (uint(pixel[1]) << 8u);
        if (plan.format == 2u) {
            color = float4((word >> 11u) & 31u, (word >> 5u) & 63u, word & 31u, 1u) /
                    float4(31, 63, 31, 1);
        } else if (plan.format == 3u) {
            color = float4((word >> 11u) & 31u, (word >> 6u) & 31u, (word >> 1u) & 31u,
                           word & 1u) / float4(31, 31, 31, 1);
        } else {
            color = float4((word >> 12u) & 15u, (word >> 8u) & 15u, (word >> 4u) & 15u,
                           word & 15u) / 15.0f;
        }
    }
    output.write(color, coordinate);
}

vertex float4 PicaLcdVertex(uint index [[vertex_id]]) {
    constexpr float2 triangle[]{{-1, -1}, {3, -1}, {-1, 3}};
    return float4(triangle[index], 0, 1);
}

struct LcdRect {
    float x, y, width, height;
};

fragment float4 PicaLcdSample(float4 position [[position]], constant LcdRect& rect [[buffer(0)]],
                              texture2d<float> source [[texture(0)]],
                              sampler filter [[sampler(0)]]) {
    const float2 local = (position.xy - float2(rect.x, rect.y)) /
                         float2(rect.width, rect.height);
    // Same corner mapping as RendererOpenGL::DrawScreens on an entire linear
    // backing. Memory row numbering is unchanged by the decoder.
    return source.sample(filter, float2(1.0f - local.y, local.x));
}

fragment float4 PicaLcdSolid(constant float4& color [[buffer(0)]]) {
    return color;
}

kernel void ExpandPicaLCD(texture2d<float, access::read> source [[texture(0)]],
                          texture2d<float, access::write> output [[texture(1)]],
                          constant uint& factor [[buffer(0)]],
                          uint2 coordinate [[thread_position_in_grid]]) {
    if ((factor != 2 && factor != 4) ||
        coordinate.x >= output.get_width() || coordinate.y >= output.get_height())
        return;

    // Match the canonical rotating scanout dimensions chosen by the host.
    const uint2 target(uint2(output.get_width(), output.get_height()) / factor);
    const uint2 extent(source.get_width(), source.get_height());
    const uint2 texel = min((coordinate / factor) * extent / target, extent - 1);
    output.write(source.read(texel), coordinate);
}
