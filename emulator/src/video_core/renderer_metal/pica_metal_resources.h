// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include "video_core/renderer_metal/pica_metal_lcd_native.h"
#include "video_core/renderer_metal/pica_metal_texture_decode.h"

namespace Cara {

struct ExactPackedMetalResources {
    std::shared_ptr<const PicaMetalTextureDecoder> textures;
    id<MTLComputePipelineState> detile_depth = nil;
    id<MTLComputePipelineState> unpack_depth = nil;
    id<MTLComputePipelineState> pack_depth = nil;
    id<MTLComputePipelineState> tiled_export = nil;
    id<MTLComputePipelineState> detile_depth_rect = nil;
    id<MTLComputePipelineState> mixed_alias = nil;
    id<MTLComputePipelineState> detile_packed_color = nil;
    id<MTLComputePipelineState> tiled_packed_color_export = nil;
};

struct PicaMetalResources {
    id<MTLCommandQueue> queue = nil;
    ExactPackedMetalResources draw;
    MetalTransfer::Pipelines transfer;
    MetalLCD::Pipelines lcd;
};

// Only the frontend's explicitly selected native graphics path should call
// this. Build every required pipeline before accepting guest operation.
std::optional<PicaMetalResources> LoadPicaMetalResources(
    id<MTLDevice> device, NSURL* library_path, MTLPixelFormat drawable_format, NSError** error);

} // namespace Cara
