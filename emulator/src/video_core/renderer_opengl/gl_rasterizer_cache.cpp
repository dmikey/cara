// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iomanip>
#if defined(__APPLE__)
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>
#include <boost/range/iterator_range.hpp>
#include <glad/glad.h>
#include "common/alignment.h"
#include "common/bit_field.h"
#include "common/color.h"
#include "common/hash.h"
#include "common/frame_timing.h"
#include "video_core/renderer_opengl/gl_texture_lookup_key.h"
#include "video_core/renderer_opengl/gl_reinterpret_lookup_cache.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/vector_math.h"
#include "core/core.h"
#include "core/custom_tex_cache.h"
#include "core/frontend/emu_window.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/pica_state.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_format_reinterpreter.h"
#include "video_core/renderer_opengl/gl_color_readback.h"
#include "video_core/renderer_opengl/gl_surface_remap.h"
#include "video_core/renderer_opengl/gl_surface_remap_layout.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_rasterizer_cache.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/utils.h"
#include "video_core/video_core.h"

namespace OpenGL {

using SurfaceType = SurfaceParams::SurfaceType;
using PixelFormat = SurfaceParams::PixelFormat;

template <u32 bytes_per_pixel>
static void FlushLinearSwapped(u8* destination, const u8* source, u32 start, u32 end) {
    auto copy_byte = [&](u32 offset) {
        const u32 pixel = offset / bytes_per_pixel * bytes_per_pixel;
        destination[offset] = source[pixel + bytes_per_pixel - 1 - offset % bytes_per_pixel];
    };
    u32 offset = start;
    for (; offset < end && offset % bytes_per_pixel; ++offset) copy_byte(offset);
    for (; offset + bytes_per_pixel <= end; offset += bytes_per_pixel) {
        for (u32 component = 0; component < bytes_per_pixel; ++component)
            destination[offset + component] = source[offset + bytes_per_pixel - 1 - component];
    }
    for (; offset < end; ++offset) copy_byte(offset);
}

struct FormatTuple {
    GLint internal_format;
    GLenum format;
    GLenum type;
};

static constexpr std::array<FormatTuple, 5> fb_format_tuples = {{
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8},     // RGBA8
    {GL_RGB8, GL_BGR, GL_UNSIGNED_BYTE},              // RGB8
    {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1}, // RGB5A1
    {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},     // RGB565
    {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4},   // RGBA4
}};

// Same as above, with minor changes for OpenGL ES. Replaced
// GL_UNSIGNED_INT_8_8_8_8 with GL_UNSIGNED_BYTE and
// GL_BGR with GL_RGB
static constexpr std::array<FormatTuple, 5> fb_format_tuples_oes = {{
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},            // RGBA8
    {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE},              // RGB8
    {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1}, // RGB5A1
    {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},     // RGB565
    {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4},   // RGBA4
}};

static constexpr std::array<FormatTuple, 4> depth_format_tuples = {{
    {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT}, // D16
    {},
    {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},   // D24
    {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8}, // D24S8
}};

constexpr FormatTuple tex_tuple = {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};

static OGLFramebuffer g_read_framebuffer;
static OGLFramebuffer g_draw_framebuffer;

const FormatTuple& GetFormatTuple(PixelFormat pixel_format) {
    const SurfaceType type = SurfaceParams::GetFormatType(pixel_format);
    if (type == SurfaceType::Color) {
        if (GLES) {
            return fb_format_tuples_oes[static_cast<unsigned int>(pixel_format)];
        }
        return fb_format_tuples[static_cast<unsigned int>(pixel_format)];
    } else if (type == SurfaceType::Depth || type == SurfaceType::DepthStencil) {
        std::size_t tuple_idx = static_cast<std::size_t>(pixel_format) - 14;
        ASSERT(tuple_idx < depth_format_tuples.size());
        return depth_format_tuples[tuple_idx];
    }
    return tex_tuple;
}

template <typename Map, typename Interval>
static constexpr auto RangeFromInterval(Map& map, const Interval& interval) {
    return boost::make_iterator_range(map.equal_range(interval));
}

template <bool morton_to_gl, PixelFormat format>
static void MortonCopyTile(u32 stride, u8* tile_buffer, u8* gl_buffer) {
    constexpr u32 bytes_per_pixel = SurfaceParams::GetFormatBpp(format) / 8;
    constexpr u32 gl_bytes_per_pixel = CachedSurface::GetGLBytesPerPixel(format);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            u8* tile_ptr = tile_buffer + VideoCore::MortonInterleave(x, y) * bytes_per_pixel;
            u8* gl_ptr = gl_buffer + ((7 - y) * stride + x) * gl_bytes_per_pixel;
            if constexpr (morton_to_gl) {
                if constexpr (format == PixelFormat::D24S8) {
                    gl_ptr[0] = tile_ptr[3];
                    std::memcpy(gl_ptr + 1, tile_ptr, 3);
                } else if (format == PixelFormat::RGBA8 && GLES) {
                    // because GLES does not have ABGR format
                    // so we will do byteswapping here
                    gl_ptr[0] = tile_ptr[3];
                    gl_ptr[1] = tile_ptr[2];
                    gl_ptr[2] = tile_ptr[1];
                    gl_ptr[3] = tile_ptr[0];
                } else if (format == PixelFormat::RGB8 && GLES) {
                    gl_ptr[0] = tile_ptr[2];
                    gl_ptr[1] = tile_ptr[1];
                    gl_ptr[2] = tile_ptr[0];
                } else {
                    std::memcpy(gl_ptr, tile_ptr, bytes_per_pixel);
                }
            } else {
                if constexpr (format == PixelFormat::D24S8) {
                    std::memcpy(tile_ptr, gl_ptr + 1, 3);
                    tile_ptr[3] = gl_ptr[0];
                } else if (format == PixelFormat::RGBA8 && GLES) {
                    // because GLES does not have ABGR format
                    // so we will do byteswapping here
                    tile_ptr[0] = gl_ptr[3];
                    tile_ptr[1] = gl_ptr[2];
                    tile_ptr[2] = gl_ptr[1];
                    tile_ptr[3] = gl_ptr[0];
                } else if (format == PixelFormat::RGB8 && GLES) {
                    tile_ptr[0] = gl_ptr[2];
                    tile_ptr[1] = gl_ptr[1];
                    tile_ptr[2] = gl_ptr[0];
                } else {
                    std::memcpy(tile_ptr, gl_ptr, bytes_per_pixel);
                }
            }
        }
    }
}

template <bool morton_to_gl, PixelFormat format>
static void MortonCopy(u32 stride, u32 height, u8* gl_buffer, PAddr base, PAddr start, PAddr end) {
    constexpr u32 bytes_per_pixel = SurfaceParams::GetFormatBpp(format) / 8;
    constexpr u32 tile_size = bytes_per_pixel * 64;

    constexpr u32 gl_bytes_per_pixel = CachedSurface::GetGLBytesPerPixel(format);
    static_assert(gl_bytes_per_pixel >= bytes_per_pixel, "");
    gl_buffer += gl_bytes_per_pixel - bytes_per_pixel;

    const PAddr aligned_down_start = base + Common::AlignDown(start - base, tile_size);
    const PAddr aligned_start = base + Common::AlignUp(start - base, tile_size);
    const PAddr aligned_end = base + Common::AlignDown(end - base, tile_size);

    ASSERT(!morton_to_gl || (aligned_start == start && aligned_end == end));

    const u32 begin_pixel_index = (aligned_down_start - base) / bytes_per_pixel;
    u32 x = (begin_pixel_index % (stride * 8)) / 8;
    u32 y = (begin_pixel_index / (stride * 8)) * 8;

    gl_buffer += ((height - 8 - y) * stride + x) * gl_bytes_per_pixel;

    auto glbuf_next_tile = [&] {
        x = (x + 8) % stride;
        gl_buffer += 8 * gl_bytes_per_pixel;
        if (!x) {
            y += 8;
            gl_buffer -= stride * 9 * gl_bytes_per_pixel;
        }
    };

    u8* tile_buffer = VideoCore::Memory()->GetPhysicalPointer(start);

    if (start < aligned_start && !morton_to_gl) {
        std::array<u8, tile_size> tmp_buf;
        MortonCopyTile<morton_to_gl, format>(stride, &tmp_buf[0], gl_buffer);
        std::memcpy(tile_buffer, &tmp_buf[start - aligned_down_start],
                    std::min(aligned_start, end) - start);

        if (end <= aligned_start) return;
        tile_buffer += aligned_start - start;
        glbuf_next_tile();
    }

    const u8* buffer_end = tile_buffer + (aligned_end - aligned_start);
    while (tile_buffer < buffer_end) {
        MortonCopyTile<morton_to_gl, format>(stride, tile_buffer, gl_buffer);
        tile_buffer += tile_size;
        glbuf_next_tile();
    }

    if (end > std::max(aligned_start, aligned_end) && !morton_to_gl) {
        std::array<u8, tile_size> tmp_buf;
        MortonCopyTile<morton_to_gl, format>(stride, &tmp_buf[0], gl_buffer);
        std::memcpy(tile_buffer, &tmp_buf[0], end - aligned_end);
    }
}

static constexpr std::array<void (*)(u32, u32, u8*, PAddr, PAddr, PAddr), 18> morton_to_gl_fns = {
    MortonCopy<true, PixelFormat::RGBA8>,  // 0
    MortonCopy<true, PixelFormat::RGB8>,   // 1
    MortonCopy<true, PixelFormat::RGB5A1>, // 2
    MortonCopy<true, PixelFormat::RGB565>, // 3
    MortonCopy<true, PixelFormat::RGBA4>,  // 4
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,                             // 5 - 13
    MortonCopy<true, PixelFormat::D16>,  // 14
    nullptr,                             // 15
    MortonCopy<true, PixelFormat::D24>,  // 16
    MortonCopy<true, PixelFormat::D24S8> // 17
};

static constexpr std::array<void (*)(u32, u32, u8*, PAddr, PAddr, PAddr), 18> gl_to_morton_fns = {
    MortonCopy<false, PixelFormat::RGBA8>,  // 0
    MortonCopy<false, PixelFormat::RGB8>,   // 1
    MortonCopy<false, PixelFormat::RGB5A1>, // 2
    MortonCopy<false, PixelFormat::RGB565>, // 3
    MortonCopy<false, PixelFormat::RGBA4>,  // 4
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,                              // 5 - 13
    MortonCopy<false, PixelFormat::D16>,  // 14
    nullptr,                              // 15
    MortonCopy<false, PixelFormat::D24>,  // 16
    MortonCopy<false, PixelFormat::D24S8> // 17
};

// Allocate an uninitialized texture of appropriate size and format for the surface
static void AllocateSurfaceTexture(GLuint texture, const FormatTuple& format_tuple, u32 width,
                                   u32 height) {
    // Keep track of previous texture bindings
    GLuint old_tex = OpenGLState::BindTexture2D(0, texture);

    glTexImage2D(GL_TEXTURE_2D, 0, format_tuple.internal_format, width, height, 0,
                 format_tuple.format, format_tuple.type, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Restore previous texture bindings
    OpenGLState::BindTexture2D(0, old_tex);
}

static void AllocateTextureCube(GLuint texture, const FormatTuple& format_tuple, u32 width) {
    // Keep track of previous texture bindings
    GLuint old_tex = OpenGLState::BindTextureCube(texture);

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);

    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, format_tuple.internal_format, width, width);

    // Restore previous texture bindings
    OpenGLState::BindTextureCube(old_tex);
}

