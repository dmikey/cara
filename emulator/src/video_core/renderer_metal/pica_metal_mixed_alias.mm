// SPDX-License-Identifier: GPL-2.0-or-later
#import "video_core/renderer_metal/pica_metal_mixed_alias.h"

namespace Cara::MetalTransfer {
namespace {

struct AliasUniforms {
    std::uint32_t rgb_base;
    std::uint32_t rgba_base;
    std::uint32_t target_base;
    std::uint32_t split;
    std::uint32_t rgb_width;
    std::uint32_t rgba_width;
};
static_assert(sizeof(AliasUniforms) == 24);

bool ValidTexture(id<MTLTexture> texture, id<MTLDevice> device, AliasLayout layout,
                  MTLTextureUsage usage) {
    return texture && texture.device == device && texture.pixelFormat == MTLPixelFormatRGBA8Unorm &&
           texture.textureType == MTLTextureType2D && texture.width == layout.width &&
           texture.height == layout.height && texture.sampleCount == 1 &&
           texture.mipmapLevelCount == 1 && texture.arrayLength == 1 &&
           texture.hazardTrackingMode == MTLHazardTrackingModeTracked &&
           texture.storageMode != MTLStorageModeMemoryless && !texture.heap &&
           !texture.parentTexture && !texture.buffer && !texture.iosurface &&
           !texture.framebufferOnly && (texture.usage & usage) == usage;
}

} // namespace

bool EncodeMixedAlias(id<MTLCommandBuffer> command, id<MTLComputePipelineState> pipeline,
                      const MixedAliasPlan& plan, id<MTLTexture> rgb,
                      id<MTLTexture> rgba, id<MTLTexture> result) {
    constexpr unsigned GroupSize = 8;
    if (!command || !command.retainedReferences ||
        command.status > MTLCommandBufferStatusEnqueued || !pipeline ||
        pipeline.device != command.device || pipeline.maxTotalThreadsPerThreadgroup < 64 ||
        !PlanMixedAlias(plan.rgb, plan.rgba, plan.tiled, plan.split) ||
        !ValidTexture(rgb, command.device, plan.rgb, MTLTextureUsageShaderRead) ||
        !ValidTexture(rgba, command.device, plan.rgba, MTLTextureUsageShaderRead) ||
        !ValidTexture(result, command.device, plan.tiled, MTLTextureUsageShaderWrite) ||
        rgb == rgba || rgb == result || rgba == result) {
        return false;
    }

    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder)
        return false;

    const AliasUniforms arguments{plan.rgb.bytes.begin, plan.rgba.bytes.begin,
                                  plan.tiled.bytes.begin, plan.split, plan.rgb.width,
                                  plan.rgba.width};
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:rgb atIndex:0];
    [encoder setTexture:rgba atIndex:1];
    [encoder setTexture:result atIndex:2];
    [encoder setBytes:&arguments length:sizeof(arguments) atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(plan.tiled.width / GroupSize,
                                               plan.tiled.height / GroupSize, 1)
             threadsPerThreadgroup:MTLSizeMake(GroupSize, GroupSize, 1)];
    [encoder endEncoding];
    return true;
}

} // namespace Cara::MetalTransfer
