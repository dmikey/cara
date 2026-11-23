// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone EGL process. Exercises Cara's actual GLAD recorder without starting the emulator.
#include <EGL/egl.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include "video_core/renderer_opengl/gl_surface_remap_shaders.h"
#include "video_core/renderer_opengl/gl_shared_vertex_shadow.h"
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>

#include "common/host_render_queue.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include "video_core/renderer_opengl/gl_ios_compat.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace {
namespace Queue = OpenGL::CommandQueue;
constexpr int width = 8;
using Pixels = std::array<unsigned char, 4 * width>;
PFNGLDRAWARRAYSPROC driver_draw;
thread_local bool submission_worker = false;
unsigned direct_draws = 0, queued_draws = 0;

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void APIENTRY CountDraw(GLenum mode, GLint first, GLsizei count) {
    Check(eglGetCurrentContext() != EGL_NO_CONTEXT, "driver draw without current context");
    ++(submission_worker ? queued_draws : direct_draws);
    driver_draw(mode, first, count);
}

struct Context {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    Context(EGLDisplay display, EGLConfig config, EGLContext share) : display(display) {
        const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, 1, EGL_NONE};
        const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, size);
        context = eglCreateContext(display, config, share, version);
        Check(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT, "create EGL context/pbuffer");
    }
    void Bind() const {
        Check(eglMakeCurrent(display, surface, surface, context), "bind EGL context");
    }
    void Release() const {
        Check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release EGL context");
    }
    ~Context() {
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
    }
};

GLuint Compile(GLenum stage, const char* source) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    Check(compiled == GL_TRUE, "probe shader compilation");
    return shader;
}

// Use the same float texture format, width and padded rows as all 24 lighting tables plus fog.
// Queue both generations and mutate the source before reading either rendered target.
void CheckLightingImages() {
    constexpr int w = OpenGL::LUTTextureWidth, h = 7;
    GLuint textures[3], fbo, vao;
    glGenTextures(3, textures);
    glGenFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 1; i <= 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    }
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glDisable(GL_DITHER);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    auto vs = Compile(GL_VERTEX_SHADER, R"(#version 300 es
void main() { vec2 p=vec2((gl_VertexID<<1)&2, gl_VertexID&2); gl_Position=vec4(p*2.-1.,0.,1.); })");
    auto fs = Compile(GL_FRAGMENT_SHADER, R"(#version 300 es
precision highp float;
uniform highp sampler2D lut;
out vec4 color;
void main() { color=vec4(texelFetch(lut,ivec2(gl_FragCoord.xy),0).rg,0.,1.); })");
    auto program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    Check(linked, "lighting image program link");
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program,"lut"),0);
    std::vector<float> table(2*w*h);
    auto value = [](std::size_t i, unsigned generation) { return (i*7 + generation*61) % 256; };
    for (unsigned generation = 0; generation < 2; ++generation) {
        for (std::size_t i = 0; i < table.size(); ++i)
            table[i] = static_cast<float>(value(i,generation)) / 255.f;
        glActiveTexture(GL_TEXTURE3);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
        OpenGL::ReplaceLightingLUTImage(textures[0], 0, reinterpret_cast<const u8*>(table.data()),
                                        table.size()*sizeof(float));
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[generation+1], 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    std::fill(table.begin(), table.end(), 0.f);
    GLint saved = 0;
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &saved);
    Check(saved == 4, "lighting upload lost unpack state");
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved);
    Check(saved == GL_TEXTURE3, "lighting upload lost active unit");
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glActiveTexture(GL_TEXTURE0);
    std::vector<u8> pixels(4*w*h);
    for (unsigned generation = 0; generation < 2; ++generation) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[generation+1], 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int i = 0; i < w*h; ++i)
            Check(pixels[i*4] == value(i*2,generation) && pixels[i*4+1] == value(i*2+1,generation)
                  && pixels[i*4+2] == 0 && pixels[i*4+3] == 255, "lighting image pixel mismatch");
    }
    Check(glGetError() == GL_NO_ERROR, "lighting image GLES error");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(3, textures);
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    std::puts("two complete lighting images matched all 14336 pixels");
}