static void BlitTextures(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint dst_tex,
                         const Common::Rectangle<u32>& dst_rect, SurfaceType type) {
    OpenGLState prev_state = OpenGLState::GetCurState();
    OpenGLState state;
    state.draw.read_framebuffer = g_read_framebuffer.handle;
    state.draw.draw_framebuffer = g_draw_framebuffer.handle;
    state.SubApply();

    u32 buffers = 0;

    if (type == SurfaceType::Color || type == SurfaceType::Texture) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src_tex,
                               0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex,
                               0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);

        buffers = GL_COLOR_BUFFER_BIT;
    } else if (type == SurfaceType::Depth) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, src_tex, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dst_tex, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

        buffers = GL_DEPTH_BUFFER_BIT;
    } else if (type == SurfaceType::DepthStencil) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               src_tex, 0);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               dst_tex, 0);

        buffers = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    }

    // TODO (wwylele): use GL_NEAREST for shadow map texture
    // Note: shadow map is treated as RGBA8 format in PICA, as well as in the rasterizer cache, but
    // doing linear intepolation componentwise would cause incorrect value. However, for a
    // well-programmed game this code path should be rarely executed for shadow map with
    // inconsistent scale.
    glBlitFramebuffer(src_rect.left, src_rect.bottom, src_rect.right, src_rect.top, dst_rect.left,
                      dst_rect.bottom, dst_rect.right, dst_rect.top, buffers,
                      buffers == GL_COLOR_BUFFER_BIT ? GL_LINEAR : GL_NEAREST);
    prev_state.SubApply();
}

static void BlitTextures(const Surface& src, const Common::Rectangle<u32>& src_rect,
                         const Surface& dst, const Common::Rectangle<u32>& dst_rect) {
#ifndef CARA_IOS_GLES
    if (src->pixel_format == dst->pixel_format &&
        src->pixel_format < SurfaceParams::PixelFormat::IA8 && src_rect.bottom < src_rect.top) {
        // same color format, same size, don't flip vertically
        u32 src_width = src_rect.GetWidth();
        u32 src_height = src_rect.GetHeight();

        u32 dst_width = dst_rect.GetWidth();
        u32 dst_height = dst_rect.GetHeight();

        if (src_width == dst_width && src_height == dst_height) {
            glCopyImageSubData(src->texture.handle, GL_TEXTURE_2D, 0, src_rect.left,
                               src_rect.bottom, 0, dst->texture.handle, GL_TEXTURE_2D, 0,
                               dst_rect.left, dst_rect.bottom, 0, src_width, src_height, 1);
            return;
        }
    }

#endif
    BlitTextures(src->texture.handle, src_rect, dst->texture.handle, dst_rect, src->type);
}

static bool FillSurface(const Surface& surface, const u8* fill_data,
                        const Common::Rectangle<u32>& fill_rect) {
    OpenGLState prev_state = OpenGLState::GetCurState();
    OpenGLState state;
    state.scissor.enabled = true;
    state.scissor.x = static_cast<GLint>(fill_rect.left);
    state.scissor.y = static_cast<GLint>(fill_rect.bottom);
    state.scissor.width = static_cast<GLsizei>(fill_rect.GetWidth());
    state.scissor.height = static_cast<GLsizei>(fill_rect.GetHeight());
    auto rasterizer = dynamic_cast<RasterizerOpenGL*>(VideoCore::Rasterizer());

    surface->InvalidateAllWatcher();

    if (surface->type == SurfaceType::Color || surface->type == SurfaceType::Texture) {
        rasterizer->BindFramebufferColor(state, surface);

        Pica::Texture::TextureInfo tex_info{};
        tex_info.format = static_cast<Pica::TexturingRegs::TextureFormat>(surface->pixel_format);
        Common::Vec4<u8> color = Pica::Texture::LookupTexture(fill_data, 0, 0, tex_info);

        std::array<GLfloat, 4> color_values = {color.x / 255.f, color.y / 255.f, color.z / 255.f,
                                               color.w / 255.f};

        state.color_mask.red_enabled = GL_TRUE;
        state.color_mask.green_enabled = GL_TRUE;
        state.color_mask.blue_enabled = GL_TRUE;
        state.color_mask.alpha_enabled = GL_TRUE;
        state.SubApply();
        glClearBufferfv(GL_COLOR, 0, &color_values[0]);
    } else if (surface->type == SurfaceType::Depth) {
        rasterizer->BindFramebufferDepth(state, surface);

        u32 value_32bit = 0;
        GLfloat value_float;

        if (surface->pixel_format == SurfaceParams::PixelFormat::D16) {
            std::memcpy(&value_32bit, fill_data, 2);
            value_float = value_32bit / 65535.0f; // 2^16 - 1
        } else if (surface->pixel_format == SurfaceParams::PixelFormat::D24) {
            std::memcpy(&value_32bit, fill_data, 3);
            value_float = value_32bit / 16777215.0f; // 2^24 - 1
        }

        state.depth.write_mask = GL_TRUE;
        state.SubApply();
        glClearBufferfv(GL_DEPTH, 0, &value_float);
    } else if (surface->type == SurfaceType::DepthStencil) {
        rasterizer->BindFramebufferDepthStencil(state, surface);

        u32 value_32bit;
        std::memcpy(&value_32bit, fill_data, sizeof(u32));

        GLfloat value_float = (value_32bit & 0xFFFFFF) / 16777215.0f; // 2^24 - 1
        GLint value_int = (value_32bit >> 24);

        state.depth.write_mask = GL_TRUE;
        state.stencil.write_mask = -1;
        state.SubApply();
        glClearBufferfi(GL_DEPTH_STENCIL, 0, value_float, value_int);
    }
    prev_state.SubApply();
    return true;
}

bool CachedSurface::CanFill(const SurfaceParams& dest_surface,
                            SurfaceInterval fill_interval) const {
    if (type == SurfaceType::Fill && IsRegionValid(fill_interval) &&
        boost::icl::first(fill_interval) >= addr &&
        boost::icl::last_next(fill_interval) <= end && // dest_surface is within our fill range
        dest_surface.FromInterval(fill_interval).GetInterval() ==
            fill_interval) { // make sure interval is a rectangle in dest surface
        if (fill_size * 8 != dest_surface.GetFormatBpp()) {
            // Check if bits repeat for our fill_size
            const u32 dest_bytes_per_pixel = std::max(dest_surface.GetFormatBpp() / 8, 1u);
            std::vector<u8> fill_test(fill_size * dest_bytes_per_pixel);

            for (u32 i = 0; i < dest_bytes_per_pixel; ++i)
                std::memcpy(&fill_test[i * fill_size], &fill_data[0], fill_size);

            for (u32 i = 0; i < fill_size; ++i)
                if (std::memcmp(&fill_test[dest_bytes_per_pixel * i], &fill_test[0],
                                dest_bytes_per_pixel) != 0)
                    return false;

            if (dest_surface.GetFormatBpp() == 4 && (fill_test[0] & 0xF) != (fill_test[0] >> 4))
                return false;
        }
        return true;
    }
    return false;
}

bool CachedSurface::CanCopy(const SurfaceParams& dest_surface,
                            SurfaceInterval copy_interval) const {
    SurfaceParams subrect_params = dest_surface.FromInterval(copy_interval);
    ASSERT(subrect_params.GetInterval() == copy_interval);
    if (CanSubRect(subrect_params))
        return true;

    if (CanFill(dest_surface, copy_interval))
        return true;

    return false;
}

void RasterizerCacheOpenGL::CopySurface(const Surface& src_surface, const Surface& dst_surface,
                                        SurfaceInterval copy_interval) {
    MICROPROFILE_SCOPE(OpenGL_CopySurface);

    SurfaceParams subrect_params = dst_surface->FromInterval(copy_interval);
    ASSERT(subrect_params.GetInterval() == copy_interval);

    ASSERT(src_surface != dst_surface);

    // This is only called when CanCopy is true, no need to run checks here
    if (src_surface->type == SurfaceType::Fill) {
        // FillSurface needs a 4 bytes buffer
        const u32 fill_offset =
            (boost::icl::first(copy_interval) - src_surface->addr) % src_surface->fill_size;
        std::array<u8, 4> fill_buffer;

        u32 fill_buff_pos = fill_offset;
        for (int i : {0, 1, 2, 3})
            fill_buffer[i] = src_surface->fill_data[fill_buff_pos++ % src_surface->fill_size];

        FillSurface(dst_surface, &fill_buffer[0], dst_surface->GetScaledSubRect(subrect_params));
        return;
    }
    if (src_surface->CanSubRect(subrect_params)) {
        BlitTextures(src_surface, src_surface->GetScaledSubRect(subrect_params), dst_surface,
                     dst_surface->GetScaledSubRect(subrect_params));
        return;
    }
    UNREACHABLE();
}

void CachedSurface::LoadGLBuffer(PAddr load_start, PAddr load_end) {
    ASSERT(type != SurfaceType::Fill);

    const u8* const texture_src_data = VideoCore::Memory()->GetPhysicalPointer(addr);
    if (texture_src_data == nullptr)
        return;

    if (gl_buffer.empty()) {
        gl_buffer.resize(width * height * GetGLBytesPerPixel(pixel_format));
    }

    // TODO: Should probably be done in ::Memory:: and check for other regions too
    if (load_start < Memory::VRAM_VADDR_END && load_end > Memory::VRAM_VADDR_END)
        load_end = Memory::VRAM_VADDR_END;

    if (load_start < Memory::VRAM_VADDR && load_end > Memory::VRAM_VADDR)
        load_start = Memory::VRAM_VADDR;

    ASSERT(load_start >= addr && load_end <= end);
    const u32 start_offset = load_start - addr;

    if (!is_tiled) {
        ASSERT(type == SurfaceType::Color);
        // TODO(liushuyu): check if the byteswap here is 100% correct, cannot fully test this
        if (pixel_format == PixelFormat::RGBA8) {
            for (std::size_t i = start_offset; i < load_end - addr; i += 4) {
                gl_buffer[i] = texture_src_data[i + 3];
                gl_buffer[i + 1] = texture_src_data[i + 2];
                gl_buffer[i + 2] = texture_src_data[i + 1];
                gl_buffer[i + 3] = texture_src_data[i];
            }
        } else if (pixel_format == PixelFormat::RGB8) {
            for (std::size_t i = start_offset; i < load_end - addr; i += 3) {
                gl_buffer[i] = texture_src_data[i + 2];
                gl_buffer[i + 1] = texture_src_data[i + 1];
                gl_buffer[i + 2] = texture_src_data[i];
            }
        } else {
            std::memcpy(&gl_buffer[start_offset], texture_src_data + start_offset,
                        load_end - load_start);
        }
    } else {
        if (type == SurfaceType::Texture) {
            Pica::Texture::TextureInfo tex_info;
            tex_info.width = width;
            tex_info.height = height;
            tex_info.format = static_cast<Pica::TexturingRegs::TextureFormat>(pixel_format);
            tex_info.SetDefaultStride();
            tex_info.physical_address = addr;

            const SurfaceInterval load_interval(load_start, load_end);
            const auto rect = GetSubRect(FromInterval(load_interval));
            ASSERT(FromInterval(load_interval).GetInterval() == load_interval);

            for (unsigned y = rect.bottom; y < rect.top; ++y) {
                for (unsigned x = rect.left; x < rect.right; ++x) {
                    auto vec4 =
                        Pica::Texture::LookupTexture(texture_src_data, x, height - 1 - y, tex_info);
                    const std::size_t offset = (x + (width * y)) * 4;
                    std::memcpy(&gl_buffer[offset], vec4.AsArray(), 4);
                }
            }
        } else {
            morton_to_gl_fns[static_cast<std::size_t>(pixel_format)](stride, height, &gl_buffer[0],
                                                                     addr, load_start, load_end);
        }
    }
}

