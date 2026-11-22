// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <future>
#include <optional>
#include <string_view>
#include <string>
#include <vector>
#include <unordered_map>
#include "pica_metal_draw.h"
#include "pica_metal_runtime_shader.h"
#include "pica_metal_source_pool.h"

namespace Cara {

void ConfigureMetalGeneralFragment(bool enabled);
bool UseMetalGeneralFragment();

void ConfigureNativeMetalWarmup(const std::string& user, std::string_view rom_name);
bool ConfigureNativeMetalLearning(const std::string& user, std::uint64_t title,
                                  const std::array<std::uint8_t, 32>& game_hash,
                                  std::string_view version);

// Taken from the original draw-state builder, before any legacy acquire or GL calls.
struct PicaPipelineInputs {
    std::string_view vertex, fragment;
    unsigned sampled_unit = 0;
    unsigned secondary_unit = NoMaterialUnit;
    bool packed_arrays = false;
    PicaMetalVertexMode vertex_mode = PicaMetalVertexMode::Packed;
    MTLPixelFormat color_format = MTLPixelFormatRGBA8Unorm;
    bool dither = true;
    // Actual RGBA8 attachment dimensions and lower-left draw clip.
    // Zero-initialized inputs cannot use the M1 driver-specific proof.
    unsigned dither_width = 0, dither_height = 0;
    std::array<unsigned, 4> dither_scissor{};
    bool shadow = false;
    bool polygon_offset = false;
    bool discard = false;
    bool sample_control = false;
    bool active_logic_op = false;
    bool clip0 = false;
    bool clip1 = false;
    double depth_near = 0, depth_far = 1;
    bool blend = false;
    bool depth_test = false;
    bool depth_write = false;
    bool stencil = false;
    unsigned rgb_op = 0, alpha_op = 0;
    unsigned rgb_src = 0, rgb_dst = 0, alpha_src = 0, alpha_dst = 0;
    unsigned color_mask = 0;
    unsigned depth_compare = 0, stencil_compare = 0;
    unsigned stencil_read = 0, stencil_write = 0;
    unsigned stencil_fail = 0, stencil_depth_fail = 0, stencil_pass = 0;
    unsigned stencil_reference = 0;
    bool cull = false;
    unsigned cull_face = 0, winding = 0;
    std::array<float,4> blend_color{};
    unsigned tertiary_unit = NoMaterialUnit;
};

struct PicaSampleInputs {
    unsigned min_filter = 0, mag_filter = 0, wrap_s = 0, wrap_t = 0;
    unsigned compare = 0, base_level = 0, max_level = 0, levels = 0;
    float min_lod = 0, max_lod = 0, lod_bias = 0, anisotropy = 1;
    bool rgba8 = false, normalized = false, identity_swizzle = false;
    bool opaque_black_border = false;
};

// Private generated assets: raw VS/FS identities and matching translated
// stages. Both active texture/sampler roles must agree with Metal reflection.
struct PicaMetalDualAsset {
    std::string vertex, fragment, msl_vertex, msl_fragment;
    unsigned primary_unit = NoMaterialUnit, secondary_unit = NoMaterialUnit;
    std::array<NSUInteger,6> primary_slots{};
    NSUInteger secondary_texture_slot = NSNotFound, secondary_sampler_slot = NSNotFound;
    // Set only for assets derived from the iOS GLES generator and verified on iOS.
    bool ios_gles_verified = false;
};

struct PicaMetalPipelineSelection {
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLDepthStencilState> depth = nil;
    id<MTLSamplerState> texel_fetch = nil, material = nil;
    std::array<NSUInteger,6> slots{};
    unsigned primary_unit = NoMaterialUnit;
    PicaMetalVertexMode vertex_mode = PicaMetalVertexMode::Packed;
    std::optional<SecondaryPicaMaterial> secondary;
    std::optional<SecondaryPicaMaterial> tertiary;
    MTLCullMode cull = MTLCullModeNone;
    MTLWinding winding = MTLWindingClockwise;
    unsigned stencil_reference = 0;
    std::array<float,4> blend_color{};

