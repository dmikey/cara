// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free actual generated fragment shader oracle. It does not model discard.
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <dlfcn.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <glad/glad.h>
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_host_scissor.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_scissor_damage.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_shader_util.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/video_core.h"
#include "video_core/pica_state.h"

namespace {
constexpr int W = 96, H = 96;
const bool probe = std::getenv("CARA_SCISSOR_ORACLE_PROBE") != nullptr;
std::uint64_t escaped{};
const bool host_helper = std::getenv("CARA_SCISSOR_ORACLE_HOST_HELPER") != nullptr;
std::array<std::uint64_t, 4> mode_hashes{1469598103934665603ULL, 1469598103934665603ULL,
                                         1469598103934665603ULL, 1469598103934665603ULL};
std::uint64_t restored_draws{};

using Rect = OpenGL::ScissorDamage::Rect;
using Mode = Pica::RasterizerRegs::ScissorMode;
std::uint64_t checks{}, cases{}, changed_color{}, changed_depth{}, changed_stencil{}, empty_cases{};
void Require(bool value, const std::string& reason) {
    ++checks;
    if (!value)
        throw std::runtime_error(reason);
}
void NoError() {
    auto e = glGetError();
    Require(e == GL_NO_ERROR, "GL error " + std::to_string(e));
}
struct Pixels {
    std::array<std::array<u8, 4>, W * H> color{};
    std::array<u32, W * H> ds{};
    void Read() {
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
        glReadPixels(0, 0, W, H, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, ds.data());
        NoError();
    }
};
struct Shader {
    GLuint program{}, ubo{};
    GLint size{};
    std::vector<u8> data;
    explicit Shader(unsigned rawmode) {
        Pica::Regs regs{};
        regs.rasterizer.scissor_test.mode.Assign(static_cast<Mode>(rawmode));
        regs.lighting.disable.Assign(1);
        const auto config = OpenGL::PicaFSConfig::BuildFromRegs(regs);
        Require(config.state.alpha_test_func == Pica::FramebufferRegs::CompareFunc::Always,
                "fixture alpha test must pass");
        auto generated = OpenGL::GenerateFragmentShader(config, false);
        if (const char* kind = std::getenv("CARA_SCISSOR_ORACLE_FRAGMENT")) {
            Require(probe, "fragment override is diagnostic-only");
            const std::string mode(kind);
            if (mode == "generated-no-extension") {
                for (const std::string extension :
                     {"#extension GL_ARB_shader_image_load_store : enable\n",
                      "#extension GL_ARB_shader_image_size : enable\n"}) {
                    const auto at = generated.find(extension);
                    Require(at != std::string::npos, "generated extension missing");
                    generated.erase(at, extension.size());
                }
            } else {
                Require(mode == "discard" || mode == "discard-extension" || mode == "rectangle" ||
                            mode == "rectangle-extension",
                        "unknown diagnostic fragment");
                generated = mode.find("extension") != std::string::npos
                                ? "#extension GL_ARB_shader_image_load_store : enable\n#extension "
                                  "GL_ARB_shader_image_size : enable\n"
                                : "";
                generated += "out vec4 color;void main(){";
                generated += mode.rfind("discard", 0) == 0
                                 ? "discard;"
                                 : "if(!(gl_FragCoord.x>=6.0&&gl_FragCoord.y>=7.0&&gl_FragCoord.x<"
                                   "18.0&&gl_FragCoord.y<16.0))discard;";
                generated += "gl_FragDepth=0.25;color=vec4(1.0,0.25,0.5,1.0);}";
            }
        }
        if (const char* dump = std::getenv("CARA_SCISSOR_ORACLE_DUMP")) {
            std::ofstream(std::string(dump) + "/generated-mode" + std::to_string(rawmode) + ".glsl")
                << generated;
        }
        Require(std::getenv("CARA_SCISSOR_ORACLE_FRAGMENT") || rawmode == 0 ||
                    generated.find("scissor_x1") != std::string::npos,
                "generated scissor shader absent");
        const char* vertex = R"(
out vec4 primary_color;out vec2 texcoord0;out vec2 texcoord1;out vec2 texcoord2;
out float texcoord0_w;out vec4 normquat;out vec3 view;
void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);
primary_color=vec4(1.0,0.25,0.5,1.0);texcoord0=vec2(0);texcoord1=vec2(0);texcoord2=vec2(0);texcoord0_w=1.0;normquat=vec4(0,0,0,1);view=vec3(0);}
)";
        GLuint vs = OpenGL::LoadShader(vertex, GL_VERTEX_SHADER),
               fs = OpenGL::LoadShader(generated.c_str(), GL_FRAGMENT_SHADER);
        for (auto s : {vs, fs}) {
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            Require(ok, "actual generated shader compile failed");
        }
        program = OpenGL::LoadProgram(false, {vs, fs});
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        Require(ok, "generated shader program link failed");
        const GLuint block = glGetUniformBlockIndex(program, "shader_data");
        if (block == GL_INVALID_INDEX && probe && std::getenv("CARA_SCISSOR_ORACLE_FRAGMENT"))
            return;
        Require(block != GL_INVALID_INDEX, "shader_data optimized away");
        glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        Require(size > 0, "invalid shader uniform block size");
        data.resize(size);
        glUniformBlockBinding(program, block, 0);
        glGenBuffers(1, &ubo);
        Set("depth_scale", 0.0f);
        Set("depth_offset", 0.25f);
    }
    template <class T>
    void Set(const char* name, T value) {
        GLuint index = GL_INVALID_INDEX;
        glGetUniformIndices(program, 1, &name, &index);
        if (index == GL_INVALID_INDEX) {
            Require(std::string(name).rfind("scissor_", 0) == 0, "required depth uniform missing");
            return;
        }
        GLint offset = -1;
        glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &offset);
        Require(offset >= 0 && offset + sizeof(T) <= data.size(), "uniform offset out of bounds");
        std::memcpy(data.data() + offset, &value, sizeof(T));
    }
    void Bind(Rect bounds) {
        if (!ubo) {
            glUseProgram(program);
            return;
        }
        Set("scissor_x1", GLint(bounds.left));
        Set("scissor_y1", GLint(bounds.bottom));
        Set("scissor_x2", GLint(bounds.right));
        Set("scissor_y2", GLint(bounds.top));
        glUseProgram(program);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, data.size(), data.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        GLint actual_binding = 0;
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &actual_binding);
        Require(actual_binding == GLint(ubo), "indexed UBO binding mismatch");
        std::vector<u8> actual(data.size());
        glGetBufferSubData(GL_UNIFORM_BUFFER, 0, actual.size(), actual.data());
        Require(actual == data, "actual uploaded uniform bytes differ");
    }
    ~Shader() {
        glDeleteBuffers(1, &ubo);
        glDeleteProgram(program);
    }
};
Rect Scale(Rect r, int scale) {
    return {r.left * scale, r.bottom * scale, r.right * scale, r.top * scale};
}
bool Contains(Rect r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.bottom && y < r.top;
}
void Run() {
    GLuint color, depth, fbo, vao;
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, W, H, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "oracle framebuffer incomplete");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
    glBindProgramPipeline(0);
    glViewport(0, 0, W, H);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    const std::array<Rect, 14> boxes{{{0, 0, 30, 30},
                                      {6, 7, 18, 16},
                                      {5, 6, 7, 7},
                                      {0, 0, 2, 2},
                                      {21, 18, 25, 22},
                                      {8, 8, 8, 12},
                                      {8, 8, 12, 8},
                                      {15, 8, 8, 12},
                                      {8, 15, 12, 8},
                                      {-4, -3, 7, 9},
                                      {3, 4, 6, 8},
                                      {19, 16, 26, 22},
                                      {4, 5, 20, 17},
                                      {9, 9, 10, 10}}};
    const std::array<GLenum, 8> operations{GL_KEEP, GL_ZERO,   GL_REPLACE,   GL_INCR,
                                           GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    std::unique_ptr<Shader> next_shader;
    if (host_helper)
        next_shader = std::make_unique<Shader>(0);
    for (unsigned rawmode : {3u, 0u, 1u, 2u}) {
        if (probe && rawmode != 3 && !std::getenv("CARA_SCISSOR_ORACLE_CONTROL_MODES"))
            continue;
        Shader shader(rawmode);
        for (int scale : {1, 2, 3})
            for (unsigned box = 0; box < boxes.size(); ++box) {
                if (probe && (scale != 1 || box != 1))
                    continue;
                const Rect current = Scale({4, 5, 20, 17}, scale),
                           bounds = Scale(boxes[box], scale);
                for (unsigned variant = 0; variant < (probe ? 13u : 16u); ++variant) {
                    glDisable(GL_SCISSOR_TEST);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glDepthMask(GL_TRUE);
                    glStencilMask(0xff);
                    const GLfloat sentinel[] = {13.0f / 255, 29.0f / 255, 47.0f / 255, 61.0f / 255};
                    glClearBufferfv(GL_COLOR, 0, sentinel);
                    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0x5a);
                    Pixels before, after;
                    before.Read();
                    for (unsigned i = 0; i < before.ds.size(); ++i) {
                        Require(before.ds[i] == 0xffffff5au,
                                "clear depth/stencil sentinel mismatch");
                        Require(before.color[i] == std::array<u8, 4>{13, 29, 47, 61},
                                "clear color sentinel mismatch");
                    }
                    shader.Bind(bounds);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(current.left, current.bottom, current.right - current.left,
                              current.top - current.bottom);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(variant == 3 ? GL_NEVER : GL_ALWAYS);
                    glDepthMask(variant % 3 == 0 ? GL_TRUE : GL_FALSE);
                    glEnable(GL_STENCIL_TEST);
                    glStencilFunc(variant == 2 ? GL_NEVER : GL_ALWAYS, 0x3c, 0xff);
                    glStencilMask(variant == 4 ? 0 : variant == 6 ? 0x55 : 0xff);
                    glStencilOp(operations[variant % 8], operations[variant % 8],
                                operations[variant % 8]);
                    glColorMask(variant == 1 ? GL_FALSE : GL_TRUE, variant % 2 ? GL_FALSE : GL_TRUE,
                                variant == 1 ? GL_FALSE : GL_TRUE,
                                variant % 2 ? GL_FALSE : GL_TRUE);
                    if (variant >= 8) {
                        glColorMask(variant == 10 || variant == 12, variant == 10 || variant == 12,
                                    variant == 10 || variant == 12, variant == 10 || variant == 12);
                        glDepthMask(variant == 11 || variant == 12);
                        glDepthFunc(GL_ALWAYS);
                        glStencilFunc(GL_ALWAYS, 0x3c, 0xff);
                        glStencilMask(0xff);
                        glStencilOp(variant == 9 ? GL_KEEP : GL_ZERO,
                                    variant == 9 ? GL_KEEP : GL_ZERO,
                                    variant == 9 ? GL_KEEP : GL_ZERO);
                        if (variant == 8)
                            glDisable(GL_STENCIL_TEST);
                    }
                    if (variant >= 13) {
                        glDisable(GL_STENCIL_TEST);
                        glStencilMask(0);
                        glColorMask(variant != 14, variant != 14, variant != 14, variant != 14);
                        glDepthMask(variant != 15);
                    }
                    GLint actual_stencil_mask = 0;
                    glGetIntegerv(GL_STENCIL_WRITEMASK, &actual_stencil_mask);
                    const bool stencil_possible =
                        glIsEnabled(GL_STENCIL_TEST) && actual_stencil_mask != 0;
                    auto damage = OpenGL::ScissorDamage::NarrowInclude(
                        current, bounds, rawmode, scale, false,
                        !std::getenv("CARA_SCISSOR_ORACLE_UNGUARDED") && stencil_possible);
                    if (rawmode != 3 ||
                        (stencil_possible && !std::getenv("CARA_SCISSOR_ORACLE_UNGUARDED")))
                        Require(!damage.eligible && damage.rect.left == current.left &&
                                    damage.rect.bottom == current.bottom &&
                                    damage.rect.right == current.right &&
                                    damage.rect.top == current.top,
                                "unsupported/stencil mode narrowed damage");
                    if (damage.eligible && std::getenv("CARA_SCISSOR_ORACLE_SHRINK"))
                        damage.rect.right = damage.rect.left;
                    OpenGL::OpenGLState tracked;
                    std::unique_ptr<OpenGL::HostScissor::ScopedInclude> host_scope;
                    tracked.scissor = {true, GLint(current.left), GLint(current.bottom),
                                       GLsizei(current.right - current.left),
                                       GLsizei(current.top - current.bottom)};
                    const auto original_scissor = tracked.scissor;
                    if (host_helper) {
                        Require(!probe, "production helper matrix is separate from tiny probes");
                        tracked.draw.read_framebuffer = fbo;
                        tracked.draw.draw_framebuffer = fbo;
                        tracked.draw.vertex_array = vao;
                        tracked.draw.shader_program = shader.program;
                        tracked.draw.uniform_buffer = shader.ubo;
                        tracked.viewport = {0, 0, W, H};
                        tracked.depth.test_enabled = glIsEnabled(GL_DEPTH_TEST);
                        GLint value{};
                        glGetIntegerv(GL_DEPTH_FUNC, &value);
                        tracked.depth.test_func = value;
                        glGetBooleanv(GL_DEPTH_WRITEMASK, &tracked.depth.write_mask);
                        tracked.stencil.test_enabled = glIsEnabled(GL_STENCIL_TEST);
                        glGetIntegerv(GL_STENCIL_FUNC, &value);
                        tracked.stencil.test_func = value;
                        glGetIntegerv(GL_STENCIL_REF, &value);
                        tracked.stencil.test_ref = value;
                        glGetIntegerv(GL_STENCIL_VALUE_MASK, &value);
                        tracked.stencil.test_mask = value;
                        glGetIntegerv(GL_STENCIL_WRITEMASK, &value);
                        tracked.stencil.write_mask = value;
                        glGetIntegerv(GL_STENCIL_FAIL, &value);
                        tracked.stencil.action_stencil_fail = value;
                        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &value);
                        tracked.stencil.action_depth_fail = value;
                        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &value);
                        tracked.stencil.action_depth_pass = value;
                        GLboolean colors[4]{};
                        glGetBooleanv(GL_COLOR_WRITEMASK, colors);
                        tracked.color_mask = {colors[0], colors[1], colors[2], colors[3]};
                        host_scope = std::make_unique<OpenGL::HostScissor::ScopedInclude>(
                            tracked, bounds, rawmode, scale, false, true);
                        const bool clipped = host_scope->Active();
                        Require(clipped == (rawmode == 3),
                                "production host clip eligibility mismatch");
                        if (clipped)
                            damage = {{tracked.scissor.x, tracked.scissor.y,
                                       std::int64_t(tracked.scissor.x) + tracked.scissor.width,
                                       std::int64_t(tracked.scissor.y) + tracked.scissor.height},
                                      true};
                        if (std::getenv("CARA_SCISSOR_ORACLE_HOST_SKIP_CLIP"))
                            tracked.scissor = original_scissor;
                        OpenGL::OpenGLState::InvalidateRasterState();
                        tracked.Apply();
                    }
                    const bool hardware_clip =
                        std::getenv("CARA_SCISSOR_ORACLE_HARDWARE_CLIP") != nullptr;
                    if (hardware_clip) {
                        Require(probe, "hardware clip is a separate diagnostic-only control");
                        if (rawmode == 3)
                            glScissor(6, 7, 12, 9); // fixed known 108-pixel INCLUDE intersection
                    }
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    NoError();
                    after.Read();
                    for (std::size_t i = 0; i < after.ds.size(); ++i) {
                        for (auto byte : after.color[i]) {
                            mode_hashes[rawmode] ^= byte;
                            mode_hashes[rawmode] *= 1099511628211ULL;
                        }
                        for (unsigned shift = 0; shift < 32; shift += 8) {
                            mode_hashes[rawmode] ^= (after.ds[i] >> shift) & 255;
                            mode_hashes[rawmode] *= 1099511628211ULL;
                        }
                    }
                    std::uint64_t changes = 0, local_c = 0, local_d = 0, local_s = 0,
                                  local_escape = 0;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const auto i = y * W + x;
                            const bool c = before.color[i] != after.color[i],
                                       d = (before.ds[i] >> 8) != (after.ds[i] >> 8),
                                       s = (before.ds[i] & 255) != (after.ds[i] & 255);
                            changed_color += c;
                            changed_depth += d;
                            changed_stencil += s;
                            local_c += c;
                            local_d += d;
                            local_s += s;
                            if (c || d || s) {
                                ++changes;
                                if (!Contains(damage.rect, x, y)) {
                                    ++escaped;
                                    ++local_escape;
                                }
                                Require(probe || Contains(damage.rect, x, y),
                                        "actual generated shader write escaped proposed footprint "
                                        "mode=" +
                                            std::to_string(rawmode) + " scale=" +
                                            std::to_string(scale) + " box=" + std::to_string(box) +
                                            " variant=" + std::to_string(variant) +
                                            " pixel=" + std::to_string(x) + "," +
                                            std::to_string(y) + " color=" + std::to_string(c) +
                                            " depth=" + std::to_string(d) +
                                            " stencil=" + std::to_string(s) +
                                            " before_ds=" + std::to_string(before.ds[i]) +
                                            " after_ds=" + std::to_string(after.ds[i]));
                            }
                        }
                    if (probe)
                        std::cout << "PROBE mode=" << rawmode << " variant=" << variant
                                  << " color=" << local_c << " depth=" << local_d
                                  << " stencil=" << local_s << " escaped=" << local_escape
                                  << " bounds=" << bounds.left << ',' << bounds.bottom << ','
                                  << bounds.right << ',' << bounds.top << '\n';
                    if (variant >= 13 &&
                        (rawmode == 0 || (rawmode == 3 && (box == 0 || box == 2 || box == 13)))) {
                        const auto expected = std::uint64_t(rawmode == 0 || box == 0 ? 192
                                                            : box == 2               ? 2
                                                                                     : 1) *
                                              scale * scale;
                        Require(local_c == (variant == 14 ? 0 : expected),
                                "non-vacuous fixed expected color coverage mismatch");
                        Require(local_d == (variant == 15 ? 0 : expected),
                                "non-vacuous fixed expected depth coverage mismatch");
                        Require(local_s == 0, "disabled stencil changed pixels");
                    }
                    if (rawmode == 3 && box == 0 && variant == 1)
                        Require(local_s == 192u * scale * scale && local_c == 0 && local_d == 0,
                                "non-vacuous stencil-only full INCLUDE mismatch");
                    if (host_helper && rawmode == 3) {
                        std::uint64_t expected = 0;
                        for (int y = current.bottom; y < current.top; ++y)
                            for (int x = current.left; x < current.right; ++x)
                                expected += float(x) + 0.5f >= float(bounds.left) &&
                                            float(x) + 0.5f < float(bounds.right) &&
                                            float(y) + 0.5f >= float(bounds.bottom) &&
                                            float(y) + 0.5f < float(bounds.top);
                        constexpr std::array<bool, 16> c{true, false, false, false, true, true,
                                                         true, true,  false, false, true, false,
                                                         true, true,  false, true};
                        constexpr std::array<bool, 16> d{true, false, false, false, false, false,
                                                         true, false, false, false, false, true,
                                                         true, true,  true,  false};
                        constexpr std::array<bool, 16> st{false, true,  true,  true,  false, true,
                                                          true,  true,  false, false, true,  true,
                                                          true,  false, false, false};
                        Require(local_c == expected * c[variant] &&
                                    local_d == expected * d[variant] &&
                                    local_s == expected * st[variant],
                                "production host clip independent channel counts mismatch");
                    }
                    if (host_helper) {
                        if (!std::getenv("CARA_SCISSOR_ORACLE_HOST_SKIP_RESTORE"))
                            host_scope->Restore();
                        tracked.Apply();
                        if (rawmode == 3) {
                            bool caught = false;
                            try {
                                OpenGL::HostScissor::ScopedInclude unwind(tracked, bounds, rawmode,
                                                                          scale, false, true);
                                Require(unwind.Active(), "throw control was not clipped");
                                tracked.Apply();
                                throw 42;
                            } catch (int value) {
                                caught = value == 42;
                            }
                            Require(caught && !tracked.TryApplyBlitState(),
                                    "exception must invalidate the raster snapshot");
                            GLint after_throw[4]{};
                            glGetIntegerv(GL_SCISSOR_BOX, after_throw);
                            Require(glIsEnabled(GL_SCISSOR_TEST) &&
                                        after_throw[0] == original_scissor.x &&
                                        after_throw[1] == original_scissor.y &&
                                        after_throw[2] == original_scissor.width &&
                                        after_throw[3] == original_scissor.height,
                                    "exception did not restore actual scissor before any "
                                    "subsequent apply");
                            // Also cover a normal early return before the explicit cleanup.
                            [&] {
                                OpenGL::HostScissor::ScopedInclude early(tracked, bounds, rawmode,
                                                                         scale, false, true);
                                tracked.Apply();
                                return;
                            }();
                            Require(!tracked.TryApplyBlitState(),
                                    "early exit must invalidate the raster snapshot");
                            GLboolean color_masks[4]{}, depth_mask{};
                            glGetBooleanv(GL_COLOR_WRITEMASK, color_masks);
                            glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
                            GLint stencil_mask{};
                            glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask);
                            Require(color_masks[0] == tracked.color_mask.red_enabled &&
                                        color_masks[1] == tracked.color_mask.green_enabled &&
                                        color_masks[2] == tracked.color_mask.blue_enabled &&
                                        color_masks[3] == tracked.color_mask.alpha_enabled &&
                                        depth_mask == tracked.depth.write_mask &&
                                        GLuint(stencil_mask) == tracked.stencil.write_mask,
                                    "exception clip restoration changed unrelated write masks");
                        }
                        GLint actual[4]{};
                        glGetIntegerv(GL_SCISSOR_BOX, actual);
                        Require(glIsEnabled(GL_SCISSOR_TEST) && actual[0] == original_scissor.x &&
                                    actual[1] == original_scissor.y &&
                                    actual[2] == original_scissor.width &&
                                    actual[3] == original_scissor.height,
                                "tracked scissor not restored");
                        // A new generated Disabled shader draw must cover the whole original
                        // rectangle, not inherit the previous INCLUDE clip.
                        auto& next = *next_shader;
                        next.Bind(bounds);
                        tracked.draw.shader_program = next.program;
                        tracked.draw.uniform_buffer = next.ubo;
                        tracked.color_mask = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
                        tracked.depth.test_enabled = true;
                        tracked.depth.test_func = GL_ALWAYS;
                        tracked.depth.write_mask = GL_TRUE;
                        tracked.stencil.test_enabled = true;
                        tracked.stencil.test_func = GL_ALWAYS;
                        tracked.stencil.write_mask = 255;
                        tracked.stencil.action_stencil_fail = GL_ZERO;
                        tracked.stencil.action_depth_fail = GL_ZERO;
                        tracked.stencil.action_depth_pass = GL_ZERO;
                        glDisable(GL_SCISSOR_TEST);
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glDepthMask(GL_TRUE);
                        glStencilMask(255);
                        glClearBufferfv(GL_COLOR, 0, sentinel);
                        glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0x5a);
                        OpenGL::OpenGLState::InvalidateRasterState();
                        tracked.Apply();
                        glDrawArrays(GL_TRIANGLES, 0, 3);
                        NoError();
                        Pixels next_pixels;
                        next_pixels.Read();
                        for (int y = 0; y < H; ++y)
                            for (int x = 0; x < W; ++x) {
                                const auto i = y * W + x;
                                const bool inside = Contains(current, x, y);
                                Require((next_pixels.color[i] != before.color[i]) == inside &&
                                            ((next_pixels.ds[i] >> 8) != (before.ds[i] >> 8)) ==
                                                inside &&
                                            ((next_pixels.ds[i] & 255) != (before.ds[i] & 255)) ==
                                                inside,
                                        "next Disabled draw inherited clipping or corrupted an "
                                        "untouched pixel");
                            }
                        ++restored_draws;
                    }
                    if (hardware_clip && rawmode == 3) {
                        const char* fragment = std::getenv("CARA_SCISSOR_ORACLE_FRAGMENT");
                        const bool unconditional =
                            fragment && std::string(fragment).rfind("discard", 0) == 0;
                        if (unconditional)
                            Require(local_c == 0 && local_d == 0 && local_s == 0,
                                    "unconditional discard wrote through hardware clip");
                        else {
                            constexpr std::array<unsigned, 13> colors{108, 0, 0, 0,   108, 108, 108,
                                                                      108, 0, 0, 108, 0,   108};
                            constexpr std::array<unsigned, 13> depths{108, 0, 0, 0, 0,   0,  108,
                                                                      0,   0, 0, 0, 108, 108};
                            constexpr std::array<unsigned, 13> stencils{
                                0, 108, 108, 108, 0, 108, 108, 108, 0, 0, 108, 108, 108};
                            Require(local_c == colors[variant] && local_d == depths[variant] &&
                                        local_s == stencils[variant],
                                    "hardware clip fixed per-channel coverage mismatch");
                        }
                    }
                    const bool empty = damage.rect.right == damage.rect.left ||
                                       damage.rect.top == damage.rect.bottom;
                    if (rawmode == 3 && damage.eligible && empty) {
                        ++empty_cases;
                        Require(changes == 0, "empty INCLUDE changed a framebuffer component");
                    }
                    if (!probe && rawmode == 3 && box == 1 && variant == 0)
                        Require(changes > 0, "nonempty INCLUDE oracle was vacuous");
                    ++cases;
                }
            }
    }
    Require(probe || (changed_color && changed_depth && changed_stencil && empty_cases),
            "required component or empty witness missing");
    Require(!probe || escaped == 0,
            "diagnostic witnessed escaped writes=" + std::to_string(escaped));
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
    glDeleteTextures(1, &depth);
}
class OracleWindow final : public Frontend::EmuWindow {
public:
    OracleWindow() {
        UpdateFramebufferLayout(400, 480);
    }
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
};
// Independent byte-address mapping for fixture expectations, not production's
// Morton decoder or damage-interval helper.
unsigned GuestOffset(unsigned x, unsigned y, unsigned width, unsigned height, unsigned bpp) {
    const unsigned gy = height - 1 - y;
    unsigned morton = 0;
    for (unsigned bit = 0; bit < 3; ++bit) {
        morton |= ((x >> bit) & 1) << (2 * bit);
        morton |= ((gy >> bit) & 1) << (2 * bit + 1);
    }
    return (((gy / 8) * (width / 8) + x / 8) * 64 + morton) * bpp;
}
void RunCoherence() {
    using namespace OpenGL;
    Memory::MemorySystem memory;
    OracleWindow window;
    Settings::values.use_hw_renderer = true;
    Settings::values.use_hw_shader = false;
    Settings::values.use_cpu_jit = false;
    Settings::values.use_shader_jit = false;
    Settings::values.use_gles = false;
    Settings::values.resolution_factor = 1;
    Settings::values.custom_textures = false;
    Settings::values.use_shader_cache = false;
    Settings::values.skip_cpu_write = false;
    OpenGLState initial;
    OpenGLState::InvalidateRasterState();
    initial.Apply();
    Require(VideoCore::Init(window, memory) == VideoCore::ResultStatus::Success,
            "ROM-free native video init failed");
    auto& cache = static_cast<RasterizerOpenGL*>(VideoCore::Rasterizer())->res_cache;
    constexpr unsigned bytes = 65536;
    constexpr PAddr scratch = Memory::VRAM_PADDR_END - bytes;
    auto* ram = memory.GetPhysicalPointer(scratch);
    Require(ram != nullptr, "scratch VRAM unavailable");
    std::vector<u8> sentinel(bytes);
    for (unsigned i = 0; i < bytes; ++i)
        sentinel[i] = u8(i * 13 + 7);
    Shader shader(3);
    GLuint vao{}, fb{};
    glGenVertexArrays(1, &vao);
    glGenFramebuffers(1, &fb);
    unsigned coherence_cases = 0;
    for (auto format : {SurfaceParams::PixelFormat::RGBA8, SurfaceParams::PixelFormat::RGB8})
        for (bool retain : {false, true})
            for (unsigned write_size : {1u, 2u, 4u, 8u})
                for (unsigned location = 0; location < 4; ++location) {
                    std::vector<u8> baseline;
                    for (bool narrow : {false, true}) {
                        cache.SetScaleFactor(1);
                        cache.retain_small_cpu_writes = retain;
                        std::copy(sentinel.begin(), sentinel.end(), ram);
                        SurfaceParams p{};
                        p.addr = scratch + 256;
                        p.width = p.stride = 32;
                        p.height = 32;
                        p.is_tiled = true;
                        p.res_scale = 1;
                        p.pixel_format = format;
                        p.UpdateParams();
                        const unsigned bpp = SurfaceParams::GetFormatBpp(format) / 8;
                        auto owner = cache.GetSurface(p, ScaleMatch::Exact, true);
                        auto other = p;
                        other.width = other.stride = 16;
                        other.height = 64;
                        other.UpdateParams();
                        auto alias = cache.GetSurface(other, ScaleMatch::Exact, true);
                        Require(owner && alias && owner != alias,
                                "overlapping different-stride aliases not established");
                        auto state = OpenGLState::GetCurState();
                        state.draw.draw_framebuffer = fb;
                        state.draw.read_framebuffer = fb;
                        state.draw.shader_program = shader.program;
                        state.draw.vertex_array = vao;
                        state.cull.enabled = false;
                        state.depth.test_enabled = false;
                        state.stencil.test_enabled = false;
                        state.blend.enabled = false;
                        state.color_mask = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
                        state.viewport = {0, 0, 32, 32};
                        state.scissor = {true, 0, 0, 32, 32};
                        state.Apply();
                        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_TEXTURE_2D, owner->texture.handle, 0);
                        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                               GL_TEXTURE_2D, 0, 0);
                        Require(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                                    GL_FRAMEBUFFER_COMPLETE,
                                "cache target incomplete");
                        shader.Bind({1, 1, 9, 3});
                        owner->InvalidateAllWatcher();
                        glDrawArrays(GL_TRIANGLES, 0, 3);
                        NoError();
                        const auto result =
                            ScissorDamage::NarrowInclude({0, 0, 32, 32}, {1, 1, 9, 3}, 3, 1, false);
                        const Rect footprint = narrow ? result.rect : Rect{0, 0, 32, 32};
                        const Common::Rectangle<u32> rectangle{
                            u32(footprint.left), u32(footprint.top), u32(footprint.right),
                            u32(footprint.bottom)};
                        const auto interval = owner->GetSubRectInterval(rectangle);
                        cache.InvalidateRegion(boost::icl::first(interval),
                                               boost::icl::length(interval), owner);
                        auto expected = sentinel;
                        const std::array<u8, 4> rgba{255, 128, 64, 255};
                        const std::array<u8, 3> rgb{128, 64, 255};
                        for (unsigned y = 1; y < 3; ++y)
                            for (unsigned x = 1; x < 9; ++x) {
                                const auto offset = 256 + GuestOffset(x, y, 32, 32, bpp);
                                for (unsigned c = 0; c < bpp; ++c)
                                    expected[offset + c] = bpp == 4 ? rgba[c] : rgb[c];
                            }
                        // An outside CPU read flushes only its requested ownership interval.
                        cache.FlushRegion(scratch, 1);
                        Require(ram[0] == expected[0], "outside CPU read changed sentinel");
                        unsigned offset = location == 0   ? GuestOffset(2, 2, 32, 32, bpp) + 1
                                          : location == 1 ? GuestOffset(20, 20, 32, 32, bpp) + 1
                                          : location == 2 ? (3 * 4 * 64 + 64) * bpp - 1
                                                          : 3 * 4 * 64 * bpp - 1;
                        offset += 256;
                        // Explicit production cache invalidation + VRAM write: this models
                        // the CPU cache policy, not an emulated ARM instruction dispatch.
                        cache.InvalidateRegion(scratch + offset, write_size, nullptr);
                        for (unsigned i = 0; i < write_size; ++i)
                            expected[offset + i] = ram[offset + i] = u8(0xa0 + i + write_size);
                        cache.FlushAll();
                        Require(std::equal(expected.begin(), expected.end(), ram),
                                "FlushAll lost GPU/CPU bytes or untouched sentinels");
                        alias = cache.GetSurface(other, ScaleMatch::Exact, true);
                        Require(bool(alias), "alias reacquisition failed");
                        alias->DownloadGLTexture(alias->GetRect());
                        alias->FlushGLBuffer(alias->addr, alias->end);
                        Require(std::equal(expected.begin(), expected.end(), ram),
                                "reacquired alias lost partial write or neighboring bytes");
                        owner = cache.GetSurface(p, ScaleMatch::Exact, true);
                        Require(bool(owner), "owner reacquisition failed");
                        owner->DownloadGLTexture(owner->GetRect());
                        owner->FlushGLBuffer(owner->addr, owner->end);
                        Require(std::equal(expected.begin(), expected.end(), ram),
                                "owner reload differs after alias flow");
                        NoError();
                        std::vector<u8> actual(ram, ram + bytes);
                        if (!narrow)
                            baseline = std::move(actual);
                        else
                            Require(actual == baseline,
                                    "narrow footprint differs from baseline final VRAM");
                        ++coherence_cases;
                    }
                }
    cache.SetScaleFactor(1);
    glDeleteFramebuffers(1, &fb);
    glDeleteVertexArrays(1, &vao);
    std::cout << "COHERENCE cases=" << coherence_cases
              << " RGBA8/RGB8 overlapping tiled aliases and1/2/4/8byte writes "
                 "inside/outside/cross-tile/cross-row passed\n";
    VideoCore::Shutdown();
}
void RunFramebufferBackingCoherence() {
    using namespace OpenGL;
    Memory::MemorySystem memory;
    OracleWindow window;
    Settings::values.use_hw_renderer=true; Settings::values.use_hw_shader=false;
    Settings::values.use_cpu_jit=false; Settings::values.use_shader_jit=false;
    Settings::values.use_gles=false; Settings::values.resolution_factor=1;
    Settings::values.custom_textures=false; Settings::values.use_shader_cache=false;
    Settings::values.skip_cpu_write=false;
    OpenGLState initial; OpenGLState::InvalidateRasterState(); initial.Apply();
    Require(VideoCore::Init(window,memory)==VideoCore::ResultStatus::Success,"backing oracle video init");
    auto& cache=static_cast<RasterizerOpenGL*>(VideoCore::Rasterizer())->res_cache;
    constexpr unsigned bytes=65536;constexpr PAddr scratch=Memory::VRAM_PADDR_END-bytes;
    auto* ram=memory.GetPhysicalPointer(scratch);Require(ram!=nullptr,"backing oracle VRAM");
    std::vector<u8> sentinel(bytes);for(unsigned i=0;i<bytes;++i)sentinel[i]=u8(i*13+7);
    Shader shader(3);GLuint vao{},fb{};glGenVertexArrays(1,&vao);glGenFramebuffers(1,&fb);
    const auto old_regs=Pica::g_state.regs;
    unsigned tested=0,selected_tight=0,selected_legacy=0;
    for(unsigned parent_rows:{48u,64u})for(bool retain:{false,true})for(unsigned write_size:{1u,2u,4u,8u})for(unsigned location=0;location<4;++location) {
        std::vector<u8> baseline;
        for(bool tight:{false,true}) {
            cache.SetScaleFactor(1);cache.retain_small_cpu_writes=retain;cache.tight_framebuffer_backing=tight;
            std::copy(sentinel.begin(),sentinel.end(),ram);
            SurfaceParams p{};p.addr=scratch+256;p.width=p.stride=32;p.height=parent_rows;
            p.is_tiled=true;p.res_scale=1;p.pixel_format=SurfaceParams::PixelFormat::RGBA8;p.UpdateParams();
            auto q=p;q.addr+=(parent_rows-32)*32*4;q.height=32;q.UpdateParams();
            auto parent=cache.GetSurface(p,ScaleMatch::Exact,true);
            auto view=cache.GetSurface(q,ScaleMatch::Exact,true);
            Require(parent && view && parent!=view,"backing oracle missing overlapping views");
            auto state=OpenGLState::GetCurState();state.draw.draw_framebuffer=state.draw.read_framebuffer=fb;
            state.draw.shader_program=shader.program;state.draw.vertex_array=vao;
            state.cull.enabled=false;state.depth.test_enabled=false;state.stencil.test_enabled=false;
            state.blend.enabled=false;state.color_mask={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
            state.viewport={0,0,32,static_cast<GLsizei>(parent_rows)};state.scissor={true,0,0,32,static_cast<GLsizei>(parent_rows)};state.Apply();
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,parent->texture.handle,0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
            Require(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"backing parent incomplete");
            shader.Bind({0,0,32,parent_rows});parent->InvalidateAllWatcher();glDrawArrays(GL_TRIANGLES,0,3);NoError();
            cache.InvalidateRegion(p.addr,p.size,parent);
            Require(!view->IsRegionValid(q.GetInterval()),"backing fixture needs invalid tight view");
            auto expected=sentinel;const std::array<u8,4> rgba{255,128,64,255};
            for(unsigned offset=0;offset<p.size;offset+=4)
                std::copy(rgba.begin(),rgba.end(),expected.begin()+256+offset);
            Pica::g_state.regs.framebuffer={};Pica::g_state.regs.texturing={};
            auto& config=Pica::g_state.regs.framebuffer.framebuffer;
            config.width.Assign(32);config.height.Assign(31);config.color_format.Assign(Pica::FramebufferRegs::ColorFormat::RGBA8);
            config.color_buffer_address.Assign(q.addr/8);
            auto [chosen,depth,rect]=cache.GetFramebufferSurfaces(true,false,{0,32,32,0});
            const bool should_tight=tight && parent_rows==64;
            Require(chosen==(should_tight?view:parent) && !depth,"actual framebuffer selector chose wrong owner");
            Require(rect==Common::Rectangle<u32>{0,32,32,0},"framebuffer coordinate changed");
            Require(chosen->IsRegionValid(q.GetInterval()),"normal framebuffer validation was skipped");
            if(should_tight)++selected_tight;else ++selected_legacy;
            state=OpenGLState::GetCurState();state.draw.draw_framebuffer=state.draw.read_framebuffer=fb;
            state.viewport={0,0,32,32};state.scissor={true,1,1,8,2};
            state.color_mask={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};state.Apply();
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,chosen->texture.handle,0);
            Require(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"chosen backing incomplete");
            glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);NoError();
            cache.InvalidateRegion(q.addr,q.size,chosen);
            for(unsigned y=1;y<3;++y)for(unsigned x=1;x<9;++x) {
                const unsigned offset=256+GuestOffset(x,y,32,parent_rows,4);
                std::fill_n(expected.begin()+offset,4,0);
            }
            unsigned offset=location==0?256+GuestOffset(2,2,32,parent_rows,4)+1:
                location==1?256+GuestOffset(20,parent_rows-8,32,parent_rows,4)+1:
                location==2?256+(parent_rows-32)*32*4+255:256+p.size-1;
            cache.InvalidateRegion(scratch+offset,write_size,nullptr);
            for(unsigned i=0;i<write_size;++i)expected[offset+i]=ram[offset+i]=u8(0xA0+i+write_size);
            cache.FlushAll();
            Require(std::equal(expected.begin(),expected.end(),ram),"backing swap lost GPU/CPU/untouched bytes");
            for(const auto& request:{q,p}) {
                auto surface=cache.GetSurface(request,ScaleMatch::Exact,true);Require(bool(surface),"backing reacquire failed");
                surface->DownloadGLTexture(surface->GetRect());surface->FlushGLBuffer(surface->addr,surface->end);
                Require(std::equal(expected.begin(),expected.end(),ram),"alias reload lost a partial write");
            }
            if(!tight)baseline.assign(ram,ram+bytes);
            else Require(std::equal(baseline.begin(),baseline.end(),ram),"tight/legacy backing output differs");
            NoError();++tested;
        }
    }
    // Whole partitions can still change a later *legacy* selection: the first
    // draw changes validity before the next draw's exact-layout sampler guard.
    // Repeat with an incompatible-stride third alias to separate that mechanism
    // from the geometric rejection of overlapping screen-sized backings.
    unsigned reselection_cases=0,feedback_off=0,feedback_on=0;
    for(bool third_alias:{false,true})for(bool retain:{false,true}) {
        std::vector<u8> baseline_sample,baseline_memory;
        for(bool tight:{false,true}) {
            cache.SetScaleFactor(1);cache.retain_small_cpu_writes=retain;cache.tight_framebuffer_backing=tight;
            std::copy(sentinel.begin(),sentinel.end(),ram);
            SurfaceParams p{};p.addr=scratch+256;p.width=p.stride=32;p.height=64;
            p.is_tiled=true;p.res_scale=1;p.pixel_format=SurfaceParams::PixelFormat::RGBA8;p.UpdateParams();
            auto q=p;q.addr+=32*32*4;q.height=32;q.UpdateParams();
            auto a=p;a.width=a.stride=64;a.height=64;a.UpdateParams();
            auto parent=cache.GetSurface(p,ScaleMatch::Exact,true);
            auto view=cache.GetSurface(q,ScaleMatch::Exact,true);
            Surface alias;
            if(third_alias) {
                alias=cache.GetSurface(a,ScaleMatch::Exact,true);
                Require(alias && alias!=parent && alias!=view && !alias->CanSubRect(q),
                        "third alias must overlap physically without matching framebuffer layout");
            }
            auto state=OpenGLState::GetCurState();state.draw.draw_framebuffer=state.draw.read_framebuffer=fb;
            state.draw.shader_program=shader.program;state.draw.vertex_array=vao;
            state.cull.enabled=false;state.depth.test_enabled=false;state.stencil.test_enabled=false;
            state.blend.enabled=false;state.color_mask={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
            state.viewport={0,0,32,64};state.scissor={true,0,0,32,64};state.Apply();
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,parent->texture.handle,0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
            Require(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"reselection parent incomplete");
            shader.Bind({0,0,32,64});parent->InvalidateAllWatcher();glDrawArrays(GL_TRIANGLES,0,3);NoError();
            cache.InvalidateRegion(p.addr,p.size,parent);
            auto expected=sentinel;const std::array<u8,4> rgba{255,128,64,255};
            for(unsigned offset=0;offset<p.size;offset+=4)
                std::copy(rgba.begin(),rgba.end(),expected.begin()+256+offset);
            Pica::g_state.regs.framebuffer={};Pica::g_state.regs.texturing={};
            auto& config=Pica::g_state.regs.framebuffer.framebuffer;
            config.width.Assign(32);config.height.Assign(31);config.color_format.Assign(Pica::FramebufferRegs::ColorFormat::RGBA8);
            config.color_buffer_address.Assign(q.addr/8);
            auto [first,first_depth,first_rect]=cache.GetFramebufferSurfaces(true,false,{0,32,32,0});
            Require(first==(tight?view:parent) && !first_depth,"reselection initial owner");
            Require(first_rect==Common::Rectangle<u32>{0,32,32,0},"reselection initial coordinates");
            state=OpenGLState::GetCurState();state.draw.draw_framebuffer=state.draw.read_framebuffer=fb;
            state.scissor={true,1,1,8,2};state.color_mask={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};state.Apply();
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,first->texture.handle,0);
            glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);NoError();
            cache.InvalidateRegion(q.addr,q.size,first);
            for(unsigned y=1;y<3;++y)for(unsigned x=1;x<9;++x)
                std::fill_n(expected.begin()+256+GuestOffset(x,y,32,64,4),4,0);
            Require(view->IsRegionValid(q.GetInterval())==tight && parent->IsRegionValid(q.GetInterval())!=tight,
                    "first draw did not establish distinct normal validity histories");
            if(alias)Require(!alias->IsRegionValid(q.GetInterval()),"third alias escaped normal invalidation");
            auto& texturing=Pica::g_state.regs.texturing;
            texturing.main_config.texture0_enable.Assign(1);
            texturing.texture0.width.Assign(32);texturing.texture0.height.Assign(32);
            texturing.texture0.address.Assign(q.addr/8);
            texturing.texture0_format.Assign(Pica::TexturingRegs::TextureFormat::RGBA8);
            auto [next,next_depth,next_rect]=cache.GetFramebufferSurfaces(true,false,{0,32,32,0});
            Require(next==(tight?view:parent) && !next_depth && next_rect==first_rect,
                    "later legacy selection did not follow validity before sampler guard");
            auto sampled=cache.GetTextureSurface(texturing.GetTextures()[0]);
            Require(sampled==view,"normal texture acquisition did not resolve exact view");
            // Same identity test and actual snapshot API as DrawTriangles. This
            // tests cache/feedback setup, not execution of a textured PICA draw.
            GLuint sampled_handle=sampled->texture.handle;
            if(sampled_handle==next->texture.handle) {
                sampled_handle=cache.GetTextureCopyHandle(sampled);
                if(tight)++feedback_on;else ++feedback_off;
                Require(sampled_handle && sampled_handle!=next->texture.handle,"feedback snapshot absent");
            }
            Require((sampled->texture_copy.handle!=0)==tight,"unexpected feedback allocation history");
            state=OpenGLState::GetCurState();state.draw.draw_framebuffer=state.draw.read_framebuffer=fb;state.Apply();
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,sampled_handle,0);
            Require(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"sample snapshot incomplete");
            std::vector<u8> sampled_pixels(32*32*4);
            glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,sampled_pixels.data());NoError();
            const std::array<u8,4> gl_rgba{255,64,128,255}; // Shader primary_color; guest RGBA8 bytes are ABGR.
            for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x) {
                const bool cleared=x>=1 && x<9 && y>=1 && y<3;
                for(unsigned c=0;c<4;++c)
                    Require(sampled_pixels[(y*32+x)*4+c]==(cleared?0:gl_rgba[c]),"sample snapshot bytes wrong");
            }
            // The selected owner remains attached for a second ordinary write.
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,next->texture.handle,0);
            state=OpenGLState::GetCurState();state.scissor={true,12,4,3,2};state.Apply();
            next->InvalidateAllWatcher();glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);NoError();
            cache.InvalidateRegion(q.addr,q.size,next);
            for(unsigned y=4;y<6;++y)for(unsigned x=12;x<15;++x)
                std::fill_n(expected.begin()+256+GuestOffset(x,y,32,64,4),4,0);
            cache.FlushAll();
            Require(std::equal(expected.begin(),expected.end(),ram),"reselection lost GPU or untouched bytes");
            std::vector<SurfaceParams> reloads{q,p};if(third_alias)reloads.push_back(a);
            for(const auto& request:reloads) {
                auto reloaded=cache.GetSurface(request,ScaleMatch::Exact,true);
                Require(bool(reloaded),"reselection alias reload missing");
                reloaded->DownloadGLTexture(reloaded->GetRect());reloaded->FlushGLBuffer(reloaded->addr,reloaded->end);
                Require(std::equal(expected.begin(),expected.end(),ram),"reselection alias reload bytes wrong");
            }
            if(!tight){baseline_sample=sampled_pixels;baseline_memory.assign(ram,ram+bytes);}
            else Require(sampled_pixels==baseline_sample && std::equal(baseline_memory.begin(),baseline_memory.end(),ram),
                         "flag states differ in sampled pixels or complete scratch VRAM");
            NoError();++reselection_cases;
        }
    }
    Require(reselection_cases==8 && feedback_off==0 && feedback_on==4,"reselection feedback counts");
    std::cout<<"FRAMEBUFFER_RESELECTION cases="<<reselection_cases<<" feedback_off="<<feedback_off
             <<" feedback_on="<<feedback_on<<" two/three aliases / actual snapshot API / sampled pixels / whole scratch VRAM passed\n";
    cache.SetScaleFactor(1);Pica::g_state.regs=old_regs;
    glDeleteFramebuffers(1,&fb);glDeleteVertexArrays(1,&vao);VideoCore::Shutdown();
    Require(selected_tight==32 && selected_legacy==96,"both actual selection paths not exercised");
    std::cout<<"FRAMEBUFFER_BACKING cases="<<tested<<" tight="<<selected_tight<<" legacy="<<selected_legacy
             <<" whole partitions / overlapping views rejected / invalid-view validation / GPU bytes outside view / CPU1,2,4,8 / tile and allocation edges passed\n";
}
} // namespace
int main() {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format{};
    CGLContextObj context{};
    GLint n{};
    if (CGLChoosePixelFormat(attrs, &format, &n) != kCGLNoError || !format)
        return 2;
    const auto e = CGLCreateContext(format, nullptr, &context);
    CGLDestroyPixelFormat(format);
    if (e != kCGLNoError || !context || CGLSetCurrentContext(context) != kCGLNoError)
        return 2;
    static void* lib =
        dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    if (!lib || !gladLoadGLLoader([](const char* name) -> void* { return dlsym(lib, name); }))
        return 2;
    OpenGL::GLES = false;
    OpenGL::AllowShadow = false;
    std::cout << "GL=" << glGetString(GL_VERSION) << " renderer=" << glGetString(GL_RENDERER)
              << '\n';
    try {
        if (std::getenv("CARA_SCISSOR_ORACLE_FRAMEBUFFER_ONLY"))
            RunFramebufferBackingCoherence();
        else if (std::getenv("CARA_SCISSOR_ORACLE_COHERENCE_ONLY"))
            RunCoherence();
        else {
            Run();
            if (!probe)
                RunCoherence();
        }
        for (unsigned mode = 0; mode < 4; ++mode)
            std::cout << "MODE_HASH " << mode << " " << mode_hashes[mode] << '\n';
        std::cout << "RESTORED_NEXT_DRAWS " << restored_draws << '\n';
        std::cout << "PASS checks=" << checks << " cases=" << cases
                  << " changed_color=" << changed_color << " changed_depth=" << changed_depth
                  << " changed_stencil=" << changed_stencil << " empty_include=" << empty_cases
                  << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(context);
}
