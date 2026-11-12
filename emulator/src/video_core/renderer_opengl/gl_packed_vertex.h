// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace OpenGL {
// Byte addresses support interleaved, unaligned and separately loaded attributes.
inline constexpr const char* PackedVertexDecoderGLSL = R"(
uniform highp usampler2D cara_vertex_inputs;
uniform highp ivec4 cara_vertex_layout[16];
uint cara_raw_word(int word) {
    uvec4 value = texelFetch(cara_vertex_inputs, ivec2((word / 4) % 128, (word / 4) / 128), 0);
    return value[word % 4];
}
uint cara_raw_bits(int address, int bytes) {
    int shift = (address & 3) * 8;
    uint value = cara_raw_word(address / 4) >> uint(shift);
    if (shift + bytes * 8 > 32)
        value |= cara_raw_word(address / 4 + 1) << uint(32 - shift);
    return bytes == 4 ? value : value & ((1u << uint(bytes * 8)) - 1u);
}
vec4 cara_decode_vertex(int vertex, int reg) {
    ivec4 info = cara_vertex_layout[reg];
    int address = info.x + vertex * info.y;
    int bytes = info.z == 3 ? 4 : (info.z == 2 ? 2 : 1);
    vec4 value = vec4(0.0, 0.0, 0.0, 1.0);
    for (int c = 0; c < 4; ++c) {
        if (c >= info.w) break;
        uint bits = cara_raw_bits(address + c * bytes, bytes);
        if (info.z == 3) value[c] = uintBitsToFloat(bits);
        else if (info.z == 2) value[c] = float(int(bits << 16u) >> 16);
        else if (info.z == 0) value[c] = float(int(bits << 24u) >> 24);
        else value[c] = float(bits);
    }
    return value;
}
)";

// A packed attribute occupies at most 16 bytes, so it can touch at most two
// RGBA32UI texels. Reuse those fetched words for every component rather than
// issuing another texture lookup for each scalar or split word.
inline constexpr const char* PackedVertexFetchReuseGLSL = R"(
uniform highp usampler2D cara_vertex_inputs;
uniform highp ivec4 cara_vertex_layout[16];
uint cara_local_word(uvec4 first, uvec4 second, int word) {
    return word < 4 ? first[word] : second[word - 4];
}
vec4 cara_decode_vertex(int vertex, int reg) {
    ivec4 info = cara_vertex_layout[reg];
    if (info.w <= 0) return vec4(0.0, 0.0, 0.0, 1.0);
    int address = info.x + vertex * info.y;
    int bytes = info.z == 3 ? 4 : (info.z == 2 ? 2 : 1);
    int texel = address / 16;
    int offset = address & 15;
    uvec4 first = texelFetch(cara_vertex_inputs, ivec2(texel % 128, texel / 128), 0);
    uvec4 second = uvec4(0u);
    if (offset + info.w * bytes > 16) {
        int next = texel + 1;
        second = texelFetch(cara_vertex_inputs, ivec2(next % 128, next / 128), 0);
    }
    vec4 value = vec4(0.0, 0.0, 0.0, 1.0);
    for (int c = 0; c < 4; ++c) {
        if (c >= info.w) break;
        int component = offset + c * bytes;
        int word = component / 4;
        int shift = (component & 3) * 8;
        uint bits = cara_local_word(first, second, word) >> uint(shift);
        if (shift + bytes * 8 > 32)
            bits |= cara_local_word(first, second, word + 1) << uint(32 - shift);
        if (bytes != 4) bits &= (1u << uint(bytes * 8)) - 1u;
        if (info.z == 3) value[c] = uintBitsToFloat(bits);
        else if (info.z == 2) value[c] = float(int(bits << 16u) >> 16);
        else if (info.z == 0) value[c] = float(int(bits << 24u) >> 24);
        else value[c] = float(bits);
    }
    return value;
}
)";
} // namespace OpenGL
