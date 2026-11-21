// SPDX-License-Identifier: GPL-2.0-or-later

#include "pica_metal_runtime_shader.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_msl.hpp>

namespace Cara {
namespace {

using namespace spirv_cross;

constexpr std::string_view Prefix = R"(#version 310 es
#define CARA_GLES
precision highp int;
precision highp float;
precision highp sampler2D;
vec4 CaraLUTFetch(sampler2D lut, int index) {
    return texelFetch(lut, ivec2(index & 1023, index >> 10), 0);
}
)";

// The macOS atlas generator emits both GL clip outputs unconditionally.
constexpr std::string_view MacAtlasClip =
    "gl_ClipDistance[0] = -gl_Position.z;\n"
    "gl_ClipDistance[1] = dot(clip_coef, gl_Position);\n";

constexpr std::string_view RawLookup = R"(
int cara_lookup_raw_index(int encoded_vertex) {
    uint header = CARA_INDEX_HEADER;
    uint topology = header & 3u;
    uint width = header >> 2u;
    uint slot = uint(encoded_vertex);
    if (topology == 1u || topology == 2u) {
        uint triangle = slot / 3u;
        uint corner = slot % 3u;
        if (topology == 1u)
            slot = triangle + (corner == 2u ? 2u : corner ^ (triangle & 1u));
        else
            slot = corner == 0u ? 0u : triangle + corner;
    }
    if (width == 0u)
        return int(slot);
    uint word = 2u + (width == 1u ? slot / 2u : slot);
    uint index = texelFetch(cara_vertex_indices, ivec2(word & 255u, word >> 8u), 0).r;
    if (width == 1u)
        index = (index >> ((slot & 1u) * 8u)) & 255u;
    return int(index) - int(CARA_INDEX_FIRST);
}
)";

void Require(bool condition, std::string_view detail) {
    if (!condition)
        throw std::runtime_error(std::string(detail));
}

void Replace(std::string& text, std::string_view old, std::string_view replacement) {
    for (std::size_t offset = 0; (offset = text.find(old, offset)) != std::string::npos;
         offset += replacement.size()) {
        text.replace(offset, old.size(), replacement);
    }
}

void ReplaceOne(std::string& text, std::string_view old, std::string_view replacement) {
    const auto offset = text.find(old);
    Require(offset != std::string::npos && text.find(old, offset + old.size()) == std::string::npos,
            "unknown raw index generator form");
    text.replace(offset, old.size(), replacement);
}

std::string PrepareSource(std::string_view original, bool desktop_clip,
                          bool raw_indices, bool raw_index_constants) {
    const bool user_clip = original.starts_with(RuntimeMetalUserClip);
    Require(!original.empty() && original.size() <= 512 * 1024 &&
                original.find("#extension") == std::string::npos &&
                original.find("#include") == std::string::npos &&
                original.find("#version") == std::string::npos,
            "unrecognized PICA GLSL source");
    std::string source = std::string(Prefix) + std::string(original);
    if (desktop_clip)
        ReplaceOne(source, "#version 310 es", "#version 450");
    if (user_clip && original.find(MacAtlasClip) == std::string_view::npos) {
        // The iOS generator leaves these outputs to the platform path.
        // Evaluate both planes before depth remapping, in original PICA space.
        constexpr std::string_view remap = "gl_Position.z = 2.0 * gl_Position.z + gl_Position.w;\n";
        ReplaceOne(source, remap, std::string(MacAtlasClip) + std::string(remap));
    }
    if (!user_clip && original.find(MacAtlasClip) != std::string_view::npos) {
        // A disabled user plane must always pass on Metal.
        ReplaceOne(source, MacAtlasClip,
                   "gl_ClipDistance[0] = -gl_Position.z;\n"
                   "gl_ClipDistance[1] = 1.0;\n");
    }
    Replace(source, "samplerBuffer", "sampler2D");
    Replace(source, "precision highp uimage2D;", "");
    Replace(source, "#define fma_safe(x, y, z) fma(x, y, z)",
            "#define fma_safe(x, y, z) ((x) * (y) + (z))");
    Replace(source, "texelFetch(texture_buffer_lut_", "CaraLUTFetch(texture_buffer_lut_");
    if (raw_indices) {
        std::string lookup(RawLookup);
        ReplaceOne(lookup, "CARA_INDEX_HEADER",
                   raw_index_constants ? "(uint(cara_vertex_layout[0].w) >> 3u) & 15u"
                                       : "texelFetch(cara_vertex_indices, ivec2(0, 0), 0).r");
        ReplaceOne(lookup, "CARA_INDEX_FIRST",
                   raw_index_constants ? "uint(cara_vertex_layout[0].w) >> 7u"
                                       : "texelFetch(cara_vertex_indices, ivec2(1, 0), 0).r");
        if (raw_index_constants) {
            ReplaceOne(source, "ivec4 info = cara_vertex_layout[reg];",
                       "ivec4 info = cara_vertex_layout[reg];\n    info.w &= 7;");
        }
        ReplaceOne(source, "void cara_load_vertex(int vertex_id) {",
                   lookup + "\nvoid cara_load_vertex(int vertex_id) {");
        ReplaceOne(source, "int(texelFetch(cara_vertex_indices, ivec2(vertex_id % 128, vertex_id / 128), 0).r)",
                   "cara_lookup_raw_index(vertex_id)");
    }
    return source;
}

