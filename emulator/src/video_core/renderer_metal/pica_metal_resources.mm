// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_resources.h"

#import <Foundation/Foundation.h>

namespace Cara {
namespace {

void MissingResource(NSError** error, NSString* description) {
    if (error)
        *error = [NSError errorWithDomain:@"org.cara.pica.metal" code:1
                                 userInfo:@{NSLocalizedDescriptionKey: description}];
}

id<MTLComputePipelineState> Prepare(id<MTLLibrary> library, NSString* name, NSError** error) {
    const auto function = [library newFunctionWithName:name];
    if (!function) {
        MissingResource(error, [@"Missing precompiled PICA Metal function " stringByAppendingString:name]);
        return nil;
    }
    return [library.device newComputePipelineStateWithFunction:function error:error];
}

} // namespace

std::optional<PicaMetalResources> LoadPicaMetalResources(
    id<MTLDevice> device, NSURL* library_path, MTLPixelFormat drawable_format, NSError** error) {
    if (!device || !device.hasUnifiedMemory || !library_path.isFileURL) {
        MissingResource(error, @"Metal PICA requires an Apple device and a bundled library URL");
        return std::nullopt;
    }

    const auto library = [device newLibraryWithURL:library_path error:error];
    if (!library)
        return std::nullopt;
    PicaMetalResources resources{};
    resources.queue = [device newCommandQueue];
    if (!resources.queue) {
        MissingResource(error, @"Could not create the Metal PICA command queue");
        return std::nullopt;
    }

    auto decoder = std::make_shared<PicaMetalTextureDecoder>(library);
    for (u32 format = 0; format < 14; ++format) {
        if (!decoder->Prepare(format, false, error))
            return std::nullopt;
    }
    resources.draw.textures = std::move(decoder);
    struct NamedCompute {
        id<MTLComputePipelineState> __strong* destination;
        NSString* function;
    };
    const NamedCompute required[] = {
        {&resources.draw.detile_depth, @"DetilePicaDepth24Stencil8"},
        {&resources.draw.detile_depth24, @"DetilePicaDepth24"},
        {&resources.draw.tiled_depth24_export, @"ExportPicaTiledDepth24"},
        {&resources.draw.detile_depth_rect, @"DetilePicaDepth24Stencil8Rect"},
        {&resources.draw.mixed_alias, @"cara_pica_mixed_linear_alias"},
        {&resources.draw.unpack_depth, @"unpack_pica_depth"},
        {&resources.draw.pack_depth, @"pack_pica_depth"},
        {&resources.draw.tiled_export, @"ExportPicaTiled32"},
        // These kernels preserve the packed RGB565, RGBA4, and RGB5A1 bytes.
        {&resources.draw.detile_packed_color, @"DetilePicaRgb565"},
        {&resources.draw.tiled_packed_color_export, @"ExportPicaTiledRGB565"},
        {&resources.transfer.copy_rows, @"cara_pica_copy_rows"},
        {&resources.transfer.fill_words, @"cara_pica_fill_words"},
        {&resources.transfer.linear_color, @"cara_pica_display_to_linear_color"},
    };
    for (const auto& shader : required) {
        *shader.destination = Prepare(library, shader.function, error);
        if (!*shader.destination)
            return std::nullopt;
    }

    const auto lcd = MetalLCD::PreparePipelines(library, drawable_format, error);
    if (!lcd)
        return std::nullopt;
    resources.lcd = *lcd;
    return resources;
}

} // namespace Cara
