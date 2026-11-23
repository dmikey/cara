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
void main(){gl_Position=vec4(positions[gl_VertexID],0,1);})");
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
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, width / 2, 1);
    glUniform4f(color, 1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glScissor(width / 2, 0, width / 2, 1);
    glUniform4f(color, 0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    Pixels pixels{};
    glReadPixels(0, 0, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    Check(glGetError() == GL_NO_ERROR, "driver error after ordered readback");
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteShader(fs);
    glDeleteShader(vs);
    return pixels;
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
        Check(queued == direct && direct_draws == 2 && queued_draws == 2,
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
            CheckHelperCall("present glBindFramebuffer", [] { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); });
            CheckHelperCall("shader worker glCreateShader", [] {
                const auto shader = glCreateShader(GL_FRAGMENT_SHADER);
                Check(shader != 0, "helper shader name");
                glDeleteShader(shader);
            });
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
                                EGL_ALPHA_SIZE, 8, EGL_NONE};
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
