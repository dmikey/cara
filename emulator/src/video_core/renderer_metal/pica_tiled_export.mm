// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_tiled_export.h"

#include <limits>

namespace Cara {
namespace {

struct Layout {
    std::uint32_t width, height, source_row, tiles_per_row, begin, end;
};

static_assert(sizeof(Layout) == 24);

} // namespace

bool EncodePicaTiledExport(id<MTLCommandBuffer> command,
                           id<MTLComputePipelineState> pipeline,
                           id<MTLBuffer> canonical, id<MTLBuffer> requested_scratch,
                           std::uint32_t width, std::uint32_t height, std::uint32_t source_row,
                           PicaGuestRange surface, PicaGuestRange requested, unsigned pixel_bytes) {
    if (!command || command.status != MTLCommandBufferStatusNotEnqueued ||
        !pipeline || !canonical || !requested_scratch || canonical == requested_scratch ||
        canonical.heap || requested_scratch.heap ||
        canonical.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        requested_scratch.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        requested_scratch.storageMode != MTLStorageModeShared ||
        pipeline.device != command.device || canonical.device != command.device ||
        requested_scratch.device != command.device || !width || !height ||
        width % 8 || height % 8 || (pixel_bytes != 2 && pixel_bytes != 4) ||
        std::uint64_t{width} * pixel_bytes > source_row) {
        return false;
    }
    // The earlier row-pitch check bounds width to allow the first product.
    const std::uint64_t surface_bytes = std::uint64_t{width} * pixel_bytes * height;
    if (surface_bytes > std::numeric_limits<std::uint32_t>::max() ||
        surface.begin >= surface.end || surface.end - surface.begin != surface_bytes ||
        surface.end > (std::uint64_t{1} << 32) ||
        requested.begin < surface.begin || requested.end > surface.end ||
        requested.begin >= requested.end ||
        std::uint64_t{source_row} * height > canonical.length ||
        requested.end - requested.begin > requested_scratch.length ||
        !pipeline.threadExecutionWidth ||
        pipeline.threadExecutionWidth > pipeline.maxTotalThreadsPerThreadgroup) {
        return false;
    }
    const Layout layout{width,height,source_row,width / 8,
                        static_cast<std::uint32_t>(requested.begin - surface.begin),
                        static_cast<std::uint32_t>(requested.end - surface.begin)};
    const NSUInteger pixels = (layout.end - 1u) / pixel_bytes - layout.begin / pixel_bytes + 1u;
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) {
        return false;
    }
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:canonical offset:0 atIndex:0];
    [encoder setBuffer:requested_scratch offset:0 atIndex:1];
    [encoder setBytes:&layout length:sizeof(layout) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(pixels, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding];
    return true;
}

bool EncodePicaPackedColorImport(id<MTLCommandBuffer> command,
                           id<MTLComputePipelineState> detile,
                           id<MTLBuffer> source, NSUInteger offset, NSUInteger source_bytes,
                           id<MTLTexture> texture) {
    if (!command || !detile || !source || !texture ||
        command.status != MTLCommandBufferStatusNotEnqueued || !command.retainedReferences ||
        command.device != detile.device || source.device != command.device ||
        texture.device != command.device ||
        (texture.pixelFormat != MTLPixelFormatB5G6R5Unorm &&
         texture.pixelFormat != MTLPixelFormatABGR4Unorm &&
         texture.pixelFormat != MTLPixelFormatA1BGR5Unorm) ||
        texture.textureType != MTLTextureType2D || texture.mipmapLevelCount != 1 ||
        texture.sampleCount != 1 || !(texture.usage & MTLTextureUsageRenderTarget) ||
        !texture.width || !texture.height || ((texture.width | texture.height) & 7u) ||
        texture.width > 4096 || texture.height > 4096 || offset % 2 ||
        offset > source.length || source_bytes > source.length - offset ||
        source_bytes != 2 * texture.width * texture.height ||
        detile.maxTotalThreadsPerThreadgroup < 64)
        return false;

    const NSUInteger row = (2 * texture.width + 255) & ~NSUInteger{255};
    const auto packed = [command.device newBufferWithLength:row * texture.height
                           options:MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeTracked];
    const auto compute = packed ? [command computeCommandEncoder] : nil;
    if (!compute)
        return false;
    const std::uint32_t size[] = {static_cast<std::uint32_t>(texture.width),
                                  static_cast<std::uint32_t>(texture.height),
                                  static_cast<std::uint32_t>(row)};
    [compute setComputePipelineState:detile];
    [compute setBuffer:source offset:offset atIndex:0];
    [compute setBuffer:packed offset:0 atIndex:1];
    [compute setBytes:size length:sizeof(size) atIndex:2];
    const auto extent = MTLSizeMake(texture.width, texture.height, 1);
    [compute dispatchThreads:extent threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [compute endEncoding];

    const auto blit = [command blitCommandEncoder];
    if (!blit)
        return false;
    [blit copyFromBuffer:packed sourceOffset:0 sourceBytesPerRow:row
      sourceBytesPerImage:row * texture.height sourceSize:extent toTexture:texture
         destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return true;
}

} // namespace Cara
