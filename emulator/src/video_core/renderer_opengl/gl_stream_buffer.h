// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <tuple>
#include <vector>
#include <memory>
#include <glad/glad.h>
#include "common/common_types.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"

namespace OpenGL {

class OGLStreamBuffer : private NonCopyable {
public:
    explicit OGLStreamBuffer(GLenum target, GLsizeiptr size);
    ~OGLStreamBuffer();

    GLuint GetHandle() const;
    GLsizeiptr GetSize() const;

    /*
     * Allocates a linear chunk of memory in the GPU buffer with at least "size" bytes
     * and the optional alignment requirement.
     * If the buffer is full, the whole buffer is reallocated which invalidates old chunks.
     * The return values are the pointer to the new chunk, the offset within the buffer,
     * and the invalidation flag for previous chunks.
     * The actual used size must be specified on unmapping the chunk.
     */
    std::tuple<u8*, GLintptr, bool> Map(GLsizeiptr size, GLintptr alignment);

    void Unmap(GLsizeiptr size);

#ifdef CARA_IOS_GLES
    // CPU-owned source for GLES 2D LUT uploads. These methods reserve the same
    // logical ring offsets without mapping GL storage; the returned pointer may
    // therefore be supplied as client texture data with PIXEL_UNPACK_BUFFER=0.
    std::tuple<u8*, GLintptr, bool> MapStaging(GLsizeiptr size, GLintptr alignment,
                                               u8* mapped_source = nullptr);
    void FinishStaging(GLsizeiptr size);
#endif

private:
    OGLBuffer gl_buffer;
    GLenum gl_target;
    GLenum gl_target_invalidate_hack;

    // CPU-owned upload reservation in command-queue mode. Never a driver map.
    std::vector<u8> queued_staging;
    std::shared_ptr<void> queued_budget;
    bool queued_native_map=false;
    GLsizeiptr queued_reserved = 0;
    bool queued_orphan = false;
    bool explicit_flush = true;
    GLintptr buffer_pos = 0;
    GLsizeiptr buffer_size = 0;
#ifdef CARA_ANDROID_GLES_ATLAS
    static constexpr unsigned PersistentPageCount = 3;
    u8* persistent_data = nullptr;
    unsigned persistent_page = 0;
    std::array<GLsync, PersistentPageCount> persistent_page_fences{};
#endif
#ifdef CARA_IOS_GLES
    std::vector<u8> staging;
    GLsizeiptr staging_reserved = 0;
#endif
};

} // namespace OpenGL
