// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lcd_native.h"

namespace Cara::MetalLCD {
namespace {

struct ScanoutArgs {
    u32 width, height, stride, format;
    u64 offset;
};
static_assert(sizeof(ScanoutArgs) == 24);

struct LcdRect {
    float x, y, width, height;
};
static_assert(sizeof(LcdRect) == 16);

bool ValidRect(const Common::Rectangle<u32>& rectangle, const Layout::FramebufferLayout& layout) {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top &&
           rectangle.right <= layout.width && rectangle.bottom <= layout.height;
}

} // namespace

id<MTLTexture> EncodeLinearScanout(id<MTLCommandBuffer> commands, const Pipelines& pipelines,
                             const Scanout& scanout, const MetalTransfer::ByteView& view) {
    const auto device = commands.device;
    const auto range = scanout.rows.footprint;
    const auto plan = PlanScanout(range.begin, scanout.width, scanout.height,
                                  scanout.rows.stride_bytes, static_cast<u32>(scanout.format));
    if (!device || commands.status != MTLCommandBufferStatusNotEnqueued ||
        pipelines.decode.device != device || !plan || plan->rows.footprint.end != range.end ||
        plan->rows.count != scanout.rows.count ||
        plan->rows.width_bytes != scanout.rows.width_bytes ||
        view.buffer.device != device || !view.guest.Contains(range) ||
        view.buffer_offset > view.buffer.length ||
        view.guest.Size() > view.buffer.length - view.buffer_offset)
        return nil;

    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                           width:scanout.width
                                                                          height:scanout.height
                                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    descriptor.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    const auto texture = [commands.device newTextureWithDescriptor:descriptor];
    if (!texture || pipelines.decode.maxTotalThreadsPerThreadgroup < 64)
        return nil;

    const auto encoder = [commands computeCommandEncoder];
    if (!encoder)
        return nil;

    const ScanoutArgs args{scanout.width, scanout.height, scanout.rows.stride_bytes,
                           static_cast<u32>(scanout.format),
                           view.buffer_offset + (scanout.rows.footprint.begin - view.guest.begin)};
    [encoder setComputePipelineState:pipelines.decode];
    [encoder setBuffer:view.buffer offset:0 atIndex:0];
    [encoder setBytes:&args length:sizeof(args) atIndex:1];
    [encoder setTexture:texture atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake((scanout.width + 7) / 8, (scanout.height + 7) / 8, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
    return texture;
}

bool EncodeComposition(id<MTLCommandBuffer> commands, id<MTLTexture> destination,
             const Layout::FramebufferLayout& layout, const Pipelines& pipelines, Filter filter,
             EdgeMode edge, const std::array<std::optional<Screen>, 2>& screens) {
    const auto device = commands.device;
    const auto rectangles = std::array{layout.top_screen, layout.bottom_screen};
    const auto enabled = std::array{layout.top_screen_enabled, layout.bottom_screen_enabled};
    if (!device || commands.status != MTLCommandBufferStatusNotEnqueued ||
        destination.device != device || destination.width != layout.width ||
        destination.height != layout.height || destination.pixelFormat != pipelines.output_format ||
        pipelines.sample.device != device || pipelines.solid.device != device)
        return false;

    const bool border = edge == EdgeMode::TransparentBlackBorder;
    const auto sampler = filter == Filter::Linear
        ? (border ? pipelines.linear_border : pipelines.linear)
        : (border ? pipelines.nearest_border : pipelines.nearest);
    if (sampler.device != device)
        return false;

    for (unsigned i = 0; i < screens.size(); ++i) {
        if (enabled[i] != screens[i].has_value() || (enabled[i] && !ValidRect(rectangles[i], layout)))
            return false;
        if (screens[i] && !screens[i]->solid) {
            const auto texture = screens[i]->texture;
            if (texture.device != device || texture == destination ||
                texture.pixelFormat != MTLPixelFormatRGBA8Unorm ||
                texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
                !(texture.usage & MTLTextureUsageShaderRead))
                return false;
        }
    }

    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    const auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder)
        return false;

    for (unsigned i = 0; i < screens.size(); ++i) {
        if (!screens[i])
            continue;
        const auto& screen = *screens[i];
        const auto& r = rectangles[i];
        const auto width = r.right - r.left;
        const auto height = r.bottom - r.top;
        [encoder setViewport:MTLViewport{double(r.left), double(r.top), double(width),
                                         double(height), 0, 1}];
        [encoder setScissorRect:MTLScissorRect{r.left, r.top, width, height}];
        if (screen.solid) {
            [encoder setRenderPipelineState:pipelines.solid];
            [encoder setFragmentBytes:screen.color.data() length:sizeof(screen.color) atIndex:0];
        } else {
            const LcdRect rect{float(r.left), float(r.top), float(width), float(height)};
            [encoder setRenderPipelineState:pipelines.sample];
            [encoder setFragmentBytes:&rect length:sizeof(rect) atIndex:0];
            [encoder setFragmentTexture:screen.texture atIndex:0];
            [encoder setFragmentSamplerState:sampler atIndex:0];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    return true;
}

std::optional<Pipelines> PreparePipelines(id<MTLLibrary> library, MTLPixelFormat output_format,
                                          NSError** error) {
    if (!library || (output_format != MTLPixelFormatBGRA8Unorm &&
                     output_format != MTLPixelFormatRGBA8Unorm))
        return std::nullopt;
    const auto device = library.device;
    const auto compute = [library newFunctionWithName:@"DecodePicaLinearLCD"];
    const auto vertex = [library newFunctionWithName:@"PicaLcdVertex"];
    const auto texture = [library newFunctionWithName:@"PicaLcdSample"];
    const auto solid = [library newFunctionWithName:@"PicaLcdSolid"];
    if (!compute || !vertex || !texture || !solid)
        return std::nullopt;
    const auto decode = [device newComputePipelineStateWithFunction:compute error:error];
    if (!decode)
        return std::nullopt;

    auto* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = texture;
    descriptor.colorAttachments[0].pixelFormat = output_format;
    const auto sample = [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    if (!sample)
        return std::nullopt;
    descriptor.fragmentFunction = solid;
    const auto fill = [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    if (!fill)
        return std::nullopt;

    auto* sampler = [MTLSamplerDescriptor new];
    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.minFilter = MTLSamplerMinMagFilterNearest;
    sampler.magFilter = MTLSamplerMinMagFilterNearest;
    const auto nearest = [device newSamplerStateWithDescriptor:sampler];
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    const auto linear = [device newSamplerStateWithDescriptor:sampler];
    sampler.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
    sampler.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
    sampler.borderColor = MTLSamplerBorderColorTransparentBlack;
    const auto linear_border = [device newSamplerStateWithDescriptor:sampler];
    sampler.minFilter = MTLSamplerMinMagFilterNearest;
    sampler.magFilter = MTLSamplerMinMagFilterNearest;
    const auto nearest_border = [device newSamplerStateWithDescriptor:sampler];
    if (!nearest || !linear || !nearest_border || !linear_border)
        return std::nullopt;

    const auto expand = [library newFunctionWithName:@"ExpandPicaLCD"];
    if (!expand)
        return std::nullopt;
    const auto upscale = [device newComputePipelineStateWithFunction:expand error:error];
    if (!upscale)
        return std::nullopt;
    return Pipelines{decode, sample, fill, nearest, linear, nearest_border, linear_border,
                     output_format, upscale};
}

} // namespace Cara::MetalLCD
