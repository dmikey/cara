#include "gl_shader_suffix_sharing.h"
#include "gl_native_quaternion_geometry.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include <stdexcept>
// Modified for Cara on 2026-11-13; see NOTICE for attribution.
// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_input_usage.h"
#include "video_core/renderer_opengl/gl_shader_decompiler.h"
#include <algorithm>
#include <atomic>
#include <sstream>
#include <cstdlib>
#include <chrono>
#include <fstream>
#ifdef CARA_IOS_GLES
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#ifdef CARA_ANDROID_GLES_ATLAS
#include "video_core/renderer_opengl/gl_android_shader_worker.h"
#ifdef CARA_GL_CONTEXT_DISPATCH
#include "video_core/renderer_opengl/gl_command_queue.h"
#endif
#endif
#endif
#include "video_core/renderer_opengl/ios_pipeline_cache.h"
#endif
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "core/cache_file.h"
#include "core/core.h"
#include "core/settings.h"
#include "video_core/pica_shader_hints.h"
#include "video_core/renderer_opengl/gl_fragment_lighting_ablation.h"
#include "video_core/renderer_opengl/gl_shader_census.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_ios_compat.h"
#include "video_core/renderer_opengl/on_screen_display.h"
#include "video_core/video_core.h"

namespace OpenGL {

static void SetShaderUniformBlockBinding(GLuint shader, const char* name, UniformBindings binding,
                                         std::size_t expected_size) {
    const GLuint ub_index = glGetUniformBlockIndex(shader, name);
    if (ub_index == GL_INVALID_INDEX) {
        return;
    }
    GLint ub_size = 0;
    glGetActiveUniformBlockiv(shader, ub_index, GL_UNIFORM_BLOCK_DATA_SIZE, &ub_size);
    ASSERT(ub_size == expected_size);
    glUniformBlockBinding(shader, ub_index, static_cast<GLuint>(binding));
}

static void SetShaderUniformBlockBindings(GLuint shader) {
    SetShaderUniformBlockBinding(shader, "shader_data", UniformBindings::Common,
                                 sizeof(UniformData));
    SetShaderUniformBlockBinding(shader, "shader_light_data", UniformBindings::Light,
                                 sizeof(UniformLightData));
    SetShaderUniformBlockBinding(shader, "vs_config", UniformBindings::VS, sizeof(VSUniformData));
}

static void SetShaderSamplerBinding(GLuint shader, const char* name,
                                    TextureUnits::TextureUnit binding) {
    GLint uniform_tex = glGetUniformLocation(shader, name);
    if (uniform_tex != -1) {
        glUniform1i(uniform_tex, binding.id);
    }
}

static void SetShaderImageBinding(GLuint shader, const char* name, GLuint binding) {
    GLint uniform_tex = glGetUniformLocation(shader, name);
    if (uniform_tex != -1) {
        glUniform1i(uniform_tex, static_cast<GLint>(binding));
    }
}

static void SetShaderSamplerBindings(GLuint shader) {
    GLuint old_program = OpenGLState::BindShaderProgram(shader);

    // Set the texture samplers to correspond to different texture units
    SetShaderSamplerBinding(shader, "tex0", TextureUnits::PicaTexture(0));
    SetShaderSamplerBinding(shader, "tex1", TextureUnits::PicaTexture(1));
    SetShaderSamplerBinding(shader, "tex2", TextureUnits::PicaTexture(2));
    SetShaderSamplerBinding(shader, "tex_cube", TextureUnits::TextureCube);
#ifdef CARA_IOS_GLES
    SetShaderSamplerBinding(shader, "cara_vertex_inputs", TextureUnits::TextureUnit{7});
    SetShaderSamplerBinding(shader, "cara_vertex_indices", TextureUnits::TextureUnit{8});
    SetShaderSamplerBinding(shader, "cara_vertex_outputs", TextureUnits::TextureUnit{9});
#endif

    // Set the texture samplers to correspond to different lookup table texture units
    SetShaderSamplerBinding(shader, "texture_buffer_lut_lf", TextureUnits::TextureBufferLUT_LF);
    SetShaderSamplerBinding(shader, "texture_buffer_lut_rg", TextureUnits::TextureBufferLUT_RG);
    SetShaderSamplerBinding(shader, "texture_buffer_lut_rgba", TextureUnits::TextureBufferLUT_RGBA);

    SetShaderImageBinding(shader, "shadow_buffer", ImageUnits::ShadowBuffer);
    SetShaderImageBinding(shader, "shadow_texture_px", ImageUnits::ShadowTexturePX);
    SetShaderImageBinding(shader, "shadow_texture_nx", ImageUnits::ShadowTextureNX);
    SetShaderImageBinding(shader, "shadow_texture_py", ImageUnits::ShadowTexturePY);
    SetShaderImageBinding(shader, "shadow_texture_ny", ImageUnits::ShadowTextureNY);
    SetShaderImageBinding(shader, "shadow_texture_pz", ImageUnits::ShadowTexturePZ);
    SetShaderImageBinding(shader, "shadow_texture_nz", ImageUnits::ShadowTextureNZ);

    OpenGLState::BindShaderProgram(old_program);
}

void PicaUniformsData::SetFromRegs(const Pica::ShaderRegs& regs,
                                   const Pica::Shader::ShaderSetup& setup) {
    std::transform(std::begin(setup.uniforms.b), std::end(setup.uniforms.b), std::begin(bools),
                   [](bool value) -> BoolAligned { return {value ? GL_TRUE : GL_FALSE}; });
    std::transform(std::begin(regs.int_uniforms), std::end(regs.int_uniforms), std::begin(i),
                   [](const auto& value) -> GLuvec4 {
                       return {value.x.Value(), value.y.Value(), value.z.Value(), value.w.Value()};
                   });
    std::transform(std::begin(setup.uniforms.f), std::end(setup.uniforms.f), std::begin(f),
                   [](const auto& value) -> GLvec4 {
                       return {value.x.ToFloat32(), value.y.ToFloat32(), value.z.ToFloat32(),
                               value.w.ToFloat32()};
                   });
}

/**
 * An object representing a shader program staging. It can be either a shader object or a program
 * object, depending on whether separable program is used.
 */
class OGLShaderStage {
public:
    explicit OGLShaderStage(bool separable) : separable(separable) {}

    void Create(const std::string& shader_code, GLenum type, u64 hash, bool defer = false) {
#ifdef CARA_IOS_GLES
        if (Settings::values.use_shader_cache) {
            source = shader_code;
            source_digest = IOSPipelineCache::Digest(source.data(), source.size());
        }
#endif
        deferred_type = defer ? type : 0;
        this->hash = hash;
        if (separable) {
            OGLShader shader;
            shader.Create(shader_code.c_str(), type, true);
            if (!shader.handle) return;
            program.Create(true, {shader.handle});
            if (!program.handle) return;
            BindSeparableProgram(type);
        } else if (!defer) {
            this->shader.Create(shader_code.c_str(), type);
        }
    }

#ifdef CARA_IOS_GLES
    bool RestoreVertexStage(const IOSPipelineCache::Entry& entry, u64 source_hash) {
        const std::vector<GLbyte> binary(entry.binary.begin(), entry.binary.end());
        program.Create(entry.format, binary, true);
        if (!program.handle)
            return false;
        source = entry.vertex;
        source_digest = IOSPipelineCache::Digest(source.data(), source.size());
        hash = source_hash;
        BindSeparableProgram(GL_VERTEX_SHADER);
        return true;
    }

    const OGLProgram& SeparableProgram() const { return program; }

#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
    void DeferHintedVertex(std::string_view code, u64 code_hash, bool rom_match) {
        source = code;
        source_digest = IOSPipelineCache::Digest(source.data(), source.size());
        hash = code_hash;
        hinted_vertex_pending = true;
        hinted_vertex_rom_match = rom_match;
    }

    bool HintedVertexPending() const { return hinted_vertex_pending; }
    bool HintedVertexRomMatch() const { return hinted_vertex_rom_match; }
    bool HintedVertexQueued() const { return hinted_vertex_queued; }
    void MarkHintedVertexQueued() { hinted_vertex_queued = true; }
    bool FirstHintedVertexFallback() {
        return !std::exchange(hinted_vertex_fallback_reported, true);
    }

    void ResolveHintedVertex(OGLProgram ready) {
        hinted_vertex_pending = false;
        if (ready.handle) {
            program = std::move(ready);
            BindSeparableProgram(GL_VERTEX_SHADER);
        } else {
            // Compile the same live source on the owner context if the shared
            // context could not compile or publish it.
            const auto code = source;
            Create(code, GL_VERTEX_SHADER, hash);
        }
    }
#endif
#endif

