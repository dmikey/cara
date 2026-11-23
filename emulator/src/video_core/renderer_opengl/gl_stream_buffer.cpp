#include "video_core/renderer_opengl/gl_ios_compat.h"
// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <deque>
#include <cstdlib>
#include <cstring>
#include "common/frame_timing.h"
#include <vector>
#ifdef CARA_IOS_GLES
#include <stdexcept>
#endif
#include "common/alignment.h"
#include "common/microprofile.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include <stdexcept>

namespace OpenGL {

OGLStreamBuffer::OGLStreamBuffer(GLenum target, GLsizeiptr size) : gl_target(StreamBufferTarget(target)), buffer_size(size) {
    // Unmap flushes the entire mapped range unless FLUSH_EXPLICIT was requested.
    // Keep synchronization/invalidation flags identical in both experiments.
    const char* implicit = std::getenv("CARA_GL_IMPLICIT_FLUSH");
    explicit_flush = !(implicit && std::strcmp(implicit, "1") == 0);
    gl_buffer.Create();
    glBindBuffer(gl_target, gl_buffer.handle);
    gl_target_invalidate_hack = Settings::values.stream_buffer_hack ? GL_TEXTURE_BUFFER : 0;
#ifdef CARA_ANDROID_GLES_ATLAS
    const char* persistent_uniforms = std::getenv("CARA_GLES_PERSISTENT_UNIFORMS");
    if (gl_target == GL_UNIFORM_BUFFER && !CommandQueue::Enabled() &&
        persistent_uniforms && std::strcmp(persistent_uniforms, "1") == 0 &&
        GLAD_GL_EXT_buffer_storage && glBufferStorageEXT) {
        constexpr GLbitfield flags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
        const auto bytes = buffer_size * PersistentPageCount;
        glBufferStorageEXT(gl_target, bytes, nullptr, flags);
        if (glGetError() == GL_NO_ERROR)
            persistent_data = static_cast<u8*>(glMapBufferRange(gl_target, 0, bytes, flags));
        if (persistent_data)
            return;

        // Immutable allocation cannot fall back to BufferData on the same name.
        glGetError();
        gl_buffer.Release();
        gl_buffer.Create();
        glBindBuffer(gl_target, gl_buffer.handle);
    }
#endif
    // prefer `glBufferData` than `glBufferStorage` on mobile device
    glBufferData(gl_target, buffer_size, nullptr, GL_STREAM_DRAW);
}

OGLStreamBuffer::~OGLStreamBuffer() {
#ifdef CARA_ANDROID_GLES_ATLAS
    for (const auto fence : persistent_page_fences) {
        if (fence)
            glDeleteSync(fence);
    }
#endif
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
#ifdef CARA_ANDROID_GLES_ATLAS
    if (persistent_data) {
        if (size <= 0 || size > buffer_size || alignment <= 0 || buffer_size % alignment != 0)
            throw std::invalid_argument("Invalid persistent uniform buffer reservation");
        buffer_pos = Common::AlignUp<std::size_t>(buffer_pos, alignment);
        const bool rotate = buffer_pos + size > buffer_size;
        if (rotate) {
            // All draws reading the departing page have been submitted.
            auto& departing = persistent_page_fences[persistent_page];
            departing = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!departing)
                glFinish();

            persistent_page = (persistent_page + 1) % PersistentPageCount;
            auto& incoming = persistent_page_fences[persistent_page];
            if (incoming) {
                const auto ready = glClientWaitSync(incoming, 0, 0);
                if (ready == GL_TIMEOUT_EXPIRED) {
                    if (Common::FrameTiming::counters.enabled)
                        ++Common::FrameTiming::counters.persistent_uniform_waits;
                    const auto waited = glClientWaitSync(incoming, GL_SYNC_FLUSH_COMMANDS_BIT,
                                                         GL_TIMEOUT_IGNORED);
                    if (waited != GL_CONDITION_SATISFIED && waited != GL_ALREADY_SIGNALED)
                        glFinish();
                } else if (ready != GL_CONDITION_SATISFIED && ready != GL_ALREADY_SIGNALED) {
                    glFinish();
                }
                glDeleteSync(incoming);
                incoming = nullptr;
            }
            buffer_pos = 0;
        }
        if (Common::FrameTiming::counters.enabled)
            ++Common::FrameTiming::counters.persistent_uniform_writes;
        const auto offset = persistent_page * buffer_size + buffer_pos;
        // The caller repopulates all bound, live uniforms at each rotation.
        return {persistent_data + offset, offset, rotate};
    }
#endif
    if (CommandQueue::Enabled() &&
        (size <= 0 || size > buffer_size || alignment <= 0 || queued_reserved != 0))
        throw std::invalid_argument("Invalid owned stream upload reservation");
    bool invalidate = false;

