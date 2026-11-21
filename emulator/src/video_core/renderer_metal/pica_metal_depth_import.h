// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_tile_merge.h"

namespace Cara {

struct PicaMetalDepthImportPipelines {
    id<MTLComputePipelineState> detile_depth;
    id<MTLComputePipelineState> unpack_depth;
    id<MTLComputePipelineState> detile_depth_rect = nil;
};

// The caller owns the input and destination. Initialize errors before encode,
// then check it after the command completes and before using the texture.
bool EncodeDepthImport(id<MTLCommandBuffer> command, const PicaMetalDepthImportPipelines& pipelines,
                       id<MTLBuffer> source, NSUInteger offset, id<MTLTexture> texture,
                       id<MTLBuffer> errors, const PicaGuestTilePatch* patch = nullptr);

} // namespace Cara