    bool IsValid() const {
        return deferred_type || hinted_vertex_pending || (separable ? program.handle : shader.handle);
    }
    // Diagnostics must never compile a deferred stage merely to inspect it.
    GLuint DiagnosticShaderHandle() const { return separable || deferred_type ? 0 : shader.handle; }
    const std::string& GetSource() const { return source; }
    u64 GetSourceDigest() const { return source_digest; }
    GLuint GetHandle() {
        if (deferred_type) {
            shader.Create(source.c_str(), deferred_type);
            deferred_type = 0;
        }
        if (separable) {
            return program.handle;
        } else {
            return shader.handle;
        }
    }

    u64 GetHash() const {
        return hash;
    }

private:
    void BindSeparableProgram(GLenum type) {
        SetShaderUniformBlockBindings(program.handle);
        if (type == GL_FRAGMENT_SHADER || type == GL_VERTEX_SHADER)
            SetShaderSamplerBindings(program.handle);
    }

    std::string source;
    u64 source_digest = 0;
    GLenum deferred_type = 0;
    bool hinted_vertex_pending = false;
    bool hinted_vertex_rom_match = false;
    bool hinted_vertex_queued = false;
    bool hinted_vertex_fallback_reported = false;
    OGLShader shader;
    OGLProgram program;
    bool separable;
    u64 hash = 0;
};

class ShaderProgramManager::Impl {
#ifdef CARA_IOS_GLES
    struct IOSStagePair { u64 key = 0; OGLProgram* program = nullptr; };
#endif

public:
    void ConfigureNativeShaderDispatch() {
#ifdef CARA_MAC_ATLAS_PIPELINE
        const char* flag=std::getenv("CARA_NATIVE_ISOLATED_SHADER_DISPATCH");
        if(!flag || std::strcmp(flag,"1")!=0) return;
        GLint major=0,minor=0;
        if(!GLES && !separable) {
            glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
        }
        native_dispatch_enabled=!GLES && !separable && (major>4 || (major==4 && minor>=1));
        if(!native_dispatch_enabled) {
            LOG_WARNING(Render_OpenGL,"Native isolated PICA dispatch requested but unsupported; retaining existing GPU route");
            return;
        }
        LOG_INFO(Render_OpenGL,"Native isolated PICA shader dispatch enabled; independent geometry selection, all GPU block bodies retained");
#endif
    }

    void ConfigureNativeQuaternionGeometry() {
#ifdef CARA_MAC_ATLAS_PIPELINE
        const char* flag=std::getenv("CARA_NATIVE_TRIANGLE_QUATERNION_GEOMETRY");
        if(!flag || std::strcmp(flag,"1")!=0) return;
        NativeQuaternionGeometry::Limits c;
        if(!GLES && !separable) {
            glGetIntegerv(GL_MAJOR_VERSION,&c.major);glGetIntegerv(GL_MINOR_VERSION,&c.minor);
            glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES,&c.vertices);
            glGetIntegerv(GL_MAX_GEOMETRY_INPUT_COMPONENTS,&c.input);
            glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_COMPONENTS,&c.output);
            glGetIntegerv(GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS,&c.total);
            glGetIntegerv(GL_MAX_CLIP_DISTANCES,&c.clips);
            glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS,&c.vertex_output);
        }
        native_geometry_enabled=NativeQuaternionGeometry::Supported(!GLES,separable,c);
        if(!native_geometry_enabled) {
            LOG_WARNING(Render_OpenGL,"Native quaternion geometry requested but unsupported; retaining existing GPU route");
            return;
        }
        const auto source=GenerateNativeQuaternionGeometryShader();
        native_geometry_shader.Create(source,GL_GEOMETRY_SHADER,Common::ComputeHash64(source.data(),source.size()));
        if(!native_geometry_shader.IsValid()) throw std::runtime_error("Native quaternion geometry shader compilation failed; no CPU fallback");
        LOG_INFO(Render_OpenGL,"Native quaternion geometry enabled: GL {}.{}, GS vertices {}, input {}, output {}, total {}, clips {}; GPU vertex-once/triangle sign route",c.major,c.minor,c.vertices,c.input,c.output,c.total,c.clips);
