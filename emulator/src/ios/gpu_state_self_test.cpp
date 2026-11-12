// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/gpu_state_self_test.h"

#include <array>
#include <algorithm>
#include <glad/glad.h>
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "common/scope_exit.h"

namespace {
using OpenGL::OpenGLState;

bool CheckStreamBuffers(std::string& error) {
    const GLuint saved = OpenGLState::GetCurState().draw.vertex_buffer;
    OpenGL::OGLStreamBuffer buffer(GL_ARRAY_BUFFER, 128);
    OpenGLState::BindVertexBuffer(buffer.GetHandle());
    bool passed = true;
    const auto check = [&](GLsizeiptr size, GLintptr wanted_offset, bool wanted_invalidate, u8 value) {
        auto [data, offset, invalidated] = buffer.Map(size, 16);
        if (!data) { error = "stream map failed"; return false; }
        std::fill_n(data, size, value);
        buffer.Unmap(size);
        bool good = offset == wanted_offset && invalidated == wanted_invalidate;
        const auto* read = static_cast<const u8*>(glMapBufferRange(GL_ARRAY_BUFFER, offset, size, GL_MAP_READ_BIT));
        if (read) {
            good &= std::all_of(read, read + size, [&](u8 byte) { return byte == value; });
            good &= glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE;
        } else good = false;
        good &= glGetError() == GL_NO_ERROR;
        if (!good) error = "stream flush, wrap or range visibility mismatch";
        return good;
    };
    passed = check(64, 0, false, 0x35);
    if (passed) passed = check(96, 0, true, 0xA7);
    if (passed) passed = check(16, 96, false, 0x59);
    OpenGLState::BindVertexBuffer(saved);
    return passed;
}

bool CheckRasterState(const OpenGLState& expected, const char* stage, std::string& error) {
    const auto integer = [&](GLenum name, GLint wanted) {
        GLint actual = 0;
        glGetIntegerv(name, &actual);
        if (actual != wanted && error.empty()) {
            error = std::string(stage) + ": state " + std::to_string(name) + " expected=" +
                    std::to_string(wanted) + " actual=" + std::to_string(actual);
        }
    };
    const auto enabled = [&](GLenum name, bool wanted) {
        if ((glIsEnabled(name) == GL_TRUE) != wanted && error.empty()) {
            error = std::string(stage) + ": enable mismatch " + std::to_string(name);
        }
    };
    enabled(GL_CULL_FACE, expected.cull.enabled);
    integer(GL_CULL_FACE_MODE, expected.cull.mode);
    integer(GL_FRONT_FACE, expected.cull.front_face);
    enabled(GL_DEPTH_TEST, expected.depth.test_enabled);
    integer(GL_DEPTH_FUNC, expected.depth.test_func);
    integer(GL_DEPTH_WRITEMASK, expected.depth.write_mask);
    enabled(GL_STENCIL_TEST, expected.stencil.test_enabled);
    integer(GL_STENCIL_FUNC, expected.stencil.test_func);
    integer(GL_STENCIL_REF, expected.stencil.test_ref);
    integer(GL_STENCIL_VALUE_MASK, expected.stencil.test_mask);
    integer(GL_STENCIL_WRITEMASK, expected.stencil.write_mask);
    integer(GL_STENCIL_FAIL, expected.stencil.action_stencil_fail);
    integer(GL_STENCIL_PASS_DEPTH_FAIL, expected.stencil.action_depth_fail);
    integer(GL_STENCIL_PASS_DEPTH_PASS, expected.stencil.action_depth_pass);
    enabled(GL_BLEND, expected.blend.enabled);
    integer(GL_BLEND_SRC_RGB, expected.blend.src_rgb_func);
    integer(GL_BLEND_DST_RGB, expected.blend.dst_rgb_func);
    integer(GL_BLEND_SRC_ALPHA, expected.blend.src_a_func);
    integer(GL_BLEND_DST_ALPHA, expected.blend.dst_a_func);
    integer(GL_BLEND_EQUATION_RGB, expected.blend.rgb_equation);
    integer(GL_BLEND_EQUATION_ALPHA, expected.blend.a_equation);
    enabled(GL_SCISSOR_TEST, expected.scissor.enabled);
    std::array<GLint, 4> box{};
    glGetIntegerv(GL_SCISSOR_BOX, box.data());
    if (box != std::array<GLint, 4>{expected.scissor.x, expected.scissor.y,
                                  expected.scissor.width, expected.scissor.height} && error.empty()) {
        error = std::string(stage) + ": scissor mismatch";
    }
    glGetIntegerv(GL_VIEWPORT, box.data());
    if (box != std::array<GLint, 4>{expected.viewport.x, expected.viewport.y,
                                  expected.viewport.width, expected.viewport.height} && error.empty()) {
        error = std::string(stage) + ": viewport mismatch";
    }
    std::array<GLboolean, 4> mask{};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask.data());
    if (mask != std::array<GLboolean, 4>{expected.color_mask.red_enabled,
                                       expected.color_mask.green_enabled,
                                       expected.color_mask.blue_enabled,
                                       expected.color_mask.alpha_enabled} && error.empty()) {
        error = std::string(stage) + ": color mask mismatch";
    }
    std::array<GLfloat, 4> color{};
    glGetFloatv(GL_BLEND_COLOR, color.data());
    if (color != std::array<GLfloat, 4>{expected.blend.color.red, expected.blend.color.green,
                                      expected.blend.color.blue, expected.blend.color.alpha} && error.empty()) {
        error = std::string(stage) + ": blend color mismatch";
    }
    const GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR && error.empty()) {
        error = std::string(stage) + ": GL error " + std::to_string(gl_error);
    }
    return error.empty();
}
} // namespace