void Parse(glslang::TShader& shader, EShLanguage language, const std::string& source) {
    const char* code = source.c_str();
    shader.setStrings(&code, 1);
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setGlobalUniformBlockName("CaraPackedLayout");
    shader.setGlobalUniformSet(0);
    shader.setGlobalUniformBinding(20);
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    constexpr auto flags = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 310, false, flags))
        throw std::runtime_error(shader.getInfoLog());
}

std::vector<std::uint32_t> EmitSpirv(glslang::TProgram& program, EShLanguage language) {
    const auto* stage = program.getIntermediate(language);
    Require(stage != nullptr, "linked PICA stage missing");
    glslang::SpvOptions options{};
    options.disableOptimizer = true;
    std::vector<std::uint32_t> code;
    glslang::GlslangToSpv(*stage, code, &options);
    Require(!code.empty(), "empty PICA SPIR-V stage");
    return code;
}

struct Stage {
    CompilerMSL compiler;
    ShaderResources resources;
    spv::ExecutionModel model;

    Stage(std::vector<std::uint32_t> code, spv::ExecutionModel model_)
        : compiler(std::move(code)), model(model_) {
        auto active = compiler.get_active_interface_variables();
        resources = compiler.get_shader_resources(active);
        compiler.set_enabled_interface_variables(std::move(active));
        Require(resources.storage_buffers.empty() && resources.storage_images.empty() &&
                    resources.separate_images.empty() && resources.separate_samplers.empty() &&
                    resources.subpass_inputs.empty() && resources.push_constant_buffers.empty(),
                "unsupported Metal stage resource");
    }

    void Bind(const Resource& resource, unsigned slot) {
        MSLResourceBinding binding{};
        binding.stage = model;
        binding.desc_set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
        binding.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
        binding.msl_buffer = binding.msl_texture = binding.msl_sampler = slot;
        compiler.add_msl_resource_binding(binding);
    }

    std::string Compile() {
        auto options = compiler.get_msl_options();
#if defined(CARA_RUNTIME_MSL_IOS)
        options.platform = CompilerMSL::Options::iOS;
#else
        options.platform = CompilerMSL::Options::macOS;
#endif
        options.set_msl_version(2, 3);
        // No fragment generator reads the clip distances as interpolants.
        // The vertex output retains Metal's actual [[clip_distance]].
        options.enable_clip_distance_user_varying = false;
        compiler.set_msl_options(options);
        if (model == spv::ExecutionModelVertex) {
            auto coordinates = compiler.get_common_options();
            coordinates.vertex.fixup_clipspace = true;
            coordinates.vertex.flip_vert_y = true;
            compiler.set_common_options(coordinates);
        }
        compiler.rename_entry_point("main", model == spv::ExecutionModelVertex ?
                                             "cara_pica_vert" : "cara_pica_frag", model);
        return compiler.compile();
    }
};

using Interface = std::tuple<std::string, unsigned, SPIRType::BaseType, unsigned>;
std::set<Interface> InterfaceElements(const Stage& stage, const SmallVector<Resource>& resources) {
    std::set<Interface> result;
    for (const auto& resource : resources) {
        const auto& type = stage.compiler.get_type(resource.type_id);
        Require(type.columns == 1 && type.array.empty() && type.width == 32 &&
                    stage.compiler.has_decoration(resource.id, spv::DecorationLocation),
                "unsupported Metal stage interface");
        const auto location = stage.compiler.get_decoration(resource.id, spv::DecorationLocation);
        Require(result.emplace(resource.name, location, type.basetype, type.vecsize).second,
                "duplicate Metal stage interface");
    }
    return result;
}