#endif
    }

    explicit Impl(bool separable)
        : separable(separable), trivial_vertex_shader(separable),
          trivial_geometry_shader(separable), native_geometry_shader(separable) {
        ConfigureNativeShaderDispatch();
#ifdef CARA_MAC_ATLAS_PIPELINE
        // Immutable per manager, sampled before any cache load. Ordinary launches OFF.
        suffix_sharing_policy = ShaderSuffixSharing::Requested(
            std::getenv("CARA_NATIVE_SHADER_SUFFIX_SHARING"), std::getenv("CARA_NATIVE_DIAGNOSTICS"));
#endif
        ConfigureNativeQuaternionGeometry();
        if (separable) {
            pipeline.Create();
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
            const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            if (Settings::values.use_shader_cache && renderer && std::strstr(renderer, "Mali")) {
                ios_archive_kind = IOSPipelineCache::ArchiveKind::VertexStages;
                LoadIOSPipelines();
            }
#endif
        }
#ifdef CARA_IOS_GLES
        else if (NativeQuaternionGeometry::TwoStageCache(Settings::values.use_shader_cache,native_geometry_enabled)) {
            LoadIOSPipelines();
        }
#else
        else if (NativeQuaternionGeometry::TwoStageCache(Settings::values.use_shader_cache,native_geometry_enabled)) {
            u64 size = LoadProgramCache();
            if (size > 0) {
                std::string log{"Load Shader Cache"};
                size >>= 20;
                if (size > 0) {
                    log = fmt::format("{} ({}MB)", log, size);
                }
                OSD::AddMessage(log, OSD::MessageType::ShaderCache, OSD::Duration::NORMAL,
                                OSD::Color::YELLOW);
            }
        }
#endif
#ifdef CARA_MAC_ATLAS_PIPELINE
        if (VertexProvenance::Enabled()) {
            static std::atomic<u64> next_session{1};
            vertex_provenance=std::make_unique<VertexProvenance::Ledger<>>(next_session.fetch_add(1));
        }
#endif
        const auto trivial = GenerateTrivialVertexShader(separable);
        trivial_vertex_shader.Create(trivial, GL_VERTEX_SHADER, 0, CanDeferIOSStage(trivial));
    }

    ~Impl() {
#ifdef CARA_MAC_ATLAS_PIPELINE
        if (vertex_provenance) {
            for(std::size_t i=0;i<vertex_provenance->size;++i) {
                std::ostringstream row;
                VertexProvenance::WriteRecord(row,"program_final",vertex_provenance->session,
                                               vertex_provenance->records[i]);
                LOG_INFO(Render_OpenGL,"CARA_VERTEX_PROVENANCE {}",row.str());
            }
            u64 unknown=0;
            for(std::size_t i=0;i<vertex_provenance->size;++i)
                if(!vertex_provenance->records[i].verified)unknown+=vertex_provenance->records[i].draw_attempts;
            LOG_INFO(Render_OpenGL,
                "CARA_VERTEX_PROVENANCE {{\"event\":\"summary\",\"session\":{},\"program_records\":{},\"capacity\":256,\"draw_attempts\":{},\"unknown_draw_attempts\":{},\"overflow_draw_attempts\":{},\"truncated\":{}}}",
                vertex_provenance->session,vertex_provenance->size,vertex_provenance->total_draw_attempts,
                unknown,vertex_provenance->overflow_draw_attempts,vertex_provenance->overflow_draw_attempts?"true":"false");
        }
#endif
        if (!separable && NativeQuaternionGeometry::TwoStageCache(Settings::values.use_shader_cache,native_geometry_enabled)) {
#ifdef CARA_IOS_GLES
            SaveIOSPipelines();
#else
            SaveProgramCache();
#endif
        }
#ifdef CARA_IOS_GLES
        if (ios_archive_kind == IOSPipelineCache::ArchiveKind::VertexStages)
            SaveIOSPipelines();
#endif
    }

    OGLShaderStage* GetShaderStageRef(const std::string& shader_code, GLenum shader_type,
                                      bool defer = false,
                                      [[maybe_unused]] std::optional<bool> hint = std::nullopt) {
        const u64 code_hash = Common::ComputeHash64(shader_code.data(), shader_code.size());
        auto [iter, new_shader] = shaders.emplace(code_hash, separable);
        OGLShaderStage& cached_shader = iter->second;
        if (new_shader) {
#ifdef CARA_IOS_GLES
            if (shader_type == GL_VERTEX_SHADER &&
                ios_archive_kind == IOSPipelineCache::ArchiveKind::VertexStages && !ios_formats.empty()) {
                CreateIOSVertexStage(cached_shader, shader_code, code_hash, hint);
            } else
#endif
            {
                cached_shader.Create(shader_code, shader_type, code_hash,
                                     defer || CanDeferIOSStage(shader_code));
            }
            if (!cached_shader.IsValid()) {
                LOG_WARNING(Render_OpenGL, "shader {:04X} create failed!", shader_type);
                shaders.erase(code_hash);
                return nullptr;
            }
            // load cached shader reference
            auto iter = reference_cache.find(code_hash);
            if (iter != reference_cache.end()) {
                for (const auto& hash : iter->second) {
                    shaders_ref[hash] = &cached_shader;
                }
            }
        }
        return &cached_shader;
    }

    bool UseProgrammableVertexShader(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup,
                                     unsigned ios_indexed_inputs, u16 selected_input_mask,
                                     bool omit_triangle_quaternion) {
        bool result = false;
        const bool native_geometry = NativeQuaternionGeometry::Selected(native_geometry_enabled,ios_indexed_inputs) &&
            regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::No;
        native_geometry_selected=native_geometry;
        const bool native_dispatch = NativeQuaternionGeometry::Selected(native_dispatch_enabled,ios_indexed_inputs) &&
            regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::No;
        native_dispatch_selected=native_dispatch;
        const bool native_suffix_sharing = suffix_sharing_policy && native_dispatch && !native_geometry &&
            !GLES && !separable && ios_indexed_inputs < 3;
        const PicaVSConfig key(regs, setup);
        u64 key_hash = Common::ComputeHash64(&key, sizeof(key));
#ifdef CARA_IOS_GLES
        const PicaFixedGSConfig ios_output(regs);
        const u16 ios_input_mask = IOSVertexInputLayout::MaskFromRegs(regs.vs) & selected_input_mask;
        key_hash ^= Common::ComputeHash64(&ios_input_mask, sizeof(ios_input_mask));
        key_hash ^= Common::ComputeHash64(&ios_indexed_inputs, sizeof(ios_indexed_inputs));
        // Output semantics are part of a fused ES vertex program, not a separate GS.
        key_hash ^= Common::ComputeHash64(&ios_output, sizeof(ios_output));
        if (omit_triangle_quaternion) {
            constexpr u32 option = 0x4e515531;
            key_hash ^= Common::ComputeHash64(&option, sizeof(option));
        }
        if(native_geometry) {const u32 option=NativeQuaternionGeometry::SourceOption;key_hash^=Common::ComputeHash64(&option,sizeof(option));}
        if(native_dispatch) {const u32 option=0x4e494431;key_hash^=Common::ComputeHash64(&option,sizeof(option));}
        if(native_suffix_sharing) {const u32 option=ShaderSuffixSharing::SourceOption;key_hash^=Common::ComputeHash64(&option,sizeof(option));}
        if (failed_ios_vertex_shaders.count(key_hash)) {native_geometry_selected=false;native_dispatch_selected=false;return false;}
#endif
        const auto iter_ref = shaders_ref.find(key_hash);
        if (iter_ref == shaders_ref.end()) {
            std::string vs_code = GenerateVertexShader(setup, key, separable
#ifdef CARA_IOS_GLES
                , &ios_output, ios_input_mask, ios_indexed_inputs, true, native_geometry, native_dispatch,
                  native_suffix_sharing, nullptr, omit_triangle_quaternion
#endif
            );
            if (vs_code.empty()) {
#ifdef CARA_IOS_GLES
                if(native_geometry || native_dispatch) {
                    const auto ordinary=GenerateVertexShader(setup,key,separable,&ios_output,ios_input_mask,ios_indexed_inputs,true,false);
                    if(!ordinary.empty()) throw std::runtime_error("Native shader route vertex source generation failed; no CPU fallback");
                    native_geometry_selected=false;
                    native_dispatch_selected=false; // Same preexisting unsupported PICA source.
                }
#endif
                LOG_WARNING(Render_OpenGL, "generate programmable vertex shader failed!");
                current_shaders.vs = nullptr;
            } else {
#ifdef CARA_IOS_GLES
                // Diagnostic injection exercises the actual retry/cache path on
                // device; it never affects normal launches.
                const char* force_retry = std::getenv("CARA_GPU_TEST_SHADER_RETRY");
                if (force_retry && std::strcmp(force_retry, "1") == 0) {
                    const auto baseline = GenerateVertexShader(setup, key, separable,
                        &ios_output, ios_input_mask, ios_indexed_inputs, false, native_geometry, native_dispatch, native_suffix_sharing);
                    if (baseline != vs_code)
                        vs_code += "\n#error Cara optional shader retry test\n";
                }
#endif
                bool defer = false;
                std::optional<bool> hint;
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
                defer = AsyncProgrammableEnabled() ||
                        ((ios_indexed_inputs == 3 || ios_indexed_inputs == 6) && AsyncIndexedEnabled());
                if (separable && ios_archive_kind == IOSPipelineCache::ArchiveKind::VertexStages &&
                    LoadShaderHints()) {
                    hint = shader_hints.Match(setup, u32(regs.vs.main_offset));
                }
                if (shader_census) {
                    const auto fingerprint =
                        Pica::ShaderHints::Catalog::Fingerprint(setup, u32(regs.vs.main_offset));
                    ShaderCensus::Record(
                        "vertex_hint refresh={} pc={} fingerprint={:016x} source={:016x} catalog={} rom_match={}",
                        VideoCore::GetCurrentFrame() + 1, u32(regs.vs.main_offset),
                        fingerprint.value_or(0), Common::ComputeHash64(vs_code.data(), vs_code.size()),
                        int(hint.has_value()), int(hint.value_or(false)));
                }
#endif
                current_shaders.vs = GetShaderStageRef(vs_code, GL_VERTEX_SHADER, defer, hint);
#ifdef CARA_IOS_GLES
                if (!current_shaders.vs) {
                    auto baseline = GenerateVertexShader(setup, key, separable,
                        &ios_output, ios_input_mask, ios_indexed_inputs, false, native_geometry, native_dispatch, native_suffix_sharing);
                    if (!baseline.empty() && baseline != vs_code) {
                        LOG_WARNING(Render_OpenGL,
                            "Optional vertex optimization failed; retrying baseline GPU shader");
                        current_shaders.vs = GetShaderStageRef(baseline, GL_VERTEX_SHADER);
                        if (current_shaders.vs) vs_code = std::move(baseline);
                    }
                }
#endif
                if((native_geometry || native_dispatch) && !current_shaders.vs) throw std::runtime_error("Native shader route vertex compilation failed; no CPU fallback");
                if (current_shaders.vs) {
                    shaders_ref[key_hash] = current_shaders.vs;
                    vertex_cache.emplace(current_shaders.vs->GetHash(), std::move(vs_code));
                    result = true;
                }
            }
        } else {
            current_shaders.vs = iter_ref->second;
            result = current_shaders.vs->IsValid();
        }
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
        if (result && current_shaders.vs->HintedVertexPending() &&
            !ResolveHintedVertex(*current_shaders.vs, !regs.framebuffer.IsShadowRendering() &&
                                                        !Settings::values.skip_slow_draw)) {
            if (current_shaders.vs->HintedVertexPending())
                return false; // Execute the same draw with Cara's existing CPU vertex path.
            result = false;
        }
#endif
#ifdef CARA_IOS_GLES
        if (!result) failed_ios_vertex_shaders.insert(key_hash);
#endif
        return result;
    }

    void UseFixedGeometryShader(const Pica::Regs& regs) {
        const PicaFixedGSConfig key(regs);
        const u64 key_hash = Common::ComputeHash64(&key, sizeof(key));
        auto [iter, new_shader] = shaders.emplace(key_hash, separable);
        OGLShaderStage& cached_shader = iter->second;
        if (new_shader) {
            std::string gs_code = GenerateFixedGeometryShader(key, separable);
            cached_shader.Create(gs_code, GL_GEOMETRY_SHADER, key_hash);
        }
        current_shaders.gs = &cached_shader;
    }

#ifdef CARA_IOS_GLES
    std::unordered_map<u64, OGLShaderStage*> runtime_tev_stages;

    bool TryRuntimeTev(const Pica::Regs& regs, RuntimeTevWords& words) {
        if (!GLES || !regs.texturing.main_config.texture0_enable)
            return false;
        auto config = PicaFSConfig::BuildFromRegs(regs);
        auto& f = config.state;
        if (f.lighting.enable || f.proctex.enable || f.shadow_rendering ||
            f.texture0_type != Pica::TexturingRegs::TextureConfig::Texture2D ||
            f.fog_mode != Pica::TexturingRegs::FogMode::None ||
            f.alpha_test_func == Pica::FramebufferRegs::CompareFunc::Never ||
            !config.SupportsRuntimeTev())
            return false;

        unsigned units = 1;
        for (unsigned i = 0; i < words.size(); ++i) {
            words[i] = config.RuntimeTevWords(i);
            for (unsigned shift : {0u, 4u, 8u, 16u, 20u, 24u}) {
                const auto source = (unsigned(words[i][0]) >> shift) & 15u;
                using Source = Pica::TexturingRegs::TevStageConfig::Source;
                if (source == unsigned(Source::Texture2) || source == unsigned(Source::Texture3))
                    return false;
                if (source == unsigned(Source::Texture1)) {
                    if (!regs.texturing.main_config.texture1_enable)
                        return false;
                    units = 3;
                }
            }
        }
        f.tev_stages = {};
        f.combiner_buffer_input = 0;
        const u64 hash = Common::ComputeHash64(&config, sizeof(config)) ^ units;
        auto& stage = runtime_tev_stages[hash];
        if (!stage) {
            auto source = GenerateFragmentShader(config, separable,
                {.gles = true, .allow_shadow = AllowShadow, .metal_runtime_tev_units = units});
            stage = GetShaderStageRef(source, GL_FRAGMENT_SHADER);
            if (!stage)
                return false;
            LOG_INFO(Render_OpenGL, "GLES runtime TEV selected units={} stage={}", units, stage->GetHash());
        }
        current_shaders.fs = stage;
        return true;
    }
