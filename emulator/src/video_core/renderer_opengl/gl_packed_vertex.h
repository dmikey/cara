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
} // namespace OpenGL
