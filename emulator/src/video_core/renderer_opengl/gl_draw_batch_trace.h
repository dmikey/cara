// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <android/log.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace OpenGL::DrawBatchTrace {
// Observation only. A zero mask identifies a pair for inspection; it never authorizes a merge.
enum Break : u16 {
    Frame = 1, List = 2, Visibility = 4, Attachment = 8, Shader = 16,
    Primitive = 32, Raster = 64, Samplers = 128, Common = 256, Light = 512,
    VertexUniform = 1024, Layout = 2048, Payload = 4096, Storage = 8192, Feedback = 16384,
};
inline bool Enabled() {
    static const bool enabled = std::getenv("CARA_TRACE_DRAW_BATCHES") &&
        std::strcmp(std::getenv("CARA_TRACE_DRAW_BATCHES"), "1") == 0;
    return enabled;
}
inline thread_local u16 boundary = 0;
inline thread_local std::array<u32, 2> attachments{};
inline thread_local bool feedback_draw = false;
inline void Fence() { if (Enabled()) boundary |= Visibility; }

struct State {
    u32 frame, shader, pipeline, mode, route, count;
    u64 list, interrupts, lut_uploads;
    std::array<u32, 2> attachments;
    std::array<u32, 3> storage;
    OpenGLState gl;
    std::array<Pica::TexturingRegs::TextureConfig, 3> textures{};
    std::array<u32, 3> formats{};
    std::array<std::array<GLint, 4>, 16> layout;
    UniformData common;
    UniformLightData light;
    VSUniformData vs;
    bool feedback;
};

template<class T> inline bool Changed(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) != 0;
}
inline bool PayloadChanged(std::vector<u8>& old, const void* data, size_t bytes) {
    const bool changed = old.size() != bytes || (bytes && std::memcmp(old.data(), data, bytes));
    if (changed) {
        old.resize(bytes);
        if (bytes) std::memcpy(old.data(), data, bytes);
    }
    return changed;
}

inline void Record(const State& s, const void* vertices, size_t vb, const void* indices, size_t ib,
                   const void* elements, size_t eb) {
    static thread_local State prev{};
    static thread_local bool started = false;
    static thread_local std::vector<u8> vertex_data, index_data, element_data;
    static thread_local std::string masks;
    static thread_local unsigned offset = 0;
    const bool frame = !started || s.frame != prev.frame;
    if (!masks.empty() && (frame || masks.size() == 4 * 192)) {
        __android_log_print(ANDROID_LOG_INFO, "CaraPerf", "batch frame=%u offset=%u masks=%s",
                            prev.frame, offset, masks.c_str());
        offset += masks.size() / 4;
        masks.clear();
    }
    if (frame) offset = 0;
    const auto& a = prev.gl;
    const auto& b = s.gl;
    u16 bits = boundary;
    boundary = 0;
    if (frame) bits |= Frame;
    if (prev.list != s.list) bits |= List;
    if (prev.interrupts != s.interrupts) bits |= Visibility;
    if (s.attachments != prev.attachments) bits |= Attachment;
    if (s.shader != prev.shader || s.pipeline != prev.pipeline) bits |= Shader;
    if (s.mode != prev.mode || s.route != prev.route) bits |= Primitive;
    if (Changed(a.cull,b.cull) || Changed(a.depth,b.depth) || Changed(a.stencil,b.stencil) ||
        Changed(a.blend,b.blend) || a.logic_op != b.logic_op || Changed(a.color_mask,b.color_mask) ||
        Changed(a.viewport,b.viewport) || Changed(a.scissor,b.scissor) ||
        a.clip_distance != b.clip_distance) bits |= Raster;
    if (Changed(a.texture_units,b.texture_units) || Changed(a.texture_cube_unit,b.texture_cube_unit) ||
        Changed(prev.textures,s.textures) || prev.formats != s.formats) bits |= Samplers;
    if (Changed(prev.common,s.common)) bits |= Common;
    if (prev.lut_uploads != s.lut_uploads || Changed(prev.light,s.light)) bits |= Light;
    if (Changed(prev.vs.uniforms.i,s.vs.uniforms.i) || Changed(prev.vs.uniforms.f,s.vs.uniforms.f))
        bits |= VertexUniform;
    for (size_t i=0; i<s.vs.uniforms.bools.size(); ++i)
        if (prev.vs.uniforms.bools[i].b != s.vs.uniforms.bools[i].b) bits |= VertexUniform;
    if (s.layout != prev.layout) bits |= Layout;
    const bool v = PayloadChanged(vertex_data, vertices, vb);
    const bool i = PayloadChanged(index_data, indices, ib);
    const bool e = PayloadChanged(element_data, elements, eb);
    if (v || i || e || s.count != prev.count) bits |= Payload;
    if (s.storage != prev.storage) bits |= Storage;
    if (s.feedback || prev.feedback) bits |= Feedback;
    for (int shift = 12; shift >= 0; shift -= 4)
        masks.push_back("0123456789abcdef"[(bits >> shift) & 15]);
    prev = s;
    started = true;
}
} // namespace OpenGL::DrawBatchTrace