#endif

    void UseFragmentShader(const Pica::Regs& regs, bool native_nearest_borrow_tex0) {
        auto key = PicaFSConfig::BuildFromRegs(regs, native_nearest_borrow_tex0);
        // Diagnostic feature ablation only: preserve guest registers and all other FS state.
        // Existing unlit generation leaves fragment colors zero; alpha coverage may change.
        if (FragmentLightingAblationEnabled()) {
            key.state.lighting.enable = false;
        }
        const u64 key_hash = Common::ComputeHash64(&key, sizeof(key));
        auto iter_ref = shaders_ref.find(key_hash);
        if (iter_ref == shaders_ref.end()) {
            const auto start = shader_census ? ShaderCensus::Clock::now() : ShaderCensus::Clock::time_point{};
            std::string fs_code = GenerateFragmentShader(key, separable);
            if (shader_census) {
                const auto generation_us = ShaderCensus::ElapsedUs(start);
                const u64 source_hash = Common::ComputeHash64(fs_code.data(), fs_code.size());
                ShaderCensus::Record("fragment_key key={:016x} source={:016x} lit={} generation_us={}",
                                     key_hash, source_hash, int(key.state.lighting.enable), generation_us);
            }
            current_shaders.fs = GetShaderStageRef(fs_code, GL_FRAGMENT_SHADER);
            if (current_shaders.fs) {
                shaders_ref[key_hash] = current_shaders.fs;
                fragment_cache.emplace(current_shaders.fs->GetHash(), std::move(fs_code));
            }
        } else {
            current_shaders.fs = iter_ref->second;
        }
    }

    void ObserveBoundVertexProgram(GLuint expected_program, unsigned input_mode) {
#ifdef CARA_MAC_ATLAS_PIPELINE
        if (!vertex_provenance) return;
        const auto* stage=current_shaders.vs;
        const GLuint managed=stage?stage->DiagnosticShaderHandle():0;
        bool first=false;
        auto* record=vertex_provenance->Observe(expected_program,managed,stage?stage->GetHash():0,
            input_mode,[&](VertexProvenance::Record& row) {
                if(separable || !managed || !expected_program){row.reason="unsupported_or_missing_program";return;}
                GLint current=0;glGetIntegerv(GL_CURRENT_PROGRAM,&current);row.current_program=GLuint(current);
                if(row.current_program!=expected_program){row.reason="current_program_mismatch";return;}
                GLint linked=0;glGetProgramiv(expected_program,GL_LINK_STATUS,&linked);row.linked=linked==GL_TRUE;
                if(!row.linked){row.reason="program_not_linked";return;}
                GLint count=0;glGetProgramiv(expected_program,GL_ATTACHED_SHADERS,&count);
                if(count<1 || count>8){row.reason="attached_shader_count_unavailable";return;}
                std::array<GLuint,8> attached{};GLsizei actual_count=0;
                glGetAttachedShaders(expected_program,attached.size(),&actual_count,attached.data());
                if(actual_count!=count){row.reason="attached_shader_count_mismatch";return;}
                unsigned vertex_count=0;
                for(GLsizei i=0;i<actual_count;++i){GLint type=0;glGetShaderiv(attached[i],GL_SHADER_TYPE,&type);
                    if(type==GL_VERTEX_SHADER){row.actual_vs=attached[i];++vertex_count;}}
                if(vertex_count!=1 || row.actual_vs!=managed){row.reason="managed_vertex_shader_mismatch";return;}
                GLint compiled=0;glGetShaderiv(row.actual_vs,GL_COMPILE_STATUS,&compiled);row.compiled=compiled==GL_TRUE;
                if(!row.compiled){row.reason="vertex_not_compiled";return;}
                GLint length=0;glGetShaderiv(row.actual_vs,GL_SHADER_SOURCE_LENGTH,&length);
                if(length<2 || length>1024*1024){row.reason="source_length_unavailable";return;}
                try {
                    std::vector<char> source(static_cast<std::size_t>(length));GLsizei written=0;
                    glGetShaderSource(row.actual_vs,length,&written,source.data());
                    if(written<=0 || written>=length){row.reason="source_length_mismatch";return;}
                    row.source_bytes=written;row.actual_source_hash=Common::ComputeHash64(source.data(),written);
                    row.decoder=VertexProvenance::Classify(std::string_view(source.data(),written));
                    if(row.decoder==VertexProvenance::Decoder::Ambiguous){row.reason="ambiguous_decoder_markers";return;}
                    row.verified=true;row.reason="ok";
                } catch(const std::exception&) {row.reason="source_capture_allocation_failed";}
            },first);
        if(first && record){std::ostringstream row;
            VertexProvenance::WriteRecord(row,"program_first_use",vertex_provenance->session,*record);
            LOG_INFO(Render_OpenGL,"CARA_VERTEX_PROVENANCE {}",row.str());}
        if(!record && !vertex_provenance_overflow_reported){vertex_provenance_overflow_reported=true;
            LOG_INFO(Render_OpenGL,"CARA_VERTEX_PROVENANCE {{\"event\":\"overflow\",\"session\":{},\"capacity\":256,\"unknown_remaining\":true}}",vertex_provenance->session);}
#endif
    }

    void UseTrivialVertexShader() {
        native_geometry_selected=false;
        native_dispatch_selected=false;
        current_shaders.vs = &trivial_vertex_shader;
    }

    void UseTrivialGeometryShader() {
        current_shaders.gs = native_geometry_selected?&native_geometry_shader:&trivial_geometry_shader;
    }

    GLuint GetSeparableVertexProgram() {
        return separable && current_shaders.vs ? current_shaders.vs->GetHandle() : 0;
    }

    void ApplyTo(OpenGLState& state, [[maybe_unused]] bool allow_software_vertices) {
        if (!current_shaders.vs || !current_shaders.fs) {
            state.draw.shader_program = 0;
            state.draw.program_pipeline = 0;
            return;
        }
        // Also covers a programmable-mode change during feedback fallback.
        if(native_geometry_enabled) current_shaders.gs=native_geometry_selected?&native_geometry_shader:&trivial_geometry_shader;
#ifdef CARA_IOS_GLES
        if (!separable && NativeQuaternionGeometry::TwoStageCache(Settings::values.use_shader_cache,native_geometry_enabled)) {
            ApplyIOSPipeline(state, allow_software_vertices);
            return;
        }
#endif
        GLuint vs = current_shaders.vs->GetHandle();
        GLuint gs = current_shaders.gs->GetHandle();
        GLuint fs = current_shaders.fs->GetHandle();
        if (separable) {
#ifdef CARA_IOS_GLES
            MaybeCheckpointIOSPipelines();
#endif
            state.draw.shader_program = 0;
            state.draw.program_pipeline = 0;
            if (!vs || !fs || !pipeline.handle)
                return;
            constexpr std::array<GLbitfield, 3> stage_bits{
                GL_VERTEX_SHADER_BIT, GL_GEOMETRY_SHADER_BIT, GL_FRAGMENT_SHADER_BIT};
            const std::array<GLuint, 3> stages{vs, gs, fs};
            for (std::size_t i = 0; i < stages.size(); ++i) {
                if (attached_stages[i] != stages[i]) {
                    glUseProgramStages(pipeline.handle, stage_bits[i], stages[i]);
                    attached_stages[i] = stages[i];
                }
            }
            state.draw.program_pipeline = pipeline.handle;
        } else {
            const std::array<u64, 3> bundle{
                current_shaders.vs->GetHash(),
                current_shaders.gs->GetHash(),
                current_shaders.fs->GetHash(),
            };
            u64 hash = Common::ComputeHash64(bundle.data(), bundle.size() * sizeof(u64));
#ifdef CARA_IOS_GLES
            if (failed_ios_programs.count(hash)) {
                if(native_geometry_selected || native_dispatch_selected) throw std::runtime_error("Native shader route program previously failed; no CPU fallback");
                state.draw.shader_program = 0;
                state.draw.program_pipeline = 0;
                return;
            }
#endif
            auto& cached_program = program_cache[hash];
            if (cached_program.handle == 0) {
                CreateProgram(cached_program, hash, vs, gs, fs);
                if (cached_program.handle) {
                    SetShaderUniformBlockBindings(cached_program.handle);
                    SetShaderSamplerBindings(cached_program.handle);
                }
#ifdef CARA_IOS_GLES
                else { if(native_geometry_selected || native_dispatch_selected) throw std::runtime_error("Native shader route program link failed; no CPU fallback"); failed_ios_programs.insert(hash); }
#endif
            }
            state.draw.shader_program = cached_program.handle;
            state.draw.program_pipeline = 0;
        }
    }

