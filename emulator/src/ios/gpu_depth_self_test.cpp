// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include "ios/gpu_depth_self_test.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"

bool RunGPUDepthSelfTest(bool quantize, std::string& error, std::array<bool, 2>* formats) {
    using namespace OpenGL;
    GLint old_draw = 0, old_read = 0;
    GLfloat old_clear_depth = 1;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
    std::array<GLint, 4> old_viewport{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_VIEWPORT, old_viewport.data());
    OGLShader vertex, fragment;
    vertex.Create("void main(){ vec2 p = gl_VertexID == 0 ? vec2(-1.0,-1.0) : (gl_VertexID == 1 ? vec2(3.0,-1.0) : vec2(-1.0,3.0)); gl_Position=vec4(p,0.0,1.0); }", GL_VERTEX_SHADER);
    std::string fs = "uniform float test_depth; uniform float levels; uniform vec4 test_color; out vec4 color; void main(){ color=test_color; gl_FragDepth=";
    fs += quantize ? "floor(test_depth * levels) / levels; }" : "test_depth; }";
    fragment.Create(fs.c_str(), GL_FRAGMENT_SHADER);
    OGLProgram program;
    program.handle = glCreateProgram();
    glAttachShader(program.handle, vertex.handle); glAttachShader(program.handle, fragment.handle);
    glLinkProgram(program.handle);
    GLint linked = 0; glGetProgramiv(program.handle, GL_LINK_STATUS, &linked);
    if (!linked) { error = "depth fixture linkage failed"; return false; }
    OGLFramebuffer framebuffer; framebuffer.Create();
    OGLTexture color, depth; color.Create(); depth.Create();
    OGLVertexArray vao; vao.Create();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, color.handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.handle);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.handle, 0);
    glViewport(0, 0, 1, 1); glBindVertexArray(vao.handle); glUseProgram(program.handle);
    glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GEQUAL); glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    bool passed = true;
    for (unsigned bits : {16u, 24u}) {
        glBindTexture(GL_TEXTURE_2D, depth.handle);
        if (bits == 16) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 1, 1, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, bits == 16 ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, depth.handle, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            passed = false; error = "depth fixture framebuffer incomplete"; break;
        }
        glClearColor(0, 0, 0, 0); glClearDepthf(0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const float levels = float((1u << bits) - 1);
        const float n = bits == 16 ? 100.0f : 100000.0f;
        glUniform1f(glGetUniformLocation(program.handle, "levels"), levels);
        // Both depths truncate to n. GEQUAL must therefore let the second draw win.
        glUniform1f(glGetUniformLocation(program.handle, "test_depth"), (n + 0.75f) / levels);
        glUniform4f(glGetUniformLocation(program.handle, "test_color"), 1, 0, 0, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform1f(glGetUniformLocation(program.handle, "test_depth"), (n + 0.25f) / levels);
        glUniform4f(glGetUniformLocation(program.handle, "test_color"), 0, 1, 0, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<unsigned char, 4> pixel{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        const bool matches = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0;
        if (formats) (*formats)[bits == 16 ? 0 : 1] = matches;
        if (!matches) {
            passed = false; error = "depth tie differs from integer PICA reference for D" + std::to_string(bits);
        }
    }
    glDisable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE); glClearDepthf(old_clear_depth);
    glBindVertexArray(0); glUseProgram(0); glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    return passed;
}
