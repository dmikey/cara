// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/exporter-adapter/pica_metal_visibility_export.h"
#include "video_core/renderer_metal/exporter-adapter/pica_metal_queued_color_export.hh"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#import <Foundation/Foundation.h>

#include <cstring>
#include "common/logging/log.h"
#include <memory>
#include <stdexcept>

namespace Cara {
namespace {

constexpr NSUInteger pitch_alignment = 256;
constexpr NSUInteger max_extent = 4096;
constexpr auto gpu_scratch =
    MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeTracked;
constexpr auto cpu_scratch =
    MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;

NSUInteger Pitch(NSUInteger bytes) {
    return (bytes + pitch_alignment - 1) & ~(pitch_alignment - 1);
}

// Matches the separately verified pack_pica_depth kernel.
struct DepthLayout {
    std::uint32_t width, height, x, y;
    std::uint32_t packed_stride, depth_stride, stencil_stride, depth_bits;
};

void CopyRows(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture,
              id<MTLBuffer> destination, NSUInteger row_bytes, NSUInteger y,
              NSUInteger rows, MTLBlitOption option) {
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0,y,0) sourceSize:MTLSizeMake(texture.width,rows,1)
                  toBuffer:destination destinationOffset:y*row_bytes
     destinationBytesPerRow:row_bytes destinationBytesPerImage:row_bytes*rows options:option];
}

bool PackDepthRows(id<MTLCommandBuffer> command, id<MTLComputePipelineState> pipeline,
                   id<MTLBuffer> canonical, id<MTLBuffer> depth, id<MTLBuffer> stencil,
                   id<MTLBuffer> errors, const DepthLayout& layout) {
    const NSUInteger group = pipeline.threadExecutionWidth;
    if (!group || group > pipeline.maxTotalThreadsPerThreadgroup) {
        return false;
    }
    id<MTLComputeCommandEncoder> encode=[command computeCommandEncoder];
    if (!encode) {
        return false;
    }
    [encode setComputePipelineState:pipeline];
    [encode setBuffer:canonical offset:0 atIndex:0];
    [encode setBuffer:depth offset:0 atIndex:1];
    [encode setBuffer:stencil offset:0 atIndex:2];
    [encode setBuffer:errors offset:0 atIndex:3];
    [encode setBytes:&layout length:sizeof(layout) atIndex:4];
    [encode dispatchThreads:MTLSizeMake(layout.width,layout.height,1)
        threadsPerThreadgroup:MTLSizeMake(group,1,1)];
    [encode endEncoding];
    return true;
}

struct FusedDepthLayout {
    std::uint32_t width, height, first_guest_row, row_count;
    std::uint32_t depth_stride, stencil_stride, begin, end;
};

id<MTLComputePipelineState> FusedDepthPipeline(id<MTLDevice> device) {
    static const bool enabled = [] {
        const char* option = std::getenv("CARA_METAL_FUSE_JOINED_DEPTH_EXPORT");
        return option && std::string_view(option) == "1";
    }();
    if (!enabled)
        return nil;

    struct Cache {
        id<MTLDevice> owner;
        id<MTLComputePipelineState> pipeline = nil;
    };
    thread_local Cache cache{};
    if (cache.owner != device) {
        cache = {device};
        const auto url = [NSBundle.mainBundle URLForResource:@"cara-pica"
                                               withExtension:@"metallib"];
        const auto lib = url ? [device newLibraryWithURL:url error:nullptr] : nil;
        const auto fn = [lib newFunctionWithName:@"ExportCheckedPicaDepth"];
        if (fn) {
            const auto pipeline = [device newComputePipelineStateWithFunction:fn error:nullptr];
            if (pipeline.threadExecutionWidth != 0 &&
                pipeline.threadExecutionWidth <= pipeline.maxTotalThreadsPerThreadgroup)
                cache.pipeline = pipeline;
        }
        if (cache.pipeline)
            LOG_INFO(Render_OpenGL, "Metal fused joined depth export enabled");
    }
    return cache.pipeline;
}

