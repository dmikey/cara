// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lcd_native.h"
#include "cara/adaptive_play.h"
#include "ios/display_scaling.h"

#include "core/hw/gpu.h"
#include "core/hw/hw.h"
#include "core/hw/lcd.h"
#include "common/logging/log.h"

namespace Cara::MetalLCD {
namespace {

static_assert(static_cast<u32>(GPU::Regs::PixelFormat::RGBA8) == 0 &&
              static_cast<u32>(GPU::Regs::PixelFormat::RGB8) == 1 &&
              static_cast<u32>(GPU::Regs::PixelFormat::RGB565) == 2 &&
              static_cast<u32>(GPU::Regs::PixelFormat::RGB5A1) == 3 &&
              static_cast<u32>(GPU::Regs::PixelFormat::RGBA4) == 4);

std::optional<Screen> PrepareScreen(id<MTLCommandBuffer> commands, unsigned index,
                                    const ResolveNativeScreen& resolve, const std::optional<Screen>* previous) {
    const auto& framebuffer = GPU::g_regs.framebuffer_config[index];
    const auto color_register = index == 0 ? LCD_REG_INDEX(color_fill_top)
                                           : LCD_REG_INDEX(color_fill_bottom);
    LCD::Regs::ColorFill fill{};
    LCD::Read(fill.raw, HW::VADDR_LCD + 4 * color_register);
    if (fill.is_enabled)
        return Screen{nil, true, {fill.color_r.Value() / 255.0f, fill.color_g.Value() / 255.0f,
                                  fill.color_b.Value() / 255.0f, 1.0f}};

    // Match the existing monoscopic SwapBuffers route for both LCDs.
    const auto address = framebuffer.active_fb == 0 ? framebuffer.address_left1
                                                    : framebuffer.address_left2;
    const auto scanout = PlanScanout(address, framebuffer.width.Value(), framebuffer.height.Value(),
                                    framebuffer.stride,
                                    static_cast<u32>(framebuffer.color_format.Value()));
    if (!scanout) {
        // The GLES upload skips this transient format/stride mismatch. Keep
        // that LCD's last image, or display black until its first valid scanout.
        if (address && framebuffer.height.Value() && framebuffer.width.Value() == 240 &&
            framebuffer.stride == 720 &&
            framebuffer.color_format == GPU::Regs::PixelFormat::RGBA8) {
            LOG_DEBUG(Render_OpenGL, "Skipping Metal LCD upload with short RGBA8 stride");
            if (previous && *previous)
                return *previous;
            return Screen{nil, true, {0.0f, 0.0f, 0.0f, 1.0f}};
        }
        LOG_ERROR(Render_OpenGL, "Metal LCD plan rejected screen={} address=0x{:08x} width={} height={} stride={} format={}",
                  index, address, framebuffer.width.Value(), framebuffer.height.Value(), framebuffer.stride,
                  static_cast<u32>(framebuffer.color_format.Value()));
        return std::nullopt;
    }

    auto texture = resolve(commands, *scanout);
    if (!texture) {
        LOG_ERROR(Render_OpenGL, "Metal LCD unresolved screen={} address=0x{:08x} width={} height={} stride={} format={}",
                  index, address, scanout->width, scanout->height, scanout->rows.stride_bytes,
                  static_cast<u32>(scanout->format));
        return std::nullopt;
    }
    return Screen{texture, false, {}};
}

bool PrepareLossless(id<MTLCommandBuffer> commands, const Layout::FramebufferLayout& layout,
                     const Pipelines& pipelines, unsigned factor, LosslessBuffers& scratch,
                     std::array<std::optional<Screen>, 2>& screens) {
    const auto device = commands.device;
    if (pipelines.lossless_upscale.device != device ||
        pipelines.lossless_upscale.maxTotalThreadsPerThreadgroup < 64)
        return false;

    const std::array rectangles{layout.top_screen, layout.bottom_screen};
    for (unsigned i = 0; i < screens.size(); ++i) {
        if (!screens[i] || screens[i]->solid)
            continue;
        const auto source = screens[i]->texture;
        if (source.device != device || source.pixelFormat != MTLPixelFormatRGBA8Unorm ||
            source.textureType != MTLTextureType2D || source.sampleCount != 1 ||
            !(source.usage & MTLTextureUsageShaderRead))
            return false;
        const auto& native = LCDPresentationRects[i];
        // Source is rotated: canonical width and height are reversed here.
        const auto width = unsigned(native.height) * factor;
        const auto height = unsigned(native.width) * factor;
        const auto& rect = rectangles[i];
        const bool integral = FinalScaleIsIntegral(height, width, rect.GetWidth(), rect.GetHeight());
        screens[i]->filter = integral ? Filter::Nearest : Filter::Linear;
        if (integral && source.width == unsigned(native.height) &&
            source.height == unsigned(native.width))
            continue;

        auto& texture = scratch[i];
        if (!texture || texture.width != width || texture.height != height) {
            const auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                      width:width height:height mipmapped:NO];
            desc.storageMode = MTLStorageModePrivate;
            desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
            desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            texture = [device newTextureWithDescriptor:desc];
        }
        if (!texture)
            return false;

        const auto encoder = [commands computeCommandEncoder];
        if (!encoder)
            return false;
        [encoder setComputePipelineState:pipelines.lossless_upscale];
        [encoder setTexture:source atIndex:0];
        [encoder setTexture:texture atIndex:1];
        [encoder setBytes:&factor length:sizeof(factor) atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake((width + 7) / 8, (height + 7) / 8, 1)
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [encoder endEncoding];
        screens[i]->texture = texture;
    }
    return true;
}

std::optional<PixelCapture> EncodeCapture(
    id<MTLCommandBuffer> command, const Layout::FramebufferLayout& layout,
    const Pipelines& pipelines, Filter filter, EdgeMode edge,
    const std::array<std::optional<Screen>, 2>& screens) {
    if (pipelines.output_format != MTLPixelFormatRGBA8Unorm &&
        pipelines.output_format != MTLPixelFormatBGRA8Unorm)
        return std::nullopt;
    const auto pixels_per_row = u64{layout.width};
    const auto stride = (pixels_per_row * 4 + 255) & ~u64{255};
    if (!stride || !layout.height || stride > command.device.maxBufferLength / layout.height)
        return std::nullopt;

    const auto desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pipelines.output_format width:layout.width
                                    height:layout.height mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    const auto texture = [command.device newTextureWithDescriptor:desc];
    const auto buffer = [command.device newBufferWithLength:stride * layout.height
                                                     options:MTLResourceStorageModeShared];
    if (!texture || !buffer ||
        !EncodeComposition(command, texture, layout, pipelines, filter, edge, screens))
        return std::nullopt;
    const auto blit = [command blitCommandEncoder];
    if (!blit)
        return std::nullopt;
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(layout.width, layout.height, 1) toBuffer:buffer
         destinationOffset:0 destinationBytesPerRow:stride
      destinationBytesPerImage:stride * layout.height];
    [blit endEncoding];
    return PixelCapture{buffer, static_cast<std::size_t>(stride), pipelines.output_format};
}

} // namespace

