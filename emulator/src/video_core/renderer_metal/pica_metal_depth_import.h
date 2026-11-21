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
// then check it after completion before CPU consumption or successful publication.
// Ordered GPU consumers may precede that check only when a retained validation
// receipt propagates failure through every dependent writer and final boundary.
bool EncodeDepthImport(id<MTLCommandBuffer> command, const PicaMetalDepthImportPipelines& pipelines,
                       id<MTLBuffer> source, NSUInteger offset, id<MTLTexture> texture,
                       id<MTLBuffer> errors, const PicaGuestTilePatch* patch = nullptr,
                       bool has_stencil = true);

} // namespace Cara