    buffer_pos = Common::AlignUp<std::size_t>(buffer_pos, alignment);
    if (buffer_pos + size > buffer_size) {
        buffer_pos = 0;
        if (gl_target_invalidate_hack == 0 || gl_target == gl_target_invalidate_hack) {
            invalidate = true;
        }
    }

    queued_native_map=CommandQueue::Enabled()&&size>256*1024-64;
    if (CommandQueue::Enabled()&&!queued_native_map) {
        queued_budget=CommandQueue::ReserveStreamPayload();
        queued_staging.resize(static_cast<std::size_t>(size));
        queued_reserved = size;
        queued_orphan = invalidate;
        return std::make_tuple(queued_staging.data(), buffer_pos, invalidate);
    }

    GLbitfield flags = GL_MAP_WRITE_BIT | (explicit_flush ? GL_MAP_FLUSH_EXPLICIT_BIT : 0) |
                       (invalidate ? GL_MAP_INVALIDATE_BUFFER_BIT : GL_MAP_UNSYNCHRONIZED_BIT);
    u8* mapped_ptr = static_cast<u8*>(glMapBufferRange(gl_target, buffer_pos, size, flags));
    return std::make_tuple(mapped_ptr, buffer_pos, invalidate);
}

void OGLStreamBuffer::Unmap(GLsizeiptr size) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::StreamUnmap);
#ifdef CARA_ANDROID_GLES_ATLAS
    if (persistent_data) {
        buffer_pos += size;
        return;
    }
#endif
    if (CommandQueue::Enabled()&&!queued_native_map) {
        if (queued_reserved == 0 || size < 0 || size > queued_reserved)
            throw std::invalid_argument("Invalid owned stream upload completion");
        queued_staging.resize(static_cast<std::size_t>(size));
        // The upload and its lifetime travel together; later guest writes cannot
        // alter this command. Orphaning preserves the old ring-wrap contract.
        CommandQueue::UploadBufferOwned(gl_target, buffer_pos, std::move(queued_staging),
                                        queued_orphan, buffer_size,explicit_flush,std::move(queued_budget),queued_reserved);
        queued_reserved = 0;
        buffer_pos += size;
        return;
    }
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

#ifdef CARA_IOS_GLES
std::tuple<u8*, GLintptr, bool> OGLStreamBuffer::MapStaging(GLsizeiptr size,
                                                         GLintptr alignment) {
    if (size <= 0 || size > buffer_size || alignment <= 0 || staging_reserved != 0) {
        throw std::invalid_argument("Invalid LUT staging reservation");
    }
    // Allocate before changing the ring so allocation failure leaves it intact.
    staging.resize(static_cast<std::size_t>(size));
    const auto aligned = Common::AlignUp<std::size_t>(buffer_pos, alignment);
    const bool invalidate = aligned > static_cast<std::size_t>(buffer_size - size);
    buffer_pos = invalidate ? 0 : static_cast<GLintptr>(aligned);
    staging_reserved = size;
    // Unlike the legacy GPU map hack, a 2D texture ring must always refresh all
    // resident tables on wrap: the following upload overwrites their texels.
    return std::make_tuple(staging.data(), buffer_pos, invalidate);
}

void OGLStreamBuffer::FinishStaging(GLsizeiptr size) {
    if (staging_reserved == 0 || size < 0 || size > staging_reserved) {
        throw std::invalid_argument("Invalid LUT staging completion");
    }
    buffer_pos += size;
    staging_reserved = 0;
}
#endif

} // namespace OpenGL
