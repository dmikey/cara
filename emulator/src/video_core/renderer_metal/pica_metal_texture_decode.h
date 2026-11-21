// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <array>
#include <cstdint>

namespace Cara {

class PicaMetalTextureDecoder {
public:
    explicit PicaMetalTextureDecoder(id<MTLLibrary> library) : library_(library) {}

    // Call before the render path can use this format. Encode never compiles.
    bool Prepare(uint32_t format, bool disable_alpha, NSError** error);

    // The caller supplies CPU-authoritative, immutable mip bytes that cover
    // the destination's complete guest layout. An individual mip is bound
    // as a 2D view. Origin and extent select which destination texels to change.
    bool Encode(id<MTLCommandBuffer> commands, id<MTLBuffer> source, NSUInteger offset,
                NSUInteger source_bytes, uint32_t tile_row_bytes, id<MTLTexture> destination,
                MTLOrigin origin, MTLSize extent, uint32_t format, bool disable_alpha) const;

private:
    static constexpr uint32_t FormatCount = 14;
    id<MTLLibrary> library_;
    std::array<id<MTLComputePipelineState>, FormatCount * 2> pipelines_{};
};

} // namespace Cara