bool RunGPUStateSelfTest(std::string& error) {
    // This oracle runs before RendererOpenGL::Init initializes the global API mode.
    const bool old_gles = OpenGL::GLES;
    const bool old_shadow = OpenGL::AllowShadow;
    OpenGL::GLES = true;
    OpenGL::AllowShadow = false;
    SCOPE_EXIT({ OpenGL::GLES = old_gles; OpenGL::AllowShadow = old_shadow; });
    if (!CheckStreamBuffers(error)) return false;
    const auto saved = OpenGLState::GetCurState();
    OpenGLState state = saved;
    state.cull.enabled = false;
    state.cull.mode = GL_FRONT;
    state.cull.front_face = GL_CW;
    state.depth.test_enabled = true;
    state.depth.test_func = GL_GEQUAL;
    state.depth.write_mask = GL_FALSE;
    state.color_mask = {GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE};
    state.stencil.test_enabled = true;
    state.stencil.test_func = GL_NOTEQUAL;
    state.stencil.test_ref = 0;
    state.stencil.test_mask = 0x5A;
    state.stencil.write_mask = 0x3C;
    state.stencil.action_stencil_fail = GL_INVERT;
    state.stencil.action_depth_fail = GL_INCR_WRAP;
    state.stencil.action_depth_pass = GL_REPLACE;
    state.blend.enabled = true;
    state.blend.rgb_equation = GL_FUNC_REVERSE_SUBTRACT;
    state.blend.a_equation = GL_FUNC_ADD;
    state.blend.src_rgb_func = GL_SRC_ALPHA;
    state.blend.dst_rgb_func = GL_ONE_MINUS_SRC_ALPHA;
    state.blend.src_a_func = GL_ONE;
    state.blend.dst_a_func = GL_ZERO;
    state.blend.color = {0.25f, 0.5f, 0.75f, 1.0f};
    state.scissor = {true, 2, 3, 17, 19};
    state.viewport = {-1, -2, 31, 29};
    OpenGLState::InvalidateRasterState();
    state.Apply();
    bool passed = CheckRasterState(state, "full apply", error);
    if (passed) {
        state.Apply();
        passed = CheckRasterState(state, "unchanged full apply", error);
    }
    if (passed) {
        // Fresh defaults deliberately differ in fields that SubApply must not
        // overwrite, such as blending, culling, depth function and stencil ops.
        OpenGLState partial;
        partial.draw.read_framebuffer = state.draw.read_framebuffer;
        partial.draw.draw_framebuffer = state.draw.draw_framebuffer;
        partial.depth.write_mask = GL_TRUE;
        partial.stencil.write_mask = 0xA5;
        partial.color_mask = {GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE};
        partial.scissor = {false, 7, 8, 9, 10};
        partial.viewport = {4, 5, 23, 24};
        auto expected = state;
        expected.depth.write_mask = partial.depth.write_mask;
        expected.stencil.write_mask = partial.stencil.write_mask;
        expected.color_mask = partial.color_mask;
        expected.scissor = partial.scissor;
        expected.viewport = partial.viewport;
        partial.SubApply();
        passed = CheckRasterState(expected, "partial apply", error);
        if (passed) {
            OpenGLState::GetCurState().Apply();
            passed = CheckRasterState(expected, "tracked partial state", error);
        }
        if (passed) {
            state.Apply();
            passed = CheckRasterState(state, "restore after partial", error);
        }
    }
    if (passed) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glViewport(0, 0, 1, 1);
        OpenGLState::InvalidateRasterState();
        state.Apply();
        passed = CheckRasterState(state, "restore after manual mutation", error);
    }
    OpenGLState::InvalidateRasterState();
    saved.Apply();
    return passed;
}
