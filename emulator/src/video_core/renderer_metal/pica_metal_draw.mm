// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_pair_indices.h"
#include "pica_metal_draw.h"
#include "pica_metal_simd_token.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_draw_stage_profile.hh"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include "video_core/renderer_metal/pica_metal_depth_clear_encoding.hh"
#include "common/logging/log.h"
#include <algorithm>
#include <bit>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

namespace Cara {
namespace {

// The captured shader addresses both input textures in 128-cell rows.
constexpr NSUInteger row_width = 128;
constexpr NSUInteger lut_width = 1024;
constexpr NSUInteger vertex_row_bytes = row_width * 16;
constexpr NSUInteger index_row_bytes = row_width * 2;
// Align for portable Metal constant-buffer offsets across the supported iOS GPU set.
constexpr NSUInteger constant_alignment = 256;

bool ValidExtraMaterial(const TexturedMetalDraw& d, const SecondaryPicaMaterial& s) {
    // Read-only slots may sample the same immutable decoded image. The
    // following checks and the draw-group ownership gate still forbid writes.
    return s.texture && s.sampler &&
           s.texture != d.color && s.texture != d.depth_stencil && s.texture != d.lighting_lut &&
           s.texture.device == d.color.device && s.sampler.device == d.color.device &&
           s.texture.pixelFormat == MTLPixelFormatRGBA8Unorm &&
           s.texture.textureType == MTLTextureType2D && s.texture.sampleCount == 1 &&
           s.texture_slot < 16 && s.sampler_slot < 16 &&
           s.texture_slot != d.texture0_slot && s.sampler_slot != d.texture0_sampler_slot &&
           s.texture_slot != d.lut_texture_slot && s.sampler_slot != d.lut_sampler_slot;
}

bool ValidExtraMaterials(const TexturedMetalDraw& d) {
    if (!d.secondary_material)
        return !d.tertiary_material;
    const auto& second = *d.secondary_material;
    if (!SupportedDualUnits(d.primary_material_unit, second.unit) || !ValidExtraMaterial(d, second))
        return false;
    if (!d.tertiary_material)
        return true;
    const auto& third = *d.tertiary_material;
    return d.primary_material_unit == 0 && second.unit == 1 && third.unit == 2 &&
           ValidExtraMaterial(d, third) && third.texture_slot != second.texture_slot &&
           third.sampler_slot != second.sampler_slot;
}

bool ValidMaterial(const TexturedMetalDraw& d) {
    if (d.texture0_slot == NSNotFound)
        return d.texture0_sampler_slot == NSNotFound && !d.texture0 && !d.texture0_sampler &&
               d.primary_material_unit == NoMaterialUnit && !d.primary_material_cube &&
               !d.secondary_material && !d.tertiary_material;
    const auto dimension = d.primary_material_cube ? MTLTextureTypeCube : MTLTextureType2D;
    return (!d.primary_material_cube || d.primary_material_unit == 0) && d.texture0_slot < 16 && d.texture0_sampler_slot < 16 && d.texture0_sampler &&
           d.texture0_sampler.device == d.color.device &&
           d.texture0 && d.texture0.device == d.color.device &&
           d.texture0.pixelFormat == MTLPixelFormatRGBA8Unorm &&
           d.texture0.textureType == dimension && d.texture0.sampleCount == 1 &&
           d.texture0 != d.color && d.texture0 != d.depth_stencil &&
           d.texture0 != d.lighting_lut && d.texture0_slot != d.lut_texture_slot &&
           d.texture0_sampler_slot != d.lut_sampler_slot && ValidExtraMaterials(d);
}

bool ValidLut(const TexturedMetalDraw& d) {
    if (d.lut_texture_slot == NSNotFound)
        return d.lut_sampler_slot == NSNotFound && !d.lighting_lut;
    return d.lut_texture_slot < 16 && d.lut_sampler_slot < 16 && d.lighting_lut &&
           d.lighting_lut.device == d.color.device &&
           d.lighting_lut.pixelFormat == MTLPixelFormatRG32Float &&
           d.lighting_lut.width == lut_width && d.lighting_lut.textureType == MTLTextureType2D &&
           d.lighting_lut != d.color && d.lighting_lut != d.depth_stencil;
}

bool ValidVertexStream(const TexturedMetalDraw& d) {
    if (!d.vertex_count || d.vertex_count % 3)
        return false;
    if (d.vertex_mode == PicaMetalVertexMode::StageIn)
        return d.expanded_indices.empty() && d.layout_uniform.empty() && d.vs_uniform.empty() &&
               d.packed_vertices.size() % sizeof(PicaMetalStageVertex) == 0 &&
               d.packed_vertices.size() / sizeof(PicaMetalStageVertex) == d.vertex_count;

    if (d.packed_vertices.empty() || d.packed_vertices.size() % vertex_row_bytes ||
        d.vs_uniform.size() != 1856)
        return false;
    if (d.vertex_mode == PicaMetalVertexMode::PairIndexed) {
        const auto& pair = d.pair_indices;
        return pair && pair->admitted && pair->pair_count > 0 && pair->pair_count <= 65535 &&
            pair->elements.size() == d.vertex_count && pair->element_buffer && pair->pair_texture &&
            pair->element_buffer.device == d.color.device && pair->pair_texture.device == d.color.device &&
            pair->pair_texture.pixelFormat == MTLPixelFormatRGBA32Uint && pair->pair_texture.width == 128 &&
            pair->pair_texture.height >= (pair->pair_count+127)/128 &&
            pair->element_buffer.length >= d.vertex_count*sizeof(std::uint16_t) && d.layout_uniform.size() == 256;
    }
    if (d.vertex_mode == MetalRawIndexMode) {
        constexpr auto raw_row_bytes = MetalRawIndexRow * sizeof(std::uint16_t);
        if (d.expanded_indices.empty() || d.expanded_indices.size() % raw_row_bytes)
            return false;
        std::uint16_t mode;
        std::memcpy(&mode, d.expanded_indices.data(), sizeof(mode));
        const auto topology = mode & 3;
        const auto width = mode >> 2;
        const auto input_count = topology == 1 || topology == 2 ? d.vertex_count / 3 + 2
                                                               : d.vertex_count;
        const auto payload_bytes = d.expanded_indices.size() - MetalRawIndexHeader * sizeof(mode);
        if (width > 2 || (width != 0 && input_count > payload_bytes / width))
            return false;
    } else if (d.expanded_indices.size() % index_row_bytes ||
               d.expanded_indices.size() / sizeof(std::uint16_t) < d.vertex_count) {
        return false;
    }
    return d.vertex_mode == PicaMetalVertexMode::DecodedIndexed ? d.layout_uniform.empty()
                                                                : d.layout_uniform.size() == 256;
}

bool Valid(const TexturedMetalDraw& d) {
    if ((d.simd_reference && (d.vertex_mode != MetalRawIndexMode || d.batched_vertex)) ||
        !d.pipeline || !d.depth_stencil_state || !d.texel_fetch_sampler ||
        !d.color || !d.depth_stencil || !ValidLut(d) || !ValidMaterial(d) ||
        d.fragment_common_slot >= 16 ||
        (d.fragment_light_slot != NSNotFound && d.fragment_light_slot >= 16) ||
        d.fragment_common_slot == d.fragment_light_slot ||
        d.color == d.depth_stencil || !ValidVertexStream(d) ||
        d.common_uniform.size()!=288 || d.light_uniform.size()!=1040) {
        return false;
    }
    const NSUInteger width=d.color.width,height=d.color.height;
    const auto& scissor=d.scissor;
    const auto& viewport=d.viewport;
    return (d.color.pixelFormat == MTLPixelFormatRGBA8Unorm ||
            d.color.pixelFormat == MTLPixelFormatB5G6R5Unorm ||
            d.color.pixelFormat == MTLPixelFormatABGR4Unorm ||
            d.color.pixelFormat == MTLPixelFormatA1BGR5Unorm) &&
           d.depth_stencil.pixelFormat==MTLPixelFormatDepth32Float_Stencil8 &&
           d.color.textureType==MTLTextureType2D &&
           d.depth_stencil.textureType==MTLTextureType2D &&
           d.color.sampleCount==1 && d.depth_stencil.sampleCount==1 &&
           d.depth_stencil.width==width && d.depth_stencil.height==height &&
           scissor.width && scissor.height && scissor.x<=width && scissor.y<=height &&
           scissor.width<=width-scissor.x && scissor.height<=height-scissor.y &&
           std::isfinite(viewport.originX) && std::isfinite(viewport.originY) &&
           std::isfinite(viewport.width) && std::isfinite(viewport.height) &&
           viewport.width>0 && viewport.height>0 && viewport.znear==0 && viewport.zfar==1;
}

} // namespace

MTLVertexDescriptor* MakePicaStageInDescriptor() {
    auto* desc = [MTLVertexDescriptor vertexDescriptor];
    constexpr NSUInteger slot = 1;
    constexpr std::array<NSUInteger, 8> offsets{
        offsetof(PicaMetalStageVertex, position), offsetof(PicaMetalStageVertex, color),
        offsetof(PicaMetalStageVertex, tex_coord0), offsetof(PicaMetalStageVertex, tex_coord1),
        offsetof(PicaMetalStageVertex, tex_coord2), offsetof(PicaMetalStageVertex, tex_coord0_w),
        offsetof(PicaMetalStageVertex, quaternion), offsetof(PicaMetalStageVertex, view)};
    constexpr std::array<MTLVertexFormat, 8> formats{
        MTLVertexFormatFloat4, MTLVertexFormatFloat4, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
        MTLVertexFormatFloat2, MTLVertexFormatFloat, MTLVertexFormatFloat4, MTLVertexFormatFloat3};
    for (NSUInteger i = 0; i < offsets.size(); ++i) {
        desc.attributes[i].offset = offsets[i];
        desc.attributes[i].format = formats[i];
        desc.attributes[i].bufferIndex = slot;
    }
    desc.layouts[slot].stride = sizeof(PicaMetalStageVertex);
    desc.layouts[slot].stepFunction = MTLVertexStepFunctionPerVertex;
    return desc;
}

namespace {

struct EncoderBindings {
    std::uint32_t vertex_buffers{}, vertex_textures{}, vertex_samplers{};
    std::uint32_t fragment_buffers{}, fragment_textures{}, fragment_samplers{};
};

struct EndEncoderOnExit {
    id<MTLCommandEncoder> encoder;
    ~EndEncoderOnExit() { if (encoder) [encoder endEncoding]; }
};

using UniformRange = std::optional<PicaMetalUploadArena::Range>;

struct UniformUploads {
    UniformRange common, light, vs;
};

struct RasterStateSnapshot {
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth;
    std::uint32_t stencil_reference;
    std::array<float,4> blend_color;
    MTLViewport viewport;
    MTLScissorRect scissor;
    MTLCullMode cull;
    MTLWinding winding;
};
thread_local std::array<std::array<std::uint64_t,2>,8> raster_state_profile{};

bool ReuseRasterState() {
    static const bool enabled=[] {
        const auto* value=std::getenv("CARA_METAL_REUSE_RASTER_STATE");
        return value && std::string_view(value)=="1";
    }();
    return enabled;
}

bool ReuseFragmentBufferBindings() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_REUSE_FRAGMENT_BUFFER_BINDINGS");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

struct FragmentBufferBinding {
    // Enabled only for commands that retain their encoded resources.
    __unsafe_unretained id<MTLBuffer> buffer = nil;
    NSUInteger offset = 0;
};

struct OpenDrawPass {
    id<MTLCommandBuffer> command;
    id<MTLRenderCommandEncoder> encoder;
    id<MTLTexture> color, depth;
    EncoderBindings bound{};
    const PicaMetalUploadArena* uniform_owner = nullptr;
    UniformUploads uniforms{};
    std::optional<RasterStateSnapshot> raster;
    std::optional<std::array<FragmentBufferBinding, 16>> fragment_buffers;
};

// A Metal command admits one contiguous native draw group on its owning lane.
// The group explicitly ends the encoder before submitting it.
thread_local std::optional<OpenDrawPass> open_pass;

bool ReuseUniformUploads() {
    // Shared Apple opt-in. This currently trades deferred vertex batching for
    // exact uniform reuse inside an open pass; keep the default unchanged.
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_METAL_REUSE_UNIFORM_UPLOADS");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

bool ReuseDeferredUniformUploads() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_METAL_REUSE_DEFERRED_UNIFORMS");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

struct UniformUploadMetrics {
    NativeMetalCost::Phase requests, hits, requested_bytes, copied_bytes;
};

constexpr UniformUploadMetrics common_upload_metrics{
    NativeMetalCost::UniformCommonUploadRequests, NativeMetalCost::UniformCommonUploadHits,
    NativeMetalCost::UniformCommonRequestedBytes, NativeMetalCost::UniformCommonCopiedBytes};
constexpr UniformUploadMetrics light_upload_metrics{
    NativeMetalCost::UniformLightUploadRequests, NativeMetalCost::UniformLightUploadHits,
    NativeMetalCost::UniformLightRequestedBytes, NativeMetalCost::UniformLightCopiedBytes};
constexpr UniformUploadMetrics vs_upload_metrics{
    NativeMetalCost::UniformVsUploadRequests, NativeMetalCost::UniformVsUploadHits,
    NativeMetalCost::UniformVsRequestedBytes, NativeMetalCost::UniformVsCopiedBytes};

UniformRange CopyDrawUniform(PicaMetalUploadArena& arena, std::span<const std::byte> source,
                             UniformRange* cached, const UniformUploadMetrics& metrics) {
    if (cached && *cached && (*cached)->bytes == source.size_bytes()) {
        // The open pass owns this arena lease. Ranges returned by Copy are
        // immutable within the pass and never used as GPU write destinations.
        const auto* previous = static_cast<const std::byte*>((*cached)->buffer.contents) +
                               (*cached)->offset;
        if (std::memcmp(previous, source.data(), source.size_bytes()) == 0) {
            NativeMetalCost::Count(metrics.requests, 1);
            NativeMetalCost::Count(metrics.hits, 1);
            NativeMetalCost::Count(metrics.requested_bytes, source.size_bytes());
            return *cached;
        }
    }
    const auto range = arena.Copy(source, constant_alignment);
    if (range) {
        if (cached)
            *cached = range;
        NativeMetalCost::Count(metrics.requests, 1);
        NativeMetalCost::Count(metrics.requested_bytes, source.size_bytes());
        NativeMetalCost::Count(metrics.copied_bytes, source.size_bytes());
    }
    return range;
}

void ClearStaleBindings(OpenDrawPass& pass, const TexturedMetalDraw& d,
                        bool stage_in, bool packed, bool computed) {
    const auto bit = [](NSUInteger i) -> std::uint32_t {
        return i == NSNotFound ? 0 : std::uint32_t{1} << i;
    };
    const bool gather = computed && d.batched_vertex->gathers_unique_vertices;
    EncoderBindings current{
        (computed ? (gather ? 9u : 8u) : stage_in ? 3u : (packed ? 7u : 3u)) | (d.simd_reference ? 8u : 0u),
        computed ? (gather ? 2u : 0u) : stage_in ? 0u : 3u,
        stage_in || computed ? 0u : 3u,
        bit(d.fragment_common_slot) | bit(d.fragment_light_slot),
        (d.lighting_lut ? bit(d.lut_texture_slot) : 0) |
            (d.texture0 ? bit(d.texture0_slot) : 0) |
            (d.secondary_material ? bit(d.secondary_material->texture_slot) : 0) |
            (d.tertiary_material ? bit(d.tertiary_material->texture_slot) : 0),
        (d.lighting_lut ? bit(d.lut_sampler_slot) : 0) |
            (d.texture0 ? bit(d.texture0_sampler_slot) : 0) |
            (d.secondary_material ? bit(d.secondary_material->sampler_slot) : 0) |
            (d.tertiary_material ? bit(d.tertiary_material->sampler_slot) : 0)};
    const auto clear = [](std::uint32_t before, std::uint32_t after, const auto& unset) {
        for (std::uint32_t stale = before & ~after; stale; stale &= stale - 1)
            unset(std::countr_zero(stale));
    };
    const auto encoder = pass.encoder;
    clear(pass.bound.vertex_buffers, current.vertex_buffers,
          [&](unsigned i) { [encoder setVertexBuffer:nil offset:0 atIndex:i]; });
    clear(pass.bound.vertex_textures, current.vertex_textures,
          [&](unsigned i) { [encoder setVertexTexture:nil atIndex:i]; });
    clear(pass.bound.vertex_samplers, current.vertex_samplers,
          [&](unsigned i) { [encoder setVertexSamplerState:nil atIndex:i]; });
    clear(pass.bound.fragment_buffers, current.fragment_buffers, [&](unsigned i) {
        [encoder setFragmentBuffer:nil offset:0 atIndex:i];
        if (pass.fragment_buffers)
            (*pass.fragment_buffers)[i] = {};
    });
    clear(pass.bound.fragment_textures, current.fragment_textures,
          [&](unsigned i) { [encoder setFragmentTexture:nil atIndex:i]; });
    clear(pass.bound.fragment_samplers, current.fragment_samplers,
          [&](unsigned i) { [encoder setFragmentSamplerState:nil atIndex:i]; });
    pass.bound = current;
}

struct DrawUploads {
    UniformRange common, light, stream, layout, vs;
    id<MTLTexture> vertices, indices;
};

std::optional<DrawUploads> UploadDraw(id<MTLCommandBuffer> commands, PicaMetalUploadArena& arena,
                                     const TexturedMetalDraw& d, bool grouped,
                                     UniformUploads* deferred_uniforms = nullptr) {
    if (!commands || commands.status!=MTLCommandBufferStatusNotEnqueued || !Valid(d) ||
        (d.vertex_mode == PicaMetalVertexMode::PairIndexed &&
         (!commands.retainedReferences || commands.device != d.pair_indices->element_buffer.device))) {
        LOG_ERROR(Render_OpenGL, "Native Metal encoder validation command={} lut={} material={} "
                  "vertex_stream={} vertex_bytes={} indices={} vs={} common={} light={}",
                  bool(commands), ValidLut(d), ValidMaterial(d), ValidVertexStream(d),
                  d.packed_vertices.size(), d.expanded_indices.size(), d.vs_uniform.size(),
                  d.common_uniform.size(), d.light_uniform.size());
        return std::nullopt;
    }
    const bool stage_in = d.vertex_mode == PicaMetalVertexMode::StageIn;
    const bool raw_indices = d.vertex_mode == MetalRawIndexMode;
    const bool paired = d.vertex_mode == PicaMetalVertexMode::PairIndexed;
    const bool packed = d.vertex_mode == PicaMetalVertexMode::Packed || raw_indices || paired;
    if (grouped && open_pass &&
        (open_pass->command != commands || open_pass->color != d.color ||
         open_pass->depth != d.depth_stencil))
        return std::nullopt;
    NativeMetalCost::Scope upload(NativeMetalCost::Upload);
    const bool reuse_uniforms = grouped && ReuseUniformUploads() && commands.retainedReferences;
    auto* cached = deferred_uniforms ? deferred_uniforms :
        reuse_uniforms && open_pass && open_pass->uniform_owner == &arena ?
            &open_pass->uniforms : nullptr;
    const auto common = CopyDrawUniform(arena, d.common_uniform, cached ? &cached->common : nullptr,
                                        common_upload_metrics);
    const auto light = CopyDrawUniform(arena, d.light_uniform, cached ? &cached->light : nullptr,
                                       light_upload_metrics);
    const auto stream = stage_in ? arena.Copy(d.packed_vertices, constant_alignment) : std::nullopt;
    const auto layout = packed ? arena.Copy(d.layout_uniform, constant_alignment) : std::nullopt;
    const auto vs = !stage_in ?
        CopyDrawUniform(arena, d.vs_uniform, cached ? &cached->vs : nullptr, vs_upload_metrics) :
        std::nullopt;
    id<MTLTexture> vertices = nil, indices = nil;
    if (!stage_in) {
        const auto format = packed ? MTLPixelFormatRGBA32Uint : MTLPixelFormatRGBA32Float;
        vertices = arena.Copy2D(d.packed_vertices, format, row_width,
                                d.packed_vertices.size() / vertex_row_bytes, vertex_row_bytes);
        const auto index_width = raw_indices ? MetalRawIndexRow : row_width;
        const auto index_bytes = index_width * sizeof(std::uint16_t);
        indices = paired ? d.pair_indices->pair_texture :
            arena.Copy2D(d.expanded_indices, MTLPixelFormatR16Uint, index_width,
                         d.expanded_indices.size() / index_bytes, index_bytes);
    }
    if (!common || !light || (stage_in && !stream) ||
        (!stage_in && (!vertices || !indices || !vs)) || (packed && !layout)) {
        LOG_ERROR(Render_OpenGL, "Native Metal encoder upload common={} light={} stream={} "
                  "layout={} vs={} vertices={} indices={}", common.has_value(), light.has_value(),
                  stream.has_value(), layout.has_value(), vs.has_value(), bool(vertices), bool(indices));
        return std::nullopt;
    }

    return DrawUploads{common, light, stream, layout, vs, vertices, indices};
}

bool EncodeUploadedDraw(id<MTLCommandBuffer> commands, PicaMetalUploadArena& arena,
                        const TexturedMetalDraw& d, const DrawUploads& data, bool grouped,
                        bool allow_fused_clear, id<MTLBuffer> computed = nil,
                        NSUInteger output_offset = 0) {
    const auto& [common, light, stream, layout, vs, vertices, indices] = data;
    const auto stage_in = d.vertex_mode == PicaMetalVertexMode::StageIn;
    const auto packed = d.vertex_mode == PicaMetalVertexMode::Packed ||
                        d.vertex_mode == MetalRawIndexMode || d.vertex_mode == PicaMetalVertexMode::PairIndexed;
    const bool reuse_uniforms = grouped && ReuseUniformUploads() && commands.retainedReferences;
    if (grouped && open_pass &&
        (open_pass->command != commands || open_pass->color != d.color ||
         open_pass->depth != d.depth_stencil))
        return false;
    NativeMetalCost::Scope open(NativeMetalCost::EncoderSetup);
    id<MTLRenderCommandEncoder> encoder = nil;
    if (grouped && open_pass) {
        encoder = open_pass->encoder;
    } else {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = d.color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = d.depth_stencil;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.texture = d.depth_stencil;
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        auto* clear = allow_fused_clear ? active_depth_clear : nullptr;
        if (clear) {
            if (clear->encoded || clear->queue != commands.commandQueue ||
                clear->depth != d.depth_stencil)
                return false;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = (clear->word & 0x00ffffffu) ? 1.0 : 0.0;
            pass.stencilAttachment.loadAction = MTLLoadActionClear;
            pass.stencilAttachment.clearStencil = clear->word >> 24;
        }
        if (grouped)
            MetalDrawStageProfile::Attach(pass, commands);
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (grouped && !encoder)
            MetalDrawStageProfile::Cancel(commands);
        if (!encoder) {
            LOG_ERROR(Render_OpenGL, "Native Metal render command encoder unavailable");
            return false;
        }
        if (clear)
            clear->encoded = true;
        NativeMetalCost::Count(NativeMetalCost::EncoderStarts, 1);
        if (grouped) {
            open_pass.emplace(OpenDrawPass{commands, encoder, d.color, d.depth_stencil, {}});
            if (ReuseFragmentBufferBindings() && commands.retainedReferences)
                open_pass->fragment_buffers.emplace();
            if (reuse_uniforms) {
                open_pass->uniform_owner = &arena;
                open_pass->uniforms = {common, light, vs};
            }
        }
    }
    if (grouped)
        ClearStaleBindings(*open_pass, d, stage_in, packed, computed != nil);
    const EndEncoderOnExit end_ungrouped{grouped ? nil : encoder};
    open.Stop();
    NativeMetalCost::Scope encode(NativeMetalCost::EncoderDraw);
    const bool reuse_raster=grouped && ReuseRasterState();
    const auto* previous=reuse_raster && open_pass->raster ? &*open_pass->raster : nullptr;
    const bool profile=NativeMetalCost::Enabled();
    const auto apply=[&](unsigned state,bool unchanged,const auto& set) {
        if (profile) {
            ++raster_state_profile[state][0];
            raster_state_profile[state][1]+=unchanged;
        }
        if (!unchanged)
            set();
    };
    apply(0,previous && previous->pipeline==d.pipeline,[&] { [encoder setRenderPipelineState:d.pipeline]; });
    apply(1,previous && previous->depth==d.depth_stencil_state,[&] { [encoder setDepthStencilState:d.depth_stencil_state]; });
    apply(2,previous && previous->stencil_reference==d.stencil_reference,[&] { [encoder setStencilReferenceValue:d.stencil_reference]; });
    apply(3,previous && EqualMetalBlendColorBits(previous->blend_color,d.blend_color),[&] {
        [encoder setBlendColorRed:d.blend_color[0] green:d.blend_color[1]
                            blue:d.blend_color[2] alpha:d.blend_color[3]];
    });
    apply(4,previous && EqualMetalViewportBits(previous->viewport,d.viewport),[&] { [encoder setViewport:d.viewport]; });
    const auto same_scissor=[&] {
        return previous && previous->scissor.x==d.scissor.x && previous->scissor.y==d.scissor.y &&
               previous->scissor.width==d.scissor.width && previous->scissor.height==d.scissor.height;
    };
    apply(5,same_scissor(),[&] { [encoder setScissorRect:d.scissor]; });
    apply(6,previous && previous->cull==d.cull,[&] { [encoder setCullMode:d.cull]; });
    apply(7,previous && previous->winding==d.winding,[&] { [encoder setFrontFacingWinding:d.winding]; });
    // Only this owner writes raster state on the open encoder. Closing or
    // aborting its pass destroys the snapshot; no state crosses an encoder.
    if (reuse_raster)
        open_pass->raster.emplace(RasterStateSnapshot{d.pipeline,d.depth_stencil_state,
            d.stencil_reference,d.blend_color,d.viewport,d.scissor,d.cull,d.winding});
    if (d.simd_reference) {
        const auto token=simd_draw_tokens.Next();
        if (!token) throw std::runtime_error("Metal SIMD draw token exhausted");
        [encoder setVertexBytes:&*token length:sizeof(*token) atIndex:3];
        NativeMetalCost::Count(NativeMetalCost::SimdReferenceDraws, 1);
    }
    if (computed) {
        [encoder setVertexBuffer:computed offset:output_offset atIndex:3];
        if (d.batched_vertex->gathers_unique_vertices) {
            [encoder setVertexBuffer:layout->buffer offset:layout->offset atIndex:0];
            [encoder setVertexTexture:indices atIndex:1];
        }
    } else if (stage_in) {
        [encoder setVertexBuffer:common->buffer offset:common->offset atIndex:0];
        [encoder setVertexBuffer:stream->buffer offset:stream->offset atIndex:1];
    } else {
        if (packed)
            [encoder setVertexBuffer:layout->buffer offset:layout->offset atIndex:0];
        [encoder setVertexBuffer:vs->buffer offset:vs->offset atIndex:packed ? 1 : 0];
        [encoder setVertexBuffer:common->buffer offset:common->offset atIndex:packed ? 2 : 1];
        [encoder setVertexTexture:vertices atIndex:packed ? 0 : 1];
        [encoder setVertexTexture:indices atIndex:packed ? 1 : 0];
        [encoder setVertexSamplerState:d.texel_fetch_sampler atIndex:0];
        [encoder setVertexSamplerState:d.texel_fetch_sampler atIndex:1];
    }
    auto* fragment_buffers = grouped && open_pass->fragment_buffers ?
        &*open_pass->fragment_buffers : nullptr;
    const auto bind_fragment = [&](NSUInteger slot, const PicaMetalUploadArena::Range& range) {
        if (fragment_buffers) {
            auto& bound = (*fragment_buffers)[slot];
            if (bound.buffer == range.buffer) {
                if (bound.offset != range.offset) {
                    [encoder setFragmentBufferOffset:range.offset atIndex:slot];
                    bound.offset = range.offset;
                }
                return;
            }
            bound = {range.buffer, range.offset};
        }
        [encoder setFragmentBuffer:range.buffer offset:range.offset atIndex:slot];
    };
    if (d.fragment_light_slot != NSNotFound)
        bind_fragment(d.fragment_light_slot, *light);
    bind_fragment(d.fragment_common_slot, *common);
    if (d.lighting_lut) {
        [encoder setFragmentTexture:d.lighting_lut atIndex:d.lut_texture_slot];
        [encoder setFragmentSamplerState:d.texel_fetch_sampler atIndex:d.lut_sampler_slot];
    }
    if (d.texture0) {
        [encoder setFragmentTexture:d.texture0 atIndex:d.texture0_slot];
        [encoder setFragmentSamplerState:d.texture0_sampler atIndex:d.texture0_sampler_slot];
    }
    if (const auto& s=d.secondary_material) {
        [encoder setFragmentTexture:s->texture atIndex:s->texture_slot];
        [encoder setFragmentSamplerState:s->sampler atIndex:s->sampler_slot];
    }
    if (const auto& third = d.tertiary_material) {
        [encoder setFragmentTexture:third->texture atIndex:third->texture_slot];
        [encoder setFragmentSamplerState:third->sampler atIndex:third->sampler_slot];
    }
    if (d.vertex_mode == PicaMetalVertexMode::PairIndexed) {
        // Mode6's invocation ID names an immutable (source,triangle-first)
        // pair. Hardware indices preserve triangle corner and provoking order.
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d.vertex_count
            indexType:MTLIndexTypeUInt16 indexBuffer:d.pair_indices->element_buffer indexBufferOffset:0];
    } else {
        // The original raw shader indexes by triangle corner; indexing this
        // stream again would change both source and quaternion reference.
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:d.vertex_count];
    }
    return true;
}

bool EncodeDraw(id<MTLCommandBuffer> command, PicaMetalUploadArena& arena,
                const TexturedMetalDraw& draw, bool grouped) {
    NativeMetalCost::Scope cost(NativeMetalCost::Encode);
    const auto data = UploadDraw(command, arena, draw, grouped);
    return data && EncodeUploadedDraw(command, arena, draw, *data, grouped, true);
}

// The snapshots and their upload leases remain owned by the pending draw group
// until the compute pass and the ordered raster pass have been encoded.
struct DeferredDraw {
    TexturedMetalDraw draw;
    DrawUploads uploads;
    std::uint32_t output_base = 0;
    bool computed = false;
    bool transform_admitted = true;
};
struct DeferredBatch {
    std::vector<DeferredDraw> draws;
    id<MTLCommandBuffer> command = nil;
    PicaMetalUploadArena* arena = nullptr;
    UniformUploads uniforms{};
};
thread_local DeferredBatch deferred;

constexpr std::array<std::uint64_t, 11> deferred_corner_bounds{
    0, 3, 12, 48, 192, 768, 3072, 8192, 16384, 65536,
    std::numeric_limits<std::uint64_t>::max()};
constexpr std::array<std::uint64_t, 8> deferred_unique_bounds{
    0, 32, 128, 512, 2048, 8192, 65536, std::numeric_limits<std::uint64_t>::max()};
constexpr std::array<const char*, 12> deferred_reason_names{
    "no_retained_command", "simd_reference", "depth_clear", "uniform_reuse",
    "no_program", "wrong_mode", "invalid_unique_count", "transform_limit",
    "raster_corner_limit", "legacy_corner_limit", "ungrouped", "admitted"};
constexpr std::array<std::uint64_t, 7> deferred_group_bounds{1, 2, 4, 8, 16, 32, 64};
struct DeferredAdmissionProfile {
    // Each cell retains draws, raster corners and declared dense source count.
    std::array<std::array<std::array<std::uint64_t, 3>, deferred_unique_bounds.size()>,
               deferred_corner_bounds.size()> histogram{};
    std::array<std::uint64_t, deferred_reason_names.size()> reasons{};
    std::array<std::uint64_t, deferred_group_bounds.size()> group_sizes{};
    std::uint64_t singleton_draws = 0;
    std::uint64_t max_output_bytes = 0;
    std::uint64_t raster_only_packets = 0;
};
thread_local DeferredAdmissionProfile deferred_admission;

template<std::size_t N>
std::size_t AdmissionBucket(std::uint64_t value, const std::array<std::uint64_t, N>& bounds) {
    return std::find_if(bounds.begin(), bounds.end(),
                        [value](auto bound) { return value <= bound; }) - bounds.begin();
}

bool CloseNativeColorPass(id<MTLCommandBuffer> command) {
    if (!open_pass)
        return true;
    if (open_pass->command != command)
        return false;
    [open_pass->encoder endEncoding];
    open_pass.reset();
    return true;
}

bool IsolatedBlend(const TexturedMetalDraw& draw) {
    static const bool enabled = [] {
        const char* option = std::getenv("CARA_METAL_ISOLATE_BLEND_PASSES");
        return option && std::string_view(option) == "1";
    }();
    return enabled && draw.isolate_color_blend;
}

struct DeviceBatchRange {
    std::uint32_t first_group, count, output_base;
};

bool DedupVertexTransforms() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_DEDUP_VERTEX_TRANSFORMS");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

bool SameUniformBytes(const UniformRange& a, const UniformRange& b, NSUInteger bytes) {
    const auto valid = [bytes](const UniformRange& range) {
        return range && range->buffer && range->bytes == bytes &&
            range->buffer.storageMode == MTLStorageModeShared && range->buffer.contents &&
            range->offset <= range->buffer.length && bytes <= range->buffer.length-range->offset;
    };
    if (!valid(a) || !valid(b))
        return false;
    const auto* left = static_cast<const std::byte*>(a->buffer.contents)+a->offset;
    const auto* right = static_cast<const std::byte*>(b->buffer.contents)+b->offset;
    return std::memcmp(left, right, bytes) == 0;
}

bool SameUniqueTransform(const DeferredDraw& a, const DeferredDraw& b) {
    // UploadDraw snapshots these inputs into immutable ranges/textures. This
    // comparison is local to one command's live arena lease, never guest spans.
    return a.draw.batched_vertex->gathers_unique_vertices &&
        b.draw.batched_vertex->gathers_unique_vertices &&
        !(a.draw.batched_vertex->resources & ((1u << 4) | (1u << 6))) &&
        !(b.draw.batched_vertex->resources & ((1u << 4) | (1u << 6))) &&
        a.draw.batched_vertex->compute == b.draw.batched_vertex->compute &&
        a.draw.unique_source_vertices == b.draw.unique_source_vertices &&
        a.uploads.vertices == b.uploads.vertices &&
        SameUniformBytes(a.uploads.layout, b.uploads.layout, 256) &&
        SameUniformBytes(a.uploads.vs, b.uploads.vs, 1856) &&
        SameUniformBytes(a.uploads.common, b.uploads.common, 288);
}

bool FlushDeferredDraws(id<MTLCommandBuffer> command) {
    if (deferred.draws.empty())
        return true;
    if (deferred.command != command || !deferred.arena || !CloseNativeColorPass(command))
        return false;
    auto& arena = *deferred.arena;
    auto pending = std::move(deferred.draws);
    deferred.draws.clear();
    deferred.command = nil;
    deferred.arena = nullptr;
    deferred.uniforms = {};

    // Group by shader, while leaving raster jobs in their original order.
    struct Group {
        const BatchedMetalVertexProgram* program;
        std::vector<std::size_t> indices;
        std::vector<std::size_t> transforms;
    };
    std::vector<Group> groups;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (!pending[i].transform_admitted)
            continue;
        const auto* p = pending[i].draw.batched_vertex.get();
        const auto match = std::find_if(groups.begin(), groups.end(), [&](const Group& group) {
            return group.program->compute == p->compute;
        });
        if (match == groups.end())
            groups.push_back({p, {i}, {}});
        else
            match->indices.push_back(i);
    }
    const auto transform_count = [](const TexturedMetalDraw& draw) {
        return draw.batched_vertex->gathers_unique_vertices
                   ? draw.unique_source_vertices : static_cast<std::uint32_t>(draw.vertex_count);
    };
    std::uint32_t total = 0;
    std::uint32_t raster_corners = 0;
    std::uint32_t unique_transforms = 0;
    for (auto& group : groups) {
        if (NativeMetalCost::Enabled())
            ++deferred_admission.group_sizes[AdmissionBucket(group.indices.size(), deferred_group_bounds)];
        if (group.indices.size() < 2) {
            if (NativeMetalCost::Enabled())
                deferred_admission.singleton_draws += group.indices.size();
            continue;
        }
        const bool dedup = DedupVertexTransforms() && group.program->gathers_unique_vertices &&
            !(group.program->resources & ((1u << 4) | (1u << 6)));
        for (const auto i : group.indices) {
            auto& job = pending[i];
            const auto count = transform_count(job.draw);
            raster_corners += static_cast<std::uint32_t>(job.draw.vertex_count);
            job.computed = true;
            if (dedup) {
                NativeMetalCost::Count(NativeMetalCost::VertexTransformDedupCandidates, 1);
                const auto previous = std::find_if(group.transforms.begin(), group.transforms.end(),
                    [&](std::size_t index) { return SameUniqueTransform(pending[index], job); });
                if (previous != group.transforms.end()) {
                    job.output_base = pending[*previous].output_base;
                    NativeMetalCost::Count(NativeMetalCost::VertexTransformDedupHits, 1);
                    NativeMetalCost::Count(NativeMetalCost::VertexTransformsAvoided, count);
                    continue;
                }
            }
            job.output_base = total;
            total += count;
            if (group.program->gathers_unique_vertices)
                unique_transforms += count;
            group.transforms.push_back(i);
        }
    }
    id<MTLBuffer> outputs = nil;
    if (total) {
        if (NativeMetalCost::Enabled())
            deferred_admission.max_output_bytes = std::max(deferred_admission.max_output_bytes,
                std::uint64_t(total) * BatchedMetalVertexStride);
        const auto device = command.device;
        outputs = [device newBufferWithLength:NSUInteger(total) * BatchedMetalVertexStride
                                      options:MTLResourceStorageModePrivate];
        if (!outputs)
            return false;
        const auto encoder = [command computeCommandEncoder];
        if (!encoder)
            return false;
        const EndEncoderOnExit end_compute{encoder};
        for (const auto& group : groups) {
            if (group.indices.size() < 2)
                continue;
            const auto& p = *group.program;
            const auto width = p.compute.threadExecutionWidth;
            if (!width || width > p.compute.maxTotalThreadsPerThreadgroup) {
                return false;
            }
            const auto arg_length = p.arguments.encodedLength;
            id<MTLBuffer> args = [device newBufferWithLength:arg_length * group.transforms.size()
                                                   options:MTLResourceStorageModeShared];
            std::vector<DeviceBatchRange> ranges;
            ranges.reserve(group.transforms.size());
            std::uint32_t first_group = 0;
            if (!args) {
                return false;
            }
            for (const auto i : group.transforms) {
                const auto& job = pending[i];
                const auto& u = job.uploads;
                const auto index = ranges.size();
                const auto vertices = transform_count(job.draw);
                ranges.push_back({first_group, vertices, job.output_base});
                first_group += static_cast<std::uint32_t>((vertices + width - 1) / width);
                [p.arguments setArgumentBuffer:args startOffset:0 arrayElement:index];
                const auto buffer = [&](unsigned slot, const UniformRange& value) {
                    if (p.resources & (1u << slot)) {
                        [p.arguments setBuffer:value->buffer offset:value->offset atIndex:slot];
                        [encoder useResource:value->buffer usage:MTLResourceUsageRead];
                    }
                };
                const auto texture = [&](unsigned slot, id<MTLTexture> value) {
                    if (p.resources & (1u << slot)) {
                        [p.arguments setTexture:value atIndex:slot];
                        [encoder useResource:value usage:MTLResourceUsageRead];
                    }
                };
                buffer(0, u.layout);
                buffer(1, u.vs);
                buffer(2, u.common);
                texture(3, u.vertices);
                texture(4, u.indices);
                for (unsigned slot : {5u, 6u}) {
                    if (p.resources & (1u << slot))
                        [p.arguments setSamplerState:job.draw.texel_fetch_sampler atIndex:slot];
                }
            }
            const std::uint32_t count = ranges.size();
            [encoder setComputePipelineState:p.compute];
            [encoder setBuffer:args offset:0 atIndex:0];
            [encoder setBytes:ranges.data() length:ranges.size() * sizeof(DeviceBatchRange) atIndex:1];
            [encoder setBuffer:outputs offset:0 atIndex:2];
            [encoder setBytes:&count length:sizeof(count) atIndex:3];
            [encoder dispatchThreadgroups:MTLSizeMake(first_group, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
            NativeMetalCost::Count(NativeMetalCost::BatchedVertexDispatches, 1);
            NativeMetalCost::Count(NativeMetalCost::BatchedVertexDraws, group.indices.size());
        }
        NativeMetalCost::Count(NativeMetalCost::BatchedVertexCorners, raster_corners);
        if (unique_transforms)
            NativeMetalCost::Count(NativeMetalCost::BatchedUniqueVertexTransforms, unique_transforms);
    }

    for (auto& job : pending) {
        const auto isolated = IsolatedBlend(job.draw);
        if (isolated && !CloseNativeColorPass(command))
            return false;
        if (job.computed)
            job.draw.pipeline = job.draw.batched_vertex->raster;
        if (!EncodeUploadedDraw(command, arena, job.draw, job.uploads, !isolated, false,
                                job.computed ? outputs : nil,
                                NSUInteger(job.output_base) * BatchedMetalVertexStride))
            return false;
        if (isolated)
            NativeMetalCost::Count(NativeMetalCost::IsolatedBlendDraws, 1);
    }
    return true;
}

bool CanDeferDraw(id<MTLCommandBuffer> command, const TexturedMetalDraw& draw) {
    static const bool unique_count_admission = [] {
        const auto* value = std::getenv("CARA_METAL_UNIQUE_TRANSFORM_ADMISSION");
        return value && std::string_view(value) == "1";
    }();
    const bool profile = NativeMetalCost::Enabled();
    if (profile && draw.vertex_mode == MetalRawIndexMode) {
        auto& cell = deferred_admission.histogram[
            AdmissionBucket(draw.vertex_count, deferred_corner_bounds)][
            AdmissionBucket(draw.unique_source_vertices, deferred_unique_bounds)];
        ++cell[0];
        cell[1] += draw.vertex_count;
        cell[2] += draw.unique_source_vertices;
    }
    const auto result = [&](std::size_t reason) {
        if (profile)
            ++deferred_admission.reasons[reason];
        return reason == deferred_reason_names.size() - 1;
    };
    // Keep every existing ownership, clear, uniform and shader admission gate.
    if (!command.retainedReferences) return result(0);
    if (draw.simd_reference) return result(1);
    if (active_depth_clear) return result(2);
    if (ReuseUniformUploads()) return result(3);
    if (!draw.batched_vertex) return result(4);
    if (draw.vertex_mode != MetalRawIndexMode) return result(5);
    const bool gather = draw.batched_vertex->gathers_unique_vertices;
    if (gather && (!draw.unique_source_vertices || draw.unique_source_vertices >= draw.vertex_count))
        return result(6);
    const auto transforms = gather ? draw.unique_source_vertices : draw.vertex_count;
    if (transforms > 8192) return result(7);
    // The original raw capture admits at most 65536 raster corners. Only unique
    // gathers may use the wider input stream; output storage remains <=8192.
    if (unique_count_admission && gather) {
        if (draw.vertex_count > 65536) return result(8);
    } else if (draw.vertex_count > 8192) {
        return result(9);
    }
    if (!NativeMetalCost::GroupDrawPasses()) return result(10);
    return result(11);
}

bool CanDeferRasterOnly(id<MTLCommandBuffer> command, const TexturedMetalDraw& draw) {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_DEFER_RASTER_FALLBACKS");
        return value && std::string_view(value) == "1";
    }();
    // Only the already-recognized unique raw program is eligible. Its rejected
    // transform metadata is never used for allocation or dispatch. Keep every
    // encoder/clear/ownership boundary and the native raw capture's input bound.
    return enabled && command.retainedReferences && !draw.simd_reference &&
        !active_depth_clear && !ReuseUniformUploads() && draw.batched_vertex &&
        draw.batched_vertex->gathers_unique_vertices && draw.vertex_mode == MetalRawIndexMode &&
        draw.vertex_count <= 65536 && NativeMetalCost::GroupDrawPasses();
}

} // namespace

