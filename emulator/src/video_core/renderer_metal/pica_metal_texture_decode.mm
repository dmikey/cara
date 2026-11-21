// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_texture_decode.h"
#include <limits>
#include "video_core/texture/texture_decode.h"

namespace Cara {
namespace {

struct DecodeRegion {
    uint32_t height;
    uint32_t tile_row_bytes;
    std::array<uint32_t, 2> origin;
    std::array<uint32_t, 2> size;
};
static_assert(sizeof(DecodeRegion) == 24);

} // namespace

bool PicaMetalTextureDecoder::Prepare(uint32_t format, bool disable_alpha, NSError** error) {
    if (!library_ || format >= FormatCount)
        return false;
    auto& pipeline = pipelines_[format * 2 + unsigned(disable_alpha)];
    if (pipeline)
        return true;

    auto* constants = [MTLFunctionConstantValues new];
    [constants setConstantValue:&format type:MTLDataTypeUInt atIndex:0];
    [constants setConstantValue:&disable_alpha type:MTLDataTypeBool atIndex:1];
    const auto function = [library_ newFunctionWithName:@"DecodePicaTexture"
                                        constantValues:constants error:error];
    if (!function)
        return false;

    pipeline = [library_.device newComputePipelineStateWithFunction:function error:error];
    return pipeline != nil;
}

bool PicaMetalTextureDecoder::Encode(id<MTLCommandBuffer> commands, id<MTLBuffer> source,
                                     NSUInteger offset, NSUInteger source_bytes,
                                     uint32_t tile_row_bytes, id<MTLTexture> destination,
                                     MTLOrigin origin, MTLSize extent, uint32_t format,
                                     bool disable_alpha) const {
    if (format >= FormatCount || !commands || !source || !destination ||
        commands.status != MTLCommandBufferStatusNotEnqueued ||
        commands.device != library_.device || source.device != commands.device ||
        destination.device != commands.device ||
        destination.textureType != MTLTextureType2D ||
        destination.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        !(destination.usage & MTLTextureUsageShaderWrite) || (offset & 255u) != 0 ||
        offset > source.length || source_bytes > source.length - offset) {
        return false;
    }

    const auto pipeline = pipelines_[format * 2 + unsigned(disable_alpha)];
    const auto width = destination.width;
    const auto height = destination.height;
    if (!pipeline || width == 0 || height == 0 ||
        width > std::numeric_limits<uint32_t>::max() - 7 ||
        height > std::numeric_limits<uint32_t>::max() - 7 ||
        origin.x >= width || origin.y >= height || origin.z != 0 ||
        extent.width == 0 || extent.height == 0 || extent.depth != 1 ||
        extent.width > width - origin.x || extent.height > height - origin.y) {
        return false;
    }

    const auto tile_bytes = Pica::Texture::CalculateTileSize(
        static_cast<Pica::TexturingRegs::TextureFormat>(format));
    const auto row_bytes = ((width + 7) / 8) * tile_bytes;
    const auto preceding_rows = (height - 1) / 8;
    if (tile_row_bytes < row_bytes || row_bytes > source_bytes ||
        preceding_rows > (source_bytes - row_bytes) / tile_row_bytes) {
        return false;
    }

    const auto encoder = [commands computeCommandEncoder];
    if (!encoder)
        return false;
    const DecodeRegion region{
        uint32_t(height), tile_row_bytes, {uint32_t(origin.x), uint32_t(origin.y)},
        {uint32_t(extent.width), uint32_t(extent.height)},
    };
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source offset:offset atIndex:0];
    [encoder setBytes:&region length:sizeof(region) atIndex:1];
    [encoder setTexture:destination atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake((extent.width + 7) / 8, (extent.height + 7) / 8, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
    return true;
}

} // namespace Cara
