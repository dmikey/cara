// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_ios_compat.h"
#ifdef CITRA_IOS_GLES
#include <algorithm>

namespace OpenGL {
constexpr unsigned LUT_WIDTH = 1024;
void AllocateIOSLUT(GLenum internal_format, std::size_t bytes) {
    const GLenum format = internal_format == GL_RG32F ? GL_RG : GL_RGBA;
    const unsigned stride = format == GL_RG ? 8 : 16;
    const unsigned height = static_cast<unsigned>((bytes / stride + LUT_WIDTH - 1) / LUT_WIDTH);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, LUT_WIDTH, height, 0, format, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}
void UploadIOSLUT(GLuint texture, unsigned unit, GLenum format, std::size_t offset,
                  const u8* data, std::size_t bytes) {
    if (!bytes) return;
    GLint active, binding, row_length, alignment;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const unsigned stride = format == GL_RG ? 8 : 16;
    std::size_t first = offset / stride, count = bytes / stride;
    while (count) {
        const unsigned x = first % LUT_WIDTH, y = first / LUT_WIDTH;
        const unsigned rows = x == 0 ? static_cast<unsigned>(count / LUT_WIDTH) : 0;
        const unsigned width = rows ? LUT_WIDTH : std::min<std::size_t>(count, LUT_WIDTH - x);
        const unsigned height = rows ? rows : 1;
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, format, GL_FLOAT, data);
        const unsigned used = width * height;
        first += used; count -= used; data += used * stride;
    }
    glBindTexture(GL_TEXTURE_2D, binding);
    glActiveTexture(active);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
}
}
#endif
