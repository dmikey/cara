// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <glad/glad.h>
#include "common/common_types.h"

namespace OpenGL {
inline GLenum LUTTextureTarget() {
#ifdef CARA_IOS_GLES
    return GL_TEXTURE_2D;
#else
    return GL_TEXTURE_BUFFER;
#endif
}
inline GLenum StreamBufferTarget(GLenum target) {
#ifdef CARA_IOS_GLES
    if (target == GL_TEXTURE_BUFFER) return GL_ARRAY_BUFFER;
#endif
    return target;
}
#ifdef CARA_IOS_GLES
void AllocateIOSLUT(GLenum internal_format, std::size_t bytes);
// Source must be ordinary CPU-owned memory, never a mapped GL buffer pointer.
void UploadIOSLUT(GLuint texture, unsigned unit, GLenum format, std::size_t offset,
                  const u8* data, std::size_t bytes);
#endif
}
