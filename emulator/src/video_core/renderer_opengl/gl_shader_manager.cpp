// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "core/cache_file.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/on_screen_display.h"

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

    void Create(const std::string& shader_code, GLenum type, u64 hash) {
        this->hash = hash;
        if (separable) {
            OGLShader shader;
            shader.Create(shader_code.c_str(), type);
            program.Create(true, {shader.handle});
            SetShaderUniformBlockBindings(program.handle);
            if (type == GL_FRAGMENT_SHADER) {
                SetShaderSamplerBindings(program.handle);
            }
        } else {
            this->shader.Create(shader_code.c_str(), type);
        }
    }

    GLuint GetHandle() const {
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
    OGLShader shader;
    OGLProgram program;
    bool separable;
    u64 hash = 0;
};

class ShaderProgramManager::Impl {
public:
    explicit Impl(bool separable)
        : separable(separable), trivial_vertex_shader(separable),
          trivial_geometry_shader(separable) {
        if (separable) {
            pipeline.Create();
        } else if (Settings::values.use_shader_cache) {
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
        trivial_vertex_shader.Create(GenerateTrivialVertexShader(separable), GL_VERTEX_SHADER, 0);
    }

    ~Impl() {
        if (!separable && Settings::values.use_shader_cache) {
            SaveProgramCache();
        }
    }

    OGLShaderStage* GetShaderStageRef(const std::string& shader_code, GLenum shader_type) {
        const u64 code_hash = Common::ComputeHash64(shader_code.data(), shader_code.size());
        auto [iter, new_shader] = shaders.emplace(code_hash, separable);
        OGLShaderStage& cached_shader = iter->second;
        if (new_shader) {
            cached_shader.Create(shader_code, shader_type, code_hash);
            if (cached_shader.GetHandle() == 0) {
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
                                     unsigned ios_indexed_inputs) {
        bool result = false;
        const PicaVSConfig key(regs, setup);
        u64 key_hash = Common::ComputeHash64(&key, sizeof(key));
#ifdef CARA_IOS_GLES
        const PicaFixedGSConfig ios_output(regs);
        const u16 ios_input_mask = IOSVertexInputLayout::MaskFromRegs(regs.vs);
        key_hash ^= Common::ComputeHash64(&ios_input_mask, sizeof(ios_input_mask));
        key_hash ^= Common::ComputeHash64(&ios_indexed_inputs, sizeof(ios_indexed_inputs));
        // Output semantics are part of a fused ES vertex program, not a separate GS.
        key_hash ^= Common::ComputeHash64(&ios_output, sizeof(ios_output));
        if (failed_ios_vertex_shaders.count(key_hash)) return false;
#endif
        const auto iter_ref = shaders_ref.find(key_hash);
        if (iter_ref == shaders_ref.end()) {
            std::string vs_code = GenerateVertexShader(setup, key, separable
#ifdef CARA_IOS_GLES
                , &ios_output, ios_input_mask, ios_indexed_inputs
#endif
            );
            if (vs_code.empty()) {
                LOG_WARNING(Render_OpenGL, "generate programmable vertex shader failed!");
                current_shaders.vs = nullptr;
            } else {
#ifdef CARA_IOS_GLES
                // Diagnostic injection exercises the actual retry/cache path on
                // device; it never affects normal launches.
                const char* force_retry = std::getenv("CARA_GPU_TEST_SHADER_RETRY");
                if (force_retry && std::strcmp(force_retry, "1") == 0) {
                    const auto baseline = GenerateVertexShader(setup, key, separable,
                        &ios_output, ios_input_mask, ios_indexed_inputs, false);
                    if (baseline != vs_code)
                        vs_code += "\n#error Cara optional shader retry test\n";
                }
#endif
                current_shaders.vs = GetShaderStageRef(vs_code, GL_VERTEX_SHADER);
#ifdef CARA_IOS_GLES
                if (!current_shaders.vs) {
                    auto baseline = GenerateVertexShader(setup, key, separable,
                        &ios_output, ios_input_mask, ios_indexed_inputs, false);
                    if (!baseline.empty() && baseline != vs_code) {
                        LOG_WARNING(Render_OpenGL,
                            "Optional vertex optimization failed; retrying baseline GPU shader");
                        current_shaders.vs = GetShaderStageRef(baseline, GL_VERTEX_SHADER);
                        if (current_shaders.vs) vs_code = std::move(baseline);
                    }
                }
#endif
                if (current_shaders.vs) {
                    shaders_ref[key_hash] = current_shaders.vs;
                    vertex_cache.emplace(current_shaders.vs->GetHash(), std::move(vs_code));
                    result = true;
                }
            }
        } else {
            current_shaders.vs = iter_ref->second;
            result = true;
        }
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

    void UseFragmentShader(const Pica::Regs& regs) {
        const auto key = PicaFSConfig::BuildFromRegs(regs);
        const u64 key_hash = Common::ComputeHash64(&key, sizeof(key));
        auto iter_ref = shaders_ref.find(key_hash);
        if (iter_ref == shaders_ref.end()) {
            std::string fs_code = GenerateFragmentShader(key, separable);
            current_shaders.fs = GetShaderStageRef(fs_code, GL_FRAGMENT_SHADER);
            if (current_shaders.fs) {
                shaders_ref[key_hash] = current_shaders.fs;
                fragment_cache.emplace(current_shaders.fs->GetHash(), std::move(fs_code));
            }
        } else {
            current_shaders.fs = iter_ref->second;
        }
    }

    void UseTrivialVertexShader() {
        current_shaders.vs = &trivial_vertex_shader;
    }

    void UseTrivialGeometryShader() {
        current_shaders.gs = &trivial_geometry_shader;
    }

    void ApplyTo(OpenGLState& state) {
        GLuint vs = current_shaders.vs->GetHandle();
        GLuint gs = current_shaders.gs->GetHandle();
        GLuint fs = current_shaders.fs->GetHandle();

        if (separable) {
            glUseProgramStages(pipeline.handle, GL_VERTEX_SHADER_BIT, vs);
            glUseProgramStages(pipeline.handle, GL_GEOMETRY_SHADER_BIT, gs);
            glUseProgramStages(pipeline.handle, GL_FRAGMENT_SHADER_BIT, fs);
            state.draw.shader_program = 0;
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
                else { failed_ios_programs.insert(hash); }
#endif
            }
            state.draw.shader_program = cached_program.handle;
            state.draw.program_pipeline = 0;
        }
    }

#ifdef CARA_IOS_GLES
    GLuint GetIOSVertexFeedbackProgram(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup) {
        const PicaVSConfig vertex_config(regs,setup);
        const PicaFixedGSConfig output_config(regs);
        const u16 input_mask=IOSVertexInputLayout::MaskFromRegs(regs.vs);
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

    static constexpr u32 PROGRAM_CACHE_VERSION = 0x9;

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
    std::unordered_map<u64, OGLShaderStage*> shaders_ref;
#ifdef CARA_IOS_GLES
    std::unordered_map<u64,OGLProgram> feedback_programs;
    OGLShader feedback_fragment;
    std::unordered_set<u64> failed_ios_vertex_shaders;
    std::unordered_set<u64> failed_ios_programs;
#endif
    std::unordered_map<u64, OGLShaderStage> shaders;

    OGLPipeline pipeline;
    std::unordered_map<u64, OGLProgram> program_cache;
};

ShaderProgramManager::ShaderProgramManager(bool separable)
    : impl(std::make_unique<Impl>(separable)) {}

ShaderProgramManager::~ShaderProgramManager() = default;

bool ShaderProgramManager::UseProgrammableVertexShader(const Pica::Regs& regs,
                                                       Pica::Shader::ShaderSetup& setup, unsigned ios_indexed_inputs) {
    return impl->UseProgrammableVertexShader(regs, setup, ios_indexed_inputs);
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

void ShaderProgramManager::UseFragmentShader(const Pica::Regs& regs) {
    impl->UseFragmentShader(regs);
}

#ifdef CARA_IOS_GLES
GLuint ShaderProgramManager::GetIOSVertexFeedbackProgram(const Pica::Regs& regs,
                                                        Pica::Shader::ShaderSetup& setup) {
    return impl->GetIOSVertexFeedbackProgram(regs,setup);
}
#endif

void ShaderProgramManager::ApplyTo(OpenGLState& state) {
    impl->ApplyTo(state);
}

} // namespace OpenGL
