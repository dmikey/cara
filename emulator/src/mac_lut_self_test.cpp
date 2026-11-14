// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free native GPU oracle. Uses the actual rasterizer upload path and samples
// its real texture buffer; expected values never come from its CPU shadow.
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <dlfcn.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <glad/glad.h>
#include "core/settings.h"
#include "common/scope_exit.h"
#include "video_core/pica_state.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_vars.h"

namespace OpenGL {
struct LUTResidencyTestAccess {
    static void Run(RasterizerOpenGL& rasterizer, int proc_mode = 0) {
        const auto saved_proctex = Pica::g_state.proctex;
        const auto saved_lighting = Pica::g_state.lighting;
        const auto saved_fog = Pica::g_state.fog;
        const auto saved_config = Pica::g_state.regs.lighting.lut_config;
        SCOPE_EXIT({
            Pica::g_state.proctex = saved_proctex;
            Pica::g_state.lighting = saved_lighting;
            Pica::g_state.fog = saved_fog;
            Pica::g_state.regs.lighting.lut_config = saved_config;
        });
        GLuint program = glCreateProgram();
        const auto shader = [](GLenum type, const char* source) {
            GLuint object = glCreateShader(type);
            glShaderSource(object, 1, &source, nullptr); glCompileShader(object);
            GLint ok{}; glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[2048]{}; glGetShaderInfoLog(object, sizeof(log), nullptr, log); throw std::runtime_error(log); }
            return object;
        };
        GLuint vs = shader(GL_VERTEX_SHADER, R"(#version 330 core
void main() { vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); gl_Position=vec4(p*2.0-1.0,0.0,1.0); })");
        GLuint fs = shader(GL_FRAGMENT_SHADER, R"(#version 330 core
uniform samplerBuffer lut; uniform int base; out vec4 color;
void main() { color=texelFetch(lut,base+int(gl_FragCoord.x)); })");
        glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
        GLint linked{}; glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) throw std::runtime_error("LUT sampler program link failed");
        glDeleteShader(vs); glDeleteShader(fs);
        GLuint texture{}, framebuffer{}, vao{};
        glGenTextures(1, &texture); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
        glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("LUT oracle framebuffer incomplete");
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_COLOR_LOGIC_OP);
        glDisable(GL_CLIP_DISTANCE0); glDisable(GL_CLIP_DISTANCE1);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(program); glBindProgramPipeline(0);
        glUniform1i(glGetUniformLocation(program, "lut"), 0);
        glBindTexture(GL_TEXTURE_BUFFER, rasterizer.texture_buffer_lut_lf.handle);
        glBindSampler(0, 0); glViewport(0, 0, 256, 1);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0); glReadBuffer(GL_COLOR_ATTACHMENT0);
        const GLint base_location = glGetUniformLocation(program, "base");
        std::array<std::array<float, 4>, 256> expected{};
        for (auto& entry : expected) entry = {0, 0, 0, 1};
        GLuint source_texture = rasterizer.texture_buffer_lut_lf.handle;
        std::size_t source_size = rasterizer.texture_lf_buffer.GetSize();
        std::size_t texel_size = sizeof(GLvec2);
        SCOPE_EXIT({
            glDeleteProgram(program); glDeleteVertexArrays(1, &vao);
            glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
        });
        std::size_t checked = 0;
        const auto check = [&](const char* stage, GLint offset, unsigned entries) {
            if (offset < 0 || static_cast<std::size_t>(offset) + entries >
                                  source_size / texel_size)
                throw std::runtime_error(std::string(stage) + ": missing or out-of-bounds GPU range");
            const GLenum prior_error = glGetError();
            if (prior_error != GL_NO_ERROR)
                throw std::runtime_error(std::string(stage) + ": upload/setup GL error " + std::to_string(prior_error));
            glBindTexture(GL_TEXTURE_BUFFER, source_texture);
            glUniform1i(base_location, offset);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            std::array<std::array<float, 4>, 256> actual;
            for (auto& entry : actual) entry.fill(-123.0f);
            glReadPixels(0, 0, entries, 1, GL_RGBA, GL_FLOAT, actual.data());
            if (glGetError() != GL_NO_ERROR) throw std::runtime_error(std::string(stage) + ": draw/readback GL error");
            for (unsigned i = 0; i < entries; ++i) {
                if (actual[i][0] != expected[i][0] || actual[i][1] != expected[i][1] || actual[i][2] != expected[i][2] || actual[i][3] != expected[i][3])
                    throw std::runtime_error(std::string(stage) + ": GPU texel " + std::to_string(i) +
                        " expected (" + std::to_string(expected[i][0]) + "," + std::to_string(expected[i][1]) +
                        ") got (" + std::to_string(actual[i][0]) + "," + std::to_string(actual[i][1]) +
                        ") offset=" + std::to_string(offset));
                ++checked;
            }
        };
        const auto lighting_offset = [&](unsigned index) {
            return rasterizer.uniform_block_data.light_data.lighting_lut_offset[index / 4][index % 4];
        };
        if (proc_mode) {
            const bool rgba = proc_mode == 2;
            const bool zero_noise = proc_mode == 3;
            auto& guest = Pica::g_state.proctex;
            for (auto& e : guest.noise_table) e.raw = zero_noise ? 0 : 4095;
            for (auto& e : guest.color_map_table) e.raw = zero_noise ? 4095 : 0;
            for (auto& e : guest.alpha_map_table) e.raw = 0;
            for (auto& e : guest.color_table) e.raw = 0;
            for (auto& e : guest.color_diff_table) e.raw = 0;
            rasterizer.SyncAndUploadLUTs();
            auto& uniforms = rasterizer.uniform_block_data;
            source_texture = rgba ? rasterizer.texture_buffer_lut_rgba.handle : rasterizer.texture_buffer_lut_rg.handle;
            source_size = rasterizer.texture_buffer.GetSize();
            texel_size = rgba ? sizeof(GLvec4) : sizeof(GLvec2);
            if (zero_noise) {
                check("initial RG zero noise beside color map", uniforms.data.proctex_noise_lut_offset, 128);
                std::cout << "PASS " << checked << " production initially zero noise GPU texels\n";
                return;
            }
            if (rgba) {
                for (auto& e : expected) e = {0, 0, 0, 0};
                check("initial RGBA zero color beside RG noise", uniforms.data.proctex_lut_offset, 256);
                check("initial RGBA zero difference beside RG noise", uniforms.data.proctex_diff_lut_offset, 256);
                guest.color_table[17].raw = 0xFF0000FF;
                uniforms.proctex_lut_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                expected[17] = {1, 0, 0, 1};
                check("partial RGBA color", uniforms.data.proctex_lut_offset, 256);
                guest.color_table[17].raw = 0;
                uniforms.proctex_lut_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                expected[17] = {0, 0, 0, 0};
                check("RGBA color nonzero to zero", uniforms.data.proctex_lut_offset, 256);
                // Preserve the existing difference storage conversion; this is
                // a residency oracle, not a new PICA interpolation definition.
                for (auto& e : guest.color_diff_table) e.r.Assign(127);
                uniforms.proctex_diff_lut_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                for (auto& e : expected) e = {127.0f / 255.0f, 0, 0, 0};
                check("RGBA difference nonzero", uniforms.data.proctex_diff_lut_offset, 256);
                for (auto& e : guest.color_diff_table) e.raw = 0;
                uniforms.proctex_diff_lut_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                for (auto& e : expected) e = {0, 0, 0, 0};
                check("RGBA difference nonzero to zero", uniforms.data.proctex_diff_lut_offset, 256);
            } else {
                check("initial RG zero color map beside noise", uniforms.data.proctex_color_map_offset, 128);
                check("initial RG zero alpha map beside noise", uniforms.data.proctex_alpha_map_offset, 128);
                for (auto& e : expected) e = {1, 0, 0, 1};
                check("initial RG nonzero noise", uniforms.data.proctex_noise_lut_offset, 128);
                const auto unchanged = uniforms.data.proctex_color_map_offset;
                uniforms.proctex_color_map_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                if (uniforms.data.proctex_color_map_offset != unchanged)
                    throw std::runtime_error("unchanged proctex zero was recopied");
                guest.color_map_table[17].raw = 4095;
                uniforms.proctex_color_map_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                for (auto& e : expected) e = {0, 0, 0, 1};
                expected[17][0] = 1;
                check("partial RG color map", uniforms.data.proctex_color_map_offset, 128);
                guest.color_map_table[17].raw = 0;
                uniforms.proctex_color_map_dirty = true;
                rasterizer.SyncAndUploadLUTs();
                expected[17][0] = 0;
                check("RG color map nonzero to zero", uniforms.data.proctex_color_map_offset, 128);
            }
            for (auto& e : guest.noise_table) e.raw = 0;
            uniforms.proctex_noise_lut_dirty = true;
            rasterizer.SyncAndUploadLUTs();
            glBindBuffer(GL_TEXTURE_BUFFER, rasterizer.texture_buffer.GetHandle());
            auto [mapped, start, orphaned] = rasterizer.texture_buffer.Map(source_size, 16);
            if (!mapped || !orphaned || start != 0) throw std::runtime_error("proctex ring wrap not forced");
            std::memset(mapped, 0x7F, source_size); rasterizer.texture_buffer.Unmap(source_size);
            uniforms.proctex_noise_lut_dirty = true;
            rasterizer.SyncAndUploadLUTs();
            if (rgba) {
                check("wrapped RGBA color", uniforms.data.proctex_lut_offset, 256);
                check("wrapped RGBA difference", uniforms.data.proctex_diff_lut_offset, 256);
            } else {
                check("wrapped RG noise", uniforms.data.proctex_noise_lut_offset, 128);
                check("wrapped RG color map", uniforms.data.proctex_color_map_offset, 128);
                check("wrapped RG alpha map", uniforms.data.proctex_alpha_map_offset, 128);
            }
            std::cout << "PASS " << checked << " production proctex " << (rgba ? "RGBA" : "RG") << " GPU texels\n";
            return;
        }
        // Constructor shadows and offsets are untouched. This first call must
        // allocate a zero table separately from its nonzero neighbor.
        for (auto& table : Pica::g_state.lighting.luts) for (auto& entry : table) entry.raw = 0;
        for (auto& entry : Pica::g_state.fog.lut) entry.raw = 0;
        for (auto& entry : Pica::g_state.lighting.luts[1]) entry.value.Assign(4095);
        rasterizer.SyncAndUploadLUTsLF();
        for (unsigned i = 0; i < Pica::LightingRegs::NumLightingSampler; ++i)
            if (i != 1) check("initial zero lighting beside nonzero table", lighting_offset(i), 256);
        check("initial zero fog beside nonzero lighting", rasterizer.uniform_block_data.data.fog_lut_offset, 128);
        for (auto& entry : expected) entry[0] = 1.0f;
        check("initial nonzero lighting", lighting_offset(1), 256);
        const auto original_zero_offset = lighting_offset(0);
        rasterizer.uniform_block_data.lighting_lut_dirty[0] = true;
        rasterizer.uniform_block_data.lighting_lut_dirty_any = true;
        rasterizer.SyncAndUploadLUTsLF();
        if (lighting_offset(0) != original_zero_offset) throw std::runtime_error("unchanged resident zero was recopied");
        Pica::g_state.lighting.luts[1][17].raw = 0; expected[17][0] = 0;
        rasterizer.uniform_block_data.lighting_lut_dirty[1] = true;
        rasterizer.uniform_block_data.lighting_lut_dirty_any = true;
        rasterizer.SyncAndUploadLUTsLF();
        check("partial lighting update", lighting_offset(1), 256);
        for (auto& entry : Pica::g_state.lighting.luts[1]) entry.raw = 0;
        for (auto& entry : expected) entry = {0, 0, 0, 1};
        rasterizer.uniform_block_data.lighting_lut_dirty[1] = true;
        rasterizer.uniform_block_data.lighting_lut_dirty_any = true;
        rasterizer.SyncAndUploadLUTsLF();
        check("lighting nonzero to zero", lighting_offset(1), 256);
        for (auto& entry : Pica::g_state.fog.lut) { entry.value.Assign(2047); entry.difference.Assign(-2047); }
        for (auto& entry : expected) entry = {1.0f, -1.0f, 0, 1};
        rasterizer.uniform_block_data.fog_lut_dirty = true;
        rasterizer.SyncAndUploadLUTsLF();
        check("fog signed difference", rasterizer.uniform_block_data.data.fog_lut_offset, 128);
        for (auto& entry : Pica::g_state.fog.lut) entry.raw = 0;
        for (auto& entry : expected) entry = {0, 0, 0, 1};
        rasterizer.uniform_block_data.fog_lut_dirty = true;
        rasterizer.SyncAndUploadLUTsLF();
        check("fog nonzero to zero", rasterizer.uniform_block_data.data.fog_lut_offset, 128);
        // Actual forced-state synchronization must still notice a restored
        // selected table and fog, without relying on command dirty callbacks.
        for (auto& entry : Pica::g_state.lighting.luts[1]) entry.value.Assign(4095);
        for (auto& entry : Pica::g_state.fog.lut) entry.value.Assign(2047);
        Pica::g_state.regs.lighting.lut_config.type.Assign(1);
        rasterizer.SyncEntireState();
        rasterizer.SyncAndUploadLUTsLF();
        for (auto& entry : expected) entry = {1.0f, 0.0f, 0, 1};
        check("forced state lighting", lighting_offset(1), 256);
        check("forced state fog", rasterizer.uniform_block_data.data.fog_lut_offset, 128);
        for (auto& entry : Pica::g_state.lighting.luts[1]) entry.raw = 0;
        for (auto& entry : Pica::g_state.fog.lut) entry.raw = 0;
        rasterizer.SyncEntireState();
        rasterizer.SyncAndUploadLUTsLF();
        for (auto& entry : expected) entry = {0, 0, 0, 1};
        check("restored zero lighting", lighting_offset(1), 256);
        check("restored zero fog", rasterizer.uniform_block_data.data.fog_lut_offset, 128);
        // Force a real stream orphan/wrap. Poison all backing bytes so a stale
        // offset or missing zero upload cannot accidentally pass this test.
        glBindBuffer(GL_TEXTURE_BUFFER, rasterizer.texture_lf_buffer.GetHandle());
        auto [data, offset, invalidated] = rasterizer.texture_lf_buffer.Map(rasterizer.texture_lf_buffer.GetSize(), 16);
        if (!data || !invalidated || offset != 0) throw std::runtime_error("oracle did not force first ring wrap");
        std::memset(data, 0x7F, rasterizer.texture_lf_buffer.GetSize());
        rasterizer.texture_lf_buffer.Unmap(rasterizer.texture_lf_buffer.GetSize());
        rasterizer.uniform_block_data.fog_lut_dirty = true;
        rasterizer.SyncAndUploadLUTsLF();
        for (unsigned i = 0; i < Pica::LightingRegs::NumLightingSampler; ++i)
            check("wrapped lighting residency", lighting_offset(i), 256);
        check("wrapped fog residency", rasterizer.uniform_block_data.data.fog_lut_offset, 128);
        std::cout << "PASS " << checked << " production LUT GPU texels; initial/partial/zero/forced-state/wrap\n";
    }
};
}