bool EncodeFusedDepth(id<MTLCommandBuffer> commands, id<MTLComputePipelineState> pipeline,
                      id<MTLBuffer> requested, id<MTLBuffer> depth, id<MTLBuffer> stencil,
                      id<MTLBuffer> errors, const FusedDepthLayout& layout) {
    const auto encoder = [commands computeCommandEncoder];
    if (!encoder)
        return false;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:depth offset:0 atIndex:0];
    [encoder setBuffer:stencil offset:0 atIndex:1];
    [encoder setBuffer:requested offset:0 atIndex:2];
    [encoder setBuffer:errors offset:0 atIndex:3];
    [encoder setBytes:&layout length:sizeof(layout) atIndex:4];
    [encoder dispatchThreads:MTLSizeMake(layout.width * layout.row_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding];
    return true;
}

// Resolve on the owner lane once per device. A device without the function
// retains the certified buffer export.
id<MTLComputePipelineState> DirectColorPipeline(id<MTLDevice> device) {
    struct Cache {
        id<MTLDevice> owner;
        id<MTLComputePipelineState> pipeline = nil;
    };
    thread_local std::unique_ptr<Cache> cache;
    if (!cache || cache->owner != device) {
        cache = std::make_unique<Cache>(Cache{device});
        const auto path = [NSBundle.mainBundle URLForResource:@"cara-pica" withExtension:@"metallib"];
        const auto library = path ? [device newLibraryWithURL:path error:nullptr] : nil;
        const auto function = [library newFunctionWithName:@"ExportPicaTiledRGBA8"];
        if (function)
            cache->pipeline = [device newComputePipelineStateWithFunction:function error:nullptr];
    }
    return cache->pipeline;
}

id<MTLBuffer> EncodeDirectTiledColor(id<MTLCommandBuffer> command, id<MTLTexture> texture,
                                   MetalRgba8Layout layout, PicaGuestRange requested) {
    if (!NativeMetalCost::DirectTiledColorExportEnabled() || texture.framebufferOnly ||
        !(texture.usage & MTLTextureUsageShaderRead) ||
        texture.storageMode == MTLStorageModeMemoryless)
        return nil;
    const auto pipeline = DirectColorPipeline(command.device);
    if (!pipeline || !pipeline.threadExecutionWidth ||
        pipeline.threadExecutionWidth > pipeline.maxTotalThreadsPerThreadgroup)
        return nil;

    const auto begin = static_cast<std::uint32_t>(requested.begin - layout.begin);
    const auto end = static_cast<std::uint32_t>(requested.end - layout.begin);
    const std::uint32_t args[] = {layout.width, layout.height, 0, layout.width / 8, begin, end};
    const auto buffer = [command.device newBufferWithLength:end - begin options:cpu_scratch];
    const auto encoder = buffer ? [command computeCommandEncoder] : nil;
    if (!encoder)
        throw std::runtime_error("Could not encode direct tiled Metal color export");
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder setBytes:args length:sizeof(args) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake((end - 1) / 4 - begin / 4 + 1, 1, 1)
         threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding];
    NativeMetalCost::Count(NativeMetalCost::DirectTiledColorExport, 1);
    return buffer;
}

} // namespace

