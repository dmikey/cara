#import "pica_metal_transfers.h"

#include <algorithm>

namespace Cara::MetalTransfer {
namespace {

struct CopyArguments {
    std::uint32_t count;
    std::uint32_t source_width;
    std::uint32_t source_stride;
    std::uint32_t destination_width;
    std::uint32_t destination_stride;
    std::uint32_t reversed_source_rows;
};

struct FillArguments {
    std::uint32_t count;
    std::uint32_t pattern;
    std::uint32_t period;
};

static_assert(sizeof(CopyArguments) == 24);
static_assert(sizeof(FillArguments) == 12);

bool CanEncode(id<MTLCommandBuffer> command) {
    if (!command || !command.retainedReferences) {
        return false;
    }
    return command.status == MTLCommandBufferStatusNotEnqueued ||
           command.status == MTLCommandBufferStatusEnqueued;
}

std::optional<NSUInteger> BufferStart(id<MTLCommandBuffer> command, ByteView view,
                                      GuestInterval accessed) {
    id<MTLBuffer> buffer = view.buffer;
    if (!buffer || buffer.device != command.device || buffer.heap ||
        buffer.hazardTrackingMode == MTLHazardTrackingModeUntracked ||
        buffer.storageMode == MTLStorageModeMemoryless || !view.guest.Contains(accessed) ||
        view.buffer_offset > buffer.length) {
        return std::nullopt;
    }
    const NSUInteger delta = accessed.begin - view.guest.begin;
    if (delta > buffer.length - view.buffer_offset) {
        return std::nullopt;
    }
    const NSUInteger start = view.buffer_offset + delta;
    if (start % sizeof(std::uint32_t) != 0 || accessed.Size() > buffer.length - start) {
        return std::nullopt;
    }
    return start;
}

void DispatchWords(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline,
                   NSUInteger words) {
    const NSUInteger threads = std::min(pipeline.maxTotalThreadsPerThreadgroup,
                                        pipeline.threadExecutionWidth);
    const NSUInteger groups = (words + threads - 1) / threads;
    [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
}

bool EncodeCopy(id<MTLCommandBuffer> command, const Pipelines& pipelines,
                const TextureCopyPlan& plan, bool flip, ByteView source, ByteView destination) {
    if (!CanEncode(command) || source.buffer == destination.buffer) {
        return false;
    }
    const auto from = BufferStart(command, source, plan.source.footprint);
    const auto to = BufferStart(command, destination, plan.destination.footprint);
    if (!from || !to) {
        return false;
    }

    if (!flip && plan.source.count == 1 && plan.destination.count == 1) {
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        if (!encoder) {
            return false;
        }
        [encoder copyFromBuffer:source.buffer sourceOffset:*from
                       toBuffer:destination.buffer destinationOffset:*to size:plan.payload_bytes];
        [encoder endEncoding];
        return true;
    }

    id<MTLComputePipelineState> pipeline = pipelines.copy_rows;
    if (!pipeline || pipeline.device != command.device) {
        return false;
    }
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) {
        return false;
    }
    const CopyArguments args{plan.payload_bytes / 4,
                             plan.source.width_bytes / 4,
                             plan.source.stride_bytes / 4,
                             plan.destination.width_bytes / 4,
                             plan.destination.stride_bytes / 4,
                             flip ? plan.source.count : 0};
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source.buffer offset:*from atIndex:0];
    [encoder setBuffer:destination.buffer offset:*to atIndex:1];
    [encoder setBytes:&args length:sizeof(args) atIndex:2];
    DispatchWords(encoder, pipeline, args.count);
    [encoder endEncoding];
    return true;
}

} // namespace

bool EncodeResolvedByteRows(id<MTLCommandBuffer> command, id<MTLComputePipelineState> pipeline,
                            id<MTLBuffer> source, NSUInteger from,
                            id<MTLBuffer> destination, NSUInteger to,
                            std::uint32_t width, std::uint32_t source_stride,
                            std::uint32_t destination_stride, std::uint32_t rows) {
    if (!CanEncode(command) || !pipeline || pipeline.device != command.device ||
        !source || !destination || source == destination ||
        source.device != command.device || destination.device != command.device ||
        source.heap || destination.heap ||
        source.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        destination.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        !width || rows < 2 || source_stride < width || destination_stride < width ||
        ((from | to | width | source_stride | destination_stride) & 3u))
        return false;
    const std::uint64_t source_bytes = std::uint64_t{rows - 1} * source_stride + width;
    const std::uint64_t destination_bytes = std::uint64_t{rows - 1} * destination_stride + width;
    const std::uint64_t words = std::uint64_t{rows} * (width / 4);
    if (from > source.length || source_bytes > source.length - from ||
        to > destination.length || destination_bytes > destination.length - to ||
        words > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto encoder = [command computeCommandEncoder];
    if (!encoder)
        return false;
    const CopyArguments args{static_cast<std::uint32_t>(words), width / 4, source_stride / 4,
                             width / 4, destination_stride / 4, 0};
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source offset:from atIndex:0];
    [encoder setBuffer:destination offset:to atIndex:1];
    [encoder setBytes:&args length:sizeof(args) atIndex:2];
    DispatchWords(encoder, pipeline, args.count);
    [encoder endEncoding];
    return true;
}

std::optional<TextureCopyPlan> EncodeTextureCopy(id<MTLCommandBuffer> command,
                                              const Pipelines& pipelines,
                                              TextureCopyRegisters regs,
                                              ByteView source, ByteView destination) {
    const auto plan = PlanTextureCopy(regs);
    if (!plan || !EncodeCopy(command, pipelines, *plan, false, source, destination)) {
        return std::nullopt;
    }
    return plan;
}

