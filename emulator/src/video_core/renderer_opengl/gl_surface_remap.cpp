// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/scope_exit.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_surface_remap.h"

namespace OpenGL {

bool SurfaceRemapper::Initialize() {
    if (initialized)
        return program.handle != 0;
    initialized = true;
    constexpr const char* vertex = R"(
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";
    constexpr const char* fragment = R"(
uniform highp sampler2D source_texture;
// Width, height, stride, tiled flag. Coordinates in textures are GL coordinates.
uniform highp ivec4 source_layout;
uniform highp ivec4 destination_layout;
uniform highp int pixel_offset;
out highp vec4 color;

int interleave(int x, int y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
           ((x & 4) << 2) | ((y & 4) << 3);
}
int deinterleave(int value) {
    return (value & 1) | ((value >> 1) & 2) | ((value >> 2) & 4);
}
void main() {
    ivec2 destination = ivec2(gl_FragCoord.xy);
    int pixel;
    if (destination_layout.w != 0) {
        int y = destination_layout.y - 1 - destination.y;
        pixel = (y / 8) * destination_layout.z * 8 + (destination.x / 8) * 64 +
                interleave(destination.x & 7, y & 7);
    } else {
        pixel = destination.y * destination_layout.z + destination.x;
    }
    pixel += pixel_offset;
    ivec2 source;
    if (source_layout.w != 0) {
        int tile = pixel / 64;
        int within = pixel & 63;
        source.x = (tile % (source_layout.z / 8)) * 8 + deinterleave(within);
        source.y = source_layout.y - 1 -
                   ((tile / (source_layout.z / 8)) * 8 + deinterleave(within >> 1));
    } else {
        source = ivec2(pixel % source_layout.z, pixel / source_layout.z);
    }
    // Both views are RGBA8: the guest ABGR packing cancels between them.
    color = texelFetch(source_texture, source, 0);
}
)";
    program.Create(vertex, fragment);
    if (!program.handle)
        return false;
    vao.Create();
    framebuffer.Create();
    const GLuint previous = OpenGLState::BindShaderProgram(program.handle);
    glUniform1i(glGetUniformLocation(program.handle, "source_texture"), 0);
    source_layout_location = glGetUniformLocation(program.handle, "source_layout");
    destination_layout_location = glGetUniformLocation(program.handle, "destination_layout");
    pixel_offset_location = glGetUniformLocation(program.handle, "pixel_offset");
    OpenGLState::BindShaderProgram(previous);
    return true;
}

bool SurfaceRemapper::RemapRGBA8(GLuint source_texture, const SurfaceParams& source,
                               GLuint destination_texture, const SurfaceParams& destination,
                               const Common::Rectangle<u32>& rectangle) {
    const auto supported = [](const SurfaceParams& surface) {
        return surface.pixel_format == SurfaceParams::PixelFormat::RGBA8 &&
               surface.res_scale == 1 && surface.width != 0 && surface.height != 0 &&
               surface.width == surface.stride && (surface.addr & 3) == 0 &&
               u64(surface.width) * surface.height * 4 == surface.size &&
               (!surface.is_tiled || ((surface.width | surface.height) & 7) == 0);
    };
    if (!source_texture || !destination_texture || source_texture == destination_texture ||
        !supported(source) || !supported(destination) || destination.addr < source.addr ||
        u64(destination.addr) + destination.size > u64(source.addr) + source.size ||
        rectangle.left >= rectangle.right || rectangle.bottom >= rectangle.top ||
        rectangle.right > destination.width || rectangle.top > destination.height) {
        return false;
    }
    const OpenGLState previous = OpenGLState::GetCurState();
    GLint previous_active_texture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    const GLboolean previous_dither = glIsEnabled(GL_DITHER);
    const GLboolean previous_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    SCOPE_EXIT({
        if (previous_dither) glEnable(GL_DITHER);
        if (previous_discard) glEnable(GL_RASTERIZER_DISCARD);
        previous.Apply();
        glActiveTexture(previous_active_texture);
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
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    glUniform4i(source_layout_location, source.width, source.height, source.stride, source.is_tiled);
    glUniform4i(destination_layout_location, destination.width, destination.height,
                destination.stride, destination.is_tiled);
    glUniform1i(pixel_offset_location, (destination.addr - source.addr) / 4);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

} // namespace OpenGL