void AppendNativeMetalDeferredAdmissionProfile(std::ostream& out, bool json) {
    if (!NativeMetalCost::Enabled())
        return;
    if (json)
        out << ",\"metal_deferred_admission_totals\":{\"reasons\":{";
    for (std::size_t i = 0; i < deferred_reason_names.size(); ++i) {
        if (json)
            out << (i ? "," : "") << '"' << deferred_reason_names[i] << "\":"
                << deferred_admission.reasons[i];
        else
            out << "deferred_admission_" << deferred_reason_names[i] << ','
                << deferred_admission.reasons[i] << ",0\n";
    }
    if (json)
        out << "},\"singleton_draws\":" << deferred_admission.singleton_draws
            << ",\"raster_only_packets\":" << deferred_admission.raster_only_packets
            << ",\"max_output_bytes\":" << deferred_admission.max_output_bytes
            << ",\"group_sizes\":[";
    else
        out << "deferred_admission_singleton_draws," << deferred_admission.singleton_draws << ",0\n"
            << "deferred_admission_raster_only_packets," << deferred_admission.raster_only_packets << ",0\n"
            << "deferred_admission_max_output_bytes," << deferred_admission.max_output_bytes << ",0\n";
    for (std::size_t i = 0; i < deferred_group_bounds.size(); ++i) {
        if (json)
            out << (i ? "," : "") << "{\"draws_le\":" << deferred_group_bounds[i]
                << ",\"groups\":" << deferred_admission.group_sizes[i] << '}';
        else
            out << "deferred_group_draws_le" << deferred_group_bounds[i] << ','
                << deferred_admission.group_sizes[i] << ",0\n";
    }
    if (json) out << "],\"histogram\":[";
    bool first = true;
    for (std::size_t c = 0; c < deferred_corner_bounds.size(); ++c) {
        for (std::size_t u = 0; u < deferred_unique_bounds.size(); ++u) {
            const auto& cell = deferred_admission.histogram[c][u];
            if (!cell[0]) continue;
            if (json) {
                out << (first ? "" : ",") << "{\"corners_le\":" << deferred_corner_bounds[c]
                    << ",\"unique_le\":" << deferred_unique_bounds[u] << ",\"draws\":" << cell[0]
                    << ",\"corners\":" << cell[1] << ",\"unique\":" << cell[2] << '}';
                first = false;
            } else {
                for (std::size_t k = 0; k < cell.size(); ++k)
                    out << "deferred_hist_c" << deferred_corner_bounds[c] << "_u"
                        << deferred_unique_bounds[u] << '_' << std::array{"draws", "corners", "unique"}[k]
                        << ',' << cell[k] << ",0\n";
            }
        }
    }
    if (json) out << "]}";
}

