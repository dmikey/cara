#pragma once

#import <Metal/Metal.h>

#include "pica_transfer_plan.h"

namespace Cara::MetalTransfer {

// The caller proves that the source bytes are current on the same ordered queue, that destination
// gap ownership is preserved, and that the backing allocations do not alias each other. Offsets
// refer to a device-owned Metal allocation and are relative to the supplied guest interval.
struct ByteView {
    id<MTLBuffer> buffer;
    GuestInterval guest;
    NSUInteger buffer_offset;
};

struct Pipelines {
    id<MTLComputePipelineState> copy_rows;
    id<MTLComputePipelineState> fill_words;
    id<MTLComputePipelineState> linear_color = nil;
};

// The command buffer must retain resources and come from the same queue used by the writer cache.
// Success means encoded only. Publish the returned stripe intervals after submission through the
// cache, retaining the resource and command buffer until consumers read or supersede them.
std::optional<TextureCopyPlan> EncodeTextureCopy(id<MTLCommandBuffer> command,
                                              const Pipelines& pipelines,
                                              TextureCopyRegisters regs,
                                              ByteView source, ByteView destination);
std::optional<DisplayTransferPlan> EncodeDisplayTransfer(
    id<MTLCommandBuffer> command, const Pipelines& pipelines, DisplayTransferRegisters regs,
    ByteView source, ByteView destination);
// The source holds canonical logical RGBA8. The destination receives the
// linear canonical view and exact guest RGB8 or RGBA8 bytes in one pass.
bool EncodeLinearColorTransfer(id<MTLCommandBuffer> command, const Pipelines& pipelines,
                               DisplayTransferRegisters regs, id<MTLTexture> source,
                               id<MTLTexture> destination, id<MTLBuffer> guest_bytes);
std::optional<FillPlan> EncodeFill(id<MTLCommandBuffer> command, const Pipelines& pipelines,
                                  FillRegisters regs, ByteView destination);

} // namespace Cara::MetalTransfer
