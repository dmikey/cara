// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_resolution.h"
#include <array>

namespace Cara {
namespace {
id<MTLTexture> StencilView(id<MTLTexture> texture) {
    return [texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
}
id<MTLTexture> PrivateTarget(id<MTLDevice> device, MTLPixelFormat format,
                              NSUInteger width, NSUInteger height, bool depth) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:width height:height mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    if (depth)
        d.usage |= MTLTextureUsagePixelFormatView;
    return [device newTextureWithDescriptor:d];
}
}

MetalRasterScale::MetalRasterScale(id<MTLLibrary> library_, unsigned scale) {
    if (scale != 2 || !library_)
        return;
    library = library_;
    auto stencil = [MTLStencilDescriptor new];
    stencil.stencilCompareFunction = MTLCompareFunctionAlways;
    stencil.depthStencilPassOperation = MTLStencilOperationReplace;
    auto depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionAlways;
    depth.depthWriteEnabled = YES;
    depth.frontFaceStencil = depth.backFaceStencil = stencil;
    writes = [library.device newDepthStencilStateWithDescriptor:depth];
    auto function = [library newFunctionWithName:@"cara_scale_scanout"];
    if (function)
        scanout = [library.device newComputePipelineStateWithFunction:function error:nil];
    if (writes && scanout)
        factor = scale;
}

bool MetalRasterScale::Prepare(MTLPixelFormat format) {
    if (auto it = pipelines.find(format); it != pipelines.end())
        return it->second != nil;
    auto d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"cara_scale_vertex"];
    d.fragmentFunction = [library newFunctionWithName:@"cara_scale_fragment"];
    d.colorAttachments[0].pixelFormat = format;
    d.depthAttachmentPixelFormat = d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pipeline = nil;
    if (d.vertexFunction && d.fragmentFunction)
        pipeline = [library.device newRenderPipelineStateWithDescriptor:d error:nil];
    pipelines.emplace(format,pipeline);
    return pipeline != nil;
}

bool MetalRasterScale::Begin(id<MTLCommandBuffer> command, id<MTLTexture> color,
                             id<MTLTexture> depth, bool backed) {
    if (factor == 1 || !color || !depth || color.width != depth.width ||
        color.height != depth.height || color.width > 4096 || color.height > 4096 ||
        color.width * color.height > 1024u * 1024u ||
        depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
        !(depth.usage & MTLTextureUsagePixelFormatView) || !Prepare(color.pixelFormat)) {
        Invalidate();
        return false;
    }
    // The native owner invalidates this relationship when guest bytes change.
    if (source.color == color && source.depth == depth && backed == backed_depth)
        return true;
    if (!target.color || target.color.pixelFormat != color.pixelFormat ||
        target.color.width != factor*color.width || target.color.height != factor*color.height) {
        target.color = PrivateTarget(command.device, color.pixelFormat, color.width*factor,
                                     color.height*factor, false);
        target.depth = PrivateTarget(command.device, depth.pixelFormat, depth.width*factor,
                                     depth.height*factor, true);
        target.stencil = StencilView(target.depth);
    }
    source = {color, depth, StencilView(depth)};
    backed_depth = backed;
    if (!target.color || !target.stencil || !source.stencil || !Copy(command, source, target)) {
        Invalidate();
        return false;
    }
    return true;
}

id<MTLTexture> MetalRasterScale::SnapshotLinear(id<MTLCommandBuffer> command, id<MTLTexture> native,
    unsigned width, unsigned height, unsigned bytes_per_pixel, bool flip) {
    const auto enhanced = EnhancedColor(native);
    if (!enhanced || native.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        (bytes_per_pixel != 3 && bytes_per_pixel != 4) || native.height != height ||
        (native.width != width && native.width != 2u*width))
        return nil;
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:width*factor height:height*factor mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    auto texture = [command.device newTextureWithDescriptor:d];
    auto enc = texture ? [command computeCommandEncoder] : nil;
    if (!enc)
        return nil;
    const std::array<uint32_t,2> flags{uint32_t(flip),bytes_per_pixel};
    [enc setComputePipelineState:scanout];
    [enc setTexture:enhanced atIndex:0];
    [enc setTexture:texture atIndex:1];
    [enc setBytes:flags.data() length:sizeof(flags) atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake((d.width+7)/8,(d.height+7)/8,1)
        threadsPerThreadgroup:MTLSizeMake(8,8,1)];
    [enc endEncoding];
    ++stats.captured_transfers;
    return texture;
}

bool MetalRasterScale::Resolve(id<MTLCommandBuffer> command) {
    if (!Copy(command, target, source))
        return false;
    ++stats.resolved_groups;
    return true;
}

bool MetalRasterScale::Copy(id<MTLCommandBuffer> command, const MetalRasterPair& from,
                            const MetalRasterPair& to) {
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = to.color;
    pass.depthAttachment.texture = pass.stencilAttachment.texture = to.depth;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction =
        backed_depth ? MTLStoreActionStore : MTLStoreActionDontCare;
    auto enc = [command renderCommandEncoderWithDescriptor:pass];
    if (!enc)
        return false;
    [enc setRenderPipelineState:pipelines.at(to.color.pixelFormat)];
    [enc setDepthStencilState:writes];
    [enc setFragmentTexture:from.color atIndex:0];
    [enc setFragmentTexture:from.depth atIndex:1];
    [enc setFragmentTexture:from.stencil atIndex:2];
    std::array<uint32_t,3> layout{uint32_t(to.color.width), uint32_t(to.color.height),
                                  uint32_t(backed_depth)};
    [enc setFragmentBytes:layout.data() length:sizeof(layout) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    return true;
}
}
