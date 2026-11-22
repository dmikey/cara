// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_metal/pica_metal_cube_plan.h"
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/texture/texture_decode.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Cara {

struct PicaMetalCubeBacking {
    id<MTLTexture> texture;
    std::array<MetalTransfer::GuestInterval, 6> faces;
};

// Each decode resolves current guest bytes on the native queue. The imported
// faces are immutable and the subsequent Metal blit retains them.
inline PicaMetalCubeBacking BuildPicaMetalCube(
    PicaMetalNativeDrawLane& lane, id<MTLCommandQueue> queue,
    const PicaMetalTextureDecoder& decoder, const MetalTransfer::CubePlan& plan,
    unsigned format, const MetalGuestAuthority::GuestReader& ram) {
    if (format > unsigned(Pica::TexturingRegs::TextureFormat::ETC1A4))
        throw std::runtime_error("Unsupported PICA cube format");
    const auto tile_bytes = Pica::Texture::CalculateTileSize(
        static_cast<Pica::TexturingRegs::TextureFormat>(format));
    using CubeSources = std::array<std::array<id<MTLTexture>, 8>, 6>;
    CubeSources sources{};
    PicaMetalCubeBacking result{nil, {}};
    struct CachedCube {
        CubeSources sources{};
        id<MTLTexture> texture = nil;
        std::uint64_t last_use = 0;
    };
    struct CubeCache {
        id<MTLCommandQueue> queue = nil;
        std::array<CachedCube, 4> entries{};
        std::uint64_t access = 0;
    };
    static thread_local CubeCache cache;
    static const bool reuse = [] {
        const auto* value = std::getenv("CARA_METAL_CACHE_CUBES");
        return !value || std::string_view(value) == "1";
    }();
    // Bound the size of the retained chains, including the six source faces.
    const bool cacheable = reuse && plan.width <= 256;
    if (cache.queue != queue)
        cache = {.queue = queue};
    for (std::size_t face = 0; face < sources.size(); ++face) {
        result.faces[face] = plan.faces[face].bytes;
        for (std::uint32_t level = 0; level < plan.levels; ++level) {
            const auto width = plan.width >> level;
            const auto row = (width / 8) * tile_bytes;
            const auto source = lane.DecodeTexture(decoder, plan.faces[face].mips[level], row,
                                                   width, width, format, false, ram);
            if (!source || source.device != queue.device || source.textureType != MTLTextureType2D ||
                source.pixelFormat != MTLPixelFormatRGBA8Unorm ||
                source.hazardTrackingMode != MTLHazardTrackingModeTracked ||
                source.width != width || source.height != width ||
                source.mipmapLevelCount != 1 || source.sampleCount != 1)
                throw std::runtime_error("Unexpected decoded PICA cube face level");
            sources[face][level] = source;
        }
    }
    // Every source still goes through the authority map. The combined
    // texture can be reused only while all decoded immutable inputs match.
    if (cacheable) {
        for (auto& entry : cache.entries) {
            if (entry.texture && entry.sources == sources) {
                entry.last_use = ++cache.access;
                result.texture = entry.texture;
                return result;
            }
        }
    }
    const auto desc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                         size:plan.width mipmapped:YES];
    desc.mipmapLevelCount = plan.levels;
    desc.storageMode = MTLStorageModePrivate;
    desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
    desc.usage = MTLTextureUsageShaderRead;
    const auto cube = [queue.device newTextureWithDescriptor:desc];
    const auto command = [queue commandBuffer];
    const auto blit = [command blitCommandEncoder];
    if (!cube || !blit)
        throw std::runtime_error("Could not allocate native PICA cube mips");
    result.texture = cube;
    for (std::size_t face = 0; face < sources.size(); ++face) {
        for (std::uint32_t level = 0; level < plan.levels; ++level) {
            const auto width = plan.width >> level;
            [blit copyFromTexture:sources[face][level] sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,width,1)
                        toTexture:cube destinationSlice:face destinationLevel:level
                 destinationOrigin:MTLOriginMake(0,0,0)];
        }
    }
    [blit endEncoding];
    lane.SubmitSampledTextureBuild(command);
    if (cacheable) {
        auto entry = std::min_element(cache.entries.begin(), cache.entries.end(),
            [](const auto& a, const auto& b) { return a.last_use < b.last_use; });
        *entry = {std::move(sources), cube, ++cache.access};
    }
    return result;
}

} // namespace Cara
