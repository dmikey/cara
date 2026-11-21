#include <metal_stdlib>
using namespace metal;

struct CopyArguments {
    uint count;
    uint source_width;
    uint source_stride;
    uint destination_width;
    uint destination_stride;
    uint reversed_source_rows;
};

struct FillArguments {
    uint count;
    uint pattern;
    uint period;
};

kernel void cara_pica_copy_rows(const device uint* source [[buffer(0)]],
                                device uint* destination [[buffer(1)]],
                                constant CopyArguments& args [[buffer(2)]],
                                uint word [[thread_position_in_grid]]) {
    if (word >= args.count) {
        return;
    }
    const uint source_row = word / args.source_width;
    const uint read_row = args.reversed_source_rows == 0
                              ? source_row
                              : args.reversed_source_rows - 1 - source_row;
    const uint read_word = read_row * args.source_stride + word % args.source_width;
    const uint write_word =
        (word / args.destination_width) * args.destination_stride + word % args.destination_width;
    destination[write_word] = source[read_word];
}

kernel void cara_pica_fill_words(device uint* destination [[buffer(0)]],
                                 constant FillArguments& args [[buffer(1)]],
                                 uint word [[thread_position_in_grid]]) {
    if (word >= args.count) {
        return;
    }
    if (args.period == 4) {
        destination[word] = args.pattern;
    } else if (args.period == 2) {
        const uint halfword = args.pattern & 0xFFFF;
        destination[word] = halfword | (halfword << 16);
    } else {
        uint value = 0;
        for (uint index = 0; index < 4; ++index) {
            const uint phase = (word * 4 + index) % 3;
            value |= ((args.pattern >> (phase * 8)) & 0xFF) << (index * 8);
        }
        destination[word] = value;
    }
}

// The output texture uses the linear guest row convention. The packed byte
// representation is independently consumable by LCD or a CPU visibility read.
kernel void cara_pica_display_to_linear_color(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    device uchar* guest_bytes [[buffer(0)]],
    constant uint2& args [[buffer(1)]], uint2 xy [[thread_position_in_grid]]) {
    const uint width = destination.get_width();
    const uint height = destination.get_height();
    if (xy.x >= width || xy.y >= height)
        return;

    const uint source_y = (args.y & 1) ? height - 1 - xy.y : xy.y;
    const bool scale_x = (args.y & 2) != 0;
    const uint source_x = xy.x << uint(scale_x);
    float4 color = source.read(uint2(source_x, source_y));
    if (scale_x)
        color = (color + source.read(uint2(source_x + 1, source_y))) * 0.5f;
    const uchar4 packed = uchar4(rint(color * 255.0f));
    const uint offset = (xy.y * width + xy.x) * args.x;
    const uint alpha_bytes = args.x - 3;
    guest_bytes[offset + alpha_bytes] = packed.b;
    guest_bytes[offset + alpha_bytes + 1] = packed.g;
    guest_bytes[offset + alpha_bytes + 2] = packed.r;
    if (alpha_bytes)
        guest_bytes[offset] = packed.a;
    else
        color.a = 1.0f;
    destination.write(color, xy);
}

// Change guest layout without subjecting source bytes to texture sampling.
static uint pica_tiled_word(uint x, uint y, uint width) {
    const uint morton = (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) |
                        ((y & 2u) << 2u) | ((x & 4u) << 2u) | ((y & 4u) << 3u);
    return (y & ~7u) * width + (x & ~7u) * 8u + morton;
}

static uint3 read_rgb565(const device uchar* source, uint offset) {
    const uint packed = uint(source[offset]) | (uint(source[offset + 1u]) << 8u);
    const uint3 channels = uint3(packed >> 11u, (packed >> 5u) & 63u, packed & 31u);
    return (channels << uint3(3u, 2u, 3u)) | (channels >> uint3(2u, 4u, 2u));
}

kernel void cara_pica_display_raw_rgba_layout(
    const device uchar* source [[buffer(0)]], device uchar* destination [[buffer(1)]],
    constant uint* args [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    const uint width = args[0];
    const uint output_width = args[4];
    const uint output_height = args[5];
    if (xy.x >= output_width || xy.y >= output_height)
        return;
    const uint y = args[2] ? output_height - 1 - xy.y : xy.y;
    const uint x = xy.x;
    if (args[3] == 8u) {
        const uint from = 4u * pica_tiled_word(2u * x, 2u * xy.y, width);
        const uint to = 4u * pica_tiled_word(x, y, output_width);
        for (uint byte = 0; byte < 4u; ++byte) {
            const uint channel = from + byte;
            destination[to + byte] = uchar((uint(source[channel]) + source[channel + 4u] +
                                           source[channel + 8u] + source[channel + 12u]) >> 2u);
        }
        return;
    }
    if (args[3] >= 5u) {
        const bool scale_xy = args[3] == 7u;
        const uint first = pica_tiled_word(2u * x, scale_xy ? 2u * xy.y : xy.y, width);
        uint packed;
        if (args[3] != 5u) {
            // Match the guest's expand, average, and truncate order.
            const uint from = first * 2u;
            uint3 rgb = read_rgb565(source, from) + read_rgb565(source, from + 2u);
            if (scale_xy)
                rgb = (rgb + read_rgb565(source, from + 4u) + read_rgb565(source, from + 6u)) >> 1u;
            rgb >>= uint3(4u, 3u, 4u);
            packed = (rgb.r << 11u) | (rgb.g << 5u) | rgb.b;
        } else {
            const uint from = first * 4u;
            const uint r = (uint(source[from + 3u]) + source[from + 7u]) >> 4u;
            const uint g = (uint(source[from + 2u]) + source[from + 6u]) >> 3u;
            const uint b = (uint(source[from + 1u]) + source[from + 5u]) >> 4u;
            packed = (r << 11u) | (g << 5u) | b;
        }
        const uint to = 2u * (scale_xy ? pica_tiled_word(x, y, output_width) : y * output_width + x);
        destination[to] = uchar(packed);
        destination[to + 1u] = uchar(packed >> 8u);
        return;
    }
    if (args[3] == 1) {
        // Packed RGBA4 and RGB565 uses the same two bytes on either side of the transfer.
        const uint from = 2u * pica_tiled_word(x, xy.y, width);
        const uint to = 2u * (y * output_width + x);
        destination[to] = source[from];
        destination[to + 1u] = source[from + 1u];
        return;
    }
    const bool to_tiles = args[3] == 0;
    const uint from = 4u * (to_tiles ? xy.y * width + x : pica_tiled_word(x, xy.y, width));
    const uint channels = to_tiles ? 4u : args[3];
    const uint to = channels * (to_tiles ? pica_tiled_word(x,y,width) : y * output_width + x);
    if (channels == 2u) {
        // The GLES reference transfer renders to an RGB565 UNORM
        // attachment. Quantize to the closest representable color.
        const uint r = (uint(source[from + 3u]) * 31u + 127u) / 255u;
        const uint g = (uint(source[from + 2u]) * 63u + 127u) / 255u;
        const uint b = (uint(source[from + 1u]) * 31u + 127u) / 255u;
        const uint packed = (r << 11u) | (g << 5u) | b;
        destination[to] = uchar(packed);
        destination[to + 1u] = uchar(packed >> 8u);
    } else {
        for (uint i = 0; i < channels; ++i)
            destination[to + i] = source[from + (4u - channels) + i];
    }
}
