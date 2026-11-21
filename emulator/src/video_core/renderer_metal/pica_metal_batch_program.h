// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <cstdint>

namespace Cara {
constexpr NSUInteger BatchedMetalVertexStride = 128;
struct BatchedMetalVertexProgram {
    id<MTLComputePipelineState> compute;
    id<MTLArgumentEncoder> arguments;
    id<MTLRenderPipelineState> raster;
    std::uint8_t resources;
    bool gathers_unique_vertices;
};
} // namespace Cara
