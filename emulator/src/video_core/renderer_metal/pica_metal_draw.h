// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pica_metal_upload.h"
#include "pica_metal_batch_program.h"
#include <memory>
#include <bit>
#include "pica_metal_dual_material.h"

namespace Cara {

// Value3 is the existing internal raw-index draw marker.
enum class PicaMetalVertexMode { Packed, DecodedIndexed, StageIn, PairIndexed = 4 };
struct MetalPairIndexPayload;

// Existing stage-in stream ABI. Field order matches PICA output locations 0–7.
struct PicaMetalStageVertex {
    std::array<float, 4> position, color;
    std::array<float, 2> tex_coord0, tex_coord1, tex_coord2;
    float tex_coord0_w;
    std::array<float, 4> quaternion;
    std::array<float, 3> view;
};
static_assert(sizeof(PicaMetalStageVertex) == 88);

MTLVertexDescriptor* MakePicaStageInDescriptor();

// Failure-only owner-thread cleanup. End an existing encoder and abandon owned
// deferred packets without encoding or submitting any additional guest draws.
void AbortGroupedMetalDraw(id<MTLCommandBuffer> command) noexcept;

inline constexpr std::array<const char*,8> NativeMetalRasterStateNames{
    "pipeline", "depth", "stencil_ref", "blend_color", "viewport", "scissor", "cull", "winding"};
// Each pair is [requests, skipped calls], collected only with renderer profiling.
std::array<std::array<std::uint64_t,2>,8> ReadNativeMetalRasterStateProfile() noexcept;

inline bool EqualMetalBlendColorBits(const std::array<float,4>& a,
                                    const std::array<float,4>& b) noexcept {
    return std::bit_cast<std::array<std::uint32_t,4>>(a) ==
           std::bit_cast<std::array<std::uint32_t,4>>(b);
}

inline bool EqualMetalViewportBits(const MTLViewport& a, const MTLViewport& b) noexcept {
    const auto same=[](double x,double y) {
        return std::bit_cast<std::uint64_t>(x)==std::bit_cast<std::uint64_t>(y);
    };
    return same(a.originX,b.originX) && same(a.originY,b.originY) &&
           same(a.width,b.width) && same(a.height,b.height) &&
           same(a.znear,b.znear) && same(a.zfar,b.zfar);
}


struct SecondaryPicaMaterial {
    id<MTLTexture> texture = nil;
    id<MTLSamplerState> sampler = nil;
    unsigned unit = NoMaterialUnit;
    NSUInteger texture_slot = NSNotFound, sampler_slot = NSNotFound;
};

// A reflected one- or two-texture PICA pair. All resources must have been selected and
// synchronized by the renderer owner before this can execute.
struct TexturedMetalDraw {
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth_stencil_state;
    id<MTLSamplerState> texel_fetch_sampler;
    id<MTLTexture> color, depth_stencil, lighting_lut, texture0;
    bool unbacked_depth = false;
    id<MTLSamplerState> texture0_sampler;
    NSUInteger lut_texture_slot, lut_sampler_slot, texture0_slot, texture0_sampler_slot;
    NSUInteger fragment_common_slot, fragment_light_slot;
    MTLViewport viewport;
    MTLScissorRect scissor;
    MTLCullMode cull;
    MTLWinding winding;
    std::uint32_t stencil_reference;
    std::array<float,4> blend_color;
    NSUInteger vertex_count;
    std::span<const std::byte> packed_vertices, expanded_indices;
    std::span<const std::byte> layout_uniform, vs_uniform, common_uniform, light_uniform;
    unsigned primary_material_unit = NoMaterialUnit;
    bool primary_material_cube = false;
    std::optional<SecondaryPicaMaterial> secondary_material{};
    PicaMetalVertexMode vertex_mode = PicaMetalVertexMode::Packed;
    std::optional<SecondaryPicaMaterial> tertiary_material{};
    bool isolate_color_blend = false;
    std::shared_ptr<const BatchedMetalVertexProgram> batched_vertex;
    std::uint32_t unique_source_vertices = 0;
    bool simd_reference = false;
    std::shared_ptr<const MetalPairIndexPayload> pair_indices;
};

// Requires an uncommitted command and an acquired upload lease. The caller
// commits exactly once via its arena. No texture ownership is inferred here.
bool EncodeTexturedMetalDraw(id<MTLCommandBuffer> commands, PicaMetalUploadArena& arena,
                            const TexturedMetalDraw& draw);

} // namespace Cara