void CachedSurface::FlushGLBuffer(PAddr flush_start, PAddr flush_end) {
    u8* const dst_buffer = VideoCore::Memory()->GetPhysicalPointer(addr);
    if (dst_buffer == nullptr)
        return;

    // TODO: Should probably be done in ::Memory:: and check for other regions too
    // same as loadglbuffer()
    if (flush_start < Memory::VRAM_VADDR_END && flush_end > Memory::VRAM_VADDR_END)
        flush_end = Memory::VRAM_VADDR_END;

    if (flush_start < Memory::VRAM_VADDR && flush_end > Memory::VRAM_VADDR)
        flush_start = Memory::VRAM_VADDR;

    ASSERT(flush_start >= addr && flush_end <= end);
    const u32 start_offset = flush_start - addr;
    const u32 end_offset = flush_end - addr;

    if (type == SurfaceType::Fill) {
        const u32 coarse_start_offset = start_offset - (start_offset % fill_size);
        const u32 backup_bytes = start_offset % fill_size;
        std::array<u8, 4> backup_data;
        if (backup_bytes)
            std::memcpy(&backup_data[0], &dst_buffer[coarse_start_offset], backup_bytes);

        for (u32 offset = coarse_start_offset; offset < end_offset; offset += fill_size) {
            std::memcpy(&dst_buffer[offset], &fill_data[0],
                        std::min(fill_size, end_offset - offset));
        }

        if (backup_bytes)
            std::memcpy(&dst_buffer[coarse_start_offset], &backup_data[0], backup_bytes);
    } else if (!is_tiled) {
        ASSERT(type == SurfaceType::Color);
        if (pixel_format == PixelFormat::RGBA8 || pixel_format == PixelFormat::RGB8) {
            // Dirty ownership can split inside a pixel after a CPU byte write.
            // Do not overwrite the neighboring CPU-owned bytes when flushing it.
            if (pixel_format == PixelFormat::RGBA8)
                FlushLinearSwapped<4>(dst_buffer, gl_buffer.data(), start_offset, end_offset);
            else
                FlushLinearSwapped<3>(dst_buffer, gl_buffer.data(), start_offset, end_offset);
        } else {
            std::memcpy(dst_buffer + start_offset, &gl_buffer[start_offset], flush_end - flush_start);
        }
    } else {
        gl_to_morton_fns[static_cast<std::size_t>(pixel_format)](stride, height, &gl_buffer[0],
                                                                 addr, flush_start, flush_end);
    }
}

const Core::CustomTexInfo* CachedSurface::LoadCustomTexture(u64 tex_hash) {
    auto& custom_tex_cache = Core::System::GetInstance().CustomTexCache();
    return custom_tex_cache.LoadTexture(tex_hash);
}

void CachedSurface::UploadGLTexture(const Common::Rectangle<u32>& rect) {
    // Required for rect to function properly with custom textures
    Common::Rectangle custom_rect = rect;
    PixelFormat custom_format = pixel_format;

    if (Settings::values.custom_textures) {
        u64 tex_hash = Common::TextureHash64(gl_buffer.data(), gl_buffer.size());
        if (!custom_tex_info || custom_tex_info->hash != tex_hash) {
            custom_tex_info = LoadCustomTexture(tex_hash);
        }
        if (custom_tex_info) {
            // always going to be using rgba8
            custom_format = PixelFormat::RGBA8;
            custom_rect.left = (custom_rect.left * custom_tex_info->width) / width;
            custom_rect.top = (custom_rect.top * custom_tex_info->height) / height;
            custom_rect.right = (custom_rect.right * custom_tex_info->width) / width;
            custom_rect.bottom = (custom_rect.bottom * custom_tex_info->height) / height;
        }
    }

    // Load data from memory to the surface
    u32 bytes_per_pixel = GetGLBytesPerPixel(custom_format);
    GLint x0 = static_cast<GLint>(custom_rect.left);
    GLint y0 = static_cast<GLint>(custom_rect.bottom);
    std::size_t buffer_offset = (y0 * stride + x0) * bytes_per_pixel;

    const FormatTuple& tuple = GetFormatTuple(custom_format);
    GLuint target_tex = texture.handle;

    // If not 1x scale, create 1x texture that we will blit from to replace texture subrect in
    // surface
    OGLTexture unscaled_tex;
    if (res_scale != 1) {
        x0 = 0;
        y0 = 0;
        unscaled_tex.Create();
        if (custom_tex_info) {
            AllocateSurfaceTexture(unscaled_tex.handle, tuple, custom_tex_info->width, custom_tex_info->height);
        } else {
            AllocateSurfaceTexture(unscaled_tex.handle, tuple, rect.GetWidth(), rect.GetHeight());
        }
        target_tex = unscaled_tex.handle;
    } else if (custom_tex_info) {
        AllocateSurfaceTexture(texture.handle, tuple, custom_tex_info->width, custom_tex_info->height);
    }

    GLuint old_tex = OpenGLState::BindTexture2D(0, target_tex);
    if (custom_tex_info) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(custom_tex_info->width));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, custom_tex_info->width, custom_tex_info->height,
                        GL_RGBA, GL_UNSIGNED_BYTE, custom_tex_info->tex.data());
    } else {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, static_cast<GLsizei>(rect.GetWidth()),
                        static_cast<GLsizei>(rect.GetHeight()), tuple.format, tuple.type,
                        &gl_buffer[buffer_offset]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    OpenGLState::BindTexture2D(0, old_tex);

    if (unscaled_tex.handle) {
        auto scaled_rect = rect;
        scaled_rect.left *= res_scale;
        scaled_rect.top *= res_scale;
        scaled_rect.right *= res_scale;
        scaled_rect.bottom *= res_scale;
        BlitTextures(unscaled_tex.handle, {0, custom_rect.GetHeight(), custom_rect.GetWidth(), 0},
                     texture.handle, scaled_rect, type);
    }

    InvalidateAllWatcher();
}