std::optional<DisplayTransferPlan> EncodeDisplayTransfer(
    id<MTLCommandBuffer> command, const Pipelines& pipelines, DisplayTransferRegisters regs,
    ByteView source, ByteView destination) {
    const auto plan = PlanDisplayTransfer(regs);
    if (!plan || !EncodeCopy(command, pipelines, plan->copy, plan->flip_vertically, source,
                            destination)) {
        return std::nullopt;
    }
    return plan;
}

std::optional<LinearColorTransferPlan> PlanNativeLinearColorTransfer(
    DisplayTransferRegisters regs) {
    if (auto ordinary = PlanLinearColorTransfer(regs))
        return ordinary;
    // ScaleX uses a 2:1 linear color blit. The accelerated GLES path
    // uses the same tiled layout with or without block_32 for both
    // RGBA8 and RGB8 destinations.
    constexpr std::uint32_t scale_x = 0x01000000;
    constexpr std::uint32_t block32 = 0x00010000;
    constexpr std::uint32_t rgb8 = 0x00001000;
    const auto flags = regs.flags & ~1u;
    if ((flags & ~(block32 | rgb8)) != scale_x || regs.address_override_enabled ||
        !regs.input_width || !regs.input_height ||
        ((regs.input_width | regs.input_height) & 7u) ||
        regs.input_width != regs.output_width || regs.input_height != regs.output_height)
        return std::nullopt;

    const auto pixels = std::uint64_t{regs.input_width} * regs.input_height;
    const std::uint32_t bpp = flags & rgb8 ? 3u : 4u;
    if (pixels > (16u * 1024u * 1024u) / 4u)
        return std::nullopt;
    const auto source = AlignedGuestInterval(regs.source_address, pixels * 4);
    const auto destination = AlignedGuestInterval(regs.destination_address, pixels / 2 * bpp);
    if (!source || !destination || source->Overlaps(*destination))
        return std::nullopt;
    return LinearColorTransferPlan{*source, *destination, regs.input_width, regs.input_height,
                                   bpp, true, bool(regs.flags & 1u)};
}

bool EncodeLinearColorTransfer(id<MTLCommandBuffer> command, const Pipelines& pipelines,
                               DisplayTransferRegisters regs, id<MTLTexture> source,
                               id<MTLTexture> destination, id<MTLBuffer> guest_bytes) {
    const auto plan = PlanNativeLinearColorTransfer(regs);
    const auto pipeline = pipelines.linear_color;
    if (!plan || !CanEncode(command) || !pipeline || pipeline.device != command.device ||
        pipeline.maxTotalThreadsPerThreadgroup < 64 || source == destination ||
        !guest_bytes || guest_bytes.device != command.device || guest_bytes.heap ||
        guest_bytes.storageMode != MTLStorageModeShared ||
        guest_bytes.hazardTrackingMode != MTLHazardTrackingModeTracked ||
        guest_bytes.length != plan->destination.Size()) {
        return false;
    }
    const bool scale_x = (regs.flags & 0x03000000u) == 0x01000000u;
    const auto output_width = plan->width >> unsigned(scale_x);
    const auto valid_texture = [&](id<MTLTexture> texture, NSUInteger width, MTLTextureUsage usage) {
        return texture && texture.device == command.device && texture.width == width &&
               texture.height == plan->height && texture.textureType == MTLTextureType2D &&
               texture.pixelFormat == MTLPixelFormatRGBA8Unorm && texture.sampleCount == 1 &&
               texture.mipmapLevelCount == 1 && texture.arrayLength == 1 &&
               texture.hazardTrackingMode == MTLHazardTrackingModeTracked &&
               texture.storageMode != MTLStorageModeMemoryless && !texture.framebufferOnly &&
               !texture.heap && !texture.buffer && !texture.parentTexture &&
               !texture.iosurface && (texture.usage & usage) == usage;
    };
    if (!valid_texture(source, plan->width, MTLTextureUsageShaderRead) ||
        !valid_texture(destination, output_width, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead)) {
        return false;
    }
    const auto encoder = [command computeCommandEncoder];
    if (!encoder)
        return false;

    // The texture's tiled convention is vertically reversed from guest rows.
    const std::uint32_t args[] = {plan->destination_bpp,
                                  unsigned(plan->source_tiled != plan->flip) | (unsigned(scale_x) << 1)};
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:source atIndex:0];
    [encoder setTexture:destination atIndex:1];
    [encoder setBuffer:guest_bytes offset:0 atIndex:0];
    [encoder setBytes:args length:sizeof(args) atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake((output_width + 7) / 8, (plan->height + 7) / 8, 1)
             threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
    return true;
}

std::optional<FillPlan> EncodeFill(id<MTLCommandBuffer> command, const Pipelines& pipelines,
                                  FillRegisters regs, ByteView destination) {
    const auto plan = PlanFill(regs);
    id<MTLComputePipelineState> pipeline = pipelines.fill_words;
    if (!plan || !CanEncode(command) || !pipeline || pipeline.device != command.device) {
        return std::nullopt;
    }
    const auto to = BufferStart(command, destination, plan->destination);
    if (!to) {
        return std::nullopt;
    }
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) {
        return std::nullopt;
    }
    const FillArguments args{plan->destination.Size() / 4, plan->little_endian_pattern,
                             plan->pattern_bytes};
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:destination.buffer offset:*to atIndex:0];
    [encoder setBytes:&args length:sizeof(args) atIndex:1];
    DispatchWords(encoder, pipeline, args.count);
    [encoder endEncoding];
    return plan;
}

} // namespace Cara::MetalTransfer
