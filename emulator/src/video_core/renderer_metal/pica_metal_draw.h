// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pica_metal_upload.h"
#include "pica_metal_batch_program.h"
#include <memory>
#include "pica_metal_dual_material.h"

namespace Cara {

enum class PicaMetalVertexMode { Packed, DecodedIndexed, StageIn };

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
    std::optional<SecondaryPicaMaterial> secondary_material{};
    PicaMetalVertexMode vertex_mode = PicaMetalVertexMode::Packed;
    std::optional<SecondaryPicaMaterial> tertiary_material{};
    bool isolate_color_blend = false;
    std::shared_ptr<const BatchedMetalVertexProgram> batched_vertex;
    std::uint32_t unique_source_vertices = 0;
};

// Requires an uncommitted command and an acquired upload lease. The caller
// commits exactly once via its arena. No texture ownership is inferred here.
bool EncodeTexturedMetalDraw(id<MTLCommandBuffer> commands, PicaMetalUploadArena& arena,
                            const TexturedMetalDraw& draw);

} // namespace Cara
