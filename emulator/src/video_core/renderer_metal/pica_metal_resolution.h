// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <cstdint>
#include <map>

namespace Cara {

// A scaled pair is private to the rasterizer. Guest writers, transfer layouts,
// and CPU visibility always reference the independently sized native textures.
struct MetalRasterPair {
    id<MTLTexture> color = nil;
    id<MTLTexture> depth = nil;
    id<MTLTexture> stencil = nil;
};

class MetalRasterScale {
public:
    MetalRasterScale() = default;
    MetalRasterScale(id<MTLLibrary> library, unsigned factor);
    unsigned Factor() const { return factor; }
    struct Statistics {
        uint64_t resolved_groups = 0;
        uint64_t captured_transfers = 0;
        uint64_t displayed_scanouts = 0;
    };
    const Statistics& Stats() const { return stats; }
    void CountPresentation() { ++stats.displayed_scanouts; }
    bool Begin(id<MTLCommandBuffer> command, id<MTLTexture> color,
               id<MTLTexture> depth, bool backed);
    id<MTLTexture> SnapshotLinear(id<MTLCommandBuffer> command, id<MTLTexture> native,
        unsigned width, unsigned height, unsigned bytes_per_pixel, bool flip);
    id<MTLTexture> SnapshotRgb565Crop(id<MTLCommandBuffer> command, id<MTLTexture> native,
        unsigned width, unsigned height, unsigned guest_first_row, bool guest_flip, id<MTLBuffer> guest_pixels);
    bool Resolve(id<MTLCommandBuffer> command);
    const MetalRasterPair& Target() const { return target; }
    id<MTLTexture> EnhancedColor(id<MTLTexture> native) const {
        return native && source.color == native ? target.color : nil;
    }
    void InvalidateBacking(id<MTLTexture> native) {
        if (source.color == native || source.depth == native)
            Invalidate();
    }
    void Invalidate() { source = {}; }

private:
    id<MTLTexture> Snapshot(id<MTLCommandBuffer> command, id<MTLTexture> enhanced,
        unsigned width, unsigned height, unsigned bytes_per_pixel, unsigned first_row,
        bool reverse_rows, unsigned source_step, id<MTLTexture> native = nil,
        id<MTLBuffer> guest_pixels = nil);
    bool Prepare(MTLPixelFormat color_format);
    bool Copy(id<MTLCommandBuffer> command, const MetalRasterPair& from,
              const MetalRasterPair& to);
    MetalRasterPair source{}, target{};
    id<MTLLibrary> library = nil;
    std::map<MTLPixelFormat, id<MTLRenderPipelineState>> pipelines;
    id<MTLComputePipelineState> scanout = nil;
    id<MTLComputePipelineState> crop_scanout = nil;
    id<MTLDepthStencilState> writes = nil;
    unsigned factor = 1;
    bool backed_depth = true;
    Statistics stats{};
};
}