void CheckRemapPixels() {
    using namespace OpenGL::SurfaceRemapShaders;
    constexpr int source_width = 32, dest_width = 16;
    GLuint textures[2], fbo, vao;
    glGenTextures(2, textures);
    glGenFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, dest_width, dest_width);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "remap destination");
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glDisable(GL_DITHER);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, dest_width, dest_width);
    const auto vs = Compile(GL_VERTEX_SHADER, (std::string("#version 300 es\n") + Vertex).c_str());
    GLuint programs[2]{};
    for (int variant = 0; variant < 2; ++variant) {
        const auto prefix = variant ? "#version 300 es\n#define CARA_REMAP_TEXEL_PAIR\n" : "#version 300 es\n";
        const auto fs = Compile(GL_FRAGMENT_SHADER, (std::string(prefix) + Fragment).c_str());
        programs[variant] = glCreateProgram();
        glAttachShader(programs[variant], vs);
        glAttachShader(programs[variant], fs);
        glLinkProgram(programs[variant]);
        GLint linked;
        glGetProgramiv(programs[variant], GL_LINK_STATUS, &linked);
        Check(linked == GL_TRUE, "remap link");
        glDeleteShader(fs);
    }
    std::array<GLubyte, source_width * source_width * 4> data{};
    uint32_t rng = 73;
    for (auto& byte : data) {
        rng = rng * 1664525u + 1013904223u;
        byte = static_cast<GLubyte>(rng >> 24);
    }
    unsigned cases = 0;
    for (int src : {3, 4}) {
        const GLenum format = src == 3 ? GL_RGB : GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, src == 3 ? GL_RGB8 : GL_RGBA8, source_width,
                     source_width, 0, format, GL_UNSIGNED_BYTE, data.data());
        for (int dst : {3, 4}) for (int tiled_src : {0, 1}) for (int tiled_dst : {0, 1}) {
            for (int offset : {0, 1, 2, 3, 181}) {
                std::array<GLubyte, dest_width * dest_width * 4> pixels[2];
                for (int variant = 0; variant < 2; ++variant) {
                    const auto prog = programs[variant];
                    glUseProgram(prog);
                    glUniform4i(glGetUniformLocation(prog, "source_layout"), source_width, source_width, source_width, tiled_src);
                    glUniform4i(glGetUniformLocation(prog, "destination_layout"), dest_width, dest_width, dest_width, tiled_dst);
                    glUniform2i(glGetUniformLocation(prog, "bytes_per_pixel"), src, dst);
                    glUniform1i(glGetUniformLocation(prog, "byte_offset"), offset);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glReadPixels(0, 0, dest_width, dest_width, GL_RGBA, GL_UNSIGNED_BYTE, pixels[variant].data());
                }
                Check(glGetError() == GL_NO_ERROR && pixels[0] == pixels[1], "remap byte mismatch");
                Check(*std::min_element(pixels[0].begin(), pixels[0].end()) !=
                      *std::max_element(pixels[0].begin(), pixels[0].end()), "empty remap reference");
                ++cases;
            }
        }
    }
    for (const auto prog : programs) glDeleteProgram(prog);
    glDeleteShader(vs);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(2, textures);
    glDeleteVertexArrays(1, &vao);
    std::printf("%u remap cases matched at all 256 pixels\n", cases);
}

#ifdef CARA_GL_CONTEXT_DISPATCH
void CheckSharedVertexUpload() {
    GLuint textures[2], framebuffer;
    glGenTextures(2, textures);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    OpenGL::SharedVertexShadow retained;
    Pixels source, expected[2];
    for (int generation = 0; generation < 2; ++generation) {
        for (unsigned byte = 0; byte < source.size(); ++byte)
            source[byte] = GLubyte(byte + 71 * generation);
        expected[generation] = source;
        glBindTexture(GL_TEXTURE_2D, textures[generation]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        Check(!Queue::TryQueueVertexUpload(width, 1, GL_RGBA, GL_UNSIGNED_BYTE, source.data(),
                                           source.size() - 1, retained), "bad upload span accepted");
        Check(Queue::TryQueueVertexUpload(width, 1, GL_RGBA, GL_UNSIGNED_BYTE, source.data(),
                                          source.size(), retained), "shared upload rejected");
        Check(std::equal(source.begin(), source.end(), retained.data()), "retained bytes differ");
        source.fill(0);
    }
    retained.clear();
    for (int generation = 0; generation < 2; ++generation) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[generation], 0);
        Pixels pixels;
        glReadPixels(0, 0, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        Check(glGetError() == GL_NO_ERROR && pixels == expected[generation], "queued vertex bytes changed");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(2, textures);
    std::puts("two shared upload generations survived source and cache mutation");
}

void CheckRemapQueue() {
    for (const bool enable : {false, true}) {
        glActiveTexture(GL_TEXTURE3);
        if (enable) {
            glEnable(GL_DITHER);
            glDisable(GL_RASTERIZER_DISCARD);
        } else {
            glDisable(GL_DITHER);
            glEnable(GL_RASTERIZER_DISCARD);
        }
        auto saved = Queue::SaveRemapStateIfEnabled();
        Check(bool(saved), "queued remap save");
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_DITHER);
        glDisable(GL_RASTERIZER_DISCARD);
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        auto complete = [] { return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; };
        Check(!Queue::CheckedRemapDraw(complete), "incomplete remap framebuffer accepted");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Check(Queue::CheckedRemapDraw(complete), "complete remap framebuffer rejected");
        Queue::RestoreRemapState(std::move(saved));
        GLint active;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        Check(active == GL_TEXTURE3 && bool(glIsEnabled(GL_DITHER)) == enable &&
              bool(glIsEnabled(GL_RASTERIZER_DISCARD)) != enable, "remap leaked GL state");
        glDeleteFramebuffers(1, &fbo);
    }
    glDisable(GL_RASTERIZER_DISCARD);
    glActiveTexture(GL_TEXTURE0);
    std::puts("checked queue remap restored GL state and returned both framebuffer outcomes");
}
#endif