static id<MTLBuffer> EncodeQueuedTiledColor(id<MTLCommandBuffer> command,
                                           id<MTLComputePipelineState> tiled_pack,
                                           id<MTLTexture> texture, PicaGuestRange surface,
                                           PicaGuestRange requested, unsigned pixel_bytes) {
    const auto width = texture.width;
    const auto height = texture.height;
    if (!command || !command.retainedReferences ||
        command.status != MTLCommandBufferStatusNotEnqueued ||
        !texture || !tiled_pack || texture.device != command.device ||
        tiled_pack.device != command.device || texture.textureType != MTLTextureType2D ||
        texture.sampleCount != 1 || texture.mipmapLevelCount != 1 ||
        texture.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        !width || !height || width > max_extent || height > max_extent ||
        (width | height) % 8 || surface.begin >= surface.end ||
        surface.end - surface.begin != width * height * pixel_bytes ||
        requested.begin < surface.begin || requested.end > surface.end ||
        requested.begin >= requested.end)
        return nil;

    if (pixel_bytes == 4) {
        const MetalRgba8Layout layout{static_cast<PAddr>(surface.begin), static_cast<PAddr>(surface.end),
                                      static_cast<u32>(width), static_cast<u32>(height), true};
        if (const auto direct = EncodeDirectTiledColor(command, texture, layout, requested))
            return direct;
    }

    const auto pitch = Pitch(width * pixel_bytes);
    const NSUInteger per_row = width / 8;
    const NSUInteger tile_bytes = 64 * pixel_bytes;
    const NSUInteger first_tile = (requested.begin - surface.begin) / tile_bytes;
    const NSUInteger last_tile = (requested.end - 1 - surface.begin) / tile_bytes;
    const NSUInteger first_row = 8 * (first_tile / per_row);
    const NSUInteger end_row = 8 * (1 + last_tile / per_row);
    const auto device = command.device;
    const auto canonical = [device newBufferWithLength:pitch * height options:gpu_scratch];
    const auto requested_bytes = [device newBufferWithLength:requested.end - requested.begin
                                                   options:cpu_scratch];
    if (!canonical || !requested_bytes)
        throw std::runtime_error("Could not allocate queued Metal color export");
    const auto blit = [command blitCommandEncoder];
    if (!blit)
        throw std::runtime_error("Could not start queued Metal color export");
    CopyRows(blit, texture, canonical, pitch, height - end_row, end_row - first_row,
             MTLBlitOptionNone);
    [blit endEncoding];
    if (!EncodePicaTiledExport(command, tiled_pack, canonical, requested_bytes,
                               width, height, pitch, surface, requested, pixel_bytes))
        throw std::runtime_error("Could not pack queued Metal color export");
    return requested_bytes;
}

id<MTLBuffer> EncodeQueuedColorExport(id<MTLCommandBuffer> command,
                                     id<MTLComputePipelineState> tiled_pack,
                                     id<MTLTexture> texture, MetalRgba8Layout layout,
                                     PicaGuestRange requested) {
    if (!layout.tiled || texture.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        texture.width != layout.width || texture.height != layout.height)
        return nil;
    return EncodeQueuedTiledColor(command, tiled_pack, texture, {layout.begin, layout.end},
                                  requested, 4);
}

id<MTLBuffer> EncodeQueuedPackedColorExport(id<MTLCommandBuffer> command,
                                           id<MTLComputePipelineState> tiled_pack,
                                           id<MTLTexture> texture, PicaGuestRange surface,
                                           PicaGuestRange requested) {
    const auto format = texture.pixelFormat;
    if (format != MTLPixelFormatB5G6R5Unorm && format != MTLPixelFormatABGR4Unorm &&
        format != MTLPixelFormatA1BGR5Unorm)
        return nil;
    return EncodeQueuedTiledColor(command, tiled_pack, texture, surface, requested, 2);
}

