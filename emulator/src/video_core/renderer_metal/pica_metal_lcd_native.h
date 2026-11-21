// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <functional>
#include <optional>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "common/common_types.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_metal/pica_metal_lcd_plan.h"
#include "video_core/renderer_metal/pica_metal_transfers.h"

namespace Cara::MetalLCD {

struct Pipelines {
    id<MTLComputePipelineState> decode;
    id<MTLRenderPipelineState> sample;
    id<MTLRenderPipelineState> solid;
    id<MTLSamplerState> nearest;
    id<MTLSamplerState> linear;
    id<MTLSamplerState> nearest_border;
    id<MTLSamplerState> linear_border;
    MTLPixelFormat output_format;
    id<MTLComputePipelineState> lossless_upscale = nil;
};

// Build before rendering. No shader compilation occurs on presentation.
std::optional<Pipelines> PreparePipelines(id<MTLLibrary> library, MTLPixelFormat output_format,
                                          NSError** error);

enum class Filter { Nearest, Linear };
enum class EdgeMode { ClampToEdge, TransparentBlackBorder };
struct Screen {
    id<MTLTexture> texture = nil;
    bool solid = false;
    std::array<float, 4> color{};
    std::optional<Filter> filter = std::nullopt;
};
using LosslessBuffers = std::array<id<MTLTexture>, 2>;

// For a source whose authority has already been resolved by the caller.
id<MTLTexture> EncodeLinearScanout(id<MTLCommandBuffer> commands, const Pipelines& pipelines,
                                  const Scanout& scanout, const MetalTransfer::ByteView& view);

// The same encoder targets a drawable or a caller-owned offscreen texture.
bool EncodeComposition(id<MTLCommandBuffer> commands, id<MTLTexture> destination,
                       const Layout::FramebufferLayout& layout, const Pipelines& pipelines,
                       Filter filter, EdgeMode edge,
                       const std::array<std::optional<Screen>, 2>& screens);

enum class Status { Submitted, Unsupported, DrawableUnavailable, Coalesced };

struct PixelCapture {
    id<MTLBuffer> buffer = nil;
    std::size_t row_stride = 0;
    MTLPixelFormat format = MTLPixelFormatInvalid;
};
struct Receipt {
    Status status;
    id<MTLCommandBuffer> commands;
    PixelCapture capture{};
};

// The renderer owns guest visibility. Resolve the exact scanout resource
// on this command buffer; a missing or unsupported resource fails the frame.
// The resolver can encode imports or format decoding before composition.
using ResolveNativeScreen = std::function<id<MTLTexture>(id<MTLCommandBuffer>, const Scanout&)>;

// Present the active LCD registers, with an optional GPU readback from the
// same sources. The caller tracks the submitted command through completion.
Receipt PresentNative(id<MTLCommandQueue> queue, CAMetalLayer* layer,
                      const Layout::FramebufferLayout& layout, const Pipelines& pipelines,
                      Filter filter, EdgeMode edge, const ResolveNativeScreen& resolve,
                      bool capture = false,
                      std::array<std::optional<Screen>, 2>* previous = nullptr,
                      unsigned upscale = 1, LosslessBuffers* lossless = nullptr,
                      bool present_drawable = true);

} // namespace Cara::MetalLCD