#ifdef CARA_ANDROID_GLES_ATLAS
void CheckDeletedTextureBindings() {
    GLuint textures[2]{};
    glGenTextures(2, textures);
    OpenGL::OpenGLState::BindTexture2D(0, textures[0]);
    OpenGL::OpenGLState::BindTexture2D(1, textures[1]);
    OpenGL::OpenGLState::BindTexture2D(2, textures[0]);
    glDeleteTextures(1, textures);
    const auto state = OpenGL::OpenGLState::GetCurState();
    Check(state.texture_units[0].texture_2d == 0 && state.texture_units[2].texture_2d == 0,
          "deleted GLES texture survived in producer state");
    Check(state.texture_units[1].texture_2d == textures[1],
          "unrelated texture binding was discarded");
    GLint binding = -1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    Check(binding == 0 && glGetError() == GL_NO_ERROR, "driver retained a deleted texture binding");
    glDeleteTextures(1, textures + 1);
    glActiveTexture(GL_TEXTURE0);
    std::puts("queued texture deletion cleared producer and driver bindings");
}
#endif

Pixels Draw() {
    const GLuint vs = Compile(GL_VERTEX_SHADER, R"(#version 300 es
const vec2 positions[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
uniform float z;
void main(){gl_Position=vec4(positions[gl_VertexID],z,1);})");
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, R"(#version 300 es
precision mediump float;
uniform vec4 color;
out vec4 outputColor;
void main(){outputColor=color;})");
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    Check(linked == GL_TRUE, "probe program link");
    glUseProgram(program);
    const GLint color = glGetUniformLocation(program, "color");
    Check(color >= 0, "color uniform");
    glViewport(0, 0, width, 1);
    const GLint z = glGetUniformLocation(program, "z");
    Check(z >= 0, "depth uniform");
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUniform1f(z, 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, width / 2, 1);
    glUniform4f(color, 1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glScissor(width / 2, 0, width / 2, 1);
    glUniform4f(color, 0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // A later, more distant red draw must preserve the nearer right-hand green.
    glUniform1f(z, 0.5f);
    glUniform4f(color, 1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    Pixels pixels{};
    glReadPixels(0, 0, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    Check(glGetError() == GL_NO_ERROR, "driver error after ordered readback");
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteShader(fs);
    glDeleteShader(vs);
    return pixels;
}

// Compare one instance pair against the full ordinary reference, including the
// vertex ID supplied by an EBO and side effects in the depth/stencil attachment.
void CheckInstancedDraws() {
    const auto vs = Compile(GL_VERTEX_SHADER, R"(#version 300 es
const vec2 points[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
uniform float z;
out vec3 vertexColor;
void main() {
    int corner = gl_VertexID % 3;
    gl_Position = vec4(points[corner], z, 1);
    vertexColor = vec3(float(gl_VertexID) / 4.0, float(corner) / 2.0, 0.25);
})");
    const auto fs = Compile(GL_FRAGMENT_SHADER, R"(#version 300 es
precision highp float;
in vec3 vertexColor;
uniform int sentinel;
out vec4 ink;
void main(){ink = sentinel != 0 ? vec4(1,0,1,1) : vec4(vertexColor,0.5);})");
    const auto program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    Check(linked == GL_TRUE, "instance program link");
    glUseProgram(program);
    const GLint z = glGetUniformLocation(program, "z");
    const GLint sentinel = glGetUniformLocation(program, "sentinel");
    Check(z >= 0 && sentinel >= 0, "instance uniforms");

    GLuint indices = 0, vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &indices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
    // Vertex 3 has the same position as vertex 0, with a different color.
    const GLushort stream[] = {3, 1, 2};
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(stream), stream, GL_STATIC_DRAW);
    const auto read = [] {
        Pixels result{};
        glReadPixels(0, 0, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
        return result;
    };

    Pixels arrays{};
    for (bool indexed : {false, true}) {
        auto draw = [indexed] {
            if (indexed) glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr);
            else glDrawArrays(GL_TRIANGLES, 0, 3);
        };
        auto repeat = [indexed] {
            if (indexed) glDrawElementsInstanced(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr, 2);
            else glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
        };
        Pixels reference{}, single{};
        for (bool instanced : {false, true}) {
            glViewport(0, 0, width, 1);
            glDisable(GL_SCISSOR_TEST);
            glStencilMask(0xff);
            glDepthMask(GL_TRUE);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_STENCIL_TEST);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LEQUAL);
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
            glUniform1f(z, 0);
            glUniform1i(sentinel, 0);
            if (instanced) {
                repeat();
            } else {
                draw();
                single = read();
                draw();
            }
            const auto blended = read();
            if (!instanced) {
                Check(blended != single, "second blended draw did not affect color");
                reference = blended;
            } else {
                Check(blended == reference, "instanced blended colors differ");
            }
            // The depth written by the first pair must reject this farther draw.
            glDisable(GL_BLEND);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glUniform1i(sentinel, 1);
            glUniform1f(z, 0.5f);
            draw();
            Check(read() == blended, "farther sentinel changed depth-tested color");
            // Verify both passes updated stencil, and preserve the rest of the row.
            glUniform1f(z, 0);
            glStencilFunc(GL_EQUAL, 2, 0xff);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 1, 1);
            draw();
            Pixels expected = blended;
            expected[0] = expected[2] = expected[3] = 255;
            expected[1] = 0;
            Check(read() == expected && expected != blended, "stencil count must equal two");
            Check(glGetError() == GL_NO_ERROR, "instance draw GL error");
        }
        if (!indexed) arrays = reference;
        else Check(arrays != reference, "guest index did not affect vertex ID output");
        std::printf("%s repeat: %d blended pixels match, depth reject and stencil=2 verified\n",
                    indexed ? "elements" : "arrays", width);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(0);
    glDeleteBuffers(1, &indices);
    glDeleteVertexArrays(1, &vao);
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

template <typename F>
bool CheckHelperCall(const char* name, F call) {
    try {
        call();
        std::printf("%s routed\n", name);
        return true;
    } catch (const std::logic_error& e) {
        if (std::string(e.what()) != "GL recorder requires its sole producer thread") throw;
        std::printf("%s rejected: %s\n", name, e.what());
        return false;
    }
}

void Run(EGLDisplay display, EGLConfig config) {
    Context rendering(display, config, EGL_NO_CONTEXT);
    Context helper(display, config, rendering.context);
    rendering.Bind();
    Check(gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(eglGetProcAddress)), "GLES loader");
    std::printf("renderer: %s\n", glGetString(GL_RENDERER));
    if (GLAD_GL_EXT_disjoint_timer_query && glGetQueryivEXT) {
        GLint elapsed_bits = 0, timestamp_bits = 0;
        glGetQueryivEXT(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &elapsed_bits);
        glGetQueryivEXT(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &timestamp_bits);
        Check(glGetError() == GL_NO_ERROR, "timer query counter capabilities");
        std::printf("GLES GPU timer counter bits: elapsed=%d timestamp=%d\n", elapsed_bits, timestamp_bits);
    } else {
        std::puts("GLES disjoint GPU timer unavailable");
    }
    driver_draw = glad_glDrawArrays;
    glad_glDrawArrays = CountDraw;
    const auto direct = Draw();
    for (int x = 0; x < width; ++x) {
        Check(direct[4*x] == (x < width/2 ? 255 : 0) &&
              direct[4*x+1] == (x < width/2 ? 0 : 255) &&
              direct[4*x+2] == 0 && direct[4*x+3] == 255, "direct pixels");
    }
    // Keep the same public native dispatch that independent EGL contexts receive.
    const auto raw_bind = glad_glBindFramebuffer;
    const auto raw_create = glad_glCreateShader;
    const auto raw_delete = glad_glDeleteShader;
    const auto raw_error = glad_glGetError;
    rendering.Release();
    Common::HostRenderQueue worker(128, 8 * 1024 * 1024,
        [&] { rendering.Bind(); submission_worker = true; },
        [&] { rendering.Release(); });
    setenv("CARA_CHECKED_REMAP_PACKET", "1", 1);
    setenv("CARA_SHARED_VERTEX_UPLOAD", "1", 1);
#ifdef CARA_GL_CONTEXT_DISPATCH
    GLsync published = nullptr;
#endif
    Queue::Install(worker);
    try {
        const auto queued = Draw();
#ifdef CARA_ANDROID_GLES_ATLAS
        CheckDeletedTextureBindings();
#endif
        Queue::Drain();
        Check(queued == direct && direct_draws == 3 && queued_draws == 3,
              "driver ownership or pixel mismatch");
        const auto counters = Queue::SnapshotStats();
        std::printf("render: direct_draws=%u worker_draws=%u identical_pixels=%d "
                    "queued_commands=%llu batches=%llu synchronous_calls=%llu\n",
                    direct_draws, queued_draws, width,
                    static_cast<unsigned long long>(counters.recorded_commands),
                    static_cast<unsigned long long>(counters.submitted_batches),
                    static_cast<unsigned long long>(counters.synchronous_calls));
        CheckInstancedDraws();
#ifdef CARA_GL_CONTEXT_DISPATCH
        CheckRemapQueue();
        CheckSharedVertexUpload();
        CheckLightingImages();
        published = Queue::CreatePublishedRenderFence();
        Check(published != nullptr, "render fence not created");
#endif
        Queue::Drain();
        std::puts("queued instanced draws passed");
        std::async(std::launch::async, [&] {
            helper.Bind();
#ifdef CARA_GL_CONTEXT_DISPATCH
            glWaitSync(published, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(published);
            published = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            Check(published && glGetError() == GL_NO_ERROR, "sibling fence exchange");
#endif
            const bool presenter = CheckHelperCall("present glBindFramebuffer", [] { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); });
            const bool compiler = CheckHelperCall("shader worker glCreateShader", [] {
                const auto shader = glCreateShader(GL_FRAGMENT_SHADER);
                Check(shader != 0, "helper shader name");
                glDeleteShader(shader);
            });
#ifdef CARA_GL_CONTEXT_DISPATCH
            Check(presenter && compiler, "thread-local dispatch failed on sibling EGL context");
#else
            Check(!presenter && !compiler, "single producer guard no longer reproduced");
#endif
            raw_bind(GL_DRAW_FRAMEBUFFER, 0);
            const auto shader = raw_create(GL_FRAGMENT_SHADER);
            Check(shader != 0, "raw helper shader name");
            raw_delete(shader);
            Check(raw_error() == GL_NO_ERROR, "native sibling context control");
            std::puts("native sibling control passed");
            helper.Release();
        }).get();
#ifdef CARA_GL_CONTEXT_DISPATCH
        Queue::QueueConsumedPresentFence(published);
        Queue::Drain();
        Check(worker.Call([&] { return !glIsSync(published); }) && glGetError() == GL_NO_ERROR, "present fence was not retired");
        std::puts("render and present fence round trip passed across EGL contexts");
#endif
    } catch (...) {
        Queue::Uninstall();
        throw;
    }
    Queue::Uninstall();
    worker.Stop();
    glad_glDrawArrays = driver_draw;
    rendering.Bind();
    CheckInstancedDraws();
    CheckRemapPixels();
    rendering.Release();
}
} // namespace

int main() {
    try {
        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        Check(eglInitialize(display, nullptr, nullptr), "initialize default EGL display");
        Check(eglBindAPI(EGL_OPENGL_ES_API), "bind OpenGL ES API");
        const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8, EGL_NONE};
        EGLConfig config{};
        EGLint count{};
        Check(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1, "ES3 RGBA8 config");
        Run(display, config);
        eglTerminate(display);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ordered GLES probe failed: %s (EGL %#x)\n", e.what(), eglGetError());
        return 1;
    }
}