id<MTLBuffer> EncodeQueuedCheckedDepthExport(
    id<MTLCommandBuffer> command, id<MTLTexture> texture,
    PicaGuestRange physical_surface, PicaGuestRange requested,
    id<MTLComputePipelineState> tiled_pack, id<MTLComputePipelineState> depth_pack,
    id<MTLBuffer> errors) {
    const auto width = texture.width, height = texture.height;
    const auto device = command.device;
    if (!command || command.status != MTLCommandBufferStatusNotEnqueued ||
        !command.retainedReferences || !texture || texture.device != device ||
        texture.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
        texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
        texture.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        texture.storageMode == MTLStorageModeMemoryless || !width || !height ||
        width > max_extent || height > max_extent || (width | height) % 8 ||
        !errors || errors.device != device || errors.length < 4 ||
        !tiled_pack || tiled_pack.device != device || !depth_pack || depth_pack.device != device ||
        physical_surface.begin >= physical_surface.end ||
        physical_surface.end - physical_surface.begin != width * height * 4 ||
        requested.begin < physical_surface.begin || requested.end > physical_surface.end ||
        requested.begin >= requested.end)
        return nil;

    constexpr NSUInteger tile_bytes = 64 * 4;
    const auto first_tile = (requested.begin - physical_surface.begin) / tile_bytes;
    const auto last_tile = (requested.end - 1 - physical_surface.begin) / tile_bytes;
    const auto per_row = width / 8;
    const auto guest_first_row = 8 * (first_tile / per_row);
    const auto guest_end_row = 8 * (1 + last_tile / per_row);
    const auto y = height - guest_end_row;
    const auto rows = guest_end_row - guest_first_row;
    const auto depth_pitch = Pitch(width * 4), stencil_pitch = Pitch(width);
    const auto fused = FusedDepthPipeline(device);
    const auto canonical = fused ? nil :
        [device newBufferWithLength:depth_pitch*height options:gpu_scratch];
    const auto depth = [device newBufferWithLength:depth_pitch*height options:gpu_scratch];
    const auto stencil = [device newBufferWithLength:stencil_pitch*height options:gpu_scratch];
    const auto requested_bytes = [device newBufferWithLength:requested.end-requested.begin
                                                   options:cpu_scratch];
    if ((!fused && !canonical) || !depth || !stencil || !requested_bytes)
        throw std::runtime_error("Could not allocate queued checked depth export");
    const auto blit = [command blitCommandEncoder];
    if (!blit)
        throw std::runtime_error("Could not start queued checked depth export");
    CopyRows(blit, texture, depth, depth_pitch, y, rows, MTLBlitOptionDepthFromDepthStencil);
    CopyRows(blit, texture, stencil, stencil_pitch, y, rows, MTLBlitOptionStencilFromDepthStencil);
    [blit endEncoding];
    if (fused) {
        const FusedDepthLayout layout{static_cast<u32>(width), static_cast<u32>(height),
            static_cast<u32>(guest_first_row), static_cast<u32>(rows),
            static_cast<u32>(depth_pitch/4), static_cast<u32>(stencil_pitch),
            static_cast<u32>(requested.begin-physical_surface.begin),
            static_cast<u32>(requested.end-physical_surface.begin)};
        if (!EncodeFusedDepth(command, fused, requested_bytes, depth, stencil, errors, layout))
            throw std::runtime_error("Could not encode fused joined depth export");
        return requested_bytes;
    }
    const DepthLayout layout{static_cast<u32>(width), static_cast<u32>(rows), 0,
        static_cast<u32>(y), static_cast<u32>(depth_pitch/4), static_cast<u32>(depth_pitch/4),
        static_cast<u32>(stencil_pitch), 24};
    if (!PackDepthRows(command, depth_pack, canonical, depth, stencil, errors, layout) ||
        !EncodePicaTiledExport(command, tiled_pack, canonical, requested_bytes,
            width, height, depth_pitch, physical_surface, requested, 4))
        throw std::runtime_error("Could not encode queued checked depth export");
    return requested_bytes;
}

