// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone EGL process. Exercises Cara's actual GLAD recorder without starting the emulator.
#include <EGL/egl.h>
#include <glad/glad.h>

#include <array>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>

#include "common/host_render_queue.h"
#include "video_core/renderer_opengl/gl_command_queue.h"

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
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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
    Queue::Install(worker);
    try {
        const auto queued = Draw();
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
        std::async(std::launch::async, [&] {
            helper.Bind();
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
    } catch (...) {
        Queue::Uninstall();
        throw;
    }
    Queue::Uninstall();
    worker.Stop();
    glad_glDrawArrays = driver_draw;
    rendering.Bind();
    CheckInstancedDraws();
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
