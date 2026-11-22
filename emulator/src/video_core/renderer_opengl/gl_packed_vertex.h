// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
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
inline bool PackedVertexRollingFetchEnabled() {
    const char* flag = std::getenv("CARA_GPU_PACKED_ROLLING_FETCH");
    return flag && std::string_view{flag} == "1";
}

// Keep the existing byte extraction and layout specialization intact. Only the
// two fetch sites gain a per-input-vertex, one-texel memo between attributes.
inline std::string MakePackedVertexRollingFetchDecoder(std::string_view decoder) {
    constexpr std::string_view declaration = "uniform highp ivec4 cara_vertex_layout[16];";
    constexpr std::string_view fetch = "texelFetch(cara_vertex_inputs, ivec2(";
    const auto layout = decoder.find(declaration);
    if (layout == decoder.npos) return {};
    std::string source(decoder);
    for (const std::string_view texel : {"texel", "next"}) {
        const std::string query = std::string(fetch) + std::string(texel) + " % 128, " +
                                  std::string(texel) + " / 128), 0)";
        const auto position = source.find(query);
        if (position == source.npos) return {};
        source.replace(position, query.size(), "cara_packed_fetch(" + std::string(texel) + ")");
    }
    source.insert(layout + declaration.size(), R"(
highp int cara_packed_cached_texel;
highp uvec4 cara_packed_cached_data;
highp uvec4 cara_packed_fetch(highp int texel) {
    if (texel != cara_packed_cached_texel) {
        cara_packed_cached_texel = texel;
        cara_packed_cached_data =
            texelFetch(cara_vertex_inputs, ivec2(texel % 128, texel / 128), 0);
    }
    return cara_packed_cached_data;
}
)");
    return source;
}

// Only descriptor integers are specialized. The original uniform remains live
// for the raw-index header stored in register zero's high component-count bits.
using PackedVertexLayout = std::array<std::array<std::int32_t, 4>, 16>;

inline constexpr std::size_t MaxSpecializedPackedLayouts = 64;
inline constexpr std::size_t MaxSpecializedPackedLayoutsPerProgram = 8;
inline constexpr bool CanAdmitPackedVertexLayout(std::size_t total, std::size_t family) {
    return total < MaxSpecializedPackedLayouts && family < MaxSpecializedPackedLayoutsPerProgram;
}


inline PackedVertexLayout CanonicalPackedVertexLayout(PackedVertexLayout layout) {
    for (auto& descriptor : layout)
        descriptor[3] &= 7;
    return layout;
}

inline std::string SpecializePackedVertexDecoder(std::string_view decoder,
                                                const PackedVertexLayout& layout) {
    constexpr std::string_view anchor = "ivec4 info = cara_vertex_layout[reg];";
    const auto position = decoder.find(anchor);
    if (position == decoder.npos || decoder.find(anchor, position + anchor.size()) != decoder.npos)
        return {}; // Unknown generator form: caller retains the generic decoder.
    const auto canonical = CanonicalPackedVertexLayout(layout);
    std::string table = "const ivec4 cara_known_vertex_layout[16] = ivec4[16](\n";
    for (std::size_t reg = 0; reg < canonical.size(); ++reg) {
        table += "ivec4(";
        for (unsigned component = 0; component < 4; ++component) {
            if (component) table += ",";
            table += std::to_string(canonical[reg][component]);
        }
        table += reg + 1 == canonical.size() ? "));\n" : "),\n";
    }
    std::string source(decoder);
    // Preserve the audited raw-index and unique-vertex rewrite anchor. Their
    // inserted count mask is harmless before this already canonical assignment.
    source.insert(position + anchor.size(), "\n    info = cara_known_vertex_layout[reg];");
    source.insert(0, table);
    return source;
}
} // namespace OpenGL
