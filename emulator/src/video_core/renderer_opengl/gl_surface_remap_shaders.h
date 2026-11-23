// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace OpenGL::SurfaceRemapShaders {
inline constexpr const char* Vertex = R"(
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";
    // Native GLES uses this shader without the atlas precision preamble.
inline constexpr const char* Fragment = R"(
#ifdef GL_ES
precision highp float;
precision highp int;
#endif
uniform highp sampler2D source_texture;
// Width, height, stride, tiled flag. Coordinates in textures are GL coordinates.
uniform highp ivec4 source_layout;
uniform highp ivec4 destination_layout;
uniform highp int byte_offset;
uniform highp ivec2 bytes_per_pixel;
uniform highp int native_reverse;
out highp vec4 color;

int interleave(int x, int y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
           ((x & 4) << 2) | ((y & 4) << 3);
}
int deinterleave(int value) {
    return (value & 1) | ((value >> 1) & 2) | ((value >> 2) & 4);
}
vec4 guest_texel(int pixel) {
    ivec2 source;
    if (source_layout.w != 0) {
        int tile = pixel / 64;
        int within = pixel & 63;
        source.x = (tile % (source_layout.z / 8)) * 8 + deinterleave(within);
        source.y = source_layout.y - 1 -
                   ((tile / (source_layout.z / 8)) * 8 + deinterleave(within >> 1));
    } else {
        source = ivec2(pixel % source_layout.z, pixel / source_layout.z);
    }
    return texelFetch(source_texture, source, 0);
}
// Read a byte in guest order, undoing GLES upload byte reversal.
float guest_byte(int offset) {
    return guest_texel(offset / bytes_per_pixel.x)[bytes_per_pixel.x - 1 - (offset % bytes_per_pixel.x)];
}
void main() {
    ivec2 destination = ivec2(gl_FragCoord.xy);
    int pixel;
    if (destination_layout.w != 0) {
        int y = destination_layout.y - 1 - destination.y;
        pixel = (y / 8) * destination_layout.z * 8 + (destination.x / 8) * 64 +
                interleave(destination.x & 7, y & 7);
    } else {
        pixel = destination.y * destination_layout.z + destination.x;
    }
    int offset = pixel * bytes_per_pixel.y + byte_offset;
    if (bytes_per_pixel.x == bytes_per_pixel.y && byte_offset % bytes_per_pixel.x == 0) {
        // Preserve the original one-fetch path for equal formats.
        int source_pixel = offset / bytes_per_pixel.x;
        ivec2 source;
        if (source_layout.w != 0) {
            int tile = source_pixel / 64;
            int within = source_pixel & 63;
            source = ivec2((tile % (source_layout.z / 8)) * 8 + deinterleave(within),
                          source_layout.y - 1 -
                          ((tile / (source_layout.z / 8)) * 8 + deinterleave(within >> 1)));
        } else {
            source = ivec2(source_pixel % source_layout.z, source_pixel / source_layout.z);
        }
        color = texelFetch(source_texture, source, 0);
        if (native_reverse != 0) color = color.abgr;
#ifdef CARA_REMAP_TEXEL_PAIR
    } else if (bytes_per_pixel.x == 3 && bytes_per_pixel.y == 4) {
        int first = offset / 3;
        vec3 a = guest_texel(first).rgb;
        vec3 b = guest_texel(first + 1).rgb;
        int lane = offset - 3 * first;
        if (lane == 0) color = vec4(b.b, a.rgb);
        else if (lane == 1) color = vec4(b.gb, a.rg);
        else color = vec4(b.rgb, a.r);
    } else if (bytes_per_pixel.x == 4 && bytes_per_pixel.y == 3) {
        int first = offset / 4;
        vec4 a = guest_texel(first);
        int lane = offset & 3;
        if (lane == 0) color = vec4(a.gba, 1.0);
        else if (lane == 1) color = vec4(a.rgb, 1.0);
        else {
            vec4 b = guest_texel(first + 1);
            color = lane == 2 ? vec4(b.a, a.rg, 1.0) : vec4(b.ba, a.r, 1.0);
        }
#endif
    } else if (bytes_per_pixel.y == 4) {
        color = vec4(guest_byte(offset + 3), guest_byte(offset + 2),
                     guest_byte(offset + 1), guest_byte(offset));
    } else {
        color = vec4(guest_byte(offset + 2), guest_byte(offset + 1),
                     guest_byte(offset), 1.0);
    }
}
)";
} // namespace OpenGL::SurfaceRemapShaders