void CachedSurface::DownloadGLTexture(const Common::Rectangle<u32>& rect) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::SurfaceDownload);
    u32 bytes_per_pixel = GetGLBytesPerPixel(pixel_format);
    const bool repack_color = GLES && type == SurfaceType::Color && pixel_format != PixelFormat::RGBA8;
    if (Common::FrameTiming::counters.enabled) {
        ++Common::FrameTiming::counters.surface_download_calls;
        Common::FrameTiming::counters.surface_download_bytes += u64(rect.GetWidth()) * rect.GetHeight() * (repack_color ? 4 : bytes_per_pixel);
    }
    const FormatTuple& tuple = GetFormatTuple(pixel_format);
    if (gl_buffer.empty()) {
        gl_buffer.resize(stride * height * bytes_per_pixel);
    }

    GLint x0 = rect.left;
    GLint y0 = rect.bottom;
    GLuint target_tex = texture.handle;

    // If not 1x scale, blit scaled texture to a new 1x texture and use that to flush
    OGLTexture unscaled_tex;
    if (res_scale != 1) {
        auto scaled_rect = rect;
        scaled_rect.left *= res_scale;
        scaled_rect.top *= res_scale;
        scaled_rect.right *= res_scale;
        scaled_rect.bottom *= res_scale;
        unscaled_tex.Create();
        x0 = 0;
        y0 = 0;
        target_tex = unscaled_tex.handle;

        Common::Rectangle<u32> unscaled_tex_rect{0, rect.GetHeight(), rect.GetWidth(), 0};
        AllocateSurfaceTexture(unscaled_tex.handle, tuple, rect.GetWidth(), rect.GetHeight());
        BlitTextures(texture.handle, scaled_rect, unscaled_tex.handle, unscaled_tex_rect, type);
    } else {
        OpenGLState::ResetTexture(target_tex);
    }

    glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(stride));
    OpenGLState::BindReadFramebuffer(g_read_framebuffer.handle);
    std::size_t buffer_offset = (rect.bottom * stride + rect.left) * bytes_per_pixel;
    if (type == SurfaceType::Color || type == SurfaceType::Texture) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex,
                               0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);
    } else if (type == SurfaceType::Depth) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, target_tex,
                               0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    } else {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               target_tex, 0);
    }
    static const bool wait_trace = [] {
        const char* value = std::getenv("CARA_READBACK_WAIT_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    last_download_wait_seconds = last_download_read_seconds = last_download_pack_seconds = 0;
    // ES only guarantees RGBA/UNSIGNED_BYTE for normalized color readback.
    // Upload tuples such as RGB/UNSIGNED_BYTE and packed 16-bit formats need
    // not be accepted by glReadPixels, even when the framebuffer is complete.
    GLenum read_format = tuple.format;
    GLenum read_type = tuple.type;
    u8* read_destination = &gl_buffer[buffer_offset];
    if (repack_color) {
        rgba_readback_buffer.resize(size_t(rect.GetWidth()) * rect.GetHeight() * 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        read_format = GL_RGBA;
        read_type = GL_UNSIGNED_BYTE;
        read_destination = rgba_readback_buffer.data();
    }
    if (wait_trace) {
        // Diagnostic only: explicitly complete queued GPU work before reading.
        // The remaining read can still include tile export/format conversion.
        auto started = std::chrono::steady_clock::now();
        {
            Common::FrameTiming::Scope wait_timing(Common::FrameTiming::Phase::GPUWait);
            glFinish();
        }
        last_download_wait_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        started = std::chrono::steady_clock::now();
        {
            Common::FrameTiming::Scope read_timing(Common::FrameTiming::Phase::SurfaceReadPixels);
            glReadPixels(x0, y0, static_cast<GLsizei>(rect.GetWidth()),
                         static_cast<GLsizei>(rect.GetHeight()), read_format, read_type,
                         read_destination);
        }
        last_download_read_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    } else {
        Common::FrameTiming::Scope read_timing(Common::FrameTiming::Phase::SurfaceReadPixels);
        glReadPixels(x0, y0, static_cast<GLsizei>(rect.GetWidth()),
                     static_cast<GLsizei>(rect.GetHeight()), read_format, read_type,
                     read_destination);
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (repack_color) {
        const auto started = std::chrono::steady_clock::now();
        Common::FrameTiming::Scope pack_timing(Common::FrameTiming::Phase::SurfacePack);
        const u8* source = rgba_readback_buffer.data();
        for (u32 y = 0; y < rect.GetHeight(); ++y) {
            u8* destination = &gl_buffer[buffer_offset + size_t(y) * stride * bytes_per_pixel];
            for (u32 x = 0; x < rect.GetWidth(); ++x, source += 4, destination += bytes_per_pixel) {
                if (pixel_format == PixelFormat::RGB8) {
                    std::memcpy(destination, source, 3);
                } else {
                    const u16 pixel = pixel_format == PixelFormat::RGB565 ? ColorReadback::PackRGB565(source)
                                    : pixel_format == PixelFormat::RGB5A1 ? ColorReadback::PackRGB5A1(source)
                                                                         : ColorReadback::PackRGBA4(source);
                    std::memcpy(destination, &pixel, sizeof(pixel));
                }
            }
        }
        last_download_pack_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
}

void CachedSurface::DumpToFile() {
    if (gl_buffer.empty()) {
        DownloadGLTexture(GetRect());
    }

    const std::string& output = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    const auto& image_interface = Core::System::GetInstance().GetImageInterface();
    std::string path =
        fmt::format("{}{:08X}_{}_{}_{}.png", output, addr, width, height, (u32)pixel_format);
    u32 bytes_per_pixel = GetGLBytesPerPixel(pixel_format);
    const FormatTuple& tuple = GetFormatTuple(pixel_format);
    std::vector<u8> pixels(width * height * bytes_per_pixel);
    ConvertToRGBA8888((u32*)pixels.data(), gl_buffer.data(), width * height, tuple.format,
                      tuple.type);
    image_interface->EncodePNG(path, pixels, width, height);
}

GLuint CachedSurface::GetTextureCopyHandle() {
    if (texture_copy.handle == 0) {
        texture_copy.Create();
        AllocateSurfaceTexture(texture_copy.handle, GetFormatTuple(pixel_format), GetScaledWidth(), GetScaledHeight());
    }
#ifdef CARA_IOS_GLES
    const Common::Rectangle<u32> rect{0, GetScaledHeight(), GetScaledWidth(), 0};
    BlitTextures(texture.handle, rect, texture_copy.handle, rect, type);
#else
    glCopyImageSubData(texture.handle, GL_TEXTURE_2D, 0, 0, 0, 0, texture_copy.handle, GL_TEXTURE_2D, 0, 0, 0, 0, GetScaledWidth(), GetScaledHeight(), 1);
#endif
    return texture_copy.handle;
}

enum MatchFlags {
    Invalid = 1,      // Flag that can be applied to other match types, invalid matches require
                      // validation before they can be used
    Exact = 1 << 1,   // Surfaces perfectly match
    SubRect = 1 << 2, // Surface encompasses params
    Copy = 1 << 3,    // Surface we can copy from
    Expand = 1 << 4,  // Surface that can expand params
    TexCopy = 1 << 5  // Surface that will match a display transfer "texture copy" parameters
};

static constexpr MatchFlags operator|(MatchFlags lhs, MatchFlags rhs) {
    return static_cast<MatchFlags>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

/// Get the best surface match (and its match type) for the given flags
template <MatchFlags find_flags>
static Surface FindMatch(const SurfaceCache& surface_cache, const SurfaceParams& params,
                         ScaleMatch match_scale_type,
                         std::optional<SurfaceInterval> validate_interval = std::nullopt,
                         ExactLookupSelection<const CachedSurface*>* selection = nullptr) {
    Surface match_surface = nullptr;
    bool match_valid = false;
    u32 match_scale = 0;
    SurfaceInterval match_interval{};

    for (const auto& pair : RangeFromInterval(surface_cache, params.GetInterval())) {
        for (const auto& surface : pair.second) {
            const bool res_scale_matched = match_scale_type == ScaleMatch::Exact
                                               ? (params.res_scale == surface->res_scale)
                                               : (params.res_scale <= surface->res_scale);
            // validity will be checked in GetCopyableInterval
            bool is_valid =
                find_flags & MatchFlags::Copy
                    ? true
                    : surface->IsRegionValid(validate_interval.value_or(params.GetInterval()));

            if (!(find_flags & MatchFlags::Invalid) && !is_valid)
                continue;

            auto IsMatch_Helper = [&](auto check_type, auto match_fn) {
                if (!(find_flags & check_type))
                    return;

                bool matched;
                SurfaceInterval surface_interval;
                std::tie(matched, surface_interval) = match_fn();
                if (!matched)
                    return;

                if (!res_scale_matched && match_scale_type != ScaleMatch::Ignore &&
                    surface->type != SurfaceType::Fill)
                    return;

                // The sink is used only by GetSurface's Exact|Invalid query.
                // Observe all eligible identities, not only winner updates;
                // validity can change later without a structural cache mutation.
                if (selection) selection->Observe(surface.get(), surface->res_scale);

                // Found a match, update only if this is better than the previous one
                auto UpdateMatch = [&] {
                    match_surface = surface;
                    match_valid = is_valid;
                    match_scale = surface->res_scale;
                    match_interval = surface_interval;
                };

                if (surface->res_scale > match_scale) {
                    UpdateMatch();
                    return;
                } else if (surface->res_scale < match_scale) {
                    return;
                }

                if (is_valid && !match_valid) {
                    UpdateMatch();
                    return;
                } else if (is_valid != match_valid) {
                    return;
                }

                if (boost::icl::length(surface_interval) > boost::icl::length(match_interval)) {
                    UpdateMatch();
                }
            };
            IsMatch_Helper(std::integral_constant<MatchFlags, MatchFlags::Exact>{}, [&] {
                return std::make_pair(surface->ExactMatch(params), surface->GetInterval());
            });
            IsMatch_Helper(std::integral_constant<MatchFlags, MatchFlags::SubRect>{}, [&] {
                return std::make_pair(surface->CanSubRect(params), surface->GetInterval());
            });
            IsMatch_Helper(std::integral_constant<MatchFlags, MatchFlags::Copy>{}, [&] {
                ASSERT(validate_interval);
                auto copy_interval =
                    params.FromInterval(*validate_interval).GetCopyableInterval(surface);
                bool matched = boost::icl::length(copy_interval & *validate_interval) != 0 &&
                               surface->CanCopy(params, copy_interval);
                return std::make_pair(matched, copy_interval);
            });
            IsMatch_Helper(std::integral_constant<MatchFlags, MatchFlags::Expand>{}, [&] {
                return std::make_pair(surface->CanExpand(params), surface->GetInterval());
            });
            IsMatch_Helper(std::integral_constant<MatchFlags, MatchFlags::TexCopy>{}, [&] {
                return std::make_pair(surface->CanTexCopy(params), surface->GetInterval());
            });
        }
    }
    return match_surface;
}

RasterizerCacheOpenGL::RasterizerCacheOpenGL() {
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::strcmp(value, "1") == 0;
    };
    texture_lookup_reuse = enabled("CARA_TEXTURE_LOOKUP_REUSE");
    texture_lookup_verify = enabled("CARA_TEXTURE_LOOKUP_VERIFY");
    trace_flushes = enabled("CARA_SURFACE_FLUSH_TRACE");
    retain_small_cpu_writes = enabled("CARA_RETAIN_SMALL_CPU_WRITES");
    gpu_surface_remap = enabled("CARA_GPU_SURFACE_REMAP");
    resolution_scale_factor = VideoCore::GetResolutionScaleFactor();
    format_reinterpreter = std::make_unique<FormatReinterpreterOpenGL>();

    g_read_framebuffer.Create();
    g_draw_framebuffer.Create();
}

RasterizerCacheOpenGL::~RasterizerCacheOpenGL() {
    if (trace_flushes) WriteFlushTrace();
    g_read_framebuffer.Release();
    g_draw_framebuffer.Release();
}

void RasterizerCacheOpenGL::RecordFlush(const Surface& surface, u32 requested_bytes, u32 width,
                                       u32 height, double seconds) {
    const u32 format = static_cast<u32>(surface->pixel_format);
    auto found = std::find_if(flush_trace.begin(), flush_trace.end(), [&](const auto& item) {
        return item.reason == flush_reason && item.requested_bytes == requested_bytes &&
               item.surface_address == surface->addr && item.pixel_format == format &&
               item.width == width && item.height == height && item.source_params.ExactMatch(*surface) &&
               (flush_reason != FlushReason::Validation || item.destination_params.ExactMatch(validation_destination));
    });
    // Bounded diagnostics only; the last bucket aggregates otherwise unseen cases.
    if (found == flush_trace.end()) {
        if (flush_trace.size() < 128) {
            flush_trace.push_back({flush_reason, requested_bytes, surface->addr, format, width, height});
            found = std::prev(flush_trace.end());
            found->source_params = *surface;
            if (flush_reason == FlushReason::Validation) {
                found->destination_params = validation_destination;
                const auto& dst = validation_destination;
                if (surface->pixel_format != dst.pixel_format) found->copy_reject_bits |= 1;
                if (surface->is_tiled != dst.is_tiled) found->copy_reject_bits |= 2;
                if (dst.stride != surface->stride && dst.height > (surface->is_tiled ? 8u : 1u)) found->copy_reject_bits |= 4;
                if (dst.addr < surface->addr || dst.end > surface->end) found->copy_reject_bits |= 8;
                if (dst.addr >= surface->addr && (dst.addr - surface->addr) % surface->BytesInPixels(surface->is_tiled ? 64 : 1)) found->copy_reject_bits |= 16;
                if (dst.addr >= surface->addr && surface->GetSubRect(dst).right > surface->stride) found->copy_reject_bits |= 32;
                if (boost::icl::length(dst.GetCopyableInterval(surface)) == 0) found->copy_reject_bits |= 64;
            }
#if defined(__APPLE__)
            if (flush_reason == FlushReason::External) {
                void* addresses[16]{};
                const int count = backtrace(addresses, 16);
                for (int i = 0; i < count; ++i) {
                    Dl_info info{};
                    dladdr(addresses[i], &info);
                    found->stack.push_back({reinterpret_cast<std::uintptr_t>(addresses[i]),
                        reinterpret_cast<std::uintptr_t>(info.dli_fbase),
                        info.dli_fname ? info.dli_fname : "",
                        info.dli_sname ? info.dli_sname : ""});
                }
            }
#endif
        } else {
            if (flush_trace.size() == 128) {
                flush_trace.push_back({FlushReason::External, 0, 0, ~0u, 0, 0});
            }
            found = std::prev(flush_trace.end());
        }
    }
    ++found->calls;
    const bool repacked = GLES && surface->type == SurfaceType::Color && surface->pixel_format != PixelFormat::RGBA8;
    found->readback_bytes += u64(width) * height * (repacked ? 4 : CachedSurface::GetGLBytesPerPixel(surface->pixel_format));
    found->seconds += seconds;
    found->wait_seconds += surface->last_download_wait_seconds;
    found->read_seconds += surface->last_download_read_seconds;
    found->pack_seconds += surface->last_download_pack_seconds;
}

void RasterizerCacheOpenGL::WriteFlushTrace() const {
    std::ofstream file(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "surface-flushes.json");
    const char* wait = std::getenv("CARA_READBACK_WAIT_TRACE");
    file << "{\"wait_instrumented\":" << (wait && std::strcmp(wait, "1") == 0 ? "true" : "false")
         << ",\"reasons\":[\"external\",\"validation\",\"small_cpu_write\",\"all\",\"cleanup\"],\"entries\":[";
    for (size_t i = 0; i < flush_trace.size(); ++i) {
        const auto& item = flush_trace[i];
        file << (i ? "," : "") << "{\"reason\":" << unsigned(item.reason)
             << ",\"requested_bytes\":" << item.requested_bytes
             << ",\"surface_address\":" << item.surface_address
             << ",\"format\":" << item.pixel_format << ",\"width\":" << item.width
             << ",\"height\":" << item.height << ",\"calls\":" << item.calls
             << ",\"readback_bytes\":" << item.readback_bytes
             << ",\"seconds\":" << item.seconds
             << ",\"wait_seconds\":" << item.wait_seconds
             << ",\"read_seconds\":" << item.read_seconds
             << ",\"pack_seconds\":" << item.pack_seconds << ",\"stack\":[";
        for (size_t frame = 0; frame < item.stack.size(); ++frame) {
            const auto& entry = item.stack[frame];
            file << (frame ? "," : "") << "{\"address\":" << entry.address
                 << ",\"image_base\":" << entry.image_base
                 << ",\"image\":" << std::quoted(entry.image)
                 << ",\"symbol\":" << std::quoted(entry.symbol) << '}';
        }
        const auto params = [&](const char* name, const SurfaceParams& p) {
            file << ",\"" << name << "\":{\"address\":" << p.addr << ",\"size\":" << p.size
                 << ",\"format\":" << unsigned(p.pixel_format) << ",\"width\":" << p.width
                 << ",\"height\":" << p.height << ",\"stride\":" << p.stride
                 << ",\"tiled\":" << (p.is_tiled ? "true" : "false") << ",\"scale\":" << p.res_scale << '}';
        };
        file << "]";
        params("source", item.source_params);
        if (item.reason == FlushReason::Validation) params("destination", item.destination_params);
        file << ",\"copy_reject_bits\":" << item.copy_reject_bits << '}';
    }
    file << "]}\n";
}

bool RasterizerCacheOpenGL::BlitSurfaces(const Surface& src_surface,
                                         const Common::Rectangle<u32>& src_rect,
                                         const Surface& dst_surface,
                                         const Common::Rectangle<u32>& dst_rect) {
    if (SurfaceParams::CheckFormatsBlittable(src_surface->pixel_format, dst_surface->pixel_format)) {
        dst_surface->InvalidateAllWatcher();
        BlitTextures(src_surface, src_rect, dst_surface, dst_rect);
        return true;
    } else {
        LOG_INFO(Render_OpenGL, "BlitSurfaces failed from: {} to: {}", (u32)src_surface->pixel_format,
                 (u32)dst_surface->pixel_format);
        return false;
    }
}

Surface RasterizerCacheOpenGL::GetSurface(const SurfaceParams& params, ScaleMatch match_res_scale,
                                          bool load_if_create) {
    if (params.addr == 0 || params.height * params.width == 0) {
        return nullptr;
    }
    // Use GetSurfaceSubRect instead
    ASSERT(params.width == params.stride);

    ASSERT(!params.is_tiled || (params.width % 8 == 0 && params.height % 8 == 0));

    // Check for an exact match in existing surfaces
    Surface surface;
    const bool count_lookup = texture_lookup_reuse && Common::FrameTiming::counters.enabled;
    const u64 selection_generation = texture_lookup_generation;
    ExactLookupSelection<const CachedSurface*> selection;
    bool lookup_hit = false;
    bool verification_mismatch = false;
    if (texture_lookup_reuse) {
        if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_checks;
        for (const auto& entry : texture_lookup_entries) {
            if (entry.generation != selection_generation || entry.scale_match != match_res_scale ||
                !SameTextureLookupParams(entry.params, params) || !entry.watcher || !entry.watcher->IsValid()) continue;
            auto candidate = entry.watcher->Get();
            if (!candidate || !candidate->registered || candidate->type == SurfaceType::Fill ||
                candidate->res_scale != entry.selected_scale || !candidate->ExactMatch(params) ||
                !candidate->IsRegionValid(params.GetInterval())) continue;
            if (texture_lookup_verify) {
                auto reference = FindMatch<MatchFlags::Exact | MatchFlags::Invalid>(surface_cache, params, match_res_scale);
                if (reference != candidate) {
                    if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_verify_mismatches;
                    surface = std::move(reference);
                    texture_lookup_entries = {};
                    texture_lookup_next = 0;
                    verification_mismatch = true;
                    break;
                }
                if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_verified_hits;
            }
            surface = std::move(candidate);
            lookup_hit = true;
            if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_hits;
            break;
        }
        if (!lookup_hit && count_lookup) ++Common::FrameTiming::counters.texture_lookup_misses;
    }
    if (!surface)
        surface = FindMatch<MatchFlags::Exact | MatchFlags::Invalid>(surface_cache, params, match_res_scale,
            std::nullopt, texture_lookup_reuse ? &selection : nullptr);

    if (surface == nullptr) {
        u16 target_res_scale = params.res_scale;
        if (match_res_scale != ScaleMatch::Exact) {
            // This surface may have a subrect of another surface with a higher res_scale, find
            // it to adjust our params
            Surface expandable = FindMatch<MatchFlags::Expand | MatchFlags::Invalid>(
                surface_cache, params, match_res_scale);
            if (expandable != nullptr) {
                target_res_scale = expandable->res_scale;
            } else if (params.pixel_format == PixelFormat::RGBA8) {
                // Keep res_scale when reinterpreting d24s8 -> rgba8
                SurfaceParams find_params = params;
                find_params.pixel_format = PixelFormat::D24S8;
                expandable = FindMatch<MatchFlags::Expand | MatchFlags::Invalid>(
                    surface_cache, find_params, match_res_scale);
                if (expandable != nullptr) {
                    target_res_scale = expandable->res_scale;
                }
            }
        }
        SurfaceParams new_params = params;
        new_params.res_scale = target_res_scale;
        surface = CreateSurface(new_params);
        RegisterSurface(surface);
    }

    if (load_if_create) {
        ValidateSurface(surface, params.addr, params.size);
    }

    if (texture_lookup_reuse && !lookup_hit && !verification_mismatch) {
        if (selection_generation != texture_lookup_generation) {
            if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_epoch_rejects;
        } else if (!selection.IsUnique(surface.get())) {
            if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_ambiguous_rejects;
        } else if (surface->registered && surface->type != SurfaceType::Fill &&
                   surface->IsRegionValid(params.GetInterval())) {
            auto watcher = surface->GetTextureLookupWatcher();
            watcher->Validate();
            auto& entry = texture_lookup_entries[texture_lookup_next];
            entry = {params, match_res_scale, selection_generation, surface->res_scale, std::move(watcher)};
            texture_lookup_next = (texture_lookup_next + 1) % TextureLookupCapacity;
            if (count_lookup) ++Common::FrameTiming::counters.texture_lookup_stores;
        }
    }
    surface->last_used_frame = VideoCore::GetCurrentFrame();

    return surface;
}

SurfaceRect_Tuple RasterizerCacheOpenGL::GetSurfaceSubRect(const SurfaceParams& params,
                                                           ScaleMatch match_res_scale,
                                                           bool load_if_create) {
    if (params.addr == 0 || params.height == 0 || params.width == 0) {
        return {};
    }

    // Attempt to find encompassing surface
    Surface surface = FindMatch<MatchFlags::SubRect | MatchFlags::Invalid>(surface_cache, params,
                                                                           match_res_scale);

    // Check if FindMatch failed because of res scaling
    // If that's the case create a new surface with
    // the dimensions of the lower res_scale surface
    // to suggest it should not be used again
    if (surface == nullptr && match_res_scale != ScaleMatch::Ignore) {
        surface = FindMatch<MatchFlags::SubRect | MatchFlags::Invalid>(surface_cache, params,
                                                                       ScaleMatch::Ignore);
        if (surface != nullptr) {
            SurfaceParams new_params = *surface;
            new_params.res_scale = params.res_scale;
            surface = CreateSurface(new_params);
            RegisterSurface(surface);
        }
    }

    SurfaceParams aligned_params = params;
    if (params.is_tiled) {
        aligned_params.height = Common::AlignUp(params.height, 8);
        aligned_params.width = Common::AlignUp(params.width, 8);
        aligned_params.stride = Common::AlignUp(params.stride, 8);
        aligned_params.UpdateParams();
    }

    // Check for a surface we can expand before creating a new one
    if (surface == nullptr) {
        if (match_res_scale == ScaleMatch::Exact && aligned_params.height <= 512) {
            SurfaceParams expand_params = aligned_params;
            expand_params.addr -= expand_params.size;
            surface = FindMatch<MatchFlags::Expand | MatchFlags::Invalid>(surface_cache, expand_params,
                                                                          match_res_scale);
        } else {
            surface = FindMatch<MatchFlags::Expand | MatchFlags::Invalid>(surface_cache, aligned_params,
                                                                          match_res_scale);
        }

        if (surface != nullptr) {
            if (aligned_params.width != aligned_params.stride) {
                // Can't have gaps in a surface
                aligned_params.width = aligned_params.stride;
                aligned_params.UpdateParams();
            }

            SurfaceParams new_params = *surface;
            new_params.addr = std::min(aligned_params.addr, surface->addr);
            new_params.end = std::max(aligned_params.end, surface->end);
            new_params.size = new_params.end - new_params.addr;
            new_params.height =
                new_params.size / aligned_params.BytesInPixels(aligned_params.stride);
            ASSERT(new_params.size % aligned_params.BytesInPixels(aligned_params.stride) == 0);

            Surface new_surface = CreateSurface(new_params);
            DuplicateSurface(surface, new_surface);

            // Delete the expanded surface, this can't be done safely yet
            // because it may still be in use
            surface->UnlinkAllWatcher(); // unlink watchers as if this surface is already deleted
            remove_surfaces.emplace(surface);

            surface = new_surface;
            RegisterSurface(new_surface);
        }
    }

    // No subrect found - create and return a new surface
    if (surface == nullptr) {
        if (aligned_params.width != aligned_params.stride) {
            // Can't have gaps in a surface
            aligned_params.width = aligned_params.stride;
            aligned_params.UpdateParams();
        }
        // GetSurface will create the new surface and possibly adjust res_scale if necessary
        surface = GetSurface(aligned_params, match_res_scale, load_if_create);
    } else if (load_if_create) {
        ValidateSurface(surface, aligned_params.addr, aligned_params.size);
    }

    surface->last_used_frame = VideoCore::GetCurrentFrame();

    return std::make_tuple(surface, surface->GetScaledSubRect(params));
}

Surface RasterizerCacheOpenGL::GetTextureSurface(
    const Pica::TexturingRegs::FullTextureConfig& config) {
    Pica::Texture::TextureInfo info =
        Pica::Texture::TextureInfo::FromPicaRegister(config.config, config.format);
    return GetTextureSurface(info, config.config.lod.max_level);
}

Surface RasterizerCacheOpenGL::GetTextureSurface(const Pica::Texture::TextureInfo& info,
                                                 u32 max_level) {
    if (info.physical_address == 0) {
        return nullptr;
    }

    SurfaceParams params;
    params.addr = info.physical_address;
    params.width = info.width;
    params.height = info.height;
    params.is_tiled = true;
    params.pixel_format = SurfaceParams::PixelFormatFromTextureFormat(info.format);
    params.UpdateParams();

    u32 min_width = info.width >> max_level;
    u32 min_height = info.height >> max_level;
    if (min_width % 8 != 0 || min_height % 8 != 0) {
        // LOG_CRITICAL(Render_OpenGL, "Texture size ({}x{}) is not multiple of 8", min_width,
        //             min_height);
        // return nullptr;
        Surface src_surface;
        Common::Rectangle<u32> rect;
        std::tie(src_surface, rect) = GetSurfaceSubRect(params, ScaleMatch::Ignore, true);

        params.res_scale = src_surface->res_scale;
        Surface tmp_surface = CreateSurface(params);
        BlitTextures(src_surface, rect, tmp_surface, tmp_surface->GetScaledRect());

        remove_surfaces.emplace(tmp_surface);
        return tmp_surface;
    }

    auto surface = GetSurface(params, ScaleMatch::Ignore, true);

    // Update mipmap if necessary
    if (max_level != 0) {
        if (info.width != (min_width << max_level) || info.height != (min_height << max_level)) {
            for (u32 i = 1; i < max_level; ++i) {
                if ((info.width >> i) == 8) {
                    if (max_level > i) {
                        max_level = i;
                        break;
                    }
                }
                if ((info.height >> i) == 8) {
                    if (max_level > i) {
                        max_level = i;
                        break;
                    }
                }
            }
            min_width = info.width >> max_level;
            min_height = info.height >> max_level;
            if (info.width != (min_width << max_level) ||
                info.height != (min_height << max_level)) {
                LOG_CRITICAL(Render_OpenGL,
                             "Texture size ({}x{}) does not support required mipmap level ({})",
                             params.width, params.height, max_level);
                return nullptr;
            }
        }

        if (max_level >= 8) {
            // since PICA only supports texture size between 8 and 1024, there are at most eight
            // possible mipmap levels including the base.
            LOG_CRITICAL(Render_OpenGL, "Unsupported mipmap level {}", max_level);
            return nullptr;
        }
        const FormatTuple& format_tuple = GetFormatTuple(params.pixel_format);

        // Allocate more mipmap level if necessary
        if (surface->max_level < max_level) {
            GLuint old_tex = OpenGLState::BindTexture2D(0, surface->texture.handle);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_level);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            u32 width;
            u32 height;
            if (surface->custom_tex_info) {
                width = surface->custom_tex_info->width;
                height = surface->custom_tex_info->height;
            } else {
                width = surface->GetScaledWidth();
                height = surface->GetScaledHeight();
            }
            for (u32 level = surface->max_level + 1; level <= max_level; ++level) {
                glTexImage2D(GL_TEXTURE_2D, level, format_tuple.internal_format, width >> level,
                             height >> level, 0, format_tuple.format, format_tuple.type, nullptr);
            }
            surface->max_level = max_level;
            if (surface->custom_tex_info) {
                // TODO: proper mipmap support for custom textures
                glGenerateMipmap(GL_TEXTURE_2D);
                OpenGLState::BindTexture2D(0, old_tex);
                return surface;
            }
            OpenGLState::BindTexture2D(0, old_tex);
        }

        // Blit mipmaps that have been invalidated
        OpenGLState prev_state = OpenGLState::GetCurState();
        OpenGLState state;
        state.draw.read_framebuffer = g_read_framebuffer.handle;
        state.draw.draw_framebuffer = g_draw_framebuffer.handle;
        SurfaceParams params = *surface;
        for (u32 level = 1; level <= max_level; ++level) {
            // In PICA all mipmap levels are stored next to each other
            params.addr += params.width * params.height * params.GetFormatBpp() / 8;
            params.width /= 2;
            params.height /= 2;
            params.stride = 0; // reset stride and let UpdateParams re-initialize it
            params.UpdateParams();
            auto& watcher = surface->level_watchers[level - 1];
            if (!watcher || !watcher->Get()) {
                auto level_surface = GetSurface(params, ScaleMatch::Ignore, true);
                if (level_surface) {
                    watcher = level_surface->CreateWatcher();
                } else {
                    watcher = nullptr;
                }
            }

            if (watcher && !watcher->IsValid()) {
                auto level_surface = watcher->Get();
                if (!level_surface->invalid_regions.empty()) {
                    ValidateSurface(level_surface, level_surface->addr, level_surface->size);
                }
                state.SubApply();
                if (!surface->custom_tex_info) {
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           level_surface->texture.handle, 0);
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                           GL_TEXTURE_2D, 0, 0);

                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           surface->texture.handle, level);
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                           GL_TEXTURE_2D, 0, 0);

                    auto src_rect = level_surface->GetScaledRect();
                    auto dst_rect = params.GetScaledRect();
                    glBlitFramebuffer(src_rect.left, src_rect.bottom, src_rect.right, src_rect.top,
                                      dst_rect.left, dst_rect.bottom, dst_rect.right, dst_rect.top,
                                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
                watcher->Validate();
            }
        }
        prev_state.SubApply();
    }

    return surface;
}

const CachedTextureCube& RasterizerCacheOpenGL::GetTextureCube(const TextureCubeConfig& config) {
    auto hash_key = Common::ComputeHash64(&config, sizeof(config));
    auto& cube = texture_cube_cache[hash_key];

    struct Face {
        Face(std::shared_ptr<SurfaceWatcher>& watcher, PAddr address, GLenum gl_face)
            : watcher(watcher), address(address), gl_face(gl_face) {}
        std::shared_ptr<SurfaceWatcher>& watcher;
        PAddr address;
        GLenum gl_face;
    };

    const std::array<Face, 6> faces{{
        {cube.px, config.px, GL_TEXTURE_CUBE_MAP_POSITIVE_X},
        {cube.nx, config.nx, GL_TEXTURE_CUBE_MAP_NEGATIVE_X},
        {cube.py, config.py, GL_TEXTURE_CUBE_MAP_POSITIVE_Y},
        {cube.ny, config.ny, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y},
        {cube.pz, config.pz, GL_TEXTURE_CUBE_MAP_POSITIVE_Z},
        {cube.nz, config.nz, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z},
    }};

    Pica::Texture::TextureInfo tex_info;
    for (const Face& face : faces) {
        if (!face.watcher || !face.watcher->Get()) {
            tex_info.physical_address = face.address;
            tex_info.height = tex_info.width = config.width;
            tex_info.format = config.format;
            tex_info.SetDefaultStride();
            auto surface = GetTextureSurface(tex_info);
            if (surface) {
                face.watcher = surface->CreateWatcher();
            } else {
                // Can occur when texture address is invalid. We mark the watcher with nullptr
                // in this case and the content of the face wouldn't get updated. These are
                // usually leftover setup in the texture unit and games are not supposed to draw
                // using them.
                face.watcher = nullptr;
            }
        }
    }

    if (cube.texture.handle == 0) {
        for (const Face& face : faces) {
            if (face.watcher) {
                auto surface = face.watcher->Get();
                cube.res_scale = std::max(cube.res_scale, surface->res_scale);
            }
        }

        cube.texture.Create();
        AllocateTextureCube(
            cube.texture.handle,
            GetFormatTuple(CachedSurface::PixelFormatFromTextureFormat(config.format)),
            cube.res_scale * config.width);
    }

    u32 scaled_size = cube.res_scale * config.width;

    OpenGLState prev_state = OpenGLState::GetCurState();
    OpenGLState state;
    state.draw.read_framebuffer = g_read_framebuffer.handle;
    state.draw.draw_framebuffer = g_draw_framebuffer.handle;
    for (const Face& face : faces) {
        if (face.watcher && !face.watcher->IsValid()) {
            auto surface = face.watcher->Get();
            if (!surface->invalid_regions.empty()) {
                ValidateSurface(surface, surface->addr, surface->size);
            }
            state.SubApply();
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   surface->texture.handle, 0);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                   0, 0);

            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, face.gl_face,
                                   cube.texture.handle, 0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                   0, 0);

            auto src_rect = surface->GetScaledRect();
            glBlitFramebuffer(src_rect.left, src_rect.bottom, src_rect.right, src_rect.top, 0, 0,
                              scaled_size, scaled_size, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            face.watcher->Validate();
        }
    }
    prev_state.SubApply();
    return cube;
}

