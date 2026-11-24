// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace OpenGL {

// Shared by the opt-in runtime combiner for ordinary 2D materials.
// Texture lookup and all fragment operations outside the TEV remain specialized.
inline constexpr char MetalRuntimeTevHelpers[] = R"(
#ifdef CARA_LIT_RUNTIME_TEV
layout(std140) uniform cara_tev_control {
    ivec4 cara_tev_words[NUM_TEV_STAGES];
};
#define CARA_TEV_EXTRA_PARAMETERS , vec4 primaryLit, vec4 secondaryLit, vec4 tex2Value
#define CARA_TEV_EXTRA_ARGUMENTS , primaryLit, secondaryLit, tex2Value
#define CARA_TEV_WORD(i) cara_tev_words[i]
#else
#define CARA_TEV_EXTRA_PARAMETERS
#define CARA_TEV_EXTRA_ARGUMENTS
#define CARA_TEV_WORD(i) lighting_lut_offset[i]
#endif
vec4 caraTevSource(uint source, vec4 primary, vec4 tex0Value, vec4 tex1Value,
                   vec4 bufferValue, vec4 constantValue, vec4 previous CARA_TEV_EXTRA_PARAMETERS) {
    switch (source & 15u) {
    case 0u: return primary;
#ifdef CARA_LIT_RUNTIME_TEV
    case 1u: return primaryLit;
    case 2u: return secondaryLit;
    case 5u: return tex2Value;
#endif
    case 3u: return tex0Value;
    case 4u: return tex1Value;
    case 13u: return bufferValue;
    case 14u: return constantValue;
    case 15u: return previous;
    default: return vec4(0.0);
    }
}
vec3 caraTevColor(vec4 x, uint modifier) {
    vec3 result;
    switch ((modifier >> 1u) & 7u) {
    case 0u: result = x.rgb; break;
    case 1u: result = x.aaa; break;
    case 2u: result = x.rrr; break;
    case 4u: result = x.ggg; break;
    case 6u: result = x.bbb; break;
    default: return vec3(0.0);
    }
    return (modifier & 1u) != 0u ? vec3(1.0) - result : result;
}
float caraTevAlpha(vec4 x, uint modifier) {
    float result = x[((modifier >> 1u) + 3u) & 3u];
    return (modifier & 1u) != 0u ? 1.0 - result : result;
}
vec3 caraTevCombine(uint op, vec3 a, vec3 b, vec3 c) {
    vec3 result;
    switch (op & 15u) {
    case 0u: result = a; break;
    case 1u: result = a * b; break;
    case 2u: result = a + b; break;
    case 3u: result = a + b - 0.5; break;
    case 4u: result = a * c + b * (1.0 - c); break;
    case 5u: result = a - b; break;
    case 8u: result = a * b + c; break;
    case 9u: result = min(a + b, vec3(1.0)) * c; break;
    default: return vec3(0.0);
    }
    return byteround(clamp(result, vec3(0), vec3(1)));
}
vec4 caraRunTev(vec4 primary, vec4 tex0Value, vec4 tex1Value CARA_TEV_EXTRA_PARAMETERS) {
    vec4 previous = vec4(0);
    vec4 bufferValue = vec4(0);
    vec4 nextBuffer = tev_combiner_buffer_color;
    for (int i = 0; i < NUM_TEV_STAGES; ++i) {
        uvec4 w = uvec4(CARA_TEV_WORD(i));
        if ((w.w & 256u) == 0u) {
            vec3 colors[3];
            vec3 alphas;
            for (uint j = 0u; j < 3u; ++j) {
                uint shift = 4u * j;
                colors[j] = caraTevColor(caraTevSource(w.x >> shift, primary, tex0Value,
                    tex1Value, bufferValue, const_color[i], previous CARA_TEV_EXTRA_ARGUMENTS), w.y >> shift);
                alphas[j] = caraTevAlpha(caraTevSource(w.x >> (16u + shift), primary, tex0Value,
                    tex1Value, bufferValue, const_color[i], previous CARA_TEV_EXTRA_ARGUMENTS), w.y >> (12u + shift));
            }
            vec3 rgb = caraTevCombine(w.z, colors[0], colors[1], colors[2]);
            float a = caraTevCombine(w.z >> 16u, alphas.xxx, alphas.yyy, alphas.zzz).x;
            previous = clamp(vec4(rgb * float(w.w & 15u), a * float((w.w >> 4u) & 15u)),
                             vec4(0), vec4(1));
        }
        bufferValue = nextBuffer;
        if ((w.w & 4096u) != 0u) nextBuffer.rgb = previous.rgb;
        if ((w.w & 8192u) != 0u) nextBuffer.a = previous.a;
    }
    return previous;
}
)";
} // namespace OpenGL