void VerifyInterface(const Stage& vs, const Stage& fs, RuntimeMetalVertexMode mode) {
    const auto output = InterfaceElements(vs, vs.resources.stage_outputs);
    const auto input = InterfaceElements(fs, fs.resources.stage_inputs);
    Require(std::includes(output.begin(), output.end(), input.begin(), input.end()),
            "PICA vertex and fragment varyings differ");
    const auto vertex_inputs = InterfaceElements(vs, vs.resources.stage_inputs);
    if (mode == RuntimeMetalVertexMode::StageIn) {
        const std::set<Interface> fields{
            {"vert_position", 0, SPIRType::Float, 4}, {"vert_color", 1, SPIRType::Float, 4},
            {"vert_texcoord0", 2, SPIRType::Float, 2}, {"vert_texcoord1", 3, SPIRType::Float, 2},
            {"vert_texcoord2", 4, SPIRType::Float, 2}, {"vert_texcoord0_w", 5, SPIRType::Float, 1},
            {"vert_normquat", 6, SPIRType::Float, 4}, {"vert_view", 7, SPIRType::Float, 3},
        };
        Require(std::includes(fields.begin(), fields.end(), vertex_inputs.begin(), vertex_inputs.end()),
                "PICA stage-in stream mismatch");
    } else {
        Require(vertex_inputs.empty(), "unexpected PICA vertex attribute stream");
    }
}

void ValidateTexture(const Stage& stage, const Resource& resource, SPIRType::BaseType component) {
    const auto& type = stage.compiler.get_type(resource.type_id);
    Require(type.basetype == SPIRType::SampledImage && type.array.empty() &&
                type.image.dim == spv::Dim2D && !type.image.arrayed && !type.image.ms &&
                !type.image.depth && stage.compiler.get_type(type.image.type).basetype == component,
            "unsupported PICA texture binding");
}

void BindUniform(Stage& stage, const Resource& block, unsigned slot, std::size_t length) {
    const auto& type = stage.compiler.get_type(block.base_type_id);
    Require(stage.compiler.get_declared_struct_size(type) == length, "PICA uniform size mismatch");
    stage.Bind(block, slot);
}

void BindVertex(Stage& stage, RuntimeMetalVertexMode mode) {
    const bool packed = mode == RuntimeMetalVertexMode::Packed;
    const bool hardware = mode != RuntimeMetalVertexMode::StageIn;
    std::set<std::string> blocks;
    for (const auto& block : stage.resources.uniform_buffers) {
        Require(blocks.insert(block.name).second, "duplicate PICA vertex block");
        if (packed && block.name == "CaraPackedLayout") {
            BindUniform(stage, block, 0, 256);
        } else if (hardware && block.name == "vs_config") {
            BindUniform(stage, block, packed ? 1 : 0, 1856);
        } else if (block.name == "shader_data") {
            BindUniform(stage, block, packed ? 2 : hardware ? 1 : 0, 288);
        } else {
            throw std::runtime_error("unknown PICA vertex uniform block");
        }
    }
    std::set<std::string> textures;
    for (const auto& resource : stage.resources.sampled_images) {
        Require(hardware && textures.insert(resource.name).second, "unexpected PICA vertex texture");
        if (resource.name == "cara_vertex_inputs") {
            ValidateTexture(stage, resource, packed ? SPIRType::UInt : SPIRType::Float);
            stage.Bind(resource, packed ? 0 : 1);
        } else if (resource.name == "cara_vertex_indices") {
            ValidateTexture(stage, resource, SPIRType::UInt);
            stage.Bind(resource, packed ? 1 : 0);
        } else {
            throw std::runtime_error("unknown PICA vertex texture");
        }
    }
}