SurfaceSurfaceRect_Tuple RasterizerCacheOpenGL::GetFramebufferSurfaces(
    bool using_color_fb, bool using_depth_fb, const Common::Rectangle<s32>& viewport_rect) {
    const auto& config = Pica::g_state.regs.framebuffer.framebuffer;

    Common::Rectangle<u32> viewport_clamped{
        static_cast<u32>(std::clamp(viewport_rect.left, 0, static_cast<s32>(config.GetWidth()))),
        static_cast<u32>(std::clamp(viewport_rect.top, 0, static_cast<s32>(config.GetHeight()))),
        static_cast<u32>(std::clamp(viewport_rect.right, 0, static_cast<s32>(config.GetWidth()))),
        static_cast<u32>(
            std::clamp(viewport_rect.bottom, 0, static_cast<s32>(config.GetHeight())))};

    // get color and depth surfaces
    SurfaceParams color_params;
    color_params.is_tiled = true;
    color_params.res_scale = resolution_scale_factor;
    color_params.width = config.GetWidth();
    color_params.height = config.GetHeight();

    SurfaceParams depth_params = color_params;

    SurfaceInterval color_vp_interval;
    SurfaceInterval depth_vp_interval;

    Common::Rectangle<u32> depth_rect{};
    Surface depth_surface = nullptr;
    if (using_depth_fb) {
        depth_params.addr = config.GetDepthBufferPhysicalAddress();
        depth_params.pixel_format = SurfaceParams::PixelFormatFromDepthFormat(config.depth_format);
        depth_params.UpdateParams();
        depth_vp_interval = depth_params.GetSubRectInterval(viewport_clamped);

        std::tie(depth_surface, depth_rect) =
            GetSurfaceSubRect(depth_params, ScaleMatch::Exact, false);
    }

    Common::Rectangle<u32> color_rect{};
    Surface color_surface = nullptr;
    if (using_color_fb) {
        color_params.addr = config.GetColorBufferPhysicalAddress();
        color_params.pixel_format = SurfaceParams::PixelFormatFromColorFormat(config.color_format);
        color_params.UpdateParams();
        color_vp_interval = color_params.GetSubRectInterval(viewport_clamped);
        if (depth_surface && depth_rect.bottom > 0) {
            SurfaceParams new_params = color_params;
            new_params.height = depth_rect.top / depth_surface->res_scale;
            new_params.UpdateParams();
            std::tie(color_surface, color_rect) =
                GetSurfaceSubRect(new_params, ScaleMatch::Exact, false);
            color_rect.bottom += depth_rect.bottom;
        } else {
            std::tie(color_surface, color_rect) =
                GetSurfaceSubRect(color_params, ScaleMatch::Exact, false);
            // adjust depth surface
            if (depth_surface && color_rect.bottom > 0) {
                SurfaceParams new_params = depth_params;
                new_params.height = color_rect.top / color_surface->res_scale;
                new_params.UpdateParams();
                remove_surfaces.emplace(depth_surface);
                std::tie(depth_surface, depth_rect) =
                        GetSurfaceSubRect(new_params, ScaleMatch::Exact, false);
                depth_rect.bottom += color_rect.bottom;
            }
        }
    }

    if (color_surface != nullptr) {
        if (depth_surface != nullptr && color_rect != depth_rect) {
            // Color and Depth surfaces must have the same dimensions and offsets
            auto new_color_surface = GetSurface(color_params, ScaleMatch::Exact, false);
            if (new_color_surface != color_surface) {
                remove_surfaces.emplace(color_surface);
                color_surface = new_color_surface;
            }
            color_rect = color_surface->GetScaledRect();
            if (color_rect != depth_rect) {
                auto new_depth_surface = GetSurface(depth_params, ScaleMatch::Exact, false);
                if (new_depth_surface != depth_surface) {
                    remove_surfaces.emplace(depth_surface);
                    depth_surface = new_depth_surface;
                }
            }
        }
    } else if (depth_surface != nullptr) {
        color_rect = depth_rect;
    }

    if (color_surface != nullptr) {
        ValidateSurface(color_surface, boost::icl::first(color_vp_interval),
                        boost::icl::length(color_vp_interval));
        color_surface->InvalidateAllWatcher();
        color_surface->last_used_frame = VideoCore::GetCurrentFrame();
    }
    if (depth_surface != nullptr) {
        ValidateSurface(depth_surface, boost::icl::first(depth_vp_interval),
                        boost::icl::length(depth_vp_interval));
        depth_surface->InvalidateAllWatcher();
        depth_surface->last_used_frame = VideoCore::GetCurrentFrame();
    }

    return std::make_tuple(color_surface, depth_surface, color_rect);
}

