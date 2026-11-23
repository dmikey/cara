// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <array>
#include <span>
#include <vector>
#include "common/common_types.h"

namespace Cara {

struct PicaLightingLutSnapshot {
    // Source must be the renderer's immutable logical RG32F bytes for
    // this draw's actual LUT generation, including any padding it can read.
    std::span<const u8> bytes;
    unsigned rows = 0;
    u64 generation = 0;
};

// Owned by one rasterizer on one command queue. Never mutate a texture
// already referenced by a recorded draw. Generation identities are issued
// by the same cache that supplies the immutable logical table bytes.
class PicaMetalLightingCache {
public:
    explicit PicaMetalLightingCache(id<MTLCommandQueue> queue);
    ~PicaMetalLightingCache();
    id<MTLTexture> Resolve(id<MTLCommandQueue> queue, const PicaLightingLutSnapshot& snapshot);

private:
    const id<MTLCommandQueue> owner_queue;
    id<MTLTexture> current = nil;
    u64 generation = 0;

    // Cached textures are immutable. Recorded commands keep the textures
    // they consume alive even after the cache drops its own reference.
    struct RetainedImage {
        id<MTLTexture> texture = nil;
        std::vector<u8> bytes;
    };
    std::array<RetainedImage, 4> recent_images;
    std::size_t next_image = 0;
    u64 requests = 0;
    u64 hits = 0;
    u64 uploaded_bytes = 0;
};

} // namespace Cara