void BindFragment(Stage& stage, RuntimeMetalShaderPair& pair) {
    pair.slots.fill(RuntimeMetalUnusedSlot);
    for (const auto& block : stage.resources.uniform_buffers) {
        if (block.name == "shader_data" && pair.slots[4] == RuntimeMetalUnusedSlot) {
            BindUniform(stage, block, 0, 288);
            pair.slots[4] = 0;
        } else if (block.name == "shader_light_data" && pair.slots[5] == RuntimeMetalUnusedSlot) {
            BindUniform(stage, block, 1, 1040);
            pair.slots[5] = 1;
        } else {
            throw std::runtime_error("unknown PICA fragment uniform block");
        }
    }
    // The draw uploader reserves slot zero even when Metal optimizes out the
    // common fragment block. Runtime pipeline reflection permits dead bindings.
    pair.slots[4] = 0;
    const auto outputs = InterfaceElements(stage, stage.resources.stage_outputs);
    Require(outputs.size() == 1 && std::get<1>(*outputs.begin()) == 0 &&
                std::get<2>(*outputs.begin()) == SPIRType::Float &&
                std::get<3>(*outputs.begin()) == 4,
            "unsupported PICA color outputs");
    // Keep the existing texture/sampler ABI. Ordinary material units are
    // ordered by guest slot and independently remapped from their Vulkan binding.
    std::map<unsigned, const Resource*> materials;
    for (const auto& resource : stage.resources.sampled_images) {
        ValidateTexture(stage, resource, SPIRType::Float);
        if (resource.name == "texture_buffer_lut_lf" && pair.slots[0] == RuntimeMetalUnusedSlot) {
            stage.Bind(resource, 0);
            // The backing texture uses texelFetch. Retain its reserved
            // sampler slot for the existing draw-dispatch ABI.
            pair.slots[0] = pair.slots[1] = 0;
        } else if (resource.name.size() == 4 && resource.name.starts_with("tex") &&
                   resource.name[3] >= '0' && resource.name[3] <= '2') {
            Require(materials.emplace(resource.name[3] - '0', &resource).second,
                    "duplicate PICA fragment material");
        } else {
            throw std::runtime_error("unknown PICA fragment texture");
        }
    }
    Require(materials.size() <= pair.units.size(), "too many PICA material textures");
    unsigned index = 0;
    for (const auto& [unit, resource] : materials) {
        pair.units[index] = unit;
        const unsigned slot = unsigned(pair.slots[0] != RuntimeMetalUnusedSlot) + index;
        stage.Bind(*resource, slot);
        if (index == 0) {
            pair.slots[2] = pair.slots[3] = slot;
        } else if (index == 1) {
            pair.secondary_texture = pair.secondary_sampler = slot;
        } else {
            pair.tertiary_texture = pair.tertiary_sampler = slot;
        }
        ++index;
    }
}

} // namespace

std::optional<RuntimeMetalShaderPair> TranslateRuntimeMetalShaders(
    std::string_view vs, std::string_view fs, RuntimeMetalVertexMode mode,
    bool raw_indices, bool raw_index_constants, std::string& error) {
    try {
        static const bool initialized = glslang::InitializeProcess();
        Require(initialized, "glslang process initialization");
        const bool user_clip = vs.starts_with(RuntimeMetalUserClip);
        Require(!user_clip || mode != RuntimeMetalVertexMode::StageIn,
                "Metal user clip plane requires generated vertex inputs");
        const bool desktop_clip = user_clip || vs.find(MacAtlasClip) != std::string_view::npos;
        const auto vertex = PrepareSource(vs, desktop_clip, raw_indices, raw_index_constants);
        const auto fragment = PrepareSource(fs, desktop_clip, false, false);
        glslang::TShader vertex_shader(EShLangVertex), fragment_shader(EShLangFragment);
        Parse(vertex_shader, EShLangVertex, vertex);
        Parse(fragment_shader, EShLangFragment, fragment);
        glslang::TProgram program;
        program.addShader(&vertex_shader);
        program.addShader(&fragment_shader);
        constexpr auto flags = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (!program.link(flags) || !program.mapIO())
            throw std::runtime_error(program.getInfoLog());
        Stage metal_vs(EmitSpirv(program, EShLangVertex), spv::ExecutionModelVertex);
        Stage metal_fs(EmitSpirv(program, EShLangFragment), spv::ExecutionModelFragment);
        VerifyInterface(metal_vs, metal_fs, mode);
        RuntimeMetalShaderPair result;
        BindVertex(metal_vs, mode);
        BindFragment(metal_fs, result);
        result.vertex = metal_vs.Compile();
        result.fragment = metal_fs.Compile();
        Require(!desktop_clip || result.vertex.find("[[clip_distance]]") != std::string::npos,
                "Metal vertex stage lost the fixed PICA clip plane");
        return result;
    } catch (const std::exception& failure) {
        error = failure.what();
        return std::nullopt;
    }
}

} // namespace Cara