#ifdef CARA_IOS_GLES
    u16 GetIOSVertexInputReadMask(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup) {
        // This constructs the SAME cached program/swizzle identities already needed
        // by UseProgrammableVertexShader. No additional buffer hash is introduced;
        // the subsequent shader selection reads the now-clean existing hashes.
        const PicaVSConfig config(regs, setup);
        const auto& state = config.state;
        const IOSVertexInputReadKey key{state.program_hash, state.swizzle_hash,
            state.main_offset, state.num_outputs, state.sanitize_mul, state.output_map};
        return ios_input_usage.Get(key, [&]() -> std::optional<u16> {
            const auto input = [](u32 reg) { return "vs_in_reg" + std::to_string(reg); };
            const auto output = [&](u32 reg) -> std::string {
                if (reg < state.output_map.size() && state.output_map[reg] < state.num_outputs)
                    return "vs_out_attr" + std::to_string(state.output_map[reg]);
                return {};
            };
            const auto result = ShaderDecompiler::DecompileProgram(setup.program_code,
                setup.swizzle_data, state.main_offset, input, output, state.sanitize_mul);
            return result ? std::optional<u16>(result->input_mask) : std::nullopt;
        });
    }

    GLuint GetIOSVertexFeedbackProgram(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup,
                                       u16 selected_input_mask) {
        const PicaVSConfig vertex_config(regs,setup);
        const PicaFixedGSConfig output_config(regs);
        const u16 input_mask=IOSVertexInputLayout::MaskFromRegs(regs.vs) & selected_input_mask;
        const std::array<u64,3> parts={Common::ComputeHash64(&vertex_config,sizeof(vertex_config)),
            Common::ComputeHash64(&output_config,sizeof(output_config)),input_mask};
        const u64 key=Common::ComputeHash64(parts.data(),sizeof(parts));
        auto [entry,inserted]=feedback_programs.try_emplace(key);
        if (!inserted) return entry->second.handle;
        const std::string source=GenerateVertexShader(setup,vertex_config,false,&output_config,input_mask,4);
        if (source.empty()) return 0;
        OGLShader vertex; vertex.Create(source.c_str(),GL_VERTEX_SHADER);
        if (!vertex.handle) return 0;
        if (!feedback_fragment.handle)
            feedback_fragment.Create("out vec4 color; void main() { color=vec4(1.0); }",GL_FRAGMENT_SHADER);
        if (!feedback_fragment.handle) return 0;
        GLuint program=glCreateProgram();
        glAttachShader(program,vertex.handle); glAttachShader(program,feedback_fragment.handle);
        const char* varyings[]={"cara_feedback0","cara_feedback1","cara_feedback2",
                                "cara_feedback3","cara_feedback4","cara_feedback5"};
        glTransformFeedbackVaryings(program,6,varyings,GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(program);
        GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
        if (!linked) { glDeleteProgram(program); return 0; }
        entry->second.handle=program;
        SetShaderUniformBlockBindings(program);
        SetShaderSamplerBindings(program);
        return program;
    }
#endif

    bool CanDeferIOSStage(const std::string& source) const {
#ifdef CARA_IOS_GLES
        if (!separable && NativeQuaternionGeometry::TwoStageCache(
                              Settings::values.use_shader_cache, native_geometry_enabled)) {
            for (std::size_t i = 0; i < ios_entries.size(); ++i) {
                const auto& e = ios_entries[i];
                const auto p = program_cache.find(ios_entry_keys[i]);
                if (p != program_cache.end() && p->second.handle &&
                    (source == e.vertex || source == e.fragment)) return true;
            }
        }
#endif
        return false;
    }
#ifdef CARA_IOS_GLES
    std::string IOSIdentity() {
        // Explicit key/UBO schema identity, independent of a possibly empty prototype SCM string.
        std::string identity = std::string(CARA_SHADER_CACHE_SIGNATURE) +
                               "\nnative-nearest-borrow-tex0-key1-common-ubo2";
        // Reject a different generator route before compiling any saved source.
        identity += "\ntwo-stage-store-v2";
#ifdef CARA_MAC_ATLAS_PIPELINE
        identity += "\nbackend=native-mac-atlas-gl41";
#elif defined(CARA_ANDROID_GLES_ATLAS)
        identity += "\nbackend=android-gles-atlas";
        const char* fetch_reuse = std::getenv("CARA_GPU_PACKED_FETCH_REUSE");
        identity += fetch_reuse && std::strcmp(fetch_reuse, "1") == 0
                        ? "\npacked-fetch-reuse=1" : "\npacked-fetch-reuse=0";
#else
        identity += "\nbackend=ios-gles";
#endif
        const char* prune = std::getenv("CARA_GLES_PRUNE_UNUSED_QUATERNION");
        if (prune && std::strcmp(prune, "1") == 0)
            identity += "\nprune-unused-quaternion=1";
#ifdef CARA_ANDROID_GLES_ATLAS
        const char* slice = std::getenv("CARA_GPU_QUATERNION_SLICE");
        if (slice && std::strcmp(slice, "1") == 0)
            identity += "\nquaternion-slice=1";
#endif
        identity += native_geometry_enabled ? "\ngeometry=1" : "\ngeometry=0";
        identity += native_dispatch_enabled ? "\nisolated-dispatch=1" : "\nisolated-dispatch=0";
        identity += suffix_sharing_policy ? "\nsuffix-sharing-policy-v1=1" : "\nsuffix-sharing-policy-v1=0";
        for (GLenum key : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION}) {
            const auto value = glGetString(key);
            identity += "\n";
            if (value) identity += reinterpret_cast<const char*>(value);
        }
#if defined(__APPLE__)
        char os[256]{};
        size_t length = sizeof(os);
        if (sysctlbyname("kern.osversion", os, &length, nullptr, 0) == 0) identity += os;
#elif defined(__ANDROID__)
        char os[PROP_VALUE_MAX]{};
        if (__system_property_get("ro.build.fingerprint", os) > 0) identity += os;
#endif
        GLint count = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
        if (count > 0 && count <= 64) {
            ios_formats.resize(count);
            glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, ios_formats.data());
        }
        for (auto format : ios_formats) identity += "\n" + std::to_string(format);
        return identity;
    }
    void BindIOSProgram(OGLProgram& p) {
        if (p.handle) {
            SetShaderUniformBlockBindings(p.handle);
            SetShaderSamplerBindings(p.handle);
        }
    }
    void LoadIOSPipelines() {
        ios_identity = IOSIdentity();
        const bool vertex_archive = ios_archive_kind == IOSPipelineCache::ArchiveKind::VertexStages;
        std::string suffix = UseNativeLUTBuffers() ? ".native-lut" : "";
        if (vertex_archive) {
            ios_identity += "\nprogram-role=separable-es320-vertex-v1";
            suffix += ".separable-vertex";
        }
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
        const char* slice = std::getenv("CARA_GPU_QUATERNION_SLICE");
        const char* prune = std::getenv("CARA_GLES_PRUNE_UNUSED_QUATERNION");
        if (slice && std::strcmp(slice, "1") == 0)
            suffix += ".quaternion-slice";
        if (prune && std::strcmp(prune, "1") == 0)
            suffix += ".prune-unused-quaternion";
        const char* expanded = std::getenv("CARA_MALI_EXTENDED_PROGRAM_CACHE");
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (expanded && std::strcmp(expanded, "1") == 0 && renderer &&
            std::strstr(renderer, "Mali")) {
            ios_cache_limits = IOSPipelineCache::ExtendedMaliLimits;
            suffix += ".mali-extended";
        }
#endif
        ios_cache_path = GetCacheFile() + suffix + ".cara-pso1";
        LOG_INFO(Render_OpenGL, "Cara iOS local pipeline cache enabled; {} binary formats",
                 ios_formats.size());
        std::ifstream file(ios_cache_path, std::ios::binary | std::ios::ate);
        if (!file) return;
        const auto size = file.tellg();
        if (size < 0 || size > static_cast<std::streamoff>(ios_cache_limits.bytes)) return;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
            !IOSPipelineCache::Decode(bytes, ios_identity, ios_entries, ios_cache_limits,
                                      ios_archive_kind)) return;
        if (vertex_archive) {
            for (const auto& entry : ios_entries)
                ios_bytes += IOSArchiveEntryBytes(entry);
            if (shader_census)
                ShaderCensus::Record("vertex_stage_archive entries={} bytes={}", ios_entries.size(),
                                     bytes.size());
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        unsigned binary_hits = 0, source_links = 0;
        // A stage shared by several saved programs needs compiling only once.
        // Keys compare the complete source; separate maps retain stage type.
        // Lifetime ends after all saved programs have linked on this GL thread.
        std::unordered_map<std::string, OGLShader> prewarm_vertices, prewarm_fragments;
        const auto stage = [](auto& stages, const std::string& source, GLenum type) {
            auto [it, inserted] = stages.try_emplace(source);
            if (inserted) it->second.Create(source.c_str(), type);
            return it->second.handle;
        };
        for (auto& e : ios_entries) {
            ios_bytes += IOSArchiveEntryBytes(e);
            const auto key = IOSPipelineCache::Key(e.vertex, e.fragment);
            ios_entry_keys.push_back(key);
            auto& p = program_cache[key];
            if (!e.binary.empty() && std::find(ios_formats.begin(), ios_formats.end(),
                                             static_cast<GLint>(e.format)) != ios_formats.end()) {
                std::vector<GLbyte> binary(e.binary.begin(), e.binary.end());
                p.Create(e.format, binary);
                if (p.handle) ++binary_hits;
            }
            if (!p.handle) {
                // Same GL/context thread as ordinary compilation. No background GL context.
                const auto vs = stage(prewarm_vertices, e.vertex, GL_VERTEX_SHADER);
                const auto fs = stage(prewarm_fragments, e.fragment, GL_FRAGMENT_SHADER);
                if (vs && fs) p.Create(false, {vs, fs});
                if (p.handle) ++source_links;
                e.binary.clear(); e.format = 0;
            }
            BindIOSProgram(p);
        }
        if (shader_census)
            ShaderCensus::Record("cache_restore entries={} binary_hits={} source_links={} bytes={}",
                                 ios_entries.size(), binary_hits, source_links, bytes.size());
        LOG_INFO(Render_OpenGL, "Cara iOS prewarm: {} binary hits, {} source links, {} us; {} formats",
                 binary_hits, source_links,
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - start).count(), ios_formats.size());
    }
    void SaveIOSPipelines() {
        const auto now = std::chrono::steady_clock::now();
        if (!ios_dirty || !ios_checkpoint_retry.CanAttempt(now)) return;
        const auto fail = [&](const char* operation) {
            ios_checkpoint_retry.Failed(now);
            LOG_WARNING(Render_OpenGL, "Cara iOS cache checkpoint failed ({}); retry delayed 30s, maximum 3 failures/session",
                        operation);
        };
        // iOS previously disabled disk caches, so this directory may not exist.
        if (!FileUtil::CreateFullPath(FileUtil::GetUserPath(FileUtil::UserPath::CacheDir))) {
            fail("create directory");
            return;
        }
        const auto temporary = ios_cache_path + ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) { fail("open"); return; }
        // Serialize at most once per permitted attempt, never on a denied retry.
        const auto bytes = IOSPipelineCache::Encode(ios_identity, ios_entries, ios_cache_limits,
                                                   ios_archive_kind);
        if (bytes.empty()) { file.close(); fail("encode"); return; }
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        if (!file) { fail("write/close"); return; }
        if (!FileUtil::Rename(temporary, ios_cache_path)) { fail("rename"); return; }
        ios_dirty = false;
        ios_checkpoint_retry.Succeeded();
        if (shader_census && ios_archive_kind == IOSPipelineCache::ArchiveKind::VertexStages)
            ShaderCensus::Record("vertex_stage_checkpoint entries={} bytes={}", ios_entries.size(),
                                 bytes.size());
        LOG_INFO(Render_OpenGL, "Cara iOS local pipeline checkpoint: {} pipelines, {} bytes",
                 ios_entries.size(), bytes.size());
    }
    void MaybeCheckpointIOSPipelines() {
        if (ios_dirty && std::chrono::steady_clock::now() - ios_last_miss > std::chrono::seconds(2))
            SaveIOSPipelines();
    }
    static std::size_t IOSArchiveEntryBytes(const IOSPipelineCache::Entry& entry) {
        return entry.vertex.size() + entry.fragment.size() + entry.binary.size() + 16;
    }
    void CreateIOSVertexStage(OGLShaderStage& stage, const std::string& shader_code, u64 code_hash,
                              [[maybe_unused]] std::optional<bool> hint) {
        const auto entry = std::find_if(ios_entries.begin(), ios_entries.end(), [&](const auto& e) {
            return e.vertex == shader_code;
        });
        if (entry != ios_entries.end()) {
            const auto format = static_cast<GLint>(entry->format);
            const bool restored = std::find(ios_formats.begin(), ios_formats.end(), format) !=
                                      ios_formats.end() &&
                                  stage.RestoreVertexStage(*entry, code_hash);
            if (shader_census)
                ShaderCensus::Record("vertex_stage_binary source={:016x} restored={}", code_hash,
                                     int(restored));
            if (restored)
                return;
            ios_bytes -= IOSArchiveEntryBytes(*entry);
            ios_entries.erase(entry);
            ios_dirty = true;
            ios_last_miss = std::chrono::steady_clock::now();
        }

#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
        if (hint) {
            stage.DeferHintedVertex(shader_code, code_hash, *hint);
            return;
        }
#endif
        stage.Create(shader_code, GL_VERTEX_SHADER, code_hash);
        if (stage.IsValid())
            RememberIOSPipeline(code_hash, stage.SeparableProgram(), shader_code, {});
    }

    void RememberIOSPipeline(u64 key, const OGLProgram& program, const std::string& vs_source,
                             const std::string& fs_source) {
        if (ios_entries.size() >= ios_cache_limits.entries ||
            vs_source.size() > IOSPipelineCache::MaxSource ||
            fs_source.size() > IOSPipelineCache::MaxSource)
            return;

        IOSPipelineCache::Entry entry{vs_source, fs_source};
        if (!ios_formats.empty()) {
            GLenum format = 0;
            std::vector<GLbyte> binary;
            program.GetProgramBinary(format, binary, IOSPipelineCache::MaxBinary);
            if (binary.size() <= IOSPipelineCache::MaxBinary &&
                std::find(ios_formats.begin(), ios_formats.end(), static_cast<GLint>(format)) !=
                    ios_formats.end()) {
                entry.format = format;
                entry.binary.assign(binary.begin(), binary.end());
            }
        }
        if (ios_archive_kind == IOSPipelineCache::ArchiveKind::VertexStages && entry.binary.empty())
            return;
        const auto cost = IOSArchiveEntryBytes(entry);
        if (cost > ios_cache_limits.bytes || ios_bytes > ios_cache_limits.bytes - cost)
            return;
        ios_bytes += cost;
        ios_entries.push_back(std::move(entry));
        if (ios_archive_kind == IOSPipelineCache::ArchiveKind::LinkedPrograms)
            ios_entry_keys.push_back(key);
        ios_dirty = true;
        ios_last_miss = std::chrono::steady_clock::now();
    }