    // The caller binds authoritative backing, raster bounds and immutable payloads.
    void Apply(TexturedMetalDraw& draw) const;
    std::shared_ptr<const BatchedMetalVertexProgram> batched_vertex;
    bool simd_reference = false;
    bool primary_material_cube = false;
    bool general_fragment = false;
};

// Same owner thread as the draw queue. Prepare is explicit startup/preparation
// work. Select is read-only and never compiles during a live draw.
class PicaMetalPipelines {
public:
    explicit PicaMetalPipelines(id<MTLDevice> device_, std::vector<PicaMetalDualAsset> dual = {});
    bool BeginWarmup();
    bool FinishWarmup();
    PicaMetalPipelines(const PicaMetalPipelines&) = delete;
    PicaMetalPipelines& operator=(const PicaMetalPipelines&) = delete;
    std::optional<std::array<unsigned, 3>> GateMaterialUnits(
        std::string_view vertex, std::string_view fragment, PicaMetalVertexMode mode) const;
    bool Prepare(const PicaPipelineInputs& pica, const PicaSampleInputs& sampler,
                 const std::optional<PicaSampleInputs>& secondary = std::nullopt,
                 const std::optional<PicaSampleInputs>& tertiary = std::nullopt);
    std::optional<PicaMetalPipelineSelection> SelectWithGeneral(
        const PicaPipelineInputs& exact, std::string_view general_fragment,
        const PicaSampleInputs& sampler, const std::optional<PicaSampleInputs>& secondary,
        const std::optional<PicaSampleInputs>& tertiary);
    std::optional<PicaMetalPipelineSelection> Select(
        const PicaPipelineInputs& pica, const PicaSampleInputs& sampler,
        const std::optional<PicaSampleInputs>& secondary = std::nullopt,
        const std::optional<PicaSampleInputs>& tertiary = std::nullopt) const;

private:
    friend struct PicaMetalPipelineCacheTestAccess;
    using Key = std::array<unsigned, 28>;
    struct WarmupRecipe {
        std::string vertex, fragment;
        Key key{};
        std::array<unsigned, 3> units{};
        unsigned mode{};
    };
    bool LoadWarmup();
    bool PrepareRecipe(const WarmupRecipe&);
    bool SaveWarmup();
    void RememberLearnedPipeline(const PicaPipelineInputs&, const Key&,
                                 MTLRenderPipelineDescriptor*);
    std::size_t warmup_recipe_bytes{};
    unsigned warmup_prepared{}, warmup_new_recipes{};
    unsigned warmup_mode{};
    bool warming{}, warmup_recording_failed{};
    std::string warmup_path;
    std::vector<WarmupRecipe> warmup_recipes;
    std::vector<MTLRenderPipelineDescriptor*> warmup_descriptors;
    id<MTLBinaryArchive> warmup_archive = nil;
    unsigned warmup_draw_misses{}, warmup_archive_hits{}, warmup_archive_misses{};
    mutable unsigned warmup_translation_misses{};
    struct Entry {
        Key key{};
        PicaMetalPipelineSelection selection{};
    };
    struct RuntimeEntry {
        std::string_view vertex_source, fragment_source;
        PicaMetalVertexMode mode;
        // Metadata retains its public shape; source ownership is in the pool.
        RuntimeMetalShaderPair stages;
        std::string_view msl_vertex, msl_fragment;
    };
    std::optional<unsigned> FindRuntime(std::string_view vertex, std::string_view fragment,
                                        PicaMetalVertexMode mode) const;
    std::optional<std::array<unsigned, 3>> RuntimeMaterialUnits(
        std::string_view vertex, std::string_view fragment, PicaMetalVertexMode mode) const;
    std::optional<Key> MakeKey(const PicaPipelineInputs&, const PicaSampleInputs&,
                               const std::optional<PicaSampleInputs>& secondary,
                               const std::optional<PicaSampleInputs>& tertiary) const;
    const Entry* Find(const Key&) const;
    const id<MTLDevice> device;
    const std::vector<PicaMetalDualAsset> dual_assets;
    const id<MTLLibrary> gate_library;
    const bool rgba8_dither_verified_host;
    // Declared before all dependent view caches, so it is destroyed last.
    mutable PicaMetalSourcePool runtime_sources{64 * 1024 * 1024};
    // Reuse successful Metal stage functions across PSO state variants.
    using StageCache = std::unordered_map<std::string_view, id<MTLFunction>>;
    StageCache vertex_functions, fragment_functions;
    std::vector<Entry> entries;
    // Storage remains stable for the stage cache's string_view keys.
    mutable std::deque<RuntimeEntry> runtime_pairs;
    struct CompiledSpecialization {
        PicaMetalPipelineSelection selection;
        MTLRenderPipelineDescriptor* descriptor;
    };
    struct Specialization {
        Key key;
        PicaPipelineInputs inputs;
        std::future<std::optional<CompiledSpecialization>> result;
    };
    bool retain_compiled_descriptor = false;
    MTLRenderPipelineDescriptor* compiled_descriptor = nil;
    std::optional<Specialization> specializer;
    std::vector<Key> failed_specializations;

};

} // namespace Cara