Surface RasterizerCacheOpenGL::GetFillSurface(const GPU::Regs::MemoryFillConfig& config) {
    Surface new_surface = std::make_shared<CachedSurface>();

    new_surface->addr = config.GetStartAddress();
    new_surface->end = config.GetEndAddress();
    new_surface->size = new_surface->end - new_surface->addr;
    new_surface->type = SurfaceType::Fill;
    new_surface->res_scale = std::numeric_limits<u16>::max();

    std::memcpy(&new_surface->fill_data[0], &config.value_32bit, 4);
    if (config.fill_32bit) {
        new_surface->fill_size = 4;
    } else if (config.fill_24bit) {
        new_surface->fill_size = 3;
    } else {
        new_surface->fill_size = 2;
    }

    RegisterSurface(new_surface);
    new_surface->last_used_frame = VideoCore::GetCurrentFrame();
    return new_surface;
}

SurfaceRect_Tuple RasterizerCacheOpenGL::GetTexCopySurface(const SurfaceParams& params) {
    Common::Rectangle<u32> rect{};

    Surface match_surface = FindMatch<MatchFlags::TexCopy | MatchFlags::Invalid>(
        surface_cache, params, ScaleMatch::Ignore);

    if (match_surface != nullptr) {
        ValidateSurface(match_surface, params.addr, params.size);

        SurfaceParams match_subrect;
        if (params.width != params.stride) {
            const u32 tiled_size = match_surface->is_tiled ? 8 : 1;
            match_subrect = params;
            match_subrect.width = match_surface->PixelsInBytes(params.width) / tiled_size;
            match_subrect.stride = match_surface->PixelsInBytes(params.stride) / tiled_size;
            match_subrect.height *= tiled_size;
        } else {
            match_subrect = match_surface->FromInterval(params.GetInterval());
            ASSERT(match_subrect.GetInterval() == params.GetInterval());
        }

        rect = match_surface->GetScaledSubRect(match_subrect);
        match_surface->last_used_frame = VideoCore::GetCurrentFrame();
    }

    return std::make_tuple(match_surface, rect);
}