Receipt PresentNative(id<MTLCommandQueue> queue, CAMetalLayer* layer,
                      const Layout::FramebufferLayout& layout, const Pipelines& pipelines,
                      Filter filter, EdgeMode edge, const ResolveNativeScreen& resolve,
                      bool capture, std::array<std::optional<Screen>, 2>* previous,
                      unsigned upscale, LosslessBuffers* lossless, bool present_drawable) {
    const auto device = queue.device;
    if (!device || !layer || layer.device != device || layer.pixelFormat != pipelines.output_format ||
        !resolve || !layout.width || !layout.height ||
        pipelines.decode.device != device || pipelines.sample.device != device ||
        pipelines.solid.device != device || pipelines.nearest.device != device ||
        pipelines.linear.device != device || pipelines.nearest_border.device != device ||
        pipelines.linear_border.device != device)
        return {Status::Unsupported, nil};
    const auto commands = [queue commandBuffer];
    if (!commands)
        return {Status::Unsupported, nil};

    std::array<std::optional<Screen>, 2> screens;
    const std::array enabled{layout.top_screen_enabled, layout.bottom_screen_enabled};
    for (unsigned i = 0; i < screens.size(); ++i) {
        if (enabled[i]) {
            screens[i] = PrepareScreen(commands, i, resolve, previous ? &(*previous)[i] : nullptr);
            if (!screens[i])
                return {Status::Unsupported, nil};
        }
    }

    // Keep scanout and its previous-image history ordered even when this
    // refresh has no host display slot or the layer has no drawable yet.
    const auto keep_scanout = [&] {
        [commands commit];
        if (previous)
            *previous = std::move(screens);
        return Receipt{Status::Coalesced, commands};
    };
    if (!present_drawable && !capture)
        return keep_scanout();

    Cara::AdaptivePresentationWork display_work;
    const auto drawable = [layer nextDrawable];
    if (!drawable)
        return previous ? keep_scanout() : Receipt{Status::DrawableUnavailable, nil};
    auto composed = screens;
    if (upscale != 1 &&
        ((upscale != 2 && upscale != 4) || !lossless ||
         !PrepareLossless(commands, layout, pipelines, upscale, *lossless, composed)))
        return {Status::Unsupported, nil};
    if (!EncodeComposition(commands, drawable.texture, layout, pipelines, filter, edge, composed))
        return {Status::Unsupported, nil};

    display_work.Stop();

    PixelCapture pixels{};
    if (capture) {
        const auto encoded = EncodeCapture(commands, layout, pipelines, filter, edge, composed);
        if (!encoded)
            return {Status::Unsupported, nil};
        pixels = *encoded;
    }

    [commands presentDrawable:drawable];
    [commands commit];
    if (previous)
        *previous = screens;
    return {Status::Submitted, commands, pixels};
}

// The retained hybrid renderer ABI still submits every drawable.
Receipt PresentNative(id<MTLCommandQueue> queue, CAMetalLayer* layer,
                      const Layout::FramebufferLayout& layout, const Pipelines& pipelines,
                      Filter filter, EdgeMode edge, const ResolveNativeScreen& resolve,
                      bool capture, std::array<std::optional<Screen>, 2>* previous,
                      unsigned upscale, LosslessBuffers* lossless) {
    return PresentNative(queue, layer, layout, pipelines, filter, edge, resolve, capture,
                         previous, upscale, lossless, true);
}

} // namespace Cara::MetalLCD
