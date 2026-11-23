// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/settings.h"
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <glad/glad.h>

#include "common/assert.h"
#include "common/file_util.h"
#include "common/frame_timing.h"
#include "common/hash.h"
#include "video_core/renderer_opengl/gl_shader_census.h"
#include "common/logging/log.h"
#include "video_core/renderer_opengl/gl_shader_util.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/gl_ios_compat.h"

namespace OpenGL {

GLuint LoadShader(const char* source, GLenum type) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::ShaderCompile);
    std::string version = GLES ? R"(#version 320 es

#define CARA_GLES

#if defined(GL_ANDROID_extension_pack_es31a)
#extension GL_ANDROID_extension_pack_es31a : enable
#endif // defined(GL_ANDROID_extension_pack_es31a)

#if defined(GL_EXT_clip_cull_distance)
#extension GL_EXT_clip_cull_distance : enable
#endif // defined(GL_EXT_clip_cull_distance)
)"
                                     : "#version 330\n";

    std::string compatible_source(source);
#ifdef CARA_IOS_GLES
#ifdef CARA_MAC_ATLAS_PIPELINE
    version = "#version 410 core\n";
#else
    version = "#version 300 es\n";
#endif
    version += R"(#define CARA_GLES
precision highp int;
precision highp float;
precision highp sampler2D;
vec4 CaraLUTFetch(sampler2D lut, int index) {
    return texelFetch(lut, ivec2(index & 1023, index >> 10), 0);
}
)";
    const auto ReplaceAll = [&](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = compatible_source.find(from, pos)) != std::string::npos) {
            compatible_source.replace(pos, from.size(), to); pos += to.size();
        }
    };
#ifdef CARA_MAC_ATLAS_PIPELINE
    // Runtime API/state remains desktop GL, where these clip planes are enabled.
    // Keep their original PICA writes without pretending an ES extension exists.
    ReplaceAll("#if !defined(CARA_GLES) || defined(GL_EXT_clip_cull_distance)", "#if 1");
    // Native generation emits these before its declarations, but our shared
    // LUT helper is already a declaration. Shadows are rejected at Init.
    ReplaceAll("#extension GL_ARB_shader_image_load_store : enable\n", "");
    ReplaceAll("#extension GL_ARB_shader_image_size : enable\n", "");
#endif
#ifdef CARA_MAC_ATLAS_PIPELINE
    // Atlas LUTs are 2D; the independent native legacy PBO converter still
    // uses a real buffer sampler and its native texelFetch signature.
    ReplaceAll("samplerBuffer texture_buffer_lut_", "sampler2D texture_buffer_lut_");
#else
    if (UseNativeLUTBuffers()) {
        version = "#version 320 es\n#define CARA_GLES\nprecision highp int;\n"
                  "precision highp float;\nprecision highp sampler2D;\n"
                  "precision highp samplerBuffer;\n";
    } else {
        ReplaceAll("samplerBuffer", "sampler2D");
    }
#endif
    ReplaceAll("precision highp uimage2D;", "");
    ReplaceAll("#define fma_safe(x, y, z) fma(x, y, z)", "#define fma_safe(x, y, z) ((x) * (y) + (z))");
    if (!UseNativeLUTBuffers())
        ReplaceAll("texelFetch(texture_buffer_lut_", "CaraLUTFetch(texture_buffer_lut_");
