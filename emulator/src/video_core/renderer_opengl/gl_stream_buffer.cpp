#include "video_core/renderer_opengl/gl_ios_compat.h"
// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <deque>
#include <cstdlib>
#include <cstring>
#include "common/frame_timing.h"
#include <vector>
#include "common/alignment.h"
#include "common/microprofile.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"

namespace OpenGL {

OGLStreamBuffer::OGLStreamBuffer(GLenum target, GLsizeiptr size) : gl_target(StreamBufferTarget(target)), buffer_size(size) {
    // Unmap flushes the entire mapped range unless FLUSH_EXPLICIT was requested.
    // Keep synchronization/invalidation flags identical in both experiments.
    const char* implicit = std::getenv("CARA_GL_IMPLICIT_FLUSH");
    explicit_flush = !(implicit && std::strcmp(implicit, "1") == 0);
    gl_buffer.Create();
    glBindBuffer(gl_target, gl_buffer.handle);
    // prefer `glBufferData` than `glBufferStorage` on mobile device
    glBufferData(gl_target, buffer_size, nullptr, GL_STREAM_DRAW);
    gl_target_invalidate_hack = Settings::values.stream_buffer_hack ? GL_TEXTURE_BUFFER : 0;
}

OGLStreamBuffer::~OGLStreamBuffer() {
    gl_buffer.Release();
}

GLuint OGLStreamBuffer::GetHandle() const {
    return gl_buffer.handle;
}

GLsizeiptr OGLStreamBuffer::GetSize() const {
    return buffer_size;
}

std::tuple<u8*, GLintptr, bool> OGLStreamBuffer::Map(GLsizeiptr size, GLintptr alignment) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::StreamMap);
    if (Common::FrameTiming::counters.enabled) Common::FrameTiming::counters.stream_mapped_bytes += size;
    bool invalidate = false;

    buffer_pos = Common::AlignUp<std::size_t>(buffer_pos, alignment);
    if (buffer_pos + size > buffer_size) {
        buffer_pos = 0;
        if (gl_target_invalidate_hack == 0 || gl_target == gl_target_invalidate_hack) {
            invalidate = true;
        }
    }

    GLbitfield flags = GL_MAP_WRITE_BIT | (explicit_flush ? GL_MAP_FLUSH_EXPLICIT_BIT : 0) |
                       (invalidate ? GL_MAP_INVALIDATE_BUFFER_BIT : GL_MAP_UNSYNCHRONIZED_BIT);
    u8* mapped_ptr = static_cast<u8*>(glMapBufferRange(gl_target, buffer_pos, size, flags));
    return std::make_tuple(mapped_ptr, buffer_pos, invalidate);
}

void OGLStreamBuffer::Unmap(GLsizeiptr size) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::StreamUnmap);
    if (size > 0 && explicit_flush) {
        // flush is relative to the start of the currently mapped range of buffer
        glFlushMappedBufferRange(gl_target, 0, size);
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            LOG_DEBUG(Render_OpenGL, "flush mapped buffer range error: {:04X}, target: {:04X}, offset: {}, size: {}, total: {}", error, gl_target, buffer_pos, size, buffer_size);
        }
    }
    glUnmapBuffer(gl_target);
    buffer_pos += size;
}

} // namespace OpenGL
