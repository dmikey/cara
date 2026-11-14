// SPDX-License-Identifier: GPL-2.0-or-later
// Actual production static helper, not a replica. The archive's cache object is
// not extracted because this TU provides its definitions.
#include <iostream>
#include <dlfcn.h>
#include "video_core/renderer_opengl/gl_rasterizer_cache.cpp"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>

namespace {
using namespace OpenGL;
constexpr int W = 16, H = 16;
unsigned checks = 0, mask_calls = 0, viewport_calls = 0, scissor_calls = 0;
PFNGLDEPTHMASKPROC original_depth_mask;
PFNGLSTENCILMASKPROC original_stencil_mask;
PFNGLCOLORMASKPROC original_color_mask;
PFNGLVIEWPORTPROC original_viewport;
PFNGLSCISSORPROC original_scissor;
void CountDepthMask(GLboolean v) {
    ++mask_calls;
    original_depth_mask(v);
}
void CountStencilMask(GLuint v) {
    ++mask_calls;
    original_stencil_mask(v);
}
void CountColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    ++mask_calls;
    original_color_mask(r, g, b, a);
}
void CountViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    ++viewport_calls;
    original_viewport(x, y, w, h);
}
void CountScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    ++scissor_calls;
    original_scissor(x, y, w, h);
}
void Require(bool ok, const char* reason) {
    ++checks;
    if (!ok)
        throw std::runtime_error(reason);
}
void NoError() {
    Require(glGetError() == GL_NO_ERROR, "unexpected GL error");
}
GLuint Texture(GLenum internal, GLenum format, GLenum type, const void* data) {
    GLuint t;
    glGenTextures(1, &t);
    const auto old = OpenGLState::BindTexture2D(0, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, W, H, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    OpenGLState::BindTexture2D(0, old);
    NoError();
    return t;
}
struct State {
    std::array<GLint, 24> values{};
    State() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &values[0]);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &values[1]);
        glGetIntegerv(GL_VIEWPORT, &values[2]);
        glGetIntegerv(GL_SCISSOR_BOX, &values[6]);
        values[10] = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_DEPTH_WRITEMASK, &values[11]);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &values[12]);
        glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &values[13]);
        glGetIntegerv(GL_COLOR_WRITEMASK, &values[14]);
        glGetIntegerv(GL_CURRENT_PROGRAM, &values[18]);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &values[19]);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &values[20]);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &values[21]);
        glGetIntegerv(GL_READ_BUFFER, &values[22]);
        glGetIntegerv(GL_DRAW_BUFFER0, &values[23]);
        NoError();
    }
};
void CheckTracked(const OpenGLState& expected) {
    const auto actual = OpenGLState::GetCurState();
    Require(actual.draw.read_framebuffer == expected.draw.read_framebuffer &&
                actual.draw.draw_framebuffer == expected.draw.draw_framebuffer,
            "tracked framebuffer changed");
    Require(actual.viewport.x == expected.viewport.x && actual.viewport.y == expected.viewport.y &&
                actual.viewport.width == expected.viewport.width &&
                actual.viewport.height == expected.viewport.height,
            "tracked viewport changed");
    Require(actual.scissor.enabled == expected.scissor.enabled &&
                actual.scissor.x == expected.scissor.x && actual.scissor.y == expected.scissor.y &&
                actual.scissor.width == expected.scissor.width &&
                actual.scissor.height == expected.scissor.height,
            "tracked scissor changed");
    Require(actual.depth.write_mask == expected.depth.write_mask &&
                actual.stencil.write_mask == expected.stencil.write_mask,
            "tracked masks changed");
    Require(actual.color_mask.red_enabled == expected.color_mask.red_enabled &&
                actual.color_mask.green_enabled == expected.color_mask.green_enabled &&
                actual.color_mask.blue_enabled == expected.color_mask.blue_enabled &&
                actual.color_mask.alpha_enabled == expected.color_mask.alpha_enabled,
            "tracked color mask changed");
}
GLuint Program() {
    auto compile = [](GLenum kind, const char* source) {
        GLuint s = glCreateShader(kind);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        Require(ok, "shader compile failed");
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER,
                        "#version 330 core\nvoid main(){vec2 "
                        "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}");
    GLuint fs = compile(GL_FRAGMENT_SHADER,
                        "#version 330 core\nout vec4 color;void main(){color=vec4(1,1,0,1);}");
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    Require(ok, "program link failed");
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}
void Run(bool enabled) {
    g_read_framebuffer.Create();
    g_draw_framebuffer.Create();
    SCOPE_EXIT({
        g_read_framebuffer.Release();
        g_draw_framebuffer.Release();
    });
    const GLuint program = Program();
    GLuint vao;
    glGenVertexArrays(1, &vao);
    GLuint sentinel_fb, read_fb;
    glGenFramebuffers(1, &sentinel_fb);
    glGenFramebuffers(1, &read_fb);
    std::array<u32, W * H> blue;
    blue.fill(0xffff0000u);
    GLuint sentinel = Texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
    OpenGLState::BindDrawFramebuffer(sentinel_fb);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sentinel, 0);
    OpenGLState::BindReadFramebuffer(sentinel_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "sentinel incomplete");
    for (bool invalid : {false, true})
        for (bool scissor_enabled : {false, true})
            for (int mode = 0; mode < 3; ++mode) {
                std::array<u32, W * H> words{};
                std::array<u16, W * H> shorts{};
                for (unsigned i = 0; i < words.size(); ++i) {
                    words[i] = mode == 0 ? (0xff000000u | i * 1003u)
                                         : ((i & 1u) ? 0xffffff00u : 0u) | (i & 255u);
                    shorts[i] = u16(i * 997u);
                }
                GLenum internal = mode == 0   ? GL_RGBA8
                                  : mode == 1 ? GL_DEPTH_COMPONENT16
                                              : GL_DEPTH24_STENCIL8;
                GLenum format = mode == 0   ? GL_RGBA
                                : mode == 1 ? GL_DEPTH_COMPONENT
                                            : GL_DEPTH_STENCIL;
                GLenum type = mode == 0   ? GL_UNSIGNED_BYTE
                              : mode == 1 ? GL_UNSIGNED_SHORT
                                          : GL_UNSIGNED_INT_24_8;
                GLuint src = Texture(internal, format, type,
                                     mode == 1 ? static_cast<void*>(shorts.data())
                                               : static_cast<void*>(words.data()));
                const std::array<u32, W * H> zero{};
                GLuint dst = Texture(internal, format, type, zero.data());
                SCOPE_EXIT({
                    glDeleteTextures(1, &src);
                    glDeleteTextures(1, &dst);
                });
                const auto old = OpenGLState::BindTexture2D(0, sentinel);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE,
                                blue.data());
                OpenGLState::BindTexture2D(0, old);
                OpenGLState state;
                state.draw.read_framebuffer = sentinel_fb;
                state.draw.draw_framebuffer = sentinel_fb;
                state.draw.shader_program = program;
                state.draw.vertex_array = vao;
                state.cull.enabled = false;
                state.depth.write_mask = GL_FALSE;
                state.stencil.write_mask = 0x36;
                state.color_mask = {GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE};
                state.viewport = {1, 2, 5, 4};
                state.scissor = {scissor_enabled, 2, 3, 2, 2};
                state.Apply();
                const State before;
                if (invalid) {
                    OpenGLState::InvalidateRasterState();
                    // The legacy fallback must repair raw mutations, not assume
                    // that an invalid snapshot still describes actual GL state.
                    glViewport(7, 7, 1, 1);
                    glScissor(0, 0, 0, 0);
                    glDisable(GL_SCISSOR_TEST);
                    glDepthMask(GL_TRUE);
                    glStencilMask(0);
                    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
                }
                mask_calls = viewport_calls = scissor_calls = 0;
                BlitTextures(src, {0, H, W, 0}, dst, {0, H, W, 0},
                             mode == 0   ? SurfaceType::Color
                             : mode == 1 ? SurfaceType::Depth
                                         : SurfaceType::DepthStencil,
                             true);
                Require(State().values == before.values, "actual GL state not restored");
                CheckTracked(state);
                Require((mask_calls == 0 && viewport_calls == 0 && scissor_calls == 0) ==
                            (enabled && !invalid),
                        "narrow/fallback call route differs");
                if (invalid)
                    Require(!state.TryApplyBlitState(),
                            "partial fallback established full validity");
                // Draw without reapplying state: verifies preserved viewport, scissor and masks.
                glDrawArrays(GL_TRIANGLES, 0, 3);
                NoError();
                std::array<u32, W * H> drawn{};
                glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, drawn.data());
                NoError();
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        Require(drawn[y * W + x] == (x >= 1 && x < 6 && y >= 2 && y < 6 &&
                                                             (!scissor_enabled ||
                                                              (x >= 2 && x < 4 && y >= 3 && y < 5))
                                                         ? 0xffffff00u
                                                         : 0xffff0000u),
                                "next draw differs from fixed expected pixels");
                OpenGLState::BindReadFramebuffer(read_fb);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0,
                                       0);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                       GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                                       mode == 0   ? GL_COLOR_ATTACHMENT0
                                       : mode == 1 ? GL_DEPTH_ATTACHMENT
                                                   : GL_DEPTH_STENCIL_ATTACHMENT,
                                       GL_TEXTURE_2D, dst, 0);
                glReadBuffer(mode == 0 ? GL_COLOR_ATTACHMENT0 : GL_NONE);
                Require(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                        "readback incomplete");
                std::array<u32, W * H> read_words{};
                std::array<u16, W * H> read_shorts{};
                glReadPixels(0, 0, W, H, format, type,
                             mode == 1 ? static_cast<void*>(read_shorts.data())
                                       : static_cast<void*>(read_words.data()));
                NoError();
                for (unsigned i = 0; i < words.size(); ++i) {
                    const auto actual = mode == 1 ? u32(read_shorts[i]) : read_words[i];
                    const auto expected = mode == 1 ? u32(shorts[i]) : words[i];
                    if (actual != expected)
                        throw std::runtime_error("copy mode=" + std::to_string(mode) +
                                                 " invalid=" + std::to_string(invalid) +
                                                 " pixel=" + std::to_string(i) +
                                                 " actual=" + std::to_string(actual) +
                                                 " expected=" + std::to_string(expected));
                    ++checks;
                }
                state.Apply();
                for (int failure = 0; failure < 4; ++failure) {
                    if (invalid)
                        OpenGLState::InvalidateRasterState();
                    const State fail_before;
                    bool threw = false;
                    GLuint intsrc = 0, intdst = 0;
                    if (failure == 3) {
                        intsrc =
                            Texture(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, words.data());
                        intdst = Texture(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
                    }
                    if (failure == 0)
                        glEnable(0xffffffffu); // deliberately pending GL_INVALID_ENUM
                    try {
                        BlitTextures(failure == 2   ? 0
                                     : failure == 3 ? intsrc
                                                    : src,
                                     {0, H, W, 0}, failure == 3 ? intdst : dst, {0, H, W, 0},
                                     failure == 1   ? static_cast<SurfaceType>(255)
                                     : failure == 3 ? SurfaceType::Color
                                     : mode == 0    ? SurfaceType::Color
                                     : mode == 1    ? SurfaceType::Depth
                                                    : SurfaceType::DepthStencil,
                                     true);
                    } catch (const std::runtime_error& e) {
                        threw = true;
                        const char* stages[] = {"preexisting", "supported attachment", "incomplete",
                                                "texture copy failed"};
                        Require(std::string(e.what()).find(stages[failure]) != std::string::npos,
                                "wrong checked-copy failure stage");
                    }
                    Require(threw, "checked-copy failure did not throw");
                    Require(State().values == fail_before.values,
                            "failure exit did not restore actual GL state");
                    CheckTracked(state);
                    if (intsrc)
                        glDeleteTextures(1, &intsrc);
                    if (intdst)
                        glDeleteTextures(1, &intdst);
                }
            }
    glDeleteFramebuffers(1, &sentinel_fb);
    glDeleteFramebuffers(1, &read_fb);
    glDeleteTextures(1, &sentinel);
    glDeleteProgram(program);
    glDeleteVertexArrays(1, &vao);
}
} // namespace
int main() {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format{};
    CGLContextObj context{};
    GLint count{};
    if (CGLChoosePixelFormat(attrs, &format, &count) != kCGLNoError || !format)
        return 2;
    const auto result = CGLCreateContext(format, nullptr, &context);
    CGLDestroyPixelFormat(format);
    if (result != kCGLNoError || !context || CGLSetCurrentContext(context) != kCGLNoError)
        return 2;
    static void* symbols =
        dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    if (!symbols || !gladLoadGLLoader([](const char* n) -> void* { return dlsym(symbols, n); }))
        return 2;
    OpenGL::GLES = false;
    original_stencil_mask = glad_glStencilMask;
    glad_glStencilMask = CountStencilMask;
    original_color_mask = glad_glColorMask;
    glad_glColorMask = CountColorMask;
    original_depth_mask = glad_glDepthMask;
    glad_glDepthMask = CountDepthMask;
    original_viewport = glad_glViewport;
    glad_glViewport = CountViewport;
    original_scissor = glad_glScissor;
    glad_glScissor = CountScissor;
    std::cout << "GL=" << glGetString(GL_VERSION) << " renderer=" << glGetString(GL_RENDERER)
              << '\n';
    try {
        OpenGL::OpenGLState initial;
        initial.Apply();
        const char* v = std::getenv("CARA_BLIT_STATE_SCOPE");
        Run(v && std::strcmp(v, "1") == 0);
        std::cout << "PASS " << checks << " production blit pixel/state/next-draw/failure checks\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(context);
    return 0;
}