#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
    bool AsyncIndexedEnabled() const {
        static const bool enabled = [] {
            const char* value = std::getenv("CARA_ANDROID_ASYNC_INDEXED_SHADERS");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled && !separable && Settings::values.use_shader_cache &&
            NativeQuaternionGeometry::TwoStageCache(true, native_geometry_enabled);
    }

    bool AsyncProgrammableEnabled() const {
        return Settings::values.gles_async_programmable_shaders &&
               !Settings::values.skip_slow_draw && !separable &&
               NativeQuaternionGeometry::TwoStageCache(Settings::values.use_shader_cache,
                                                       native_geometry_enabled);
    }

    bool HasReadySoftwareVertexProgram() const {
        const auto key = IOSPipelineCache::KeyFromDigests(trivial_vertex_shader.GetSourceDigest(),
                                                          current_shaders.fs->GetSourceDigest());
        const auto program = program_cache.find(key);
        return program != program_cache.end() && program->second.handle != 0;
    }

    AndroidShaderWorker& GetShaderWorker(std::unique_ptr<AndroidShaderWorker>& worker) {
        if (!worker) {
            const auto create_worker = [] { return std::make_unique<AndroidShaderWorker>(); };
#ifdef CARA_GL_CONTEXT_DISPATCH
            // Create the share group member on the thread that owns the core context.
            if (auto* queue = CommandQueue::Queue()) {
                CommandQueue::Flush();
                worker = queue->Call(create_worker);
            } else
#endif
            {
                worker = create_worker();
            }
        }
        return *worker;
    }

    bool LoadShaderHints() {
        if (!shader_hints.Loaded())
            shader_hints.Load(Core::System::GetInstance().GetTitleID());
        return shader_hints.Enabled();
    }

    bool ResolveHintedVertex(OGLShaderStage& stage, bool allow_software) {
        const u32 frame = VideoCore::GetCurrentFrame();
        const auto fallback = [&](const char* reason) {
            if (shader_census && stage.FirstHintedVertexFallback())
                ShaderCensus::Record(
                    "vertex_hint_fallback refresh={} source={:016x} rom_match={} reason={}",
                    frame + 1, stage.GetHash(), int(stage.HintedVertexRomMatch()), reason);
            return false;
        };
        if (!stage.HintedVertexQueued() && allow_software &&
            !shader_hints.Admit(frame, stage.HintedVertexRomMatch()))
            return fallback("refresh_budget");

        auto& worker = GetShaderWorker(hint_vertex_worker);
        const auto start = ShaderCensus::Clock::now();
        auto build = worker.Request(stage.GetHash(), stage.GetSource(), {}, false);
        using Status = AndroidShaderWorker::Status;
        if (build.status == Status::Pending && !stage.HintedVertexQueued()) {
            stage.MarkHintedVertexQueued();
            if (shader_census)
                ShaderCensus::Record("vertex_hint_queue refresh={} source={:016x} rom_match={} cpu_fallback={}",
                                     frame + 1, stage.GetHash(), int(stage.HintedVertexRomMatch()),
                                     int(allow_software));
        }
        if (allow_software && (build.status == Status::Pending || build.status == Status::Busy))
            return fallback(build.status == Status::Pending ? "pending" : "queue_full");

        if (build.status == Status::Pending)
            build.program = worker.WaitForVertexStage(stage.GetHash());
        const bool worker_ready = build.program.handle != 0;
        stage.ResolveHintedVertex(std::move(build.program));
        if (shader_census)
            ShaderCensus::Record("vertex_hint_resolve refresh={} source={:016x} rom_match={} worker={} us={}",
                                 frame + 1, stage.GetHash(), int(stage.HintedVertexRomMatch()),
                                 int(worker_ready), ShaderCensus::ElapsedUs(start));
        if (stage.IsValid())
            RememberIOSPipeline(stage.GetHash(), stage.SeparableProgram(), stage.GetSource(), {});
        return stage.IsValid();
    }

    AndroidShaderWorker::Status TryBuildSharedProgram(const IOSStagePair& pair, bool reuse_stages) {
        const auto& vs = current_shaders.vs->GetSource();
        const auto& fs = current_shaders.fs->GetSource();
        auto result = GetShaderWorker(android_shader_worker).Request(pair.key, vs, fs, reuse_stages);
        if (result.status == AndroidShaderWorker::Status::Ready) {
            *pair.program = std::move(result.program);
            BindIOSProgram(*pair.program);
            RememberIOSPipeline(pair.key, *pair.program, vs, fs);
        }
        return result.status;
    }

    void PrepareHintedProgram(const Pica::Shader::ShaderSetup& setup, unsigned entry) {
        if (!LoadShaderHints() || !AsyncProgrammableEnabled() || native_dispatch_selected ||
            !current_shaders.vs || !current_shaders.fs || !HasReadySoftwareVertexProgram())
            return;
        const auto& pair = ios_stage_pairs.Get(current_shaders.vs, current_shaders.fs, [&] {
            const auto key = IOSPipelineCache::KeyFromDigests(current_shaders.vs->GetSourceDigest(),
                                                            current_shaders.fs->GetSourceDigest());
            return IOSStagePair{key, &program_cache[key]};
        });
        if (pair.program->handle || failed_ios_programs.count(pair.key))
            return;
        const bool rom_match = shader_hints.Match(setup, entry);
        if (shader_hints.Admit(VideoCore::GetCurrentFrame(), rom_match))
            TryBuildSharedProgram(pair, true);
    }

    bool TryApplyAsyncIndexed(OpenGLState& state) {
        if (!AsyncIndexedEnabled()) {
            return true;
        }
        const auto& pair = ios_stage_pairs.Get(current_shaders.vs, current_shaders.fs, [&] {
            const auto key = IOSPipelineCache::KeyFromDigests(current_shaders.vs->GetSourceDigest(),
                                                            current_shaders.fs->GetSourceDigest());
            return IOSStagePair{key, &program_cache[key]};
        });
        auto& cached_program = *pair.program;
        if (!cached_program.handle &&
            TryBuildSharedProgram(pair, false) != AndroidShaderWorker::Status::Ready)
            return false;
        state.draw.shader_program = cached_program.handle;
        state.draw.program_pipeline = 0;
        return true;
    }
#endif

    void ApplyIOSPipeline(OpenGLState& state, [[maybe_unused]] bool allow_software_vertices) {
        const auto& vs_source = current_shaders.vs->GetSource();
        const auto& fs_source = current_shaders.fs->GetSource();
        const auto& pair = ios_stage_pairs.Get(current_shaders.vs, current_shaders.fs, [&] {
            const auto key = IOSPipelineCache::KeyFromDigests(current_shaders.vs->GetSourceDigest(),
                                                            current_shaders.fs->GetSourceDigest());
            return IOSStagePair{key, &program_cache[key]};
        });
        const auto key = pair.key;
        auto& p = *pair.program;
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
        if (!p.handle && !failed_ios_programs.count(key) && allow_software_vertices &&
            !native_dispatch_selected && AsyncProgrammableEnabled() &&
            HasReadySoftwareVertexProgram() &&
            TryBuildSharedProgram(pair, true) == AndroidShaderWorker::Status::Pending) {
            // The caller retries the same PICA draw with CPU-transformed vertices.
            // The fallback is already linked, so the foreground context can draw
            // without compiling a second program alongside the worker.
            state.draw.shader_program = 0;
            state.draw.program_pipeline = 0;
            return;
        }
#endif
        if (!p.handle && !failed_ios_programs.count(key)) {
            const auto vs = current_shaders.vs->GetHandle(), fs = current_shaders.fs->GetHandle();
            if (vs && fs) p.Create(false, {vs, fs});
            BindIOSProgram(p);
            if (!p.handle) failed_ios_programs.insert(key);
            else
                RememberIOSPipeline(key, p, vs_source, fs_source);
        }
        if (!p.handle && (native_geometry_selected || native_dispatch_selected))
            throw std::runtime_error("Native shader route cached program failed; no CPU fallback");
        // Checkpoint only after a quiet interval, keeping disk writes out of compile bursts.
        MaybeCheckpointIOSPipelines();
        state.draw.shader_program = p.handle;
        state.draw.program_pipeline = 0;
    }
#endif

    void CreateProgram(OGLProgram& program, u64 hash, GLuint vs, GLuint gs, GLuint fs) {
        auto iter = binary_cache.find(hash);
        // load opengl program binary cache
        if (iter != binary_cache.end()) {
            program.Create(iter->second.format, iter->second.binary);
            if (program.handle == 0) {
                // cache data corrupted
                binary_cache.clear();
            }
        }
        if (program.handle == 0) {
            GLenum format;
            std::vector<GLbyte> binary;
            program.Create(false, {vs, gs, fs});
            program.GetProgramBinary(format, binary);
            if (!binary.empty()) {
                binary_cache.emplace(hash, ProgramCacheEntity{format, std::move(binary)});
            } else {
                LOG_DEBUG(Render_OpenGL, "failed to get program binary!");
            }
        }
    }

#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    // Desktop Apple GL now lowers switch dispatch like the GLES generator. Never
    // reload older switch-based source from the persistent program cache.
    static constexpr u32 PROGRAM_CACHE_VERSION = 0xC;
#else
    static constexpr u32 PROGRAM_CACHE_VERSION = 0xB;
#endif

    static std::string GetCacheFile() {
        u64 program_id = 0;
        Core::System::GetInstance().GetAppLoader().ReadProgramId(program_id);
        const std::string& dir = FileUtil::GetUserPath(FileUtil::UserPath::CacheDir);
        return fmt::format("{}{:016X}.cache", dir, program_id);
    }

    void SaveProgramCache() {
        Core::CacheFile file(GetCacheFile(), Core::CacheFile::MODE_SAVE);

        u32 verion = PROGRAM_CACHE_VERSION;
        file.DoHeader(verion);

        s32 count = static_cast<s32>(binary_cache.size());
        file.Do(count);

        u64 hash;
        GLenum format;
        std::vector<GLbyte> binary;
        for (auto& pair : binary_cache) {
            hash = pair.first;
            format = pair.second.format;
            file.Do(hash);
            file.Do(format);
            file.Do(pair.second.binary);
        }

        if (!file.IsGood()) {
            return;
        }

        file.DoMarker("ShadersRef");
        SaveShadersRef(file);
        if (!file.IsGood()) {
            return;
        }

        file.DoMarker("VertexCache");
        file.Do(vertex_cache);

        file.DoMarker("FragmentCache");
        file.Do(fragment_cache);
    }

    u64 LoadProgramCache() {
        Core::CacheFile file(GetCacheFile(), Core::CacheFile::MODE_LOAD);

        u32 verion = 0;
        file.DoHeader(verion);
        if (verion != PROGRAM_CACHE_VERSION) {
            FileUtil::Delete(GetCacheFile());
            return 0;
        }

        s32 count = 0;
        file.Do(count);

        u64 hash;
        GLenum format;
        std::vector<GLbyte> binary;
        for (s32 i = 0; i < count; ++i) {
            file.Do(hash);
            file.Do(format);
            file.Do(binary);
            binary_cache.emplace(hash, ProgramCacheEntity{format, std::move(binary)});
        }

        if (!file.IsGood()) {
            binary_cache.clear();
            return 0;
        }

        file.DoMarker("ShadersRef");
        LoadShadersRef(file);
        if (!file.IsGood()) {
            reference_cache.clear();
            return 0;
        }

        file.DoMarker("VertexCache");
        file.Do(vertex_cache);
        if (!file.IsGood()) {
            vertex_cache.clear();
            return 0;
        }

        file.DoMarker("FragmentCache");
        file.Do(fragment_cache);
        if (!file.IsGood()) {
            fragment_cache.clear();
            return 0;
        }

        // load vertex shader cache
        std::vector<u64> error_caches;
        for (const auto& entity : vertex_cache) {
            if (!GetShaderStageRef(entity.second, GL_VERTEX_SHADER)) {
                error_caches.push_back(entity.first);
            }
        }
        for (auto key : error_caches) {
            vertex_cache.erase(key);
        }

        // load fragment shader cache
        error_caches.clear();
        for (const auto& entity : fragment_cache) {
            if (!GetShaderStageRef(entity.second, GL_FRAGMENT_SHADER)) {
                error_caches.push_back(entity.first);
            }
        }
        for (auto key : error_caches) {
            fragment_cache.erase(key);
        }

        return file.GetSize();
    }

    void SaveShadersRef(Core::CacheFile& file) {
        for (const auto& ref : shaders_ref) {
            u64 key_hash = ref.first;
            u64 code_hash = ref.second->GetHash();
            auto iter = reference_cache.find(code_hash);
            if (iter != reference_cache.end()) {
                iter->second.insert(key_hash);
            } else {
                reference_cache[code_hash] = {key_hash};
            }
        }

        u32 count = reference_cache.size();
        file.Do(count);

        for (auto& ref : reference_cache) {
            file.Do(ref.first);
            file.Do(ref.second);
        }
    }

    void LoadShadersRef(Core::CacheFile& file) {
        s32 count = 0;
        file.Do(count);

        for (s32 i = 0; i < count; ++i) {
            u64 code_hash = 0;
            std::unordered_set<u64> hash_set;
            file.Do(code_hash);
            file.Do(hash_set);
            reference_cache[code_hash] = std::move(hash_set);
        }
    }

private:
    bool separable;
    bool native_geometry_enabled=false;
    bool native_dispatch_enabled=false;
    bool suffix_sharing_policy=false;
    bool native_geometry_selected=false;
    bool native_dispatch_selected=false;

    struct {
        OGLShaderStage* vs;
        OGLShaderStage* gs;
        OGLShaderStage* fs;
    } current_shaders{};

    struct ProgramCacheEntity {
        explicit ProgramCacheEntity(GLenum format, std::vector<GLbyte>&& binary)
            : format(format), binary(binary) {}
        GLenum format;
        std::vector<GLbyte> binary;
    };
    std::unordered_map<u64, ProgramCacheEntity> binary_cache;
    std::unordered_map<u64, std::unordered_set<u64>> reference_cache;
    std::unordered_map<u64, std::string> vertex_cache;
    std::unordered_map<u64, std::string> fragment_cache;

    OGLShaderStage trivial_vertex_shader;
    OGLShaderStage trivial_geometry_shader;
    OGLShaderStage native_geometry_shader;
    std::unordered_map<u64, OGLShaderStage*> shaders_ref;
    const bool shader_census = ShaderCensus::Enabled();
#ifdef CARA_IOS_GLES
    IOSPipelineCache::StagePairMemo<IOSStagePair> ios_stage_pairs;
    std::vector<IOSPipelineCache::Entry> ios_entries;
    std::vector<u64> ios_entry_keys;
    std::vector<GLint> ios_formats;
    std::string ios_identity;
    std::string ios_cache_path;
    IOSPipelineCache::Limits ios_cache_limits = IOSPipelineCache::DefaultLimits;
    IOSPipelineCache::ArchiveKind ios_archive_kind = IOSPipelineCache::ArchiveKind::LinkedPrograms;
    IOSPipelineCache::CheckpointRetry ios_checkpoint_retry;
    std::size_t ios_bytes = 8192; // Header/identity/checksum reserve included in the disk budget.
    bool ios_dirty = false;
    std::chrono::steady_clock::time_point ios_last_miss{};
    IOSVertexInputUsage ios_input_usage;
    std::unordered_map<u64,OGLProgram> feedback_programs;
    OGLShader feedback_fragment;
    std::unordered_set<u64> failed_ios_vertex_shaders;
    std::unordered_set<u64> failed_ios_programs;
#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
    std::unique_ptr<AndroidShaderWorker> android_shader_worker;
    std::unique_ptr<AndroidShaderWorker> hint_vertex_worker;
    Pica::ShaderHints::Catalog shader_hints;
#endif
#endif
#ifdef CARA_MAC_ATLAS_PIPELINE
    std::unique_ptr<VertexProvenance::Ledger<>> vertex_provenance;
    bool vertex_provenance_overflow_reported=false;
#endif
    std::unordered_map<u64, OGLShaderStage> shaders;

    OGLPipeline pipeline;
    std::array<GLuint, 3> attached_stages{};
    std::unordered_map<u64, OGLProgram> program_cache;
};

