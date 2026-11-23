#include "video_core/renderer_opengl/gl_backlog_profile.h"
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
#include "video_core/renderer_opengl/gl_command_queue.h"
#endif
#include "video_core/renderer_opengl/gl_driver_transactions.h"
// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include "common/scope_exit.h"
#include "video_core/renderer_opengl/gl_native_rgba8_remap.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_surface_remap.h"
#include "video_core/renderer_opengl/gl_surface_remap_shaders.h"

namespace OpenGL {

bool SurfaceRemapper::Initialize() {
    if (initialized)
        return program.handle != 0;
    initialized = true;
    const char* fragment = SurfaceRemapShaders::Fragment;
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    const char* requested = std::getenv("CARA_REMAP_TEXEL_PAIR");
    std::string variant;
    if (requested && std::strcmp(requested, "1") == 0) {
        variant = std::string("#define CARA_REMAP_TEXEL_PAIR\n") + fragment;
        fragment = variant.c_str();
    }
#endif
    program.Create(SurfaceRemapShaders::Vertex, fragment);
    if (!program.handle)
        return false;
    vao.Create();
    framebuffer.Create();
    const GLuint previous = OpenGLState::BindShaderProgram(program.handle);
    glUniform1i(glGetUniformLocation(program.handle, "source_texture"), 0);
    source_layout_location = glGetUniformLocation(program.handle, "source_layout");
    destination_layout_location = glGetUniformLocation(program.handle, "destination_layout");
    byte_offset_location = glGetUniformLocation(program.handle, "byte_offset");
    bytes_per_pixel_location = glGetUniformLocation(program.handle, "bytes_per_pixel");
    native_reverse_location = glGetUniformLocation(program.handle, "native_reverse");
    OpenGLState::BindShaderProgram(previous);
    return true;
}

bool SurfaceRemapper::RemapRGBA8(GLuint source_texture, const SurfaceParams& source,
                               GLuint destination_texture, const SurfaceParams& destination,
                               const Common::Rectangle<u32>& rectangle, bool native_rgba8) {
    if (source.pixel_format != SurfaceParams::PixelFormat::RGBA8 ||
        destination.pixel_format != SurfaceParams::PixelFormat::RGBA8) return false;
    return RemapBytes(source_texture, source, destination_texture, destination, rectangle, native_rgba8);
}

bool SurfaceRemapper::RemapBytes(GLuint source_texture, const SurfaceParams& source,
                               GLuint destination_texture, const SurfaceParams& destination,
                               const Common::Rectangle<u32>& rectangle, bool native_rgba8) {
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    CommandQueue::Backlog::Scope scope(CommandQueue::Backlog::Source::SurfaceRemap);
#endif
    if (const auto* bridge=DriverTransactions::Pending())
        return bridge->sync_bool(DriverTransactions::Kind::Remap,[&] {
            return RemapBytes(source_texture,source,destination_texture,destination,rectangle,native_rgba8);
        });
    const auto native_encoding = PlanNativeRGBA8RemapEncoding(
        source.pixel_format == SurfaceParams::PixelFormat::RGBA8,
        destination.pixel_format == SurfaceParams::PixelFormat::RGBA8,
        source.addr, destination.addr, source.is_tiled, destination.is_tiled);
    if ((!GLES && (!native_rgba8 || !native_encoding.supported ||
                   source.type != SurfaceParams::SurfaceType::Color ||
                   destination.type != SurfaceParams::SurfaceType::Color)) || (GLES && native_rgba8))
        return false;
    const auto supported = [](const SurfaceParams& surface) {
        return (surface.pixel_format == SurfaceParams::PixelFormat::RGBA8 ||
                surface.pixel_format == SurfaceParams::PixelFormat::RGB8) &&
               surface.res_scale == 1 && surface.width != 0 && surface.height != 0 &&
               surface.width == surface.stride &&
               u64(surface.width) * surface.height * (surface.GetFormatBpp() / 8) == surface.size &&
               surface.size <= u32(std::numeric_limits<GLint>::max()) &&
               (!surface.is_tiled || ((surface.width | surface.height) & 7) == 0);
    };
    if (!source_texture || !destination_texture || source_texture == destination_texture ||
        !supported(source) || !supported(destination) ||
        rectangle.left >= rectangle.right || rectangle.bottom >= rectangle.top ||
        rectangle.right > destination.width || rectangle.top > destination.height) {
        return false;
    }
    // Require whole tiles; the conservative enclosing interval must be inside
    // the source. The cache additionally chooses rectangles without row gaps.
    if (destination.is_tiled && ((rectangle.left | rectangle.right | rectangle.top |
                                  rectangle.bottom) & 7)) return false;
    const auto interval = destination.GetSubRectInterval(rectangle);
    const auto begin = boost::icl::first(interval);
    const auto end = boost::icl::last_next(interval);
    const s64 byte_offset = s64(destination.addr) - s64(source.addr);
    if (begin < source.addr || u64(end) > u64(source.addr) + source.size ||
        begin >= end || byte_offset < std::numeric_limits<GLint>::min() ||
        byte_offset > std::numeric_limits<GLint>::max()) return false;
    const OpenGLState previous = OpenGLState::GetCurState();
    GLint previous_active_texture = GL_TEXTURE0;
    GLboolean previous_dither = GL_FALSE, previous_discard = GL_FALSE;
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    const auto queued_state = CommandQueue::SaveRemapStateIfEnabled();
    if (!queued_state)
#endif
    {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
        previous_dither = glIsEnabled(GL_DITHER);
        previous_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    }
    SCOPE_EXIT({
        previous.Apply();
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
        if (queued_state) {
            CommandQueue::RestoreRemapState(queued_state);
        } else
#endif
        {
            if (previous_dither) glEnable(GL_DITHER);
            if (previous_discard) glEnable(GL_RASTERIZER_DISCARD);
            glActiveTexture(previous_active_texture);
        }
    });
    if (!Initialize())
        return false;

    OpenGLState state;
    state.cull.enabled = false;
    state.depth.write_mask = GL_FALSE;
    state.draw.read_framebuffer = previous.draw.read_framebuffer;
    state.draw.draw_framebuffer = framebuffer.handle;
    state.draw.shader_program = program.handle;
    state.draw.vertex_array = vao.handle;
    state.texture_units[0].texture_2d = source_texture;
    state.viewport = {static_cast<GLint>(rectangle.left), static_cast<GLint>(rectangle.bottom),
                      static_cast<GLsizei>(rectangle.GetWidth()),
                      static_cast<GLsizei>(rectangle.GetHeight())};
    state.Apply();
    glDisable(GL_DITHER);
    glDisable(GL_RASTERIZER_DISCARD);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           destination_texture, 0);
    SCOPE_EXIT({
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    });
    // This private VAO uses gl_VertexID. The producer owns all renderer metadata
    // and waits for the real framebuffer status and draw error before returning.
    const auto checked_draw = [&] {
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        glUniform4i(source_layout_location, source.width, source.height, source.stride, source.is_tiled);
        glUniform4i(destination_layout_location, destination.width, destination.height,
                    destination.stride, destination.is_tiled);
        glUniform1i(byte_offset_location, static_cast<GLint>(byte_offset));
        glUniform2i(bytes_per_pixel_location, source.GetFormatBpp() / 8,
                    destination.GetFormatBpp() / 8);
        glUniform1i(native_reverse_location, !GLES && native_encoding.reverse_components);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        return glGetError() == GL_NO_ERROR;
    };
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    if (queued_state) return CommandQueue::CheckedRemapDraw(checked_draw);
#endif
    return checked_draw();
}

} // namespace OpenGL