void RasterizerCacheOpenGL::DuplicateSurface(const Surface& src_surface,
                                             const Surface& dest_surface) {
    ASSERT(dest_surface->addr <= src_surface->addr && dest_surface->end >= src_surface->end);

    BlitSurfaces(src_surface, src_surface->GetScaledRect(), dest_surface,
                 dest_surface->GetScaledSubRect(*src_surface));

    dest_surface->invalid_regions -= src_surface->GetInterval();
    dest_surface->invalid_regions += src_surface->invalid_regions;

    SurfaceRegions regions;
    for (const auto& pair : RangeFromInterval(dirty_regions, src_surface->GetInterval())) {
        if (pair.second == src_surface) {
            regions += pair.first;
        }
    }
    for (const auto& interval : regions) {
        dirty_regions.set({interval, dest_surface});
    }
}

bool RasterizerCacheOpenGL::TryRemapSurface(const Surface& destination, const SurfaceParams& params,
                                               SurfaceInterval* remapped_interval,
                                               const SurfaceInterval* requested_interval) {
    static const bool byte_remap = [] {
        const char* value = std::getenv("CARA_GPU_BYTE_SURFACE_REMAP");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!byte_remap) {
        const auto eligible = [](const Surface& surface) {
            return surface->pixel_format == PixelFormat::RGBA8 && surface->res_scale == 1 &&
                   surface->width != 0 && surface->height != 0 && surface->width == surface->stride &&
                   (!surface->is_tiled || ((surface->width | surface->height) & 7) == 0) &&
                   (surface->addr & 3) == 0 && surface->texture.handle != 0 && !surface->custom_tex_info;
        };
        if (!GLES || !eligible(destination)) return false;
        const auto interval = params.GetInterval();
        if (params.addr < destination->addr || params.end > destination->end) return false;
        for (const auto& group : RangeFromInterval(surface_cache, interval)) {
            for (const auto& source : group.second) {
                if (source == destination || !eligible(source) ||
                    source->texture.handle == destination->texture.handle ||
                    params.addr < source->addr || params.end > source->end ||
                    !source->IsRegionValid(interval) || destination->addr < source->addr ||
                    destination->end > source->end) continue;
                Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::SurfaceRemap);
                if (!surface_remapper) surface_remapper = std::make_unique<SurfaceRemapper>();
                if (!surface_remapper->RemapRGBA8(source->texture.handle, *source,
                                                  destination->texture.handle, *destination,
                                                  destination->GetSubRect(params))) continue;
                destination->InvalidateAllWatcher();
                if (Common::FrameTiming::counters.enabled) {
                    ++Common::FrameTiming::counters.surface_remap_calls;
                    Common::FrameTiming::counters.surface_remap_bytes += params.size;
                }
                if (remapped_interval) *remapped_interval = interval;
                return true;
            }
        }
        return false;
    }
    const auto eligible = [](const Surface& surface) {
        return (surface->pixel_format == PixelFormat::RGBA8 ||
                surface->pixel_format == PixelFormat::RGB8) && surface->res_scale == 1 &&
               surface->width != 0 && surface->height != 0 && surface->width == surface->stride &&
               (!surface->is_tiled || ((surface->width | surface->height) & 7) == 0) &&
               surface->texture.handle != 0 && !surface->custom_tex_info;
    };
    if (!GLES || !eligible(destination)) return false;
    const auto interval = requested_interval ? *requested_interval : params.GetInterval();
    if (params.addr < destination->addr || params.end > destination->end) return false;
    for (const auto& group : RangeFromInterval(surface_cache, interval)) {
        for (const auto& source : group.second) {
            if (source == destination || !eligible(source) ||
                source->texture.handle == destination->texture.handle) continue;
            // Split at valid source ownership boundaries before selecting a whole
            // pixel/tile rectangle. Never align outwards across CPU-owned bytes.
            const auto valid_regions = SurfaceRegions(interval & source->GetInterval()) -
                                       source->invalid_regions;
            for (const auto& valid : valid_regions) {
                const u64 unit = destination->BytesInPixels(destination->is_tiled ? 64 : 1);
                const u64 row = destination->BytesInPixels(destination->stride *
                                                          (destination->is_tiled ? 8 : 1));
                const auto span = SurfaceRemapLayout::RectangleSpan(destination->addr, row, unit,
                    {boost::icl::first(valid), boost::icl::last_next(valid)});
                if (!span) continue;
                const SurfaceInterval candidate(static_cast<PAddr>(span.begin),
                                                static_cast<PAddr>(span.end));
                if (!remapped_interval && candidate != interval) continue;
                const auto copy_params = destination->FromInterval(candidate);
                if (copy_params.GetInterval() != candidate || !source->IsRegionValid(candidate)) continue;
                Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::SurfaceRemap);
                if (!surface_remapper) surface_remapper = std::make_unique<SurfaceRemapper>();
                if (!surface_remapper->RemapBytes(source->texture.handle, *source,
                                                  destination->texture.handle, *destination,
                                                  destination->GetSubRect(copy_params))) continue;
                destination->InvalidateAllWatcher();
                if (Common::FrameTiming::counters.enabled) {
                    ++Common::FrameTiming::counters.surface_remap_calls;
                    Common::FrameTiming::counters.surface_remap_bytes += span.end - span.begin;
                }
                if (remapped_interval) *remapped_interval = candidate;
                return true;
            }
        }
    }
    return false;
}

void RasterizerCacheOpenGL::ValidateSurface(const Surface& surface, PAddr addr, u32 size) {
    if (size == 0)
        return;

    const SurfaceInterval validate_interval(addr, addr + size);

    if (surface->type == SurfaceType::Fill) {
        // Sanity check, fill surfaces will always be valid when used
        ASSERT(surface->IsRegionValid(validate_interval));
        return;
    }

    auto validate_regions = surface->invalid_regions & validate_interval;
    auto notify_validated = [&](SurfaceInterval interval) {
        surface->invalid_regions.erase(interval);
        validate_regions.erase(interval);
    };

    while (true) {
        const auto it = validate_regions.begin();
        if (it == validate_regions.end())
            break;

        const auto interval = *it & validate_interval;
        // Look for a valid surface to copy from
        SurfaceParams params = surface->FromInterval(interval);

        Surface copy_surface =
            FindMatch<MatchFlags::Copy>(surface_cache, params, ScaleMatch::Ignore, interval);
        if (copy_surface != nullptr) {
            SurfaceInterval copy_interval = params.GetCopyableInterval(copy_surface);
            CopySurface(copy_surface, surface, copy_interval);
            notify_validated(copy_interval);
            continue;
        }

        // Preserve guest byte addressing across a tiling/stride alias without
        // synchronously exporting the source render target through CPU memory.
        SurfaceInterval remapped_interval;
        if (gpu_surface_remap && TryRemapSurface(surface, params, &remapped_interval, &interval)) {
            notify_validated(remapped_interval);
            continue;
        }

        // Try to find surface in cache with different format
        // that can can be reinterpreted to the requested format.
        if (ValidateByReinterpretation(surface, params, interval)) {
            notify_validated(interval);
            continue;
        }

        // Could not find a matching reinterpreter, check if we need to implement a
        // reinterpreter
        if (NoUnimplementedReinterpretations(surface, params, interval) &&
            !IntervalHasInvalidPixelFormat(params, interval)) {
            // No surfaces were found in the cache that had a matching bit-width.
            // If the region was created entirely on the GPU,
            // assume it was a developer mistake and skip flushing.
            if (boost::icl::contains(dirty_regions, interval)) {
                LOG_DEBUG(Render_OpenGL, "Region created fully on GPU and reinterpretation is "
                                         "invalid. Skipping validation");
                validate_regions.erase(interval);
                continue;
            }
        }

        // Load data from 3DS memory
        if (surface->pixel_format < PixelFormat::D16) {
            {
                const auto previous_reason = flush_reason;
                const auto previous_destination = validation_destination;
                if (trace_flushes) validation_destination = surface->FromInterval(interval);
                flush_reason = FlushReason::Validation;
                SCOPE_EXIT({ flush_reason = previous_reason; validation_destination = previous_destination; });
                FlushRegion(params.addr, params.size);
            }
            surface->LoadGLBuffer(params.addr, params.end);
            surface->UploadGLTexture(surface->GetSubRect(params));
        } else {
            LOG_INFO(Render_OpenGL, "ValidateSurface load depth: {}", (u32)surface->pixel_format);
        }
        notify_validated(params.GetInterval());
    }
}

bool RasterizerCacheOpenGL::NoUnimplementedReinterpretations(const Surface& surface,
                                                             SurfaceParams& params,
                                                             const SurfaceInterval& interval) {
    const bool profile = Common::FrameTiming::RenderBookkeepingProfileEnabled();
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::ReinterpretCheck, profile);
    static const bool reuse = [] {
        const char* value = std::getenv("CARA_RENDER_BOOKKEEPING_REUSE");
        return value && std::strcmp(value, "1") == 0;
    }();
    const bool count_lookups = Common::FrameTiming::counters.enabled && (profile || reuse);
    // RGBA8 is the only repeated entry (nine implicit zero initializers).
    // The first matching format for 32bpp is RGBA8, so one cached slot suffices.
    ReinterpretLookupCache<PixelFormat, Surface, 1> lookup_cache;
    bool seen_rgba8 = false;
    if (count_lookups) ++Common::FrameTiming::counters.render_reinterpret_invocations;
    static constexpr std::array<PixelFormat, 17> all_formats{
        PixelFormat::RGBA8, PixelFormat::RGB8,   PixelFormat::RGB5A1, PixelFormat::RGB565,
        PixelFormat::RGBA4, PixelFormat::D16,    PixelFormat::D24, PixelFormat::D24S8,
    };
    bool implemented = true;
    for (PixelFormat format : all_formats) {
        if (SurfaceParams::GetFormatBpp(format) == surface->GetFormatBpp()) {
            params.pixel_format = format;
            // This could potentially be expensive,
            // although experimentally it hasn't been too bad
            if (count_lookups) {
                if (format == PixelFormat::RGBA8) {
                    if (seen_rgba8) ++Common::FrameTiming::counters.render_reinterpret_repeat_opportunities;
                    seen_rgba8 = true;
                }
            }
            const auto result = lookup_cache.Get(reuse, format, [&] {
                if (count_lookups) ++Common::FrameTiming::counters.render_reinterpret_find_calls;
                return FindMatch<MatchFlags::Copy>(surface_cache, params, ScaleMatch::Ignore, interval);
            });
            if (count_lookups && result.reused)
                ++Common::FrameTiming::counters.render_reinterpret_avoided_calls;
            const Surface& test_surface = result.value;
            if (test_surface != nullptr) {
                LOG_WARNING(Render_OpenGL, "Missing pixel_format reinterpreter: {} -> {}", (u32)format,
                            (u32)surface->pixel_format);
                implemented = false;
            }
        }
    }
    return implemented;
}