ShaderProgramManager::ShaderProgramManager(bool separable)
    : impl(std::make_unique<Impl>(separable)) {}

ShaderProgramManager::~ShaderProgramManager() = default;

bool ShaderProgramManager::UseProgrammableVertexShader(const Pica::Regs& regs,
                                                       Pica::Shader::ShaderSetup& setup,
                                                       unsigned ios_indexed_inputs, u16 ios_input_mask,
                                                       bool omit_triangle_quaternion) {
    return impl->UseProgrammableVertexShader(regs, setup, ios_indexed_inputs, ios_input_mask,
                                             omit_triangle_quaternion);
}

void ShaderProgramManager::UseTrivialVertexShader() {
    impl->UseTrivialVertexShader();
}

void ShaderProgramManager::UseFixedGeometryShader(const Pica::Regs& regs) {
    impl->UseFixedGeometryShader(regs);
}

void ShaderProgramManager::UseTrivialGeometryShader() {
    impl->UseTrivialGeometryShader();
}

bool ShaderProgramManager::UseFragmentShader(const Pica::Regs& regs,
                                            bool native_nearest_borrow_tex0) {
    if (native_nearest_borrow_tex0 && !PicaFSConfig::SupportsNativeNearestBorrow(regs)) {
        return false;
    }
    impl->UseFragmentShader(regs, native_nearest_borrow_tex0);
    return true;
}

