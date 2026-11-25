// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include "video_core/renderer_opengl/gl_shader_adreno_unused_quaternion.h"
#include <glad/glad.h>
#include "video_core/regs_lighting.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vertex_provenance.h"
#include "video_core/renderer_opengl/pica_to_gl.h"

namespace OpenGL {

enum class UniformBindings : GLuint { Common, Light, VS, GS };

struct LightSrc {
    alignas(16) GLvec3 specular_0;
    alignas(16) GLvec3 specular_1;
    alignas(16) GLvec3 diffuse;
    alignas(16) GLvec3 ambient;
    alignas(16) GLvec3 position;
    alignas(16) GLvec3 spot_direction; // negated
    GLfloat dist_atten_bias;
    GLfloat dist_atten_scale;
};

/// Uniform structure for the Uniform Buffer Object, all vectors must be 16-byte aligned
// NOTE: Always keep a vec4 at the end. The GL spec is not clear wether the alignment at
//       the end of a uniform block is included in UNIFORM_BLOCK_DATA_SIZE or not.
//       Not following that rule will cause problems on some AMD drivers.
struct UniformData {
    GLint alphatest_ref;
    GLfloat depth_scale;
    GLfloat depth_offset;
    GLfloat shadow_bias_constant;
    GLfloat shadow_bias_linear;
    GLint scissor_x1;
    GLint scissor_y1;
    GLint scissor_x2;
    GLint scissor_y2;
    GLint fog_lut_offset;
    GLint proctex_noise_lut_offset;
    GLint proctex_color_map_offset;
    GLint proctex_alpha_map_offset;
    GLint proctex_lut_offset;
    GLint proctex_diff_lut_offset;
    GLfloat proctex_bias;
    alignas(16) GLvec3 fog_color;
    alignas(8) GLvec2 proctex_noise_f;
    alignas(8) GLvec2 proctex_noise_a;
    alignas(8) GLvec2 proctex_noise_p;
    alignas(16) GLvec4 const_color[6]; // A vec4 color for each of the six tev stages
    alignas(16) GLvec4 tev_combiner_buffer_color;
    alignas(16) GLvec4 clip_coef;
#ifdef CARA_IOS_GLES
    alignas(16) GLvec4 cara_viewport;
#endif
    // GL lower-left owner origin/logical size, then half-open logical read crop.
    alignas(16) GLivec4 cara_tex0_view{};
    alignas(16) GLivec4 cara_tex0_crop{};
};

struct UniformLightData {
    alignas(16) GLivec4 lighting_lut_offset[Pica::LightingRegs::NumLightingSampler / 4];
    alignas(16) GLvec3 lighting_global_ambient;
    LightSrc light_src[8];
    GLfloat lighting_lut_scales[7];
    GLint shadow_texture_bias;
};

static_assert(
#ifdef CARA_IOS_GLES
    sizeof(UniformData) == 0x120,
#else
    sizeof(UniformData) == 0x110,
#endif
    "The size of the UniformData structure has changed, update the structure in the shader");
#ifdef CARA_IOS_GLES
static_assert(offsetof(UniformData, cara_tex0_view) == 0x100);
static_assert(offsetof(UniformData, cara_tex0_crop) == 0x110);
#else
static_assert(offsetof(UniformData, cara_tex0_view) == 0xF0);
static_assert(offsetof(UniformData, cara_tex0_crop) == 0x100);
#endif
static_assert(sizeof(GLivec4) == 16);
static_assert(sizeof(UniformData) < 0x4000,
              "UniformData structure must be less than 16kb as per the OpenGL spec");
static_assert(
        sizeof(UniformLightData) == 0x410,
        "The size of the UniformData structure has changed, update the structure in the shader");

/// Uniform struct for the Uniform Buffer Object that contains PICA vertex/geometry shader uniforms.
// NOTE: the same rule from UniformData also applies here.
struct PicaUniformsData {
    void SetFromRegs(const Pica::ShaderRegs& regs, const Pica::Shader::ShaderSetup& setup);

    struct BoolAligned {
        alignas(16) GLint b;
    };

    std::array<BoolAligned, 16> bools;
    alignas(16) std::array<GLuvec4, 4> i;
    alignas(16) std::array<GLvec4, 96> f;
};

struct VSUniformData {
    PicaUniformsData uniforms;
};
static_assert(
    sizeof(VSUniformData) == 1856,
    "The size of the VSUniformData structure has changed, update the structure in the shader");
static_assert(sizeof(VSUniformData) < 0x4000,
              "VSUniformData structure must be less than 16kb as per the OpenGL spec");

/// A class that manage different shader stages and configures them with given config data.
class ShaderProgramManager {
public:
    ShaderProgramManager(bool separable, AdrenoUnusedQuaternion::Policy adreno_policy = {});
    ~ShaderProgramManager();

    bool UseProgrammableVertexShader(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup,
                                     unsigned ios_indexed_inputs = false, u16 ios_input_mask = 0xffff,
                                     bool omit_triangle_quaternion = false);

    void UseTrivialVertexShader();

    void UseFixedGeometryShader(const Pica::Regs& regs);

    void UseTrivialGeometryShader();

    // False leaves the selected fragment stage untouched; caller must restore baseline resources.
    bool UseFragmentShader(const Pica::Regs& regs, bool native_nearest_borrow_tex0 = false);
    bool CanOmitAdrenoQuaternion(const Pica::Regs& regs);

#ifdef CARA_IOS_GLES
    using RuntimeTevWords = std::array<GLivec4, 6>;
    bool TryRuntimeTev(const Pica::Regs& regs, RuntimeTevWords& words);
    u16 GetIOSVertexInputReadMask(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup);
    GLuint GetIOSVertexFeedbackProgram(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup,
                                       u16 ios_input_mask = 0xffff);
#endif
    static bool VertexProvenanceEnabled() { return VertexProvenance::Enabled(); }
    void ObserveBoundVertexProgram(GLuint expected_program, unsigned input_mode);

    void ApplyTo(OpenGLState& state, bool allow_software_vertices = false);
    GLuint GetSeparableVertexProgram();
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
    void PrepareHintedProgram(const Pica::Shader::ShaderSetup& setup, unsigned entry);
    // False means the caller must use the regular vertex program for this draw.
    bool TryApplyAsyncIndexed(OpenGLState& state);
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace OpenGL