bool EncodeTexturedMetalDraw(id<MTLCommandBuffer> commands, PicaMetalUploadArena& arena,
                            const TexturedMetalDraw& draw) {
    return EncodeDraw(commands, arena, draw, false);
}

std::array<std::array<std::uint64_t,2>,8> ReadNativeMetalRasterStateProfile() noexcept {
    return raster_state_profile;
}

bool CloseGroupedMetalDraw(id<MTLCommandBuffer> command) {
    return FlushDeferredDraws(command) && CloseNativeColorPass(command);
}

void AbortGroupedMetalDraw(id<MTLCommandBuffer> command) noexcept {
    if (!command)
        return;
    // Do not call CloseGroupedMetalDraw: that would execute deferred packets
    // after the renderer has already rejected the draw which caused the fault.
    if (open_pass && open_pass->command == command) {
        [open_pass->encoder endEncoding];
        open_pass.reset();
    }
    if (deferred.command == command)
        deferred = {};
    MetalDrawStageProfile::Cancel(command);
}

bool EncodeGroupedMetalDraw(id<MTLCommandBuffer> command, PicaMetalUploadArena& arena,
                           const TexturedMetalDraw& draw) {
    const bool transform_admitted = CanDeferDraw(command, draw);
    if (transform_admitted || CanDeferRasterOnly(command, draw)) {
        if (deferred.command && (deferred.command != command || deferred.arena != &arena))
            return false;
        // Keep compute and raster capacity aligned so a wide draw group does
        // not force an intermediate encoder/tile-store boundary at 32 draws.
        if (deferred.draws.size() >= NativeMetalCost::DrawComputeGroupLimit() &&
            !FlushDeferredDraws(command))
            return false;
        // A pending batch owns exactly one retained command and arena lease.
        // Its immutable uniform ranges remain valid through the later compute
        // and raster encoding. Flush/abort discard this memo before any lease
        // can rotate, so reuse does not require disabling compute batching.
        if (deferred.draws.empty())
            deferred.uniforms = {};
        auto* uniforms = ReuseDeferredUniformUploads() ? &deferred.uniforms : nullptr;
        auto data = UploadDraw(command, arena, draw, false, uniforms);
        if (!data)
            return false;
        auto snapshot = draw;
        snapshot.packed_vertices = {};
        snapshot.expanded_indices = {};
        snapshot.layout_uniform = {};
        snapshot.vs_uniform = {};
        snapshot.common_uniform = {};
        snapshot.light_uniform = {};
        deferred.draws.push_back({std::move(snapshot), std::move(*data), 0, false, transform_admitted});
        if (!transform_admitted && NativeMetalCost::Enabled())
            ++deferred_admission.raster_only_packets;
        deferred.command = command;
        deferred.arena = &arena;
        return true;
    }
    if (!FlushDeferredDraws(command))
        return false;
    const bool grouped = NativeMetalCost::GroupDrawPasses();
    if (grouped && IsolatedBlend(draw)) {
        if (!CloseNativeColorPass(command) || !EncodeDraw(command, arena, draw, false))
            return false;
        NativeMetalCost::Count(NativeMetalCost::IsolatedBlendDraws, 1);
        return true;
    }
    return EncodeDraw(command, arena, draw, grouped);
}

} // namespace Cara