#ifdef CARA_IOS_GLES
bool ShaderProgramManager::TryRuntimeTev(const Pica::Regs& regs, RuntimeTevWords& words) {
    return impl->TryRuntimeTev(regs, words);
}
u16 ShaderProgramManager::GetIOSVertexInputReadMask(const Pica::Regs& regs,
                                                   Pica::Shader::ShaderSetup& setup) {
    return impl->GetIOSVertexInputReadMask(regs, setup);
}
GLuint ShaderProgramManager::GetIOSVertexFeedbackProgram(const Pica::Regs& regs,
                                                        Pica::Shader::ShaderSetup& setup,
                                                        u16 ios_input_mask) {
    return impl->GetIOSVertexFeedbackProgram(regs,setup,ios_input_mask);
}
#endif

void ShaderProgramManager::ObserveBoundVertexProgram(GLuint expected_program, unsigned input_mode) {
    impl->ObserveBoundVertexProgram(expected_program,input_mode);
}

#if defined(__ANDROID__) && defined(CARA_ANDROID_GLES_ATLAS)
void ShaderProgramManager::PrepareHintedProgram(const Pica::Shader::ShaderSetup& setup,
                                                unsigned entry) {
    impl->PrepareHintedProgram(setup, entry);
}

bool ShaderProgramManager::TryApplyAsyncIndexed(OpenGLState& state) {
    return impl->TryApplyAsyncIndexed(state);
}
#endif

void ShaderProgramManager::ApplyTo(OpenGLState& state, bool allow_software_vertices) {
    impl->ApplyTo(state, allow_software_vertices);
}

GLuint ShaderProgramManager::GetSeparableVertexProgram() {
    return impl->GetSeparableVertexProgram();
}

} // namespace OpenGL
