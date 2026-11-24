#include "common/frame_timing.h"
#include "video_core/renderer_opengl/gl_driver_transactions.h"
#ifdef CARA_GL_CONTEXT_DISPATCH
#include "video_core/renderer_opengl/gl_command_queue.h"
#endif
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_ios_compat.h"
#ifdef CARA_IOS_GLES
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace OpenGL {
#ifdef CARA_ANDROID_GLES_ATLAS
bool UseLightingLUTSnapshots() {
    static const bool enabled = [] {
        const char* requested = std::getenv("CARA_LIGHTING_LUT_SNAPSHOTS");
        return requested && requested[0] == '1' && !requested[1] && !UseNativeLUTBuffers();
    }();
    return enabled;
}
bool UseNativeLUTBuffers() {
    static const bool enabled = [] {
        const char* requested = std::getenv("CARA_ANDROID_NATIVE_LUTS");
        return requested && requested[0] == '1' && !requested[1] &&
               GLAD_GL_ES_VERSION_3_2 && glTexBuffer;
    }();
    return enabled;
}
#endif
void AllocateIOSLUT(GLenum internal_format, std::size_t bytes) {
    const GLenum format = internal_format == GL_RG32F ? GL_RG : GL_RGBA;
    const unsigned stride = format == GL_RG ? 8 : 16;
    const unsigned height = static_cast<unsigned>((bytes / stride + LUTTextureWidth - 1) / LUTTextureWidth);
    GLint unpack_buffer;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, LUTTextureWidth, height, 0, format, GL_FLOAT, nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}
template <bool ReplaceImage>
static void UploadLUTData(GLuint texture, unsigned unit, GLenum format, std::size_t offset,
                          const u8* data, std::size_t bytes, GLuint source_buffer = 0,
                          std::size_t source_offset = 0) {
    if (!bytes) return;
    const unsigned stride = format == GL_RG ? 8 : 16;
    if ((format != GL_RG && format != GL_RGBA) || (!source_buffer && !data) ||
        offset % stride || bytes % stride || source_offset % stride) {
        throw std::invalid_argument("Invalid CPU LUT upload span");
    }
    namespace Timing = Common::FrameTiming;
    const bool trace = Timing::counters.enabled && Timing::RenderBookkeepingProfileEnabled();
    if (trace)
        Timing::counters.lut_texture_bytes += bytes;
    GLint active, binding, row_length, alignment, skip_rows, skip_pixels, unpack_buffer;
    {
        Timing::Scope scope(Timing::Phase::LUTTextureSetup, trace);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, source_buffer);
    }
    {
        Timing::Scope scope(Timing::Phase::LUTTextureTransfer, trace);
        if constexpr (ReplaceImage) {
            const std::size_t row_bytes = LUTTextureWidth * stride;
            if (offset != 0 || format != GL_RG || bytes % row_bytes != 0)
                throw std::invalid_argument("Incomplete lighting table image");
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, LUTTextureWidth,
                         static_cast<GLsizei>(bytes / row_bytes), 0, GL_RG, GL_FLOAT, data);
        } else {
            std::size_t first = offset / stride, count = bytes / stride;
            while (count) {
                const unsigned x = first % LUTTextureWidth, y = first / LUTTextureWidth;
                const unsigned rows = x == 0 ? static_cast<unsigned>(count / LUTTextureWidth) : 0;
                const unsigned width = rows ? LUTTextureWidth : std::min<std::size_t>(count, LUTTextureWidth - x);
                const unsigned height = rows ? rows : 1;
                const void* source = source_buffer ? reinterpret_cast<const void*>(source_offset)
                                                   : data;
                glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, format, GL_FLOAT, source);
                const unsigned used = width * height;
                first += used;
                count -= used;
                if (source_buffer)
                    source_offset += used * stride;
                else
                    data += used * stride;
            }
        }
    }
    {
        Timing::Scope scope(Timing::Phase::LUTTextureRestore, trace);
        glBindTexture(GL_TEXTURE_2D, binding);
        glActiveTexture(active);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer));
    }
}
template <bool ReplaceImage>
static void UploadLUT(GLuint texture, unsigned unit, GLenum format, std::size_t offset,
                  const u8* data, std::size_t bytes) {
#ifdef CARA_GL_CONTEXT_DISPATCH
    if (CommandQueue::Enabled() && !DriverTransactions::Executing()) {
        // Capture once. All seven unpack/binding queries, uploads and restoration
        // execute on the same owner as the preceding and following draws.
        if (Common::FrameTiming::counters.enabled)
            Common::FrameTiming::counters.lut_texture_bytes += bytes;
        CommandQueue::QueueLUT(&UploadLUT<ReplaceImage>, texture, unit, format, offset, data, bytes);
        return;
    }
#endif
    if (const auto* bridge=DriverTransactions::Pending()) {
        bridge->lut(&UploadLUT<ReplaceImage>,texture,unit,format,offset,data,bytes);return;
    }
    UploadLUTData<ReplaceImage>(texture, unit, format, offset, data, bytes);
}

#ifdef CARA_ANDROID_GLES_ATLAS
void UploadLightingLUTFromBuffer(GLuint texture, unsigned unit, std::size_t offset,
                                 GLuint buffer, std::size_t source_offset, std::size_t bytes) {
    UploadLUTData<false>(texture, unit, GL_RG, offset, nullptr, bytes, buffer, source_offset);
}
#endif

void UploadIOSLUT(GLuint texture, unsigned unit, GLenum format, std::size_t offset,
                  const u8* data, std::size_t bytes) {
    UploadLUT<false>(texture, unit, format, offset, data, bytes);
}

void ReplaceLightingLUTImage(GLuint texture, unsigned unit, const u8* data, std::size_t bytes) {
    UploadLUT<true>(texture, unit, GL_RG, 0, data, bytes);
}

}
#endif
