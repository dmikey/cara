// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_depth_import.h"

#include <array>
#include <limits>
#include "common/common_types.h"

namespace Cara {
namespace {

struct DepthLayout {
    u32 width, height, x, y, packed_stride, depth_stride, stencil_stride, depth_bits;
};
static_assert(sizeof(DepthLayout) == 32);

void DispatchImport(id<MTLComputeCommandEncoder> encoder, unsigned width, unsigned height) {
    [encoder dispatchThreadgroups:MTLSizeMake((width + 7) / 8, (height + 7) / 8, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

} // namespace

bool EncodeDepthImport(id<MTLCommandBuffer> command, const PicaMetalDepthImportPipelines& pipelines,
                       id<MTLBuffer> source, NSUInteger offset, id<MTLTexture> texture,
                       id<MTLBuffer> errors, const PicaGuestTilePatch* patch) {
    const auto device = command.device;
    const auto width = patch ? patch->width : static_cast<u32>(texture.width);
    const auto height = patch ? patch->height : static_cast<u32>(texture.height);
    const auto detile_pipeline = patch ? pipelines.detile_depth_rect : pipelines.detile_depth;
    const NSUInteger depth_pitch = (NSUInteger{width} * 4 + 255) & ~NSUInteger{255};
    const NSUInteger stencil_pitch = (NSUInteger{width} + 255) & ~NSUInteger{255};
    const auto max_bytes = device.maxBufferLength;
    if (!width || !height || !device || !command.retainedReferences ||
        command.status != MTLCommandBufferStatusNotEnqueued ||
        texture.device != device || texture.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
        source.device != device || errors.device != device || errors.length < sizeof(u32) ||
        offset > source.length || offset % 4 ||
        u64{width} * height > (source.length - offset) / 4 ||
        detile_pipeline.device != device || detile_pipeline.maxTotalThreadsPerThreadgroup < 64 ||
        pipelines.unpack_depth.device != device || pipelines.unpack_depth.maxTotalThreadsPerThreadgroup < 64 ||
        depth_pitch > max_bytes / height || stencil_pitch > max_bytes / height ||
        depth_pitch / 4 * height > std::numeric_limits<u32>::max() ||
        stencil_pitch * height > std::numeric_limits<u32>::max())
        return false;

    const auto packed = [device newBufferWithLength:NSUInteger{width} * height * 4
                                            options:MTLResourceStorageModePrivate];
    const auto depth = [device newBufferWithLength:depth_pitch * height
                                           options:MTLResourceStorageModePrivate];
    const auto stencil = [device newBufferWithLength:stencil_pitch * height
                                             options:MTLResourceStorageModePrivate];
    if (!packed || !depth || !stencil)
        return false;

    const auto detile = [command computeCommandEncoder];
    if (!detile)
        return false;
    const std::array<u32, 2> dimensions{width, height};
    [detile setComputePipelineState:detile_pipeline];
    [detile setBuffer:source offset:offset atIndex:0];
    [detile setBuffer:packed offset:0 atIndex:1];
    if (patch) {
        const std::array<u32, 6> rect{static_cast<u32>(texture.width), static_cast<u32>(texture.height),
                                     patch->x, patch->y, patch->width, patch->height};
        [detile setBytes:rect.data() length:sizeof(rect) atIndex:2];
    } else {
        [detile setBytes:dimensions.data() length:sizeof(dimensions) atIndex:2];
    }
    DispatchImport(detile, width, height);
    [detile endEncoding];

    const auto unpack = [command computeCommandEncoder];
    if (!unpack)
        return false;
    const DepthLayout conversion{width, height, 0, 0, width, static_cast<u32>(depth_pitch / 4),
                                 static_cast<u32>(stencil_pitch), 24};
    [unpack setComputePipelineState:pipelines.unpack_depth];
    [unpack setBuffer:packed offset:0 atIndex:0];
    [unpack setBuffer:depth offset:0 atIndex:1];
    [unpack setBuffer:stencil offset:0 atIndex:2];
    [unpack setBuffer:errors offset:0 atIndex:3];
    [unpack setBytes:&conversion length:sizeof(conversion) atIndex:4];
    DispatchImport(unpack, width, height);
    [unpack endEncoding];

    const auto blit = [command blitCommandEncoder];
    if (!blit)
        return false;
    const auto size = MTLSizeMake(width, height, 1);
    const auto origin = patch ? MTLOriginMake(patch->x, patch->y, 0) : MTLOriginMake(0, 0, 0);
    [blit copyFromBuffer:depth sourceOffset:0 sourceBytesPerRow:depth_pitch
     sourceBytesPerImage:depth_pitch * height sourceSize:size toTexture:texture
        destinationSlice:0 destinationLevel:0 destinationOrigin:origin
                 options:MTLBlitOptionDepthFromDepthStencil];
    [blit copyFromBuffer:stencil sourceOffset:0 sourceBytesPerRow:stencil_pitch
     sourceBytesPerImage:stencil_pitch * height sourceSize:size toTexture:texture
        destinationSlice:0 destinationLevel:0 destinationOrigin:origin
                 options:MTLBlitOptionStencilFromDepthStencil];
    [blit endEncoding];
    return true;
}

} // namespace Cara