MetalGuestExport MakePicaTiledVisibilityExporter(
    id<MTLTexture> texture, std::uint64_t backing_generation, PicaGuestRange physical_surface,
    PicaExportFormat format, id<MTLComputePipelineState> tiled_pack,
    id<MTLComputePipelineState> depth24_pack) {
    const bool depth = format == PicaExportFormat::Depth24Stencil8;
    const bool rgb565 = format == PicaExportFormat::Rgb565;
    const bool rgba4 = format == PicaExportFormat::Rgba4;
    const bool rgb5a1 = format == PicaExportFormat::Rgb5A1;
    const unsigned pixel_bytes = rgb565 || rgba4 || rgb5a1 ? 2 : 4;
    auto expected = MTLPixelFormatRGBA8Unorm;
    if (depth)
        expected = MTLPixelFormatDepth32Float_Stencil8;
    else if (rgb565)
        expected = MTLPixelFormatB5G6R5Unorm;
    else if (rgba4)
        expected = MTLPixelFormatABGR4Unorm;
    else if (rgb5a1)
        expected = MTLPixelFormatA1BGR5Unorm;
    if (!texture || !backing_generation || !tiled_pack ||
        (format != PicaExportFormat::Rgba8 && !depth && !rgb565 && !rgba4 && !rgb5a1) ||
        texture.pixelFormat != expected || texture.textureType != MTLTextureType2D ||
        texture.sampleCount != 1 || texture.width == 0 || texture.height == 0 ||
        texture.width > max_extent || texture.height > max_extent ||
        texture.width % 8 || texture.height % 8 ||
        texture.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        tiled_pack.device != texture.device || (depth && depth24_pack.device != texture.device) ||
        physical_surface.begin >= physical_surface.end ||
        physical_surface.end > (std::uint64_t{1} << 32) ||
        physical_surface.end - physical_surface.begin != texture.width * texture.height * pixel_bytes) {
        return {};
    }

    return [texture,backing_generation,physical_surface,tiled_pack,depth24_pack,depth,pixel_bytes](
        id<MTLCommandQueue> queue, id<MTLResource> resource, u64 generation,
        PAddr begin, PAddr end, std::span<u8> output) {
        if (!queue || queue.device != texture.device || resource != texture ||
            generation != backing_generation || begin >= end ||
            begin < physical_surface.begin || end > physical_surface.end ||
            output.size() != end-begin) {
            return false;
        }

        const auto width = texture.width, height = texture.height;
        const auto per_row = width / 8;
        const NSUInteger tile_bytes = 64 * pixel_bytes;
        const NSUInteger first_tile = (begin - physical_surface.begin) / tile_bytes;
        const NSUInteger last_tile = (end - 1 - physical_surface.begin) / tile_bytes;
        const NSUInteger guest_first_row = 8 * (first_tile / per_row);
        const NSUInteger guest_end_row = 8 * (1 + last_tile / per_row);
        const NSUInteger y = height - guest_end_row;
        const NSUInteger rows = guest_end_row - guest_first_row;
        const auto color_pitch = Pitch(width * pixel_bytes), stencil_pitch = Pitch(width);

        id<MTLDevice> device=queue.device;
        id<MTLBuffer> canonical=[device newBufferWithLength:color_pitch*height options:gpu_scratch];
        id<MTLBuffer> requested=[device newBufferWithLength:output.size() options:cpu_scratch];
        id<MTLBuffer> depth_values=nil, stencil=nil, errors=nil;
        if (depth) {
            depth_values=[device newBufferWithLength:color_pitch*height options:gpu_scratch];
            stencil=[device newBufferWithLength:stencil_pitch*height options:gpu_scratch];
            errors=[device newBufferWithLength:sizeof(std::uint32_t) options:cpu_scratch];
        }
        id<MTLCommandBuffer> commands=[queue commandBuffer];
        if (!commands || !canonical || !requested || (depth && (!depth_values || !stencil || !errors))) {
            return false;
        }
        id<MTLBlitCommandEncoder> blit=[commands blitCommandEncoder];
        if (!blit) {
            return false;
        }
        if (depth) {
            *static_cast<std::uint32_t*>(errors.contents)=0;
            CopyRows(blit,texture,depth_values,color_pitch,y,rows,MTLBlitOptionDepthFromDepthStencil);
            CopyRows(blit,texture,stencil,stencil_pitch,y,rows,MTLBlitOptionStencilFromDepthStencil);
        } else {
            CopyRows(blit,texture,canonical,color_pitch,y,rows,MTLBlitOptionNone);
        }
        [blit endEncoding];
        if (depth) {
            const DepthLayout layout{
                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(rows), 0,
                static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(color_pitch/4),
                static_cast<std::uint32_t>(color_pitch/4), static_cast<std::uint32_t>(stencil_pitch),24
            };
            if (!PackDepthRows(commands,depth24_pack,canonical,depth_values,stencil,errors,layout)) {
                return false;
            }
        }
        if (!EncodePicaTiledExport(commands,tiled_pack,canonical,requested,
                                   static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),
                                   static_cast<std::uint32_t>(color_pitch), physical_surface, {begin,end},
                                   pixel_bytes)) {
            return false;
        }

        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted ||
            (depth && *static_cast<const std::uint32_t*>(errors.contents))) {
            return false;
        }
        std::memcpy(output.data(),requested.contents,output.size());
        return true;
    };
}

} // namespace Cara