bool RasterizerCacheOpenGL::IntervalHasInvalidPixelFormat(SurfaceParams& params,
                                                          const SurfaceInterval& interval) {
    params.pixel_format = PixelFormat::Invalid;
    for (const auto& set : RangeFromInterval(surface_cache, interval))
        for (const auto& surface : set.second)
            if (surface->pixel_format == PixelFormat::Invalid) {
                LOG_WARNING(Render_OpenGL, "Surface found with invalid pixel format");
                return true;
            }
    return false;
}

bool RasterizerCacheOpenGL::ValidateByReinterpretation(const Surface& surface,
                                                       SurfaceParams& params,
                                                       const SurfaceInterval& interval) {
    for (const auto& entity : format_reinterpreter->GetReinterpreters()) {
        if (entity.dst_format != surface->pixel_format) {
            continue;
        }
        params.pixel_format = entity.src_format;
        Surface reinterpret_surface =
            FindMatch<MatchFlags::Copy>(surface_cache, params, ScaleMatch::Ignore, interval);
        if (reinterpret_surface != nullptr) {
            SurfaceInterval reinterpret_interval = params.GetCopyableInterval(reinterpret_surface);
            SurfaceParams reinterpret_params = surface->FromInterval(reinterpret_interval);
            auto src_rect = reinterpret_surface->GetScaledSubRect(reinterpret_params);
            auto dest_rect = surface->GetScaledSubRect(reinterpret_params);
            entity.reinterpreter->Reinterpret(reinterpret_surface->texture.handle, src_rect,
                                              g_read_framebuffer.handle, surface->texture.handle,
                                              dest_rect, g_draw_framebuffer.handle);
            return true;
        }
    }
    return false;
}

void RasterizerCacheOpenGL::FlushRegion(PAddr addr, u32 size, const Surface& flush_surface) {
    if (size == 0 || dirty_regions.empty() || dirty_regions.rbegin()->first.upper() < addr) {
        return;
    }

    const SurfaceInterval flush_interval(addr, addr + size);
    SurfaceRegions flushed_intervals;

    for (auto& pair : RangeFromInterval(dirty_regions, flush_interval)) {
        // small sizes imply that this most likely comes from the cpu, flush the entire region
        // the point is to avoid thousands of small writes every frame if the cpu decides to
        // access that region, anything higher than 8 you're guaranteed it comes from a service
        const auto interval = size <= 8 ? pair.first : pair.first & flush_interval;
        auto& surface = pair.second;

        if (flush_surface != nullptr && surface != flush_surface)
            continue;

        if (!GLES || surface->pixel_format < PixelFormat::D16) {
            if (surface->type != SurfaceType::Fill) {
                SurfaceParams params = surface->FromInterval(interval);
                const auto rect = surface->GetSubRect(params);
                if (trace_flushes) {
                    const auto started = std::chrono::steady_clock::now();
                    surface->DownloadGLTexture(rect);
                    RecordFlush(surface, size, rect.GetWidth(), rect.GetHeight(),
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
                } else {
                    surface->DownloadGLTexture(rect);
                }
            }
            surface->FlushGLBuffer(boost::icl::first(interval), boost::icl::last_next(interval));
        }
        flushed_intervals += interval;
    }
    // Reset dirty regions
    dirty_regions -= flushed_intervals;
}

void RasterizerCacheOpenGL::FlushAll() {
    const auto previous_reason = flush_reason;
    flush_reason = FlushReason::All;
    SCOPE_EXIT({ flush_reason = previous_reason; });
    FlushRegion(0, 0xFFFFFFFF);
}

void RasterizerCacheOpenGL::CleanUp(u32 deadline_frame) {
    const SurfaceInterval interval(0, 0xFFFFFFFF);
    std::vector<Surface> unused;
    for (const auto& pair : RangeFromInterval(surface_cache, interval)) {
        auto& surfaceSet = pair.second;
        for (const auto& surface : surfaceSet) {
            if (surface->last_used_frame < deadline_frame) {
                unused.push_back(surface);
            }
        }
    }
    static const bool selective = [] {
        const char* value = std::getenv("CARA_SELECTIVE_SURFACE_CLEANUP");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (selective) {
        // Interval-map segments may list the same surface more than once. Only
        // export bytes owned by each evicted producer; a stale clean alias must
        // not force unrelated live render targets back through guest RAM.
        SurfaceSet evicted(unused.begin(), unused.end());
        // Deferred framebuffer replacement can unregister an old producer while
        // dirty_regions still owns its untouched bytes. These orphan producers
        // no longer appear in surface_cache, but must be drained before their
        // final GPU allocation is released. Registered live owners stay resident.
        for (const auto& entry : dirty_regions)
            if (!entry.second->registered) evicted.insert(entry.second);
        const auto previous_reason = flush_reason;
        flush_reason = FlushReason::Cleanup;
        SCOPE_EXIT({ flush_reason = previous_reason; });
        for (const auto& surface : evicted) {
            FlushRegion(surface->addr, surface->size, surface);
            UnregisterSurface(surface);
        }
        return;
    }
    if (!unused.empty()) {
        FlushAll();
        for (const auto& surface : unused) {
            UnregisterSurface(surface);
        }
    }
}

u16 RasterizerCacheOpenGL::GetScaleFactor() const {
    return resolution_scale_factor;
}

void RasterizerCacheOpenGL::SetScaleFactor(u16 scale) {
    FlushAll();
    while (!surface_cache.empty())
        UnregisterSurface(*surface_cache.begin()->second.begin());
    texture_cube_cache.clear();
    resolution_scale_factor = scale;
}

void RasterizerCacheOpenGL::InvalidateRegion(PAddr addr, u32 size, const Surface& region_owner) {
    if (size == 0 || surface_cache.empty() || surface_cache.rbegin()->first.upper() < addr) {
        return;
    }

    const SurfaceInterval invalid_interval(addr, addr + size);

    if (region_owner != nullptr) {
        ASSERT(region_owner->type != SurfaceType::Texture);
        ASSERT(addr >= region_owner->addr && addr + size <= region_owner->end);
        // Surfaces can't have a gap
        ASSERT(region_owner->width == region_owner->stride);
        region_owner->invalid_regions.erase(invalid_interval);
    }

    for (const auto& pair : RangeFromInterval(surface_cache, invalid_interval)) {
        for (const auto& cached_surface : pair.second) {
            if (cached_surface == region_owner)
                continue;

            // If cpu is invalidating this region we want to remove it
            // to (likely) mark the memory pages as uncached
            const bool retain_small_write = retain_small_cpu_writes && !Settings::values.skip_cpu_write &&
                                            cached_surface->pixel_format == PixelFormat::RGBA8;
            if (region_owner == nullptr && size <= 8 && !retain_small_write) {
                if (Settings::values.skip_cpu_write) {
                    continue;
                }
                {
                    const auto previous_reason = flush_reason;
                    flush_reason = FlushReason::SmallCPUWrite;
                    SCOPE_EXIT({ flush_reason = previous_reason; });
                    FlushRegion(cached_surface->addr, cached_surface->size, cached_surface);
                }
                remove_surfaces.emplace(cached_surface);
                if (Common::FrameTiming::counters.enabled)
                    ++Common::FrameTiming::counters.cpu_surface_invalidations;
                LOG_WARNING(Render_OpenGL, "invalidate region by cpu");
                continue;
            }

            // The retained path uses the same interval ownership as bulk CPU writes:
            // discard only overwritten GPU bytes and preserve every other dirty byte.
            const auto interval = cached_surface->GetInterval() & invalid_interval;
            cached_surface->invalid_regions.insert(interval);
            cached_surface->InvalidateAllWatcher();

            // Remove only "empty" fill surfaces to avoid destroying and recreating OGL textures
            if (cached_surface->type == SurfaceType::Fill &&
                cached_surface->IsSurfaceFullyInvalid()) {
                remove_surfaces.emplace(cached_surface);
            }
        }
    }

    if (region_owner != nullptr)
        dirty_regions.set({invalid_interval, region_owner});
    else
        dirty_regions.erase(invalid_interval);

    for (const auto& remove_surface : remove_surfaces) {
        if (remove_surface == region_owner) {
            Surface expanded_surface = FindMatch<MatchFlags::SubRect | MatchFlags::Invalid>(
                surface_cache, *region_owner, ScaleMatch::Ignore);
            ASSERT(expanded_surface);

            if ((region_owner->invalid_regions - expanded_surface->invalid_regions).empty()) {
                DuplicateSurface(region_owner, expanded_surface);
            } else {
                continue;
            }
        }
        UnregisterSurface(remove_surface);
    }
    remove_surfaces.clear();
}

Surface RasterizerCacheOpenGL::CreateSurface(const SurfaceParams& params) {
    Surface surface = std::make_shared<CachedSurface>();
    static_cast<SurfaceParams&>(*surface) = params;

    surface->texture.Create();
    surface->invalid_regions.insert(surface->GetInterval());
    AllocateSurfaceTexture(surface->texture.handle, GetFormatTuple(surface->pixel_format),
                           surface->GetScaledWidth(), surface->GetScaledHeight());

    return surface;
}

void RasterizerCacheOpenGL::RegisterSurface(const Surface& surface) {
    if (surface->registered) {
        return;
    }
    surface->registered = true;
    AdvanceTextureLookupGeneration();
    surface_cache.add({surface->GetInterval(), SurfaceSet{surface}});
    UpdatePagesCachedCount(surface->addr, surface->size, 1);
}

void RasterizerCacheOpenGL::UnregisterSurface(const Surface& surface) {
    if (!surface->registered) {
        return;
    }
    surface->registered = false;
    AdvanceTextureLookupGeneration();
    UpdatePagesCachedCount(surface->addr, surface->size, -1);
    surface_cache.subtract({surface->GetInterval(), SurfaceSet{surface}});
}

void RasterizerCacheOpenGL::UpdatePagesCachedCount(PAddr addr, u32 size, int delta) {
    const u32 num_pages =
        ((addr + size - 1) >> Memory::PAGE_BITS) - (addr >> Memory::PAGE_BITS) + 1;
    const u32 page_start = addr >> Memory::PAGE_BITS;
    const u32 page_end = page_start + num_pages;

    // Interval maps will erase segments if count reaches 0, so if delta is negative we have to
    // subtract after iterating
    const auto pages_interval = PageMap::interval_type::right_open(page_start, page_end);
    if (delta > 0)
        cached_pages.add({pages_interval, delta});

    for (const auto& pair : RangeFromInterval(cached_pages, pages_interval)) {
        const auto interval = pair.first & pages_interval;
        const int count = pair.second;

        const PAddr interval_start_addr = boost::icl::first(interval) << Memory::PAGE_BITS;
        const PAddr interval_end_addr = boost::icl::last_next(interval) << Memory::PAGE_BITS;
        const u32 interval_size = interval_end_addr - interval_start_addr;

        if (count == delta) {
            VideoCore::Memory()->RasterizerMarkRegionCached(interval_start_addr, interval_size,
                                                            true);
        } else if (count == -delta) {
            VideoCore::Memory()->RasterizerMarkRegionCached(interval_start_addr, interval_size,
                                                            false);
        }
    }

    if (delta < 0)
        cached_pages.add({pages_interval, delta});
}

} // namespace OpenGL