int main(int argc, char** argv) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core), static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format{}; CGLContextObj context{}; GLint count{};
    if (CGLChoosePixelFormat(attrs, &format, &count) != kCGLNoError || !format) return 2;
    auto result = CGLCreateContext(format, nullptr, &context); CGLDestroyPixelFormat(format);
    if (result != kCGLNoError || !context || CGLSetCurrentContext(context) != kCGLNoError) return 2;
    static void* symbols = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    if (!symbols || !gladLoadGLLoader([](const char* name)->void* { return dlsym(symbols, name); })) return 2;
    OpenGL::GLES = false; Settings::values.stream_buffer_hack = false;
    std::cout << "GL=" << glGetString(GL_VERSION) << " renderer=" << glGetString(GL_RENDERER) << '\n' << std::flush;
    try {
        // Reconstructing the renderer covers the initialization path used after
        // context recreation/state-load; neither run relies on previous ranges.
        int first_mode = 0, last_mode = 3;
        if (argc > 1) {
            const std::string mode = argv[1];
            if (mode == "--proctex-rg") first_mode = last_mode = 1;
            else if (mode == "--proctex-rgba") first_mode = last_mode = 2;
            else if (mode == "--proctex-zero-noise") first_mode = last_mode = 3;
            else throw std::runtime_error("unknown oracle mode");
        }
        for (unsigned run = 0; run < 2; ++run) for (int mode = first_mode; mode <= last_mode; ++mode) {
            OpenGL::OpenGLState initial; OpenGL::OpenGLState::InvalidateRasterState(); initial.Apply();
            OpenGL::RasterizerOpenGL rasterizer;
            OpenGL::LUTResidencyTestAccess::Run(rasterizer, mode);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
    CGLSetCurrentContext(nullptr); CGLDestroyContext(context);
}