#endif
    const char* debug_type;
    switch (type) {
    case GL_VERTEX_SHADER:
        debug_type = "vertex";
        break;
    case GL_GEOMETRY_SHADER:
        debug_type = "geometry";
        break;
    case GL_FRAGMENT_SHADER:
        debug_type = "fragment";
        break;
    case GL_COMPUTE_SHADER:
        debug_type = "compute";
        break;
    default:
        UNREACHABLE();
    }

    std::array<const char*, 2> src_arr{version.data(), compatible_source.c_str()};
    GLuint shader_id = glCreateShader(type);
    LOG_DEBUG(Render_OpenGL, "Compiling {} shader: {}", debug_type, shader_id);
    glShaderSource(shader_id, static_cast<GLsizei>(src_arr.size()), src_arr.data(), nullptr);
    const bool census = ShaderCensus::Enabled();
    const auto compile_start = census ? ShaderCensus::Clock::now() : ShaderCensus::Clock::time_point{};
    glCompileShader(shader_id);

    GLint result = GL_FALSE;
    GLint info_log_length;
    glGetShaderiv(shader_id, GL_COMPILE_STATUS, &result);
    if (census) {
        const auto elapsed = ShaderCensus::ElapsedUs(compile_start);
        const std::string_view original{source};
        const u64 source_hash = Common::ComputeHash64(original.data(), original.size());
        ShaderCensus::Record("compile stage={} shader={} source={:016x} ok={} us={}",
                             debug_type, shader_id, source_hash, result, elapsed);
    }
    glGetShaderiv(shader_id, GL_INFO_LOG_LENGTH, &info_log_length);

    if (info_log_length > 1) {
        std::vector<char> shader_error(info_log_length);
        glGetShaderInfoLog(shader_id, info_log_length, nullptr, &shader_error[0]);
        if (result == GL_TRUE) {
            LOG_DEBUG(Render_OpenGL, "{}", &shader_error[0]);
        } else {
            LOG_ERROR(Render_OpenGL, "Error compiling {} shader:\n{}", debug_type,
                      &shader_error[0]);
            LOG_ERROR(Render_OpenGL, "Shader source code:\n{}{}", src_arr[0], src_arr[1]);
            // dump shader source code to file
            static std::atomic<u32> file_id{0};
            const std::string& log_dir = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
            const std::string filepath = fmt::format("{}{}_{}.txt", log_dir, debug_type, file_id.fetch_add(1, std::memory_order_relaxed));
            std::string shader(src_arr[0]);
            shader.append(src_arr[1]);
            shader.append("\n\n");
            shader.append(shader_error.data(), shader_error.size());
            FileUtil::WriteStringToFile(true, filepath, shader);
            glDeleteShader(shader_id);
            shader_id = 0;
        }
    }

#ifdef CARA_MAC_ATLAS_PIPELINE
    if (result != GL_TRUE && shader_id) { glDeleteShader(shader_id); shader_id=0; }
#endif
    return shader_id;
}

GLuint LoadProgram(bool separable_program, const std::vector<GLuint>& shaders) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::ShaderCompile);
    // Link the program
    LOG_DEBUG(Render_OpenGL, "Linking program...");

    GLuint program_id = glCreateProgram();

    for (GLuint shader : shaders) {
        if (shader != 0) {
            glAttachShader(program_id, shader);
        }
    }

    if (separable_program) {
        glProgramParameteri(program_id, GL_PROGRAM_SEPARABLE, GL_TRUE);
    }

#ifdef CARA_IOS_GLES
    if (Settings::values.use_shader_cache) {
        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        if (formats > 0) glProgramParameteri(program_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
#endif
    const bool census = ShaderCensus::Enabled();
    const auto link_start = census ? ShaderCensus::Clock::now() : ShaderCensus::Clock::time_point{};
    glLinkProgram(program_id);

    // Check the program
    GLint result = GL_FALSE;
    GLint info_log_length;
    glGetProgramiv(program_id, GL_LINK_STATUS, &result);
    if (census) {
        const auto elapsed = ShaderCensus::ElapsedUs(link_start);
        std::string stages;
        for (GLuint shader : shaders) {
            if (shader)
                fmt::format_to(std::back_inserter(stages), "{}{}", stages.empty() ? "" : ",", shader);
        }
        ShaderCensus::Record("link program={} shaders={} ok={} us={}", program_id, stages, result, elapsed);
    }
    glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);

    if (info_log_length > 1) {
        std::vector<char> program_error(info_log_length);
        glGetProgramInfoLog(program_id, info_log_length, nullptr, &program_error[0]);
        if (result == GL_TRUE) {
            LOG_DEBUG(Render_OpenGL, "{}", &program_error[0]);
        } else {
            LOG_ERROR(Render_OpenGL, "Error linking shader:\n{}", &program_error[0]);
        }
    }

#ifdef CARA_IOS_GLES
    if (result != GL_TRUE) {
        glDeleteProgram(program_id);
        return 0;
    }
#else
    ASSERT_MSG(result == GL_TRUE, "Shader not linked");
#endif

    for (GLuint shader : shaders) {
        if (shader != 0) {
            glDetachShader(program_id, shader);
        }
    }

    return program_id;
}

} // namespace OpenGL
