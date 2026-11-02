// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_software.h"
#include "common/color.h"
#include "core/frontend/emu_window.h"
#include "core/hw/gpu.h"
#include "core/hw/lcd.h"
#include "core/memory.h"
#include "video_core/video_core.h"

namespace VideoCore {
void RendererSoftware::SwapBuffers() {
    for (unsigned screen = 0; screen < 2; ++screen) {
        const auto& fb = GPU::g_regs.framebuffer_config[screen];
        const auto fill = screen == 0 ? LCD::g_regs.color_fill_top : LCD::g_regs.color_fill_bottom;
        const unsigned width = screen == 0 ? 400 : 320;
        constexpr unsigned height = 240;
        std::vector<u8> pixels(width * height * 4, 0);
        const PAddr address = fb.active_fb == 0 ? fb.address_left1 : fb.address_left2;
        const auto format = fb.color_format.Value();
        const bool valid_format = static_cast<u32>(format) <= 4;
        const u32 bpp = valid_format ? GPU::Regs::BytesPerPixel(format) : 0;
        const u64 span = u64(width - 1) * fb.stride + u64(height) * bpp;
        const u64 end = u64(address) + span;
        const bool valid_region =
            (address >= Memory::VRAM_PADDR && end <= Memory::VRAM_PADDR_END) ||
            (address >= Memory::FCRAM_PADDR && end <= Memory::FCRAM_N3DS_PADDR_END);
        const u8* source = valid_format && span > 0 && valid_region
                               ? Memory()->GetPhysicalPointer(address) : nullptr;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                Common::Vec4<u8> color{0, 0, 0, 255};
                if (fill.is_enabled) {
                    color = {fill.color_r, fill.color_g, fill.color_b, 255};
                } else if (source && valid_format && fb.width >= height && fb.height >= width &&
                           fb.stride >= height * bpp) {
                    // The LCD buffers are stored sideways, bottom to top within each column.
                    const u8* p = source + x * fb.stride + (height - 1 - y) * bpp;
                    switch (format) {
                    case GPU::Regs::PixelFormat::RGBA8: color = Color::DecodeRGBA8(p); break;
                    case GPU::Regs::PixelFormat::RGB8: color = Color::DecodeRGB8(p); break;
                    case GPU::Regs::PixelFormat::RGB565: color = Color::DecodeRGB565(p); break;
                    case GPU::Regs::PixelFormat::RGB5A1: color = Color::DecodeRGB5A1(p); break;
                    case GPU::Regs::PixelFormat::RGBA4: color = Color::DecodeRGBA4(p); break;
                    }
                }
                const auto offset = (y * width + x) * 4;
                pixels[offset] = color.r();
                pixels[offset + 1] = color.g();
                pixels[offset + 2] = color.b();
                pixels[offset + 3] = 255;
            }
        }
        render_window.PresentSoftwareFrame(screen, width, height, pixels);
    }
}
}
