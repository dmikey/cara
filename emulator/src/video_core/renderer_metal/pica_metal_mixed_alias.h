// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>

#include "video_core/renderer_metal/pica_metal_mixed_alias_plan.h"

namespace Cara::MetalTransfer {

// Input textures hold logical RGBA8 components. Linear row zero is the
// first guest row. The result follows Cara's tiled RGBA8 sampling convention.
bool EncodeMixedAlias(id<MTLCommandBuffer> command, id<MTLComputePipelineState> pipeline,
                      const MixedAliasPlan& plan, id<MTLTexture> rgb,
                      id<MTLTexture> rgba, id<MTLTexture> result);

} // namespace Cara::MetalTransfer
