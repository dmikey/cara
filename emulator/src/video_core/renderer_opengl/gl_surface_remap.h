// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/math_util.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_surface_params.h"

namespace OpenGL {

// Copies the same guest RGBA8 bytes between different linear/tiled layouts.
// The caller must establish valid source ownership for every destination byte.
class SurfaceRemapper {
public:
    bool RemapRGBA8(GLuint source_texture, const SurfaceParams& source,
                   GLuint destination_texture, const SurfaceParams& destination,
                   const Common::Rectangle<u32>& destination_rect);

private:
    bool Initialize();
    bool initialized = false;
    OGLProgram program;
    OGLVertexArray vao;
    OGLFramebuffer framebuffer;
    GLint source_layout_location = -1;
    GLint destination_layout_location = -1;
    GLint pixel_offset_location = -1;
};

} // namespace OpenGL
