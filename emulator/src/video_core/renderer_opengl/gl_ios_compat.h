// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <glad/glad.h>
#include "common/common_types.h"

namespace OpenGL {
// Android may use its GLES 3.2 buffer textures while retaining atlas vertices.
#ifdef CARA_IOS_GLES
bool UseNativeLUTBuffers();
#endif
inline GLenum LUTTextureTarget() {
#ifdef CARA_IOS_GLES
    return UseNativeLUTBuffers() ? GL_TEXTURE_BUFFER : GL_TEXTURE_2D;
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
void UploadIOSLUT(GLuint texture, unsigned unit, GLenum format, std::size_t offset,
                  const u8* data, std::size_t bytes);
#endif
}
