// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lighting_cache.h"

namespace Cara {
namespace {

id<MTLTexture> NewTexture(id<MTLDevice> device, MTLPixelFormat format, unsigned width,
                          unsigned height, MTLTextureUsage usage, MTLStorageMode storage) {
    auto* layout = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width
                                                                   height:height mipmapped:NO];
    layout.storageMode = storage;
    layout.hazardTrackingMode = MTLHazardTrackingModeTracked;
    layout.usage = usage;
    return [device newTextureWithDescriptor:layout];
}

} // namespace

PicaMetalLightingCache::PicaMetalLightingCache(id<MTLCommandQueue> queue)
    : owner_queue(queue) {}

id<MTLTexture> PicaMetalLightingCache::Resolve(id<MTLCommandQueue> queue,
                                             const PicaLightingLutSnapshot& snapshot) {
    constexpr unsigned width = 1024;
    constexpr std::size_t texel_bytes = sizeof(float) * 2;
    if (!queue || queue != owner_queue || !snapshot.generation || !snapshot.rows ||
        snapshot.rows > 512 || snapshot.bytes.size() != u64{width} * snapshot.rows * texel_bytes ||
        snapshot.generation < generation)
        return nil;
    if (snapshot.generation == generation)
        return current.height == snapshot.rows ? current : nil;

    const auto texture = NewTexture(queue.device, MTLPixelFormatRG32Float, width, snapshot.rows,
                                    MTLTextureUsageShaderRead, MTLStorageModeShared);
    if (!texture)
        return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, snapshot.rows) mipmapLevel:0
                 withBytes:snapshot.bytes.data() bytesPerRow:width * texel_bytes];
    current = texture;
    generation = snapshot.generation;
    return texture;
}

} // namespace Cara
