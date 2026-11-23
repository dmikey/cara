// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lighting_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Cara {
namespace {

bool ExactLutReuseEnabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_REUSE_EXACT_LUT_IMAGES");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool LutProfileEnabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_FRAME_COST");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

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

PicaMetalLightingCache::~PicaMetalLightingCache() {
    if (LutProfileEnabled())
        std::fprintf(stderr, "CARA_METAL_LUT_IMAGES requests=%llu hits=%llu uploaded_bytes=%llu\n",
                     static_cast<unsigned long long>(requests), static_cast<unsigned long long>(hits),
                     static_cast<unsigned long long>(uploaded_bytes));
}

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

    const bool measure = LutProfileEnabled();
    if (measure)
        ++requests;
    const bool reuse = ExactLutReuseEnabled();
    if (reuse) {
        for (std::size_t age = 1; age <= recent_images.size(); ++age) {
            const auto index = (next_image + recent_images.size() - age) % recent_images.size();
            const auto& image = recent_images[index];
            if (image.bytes.size() == snapshot.bytes.size() &&
                std::memcmp(image.bytes.data(), snapshot.bytes.data(), snapshot.bytes.size()) == 0) {
                current = image.texture;
                generation = snapshot.generation;
                if (measure)
                    ++hits;
                return current;
            }
        }
    }

    const auto texture = NewTexture(queue.device, MTLPixelFormatRG32Float, width, snapshot.rows,
                                    MTLTextureUsageShaderRead, MTLStorageModeShared);
    if (!texture)
        return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, snapshot.rows) mipmapLevel:0
                 withBytes:snapshot.bytes.data() bytesPerRow:width * texel_bytes];
    if (reuse) {
        auto& image = recent_images[next_image];
        image.bytes.assign(snapshot.bytes.begin(), snapshot.bytes.end());
        image.texture = texture;
        next_image = (next_image + 1) % recent_images.size();
    }
    if (measure)
        uploaded_bytes += snapshot.bytes.size();
    current = texture;
    generation = snapshot.generation;
    return texture;
}

} // namespace Cara
