// Copyright 2020 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <type_traits>
#include <string>
#include <vector>
#include <glad/glad.h>
#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_surface_params.h"

namespace OpenGL {

class RasterizerCacheOpenGL;

struct PixelFormatPair {
    const SurfaceParams::PixelFormat dst_format, src_format;
    struct less {
        using is_transparent = void;
        constexpr bool operator()(OpenGL::PixelFormatPair lhs, OpenGL::PixelFormatPair rhs) const {
            return std::tie(lhs.dst_format, lhs.src_format) <
                   std::tie(rhs.dst_format, rhs.src_format);
        }
        constexpr bool operator()(OpenGL::SurfaceParams::PixelFormat lhs,
                                  OpenGL::PixelFormatPair rhs) const {
            return lhs < rhs.dst_format;
        }
        constexpr bool operator()(OpenGL::PixelFormatPair lhs,
                                  OpenGL::SurfaceParams::PixelFormat rhs) const {
            return lhs.dst_format < rhs;
        }
    };
};

class FormatReinterpreterBase {
public:
    virtual ~FormatReinterpreterBase() = default;

    virtual void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect,
                             GLuint read_fb_handle, GLuint dst_tex,
                             const Common::Rectangle<u32>& dst_rect, GLuint draw_fb_handle) = 0;
    // Extent-aware checked entry for GLES implementations that must attach the
    // source depth/stencil image. Existing desktop implementations are unchanged.
    virtual bool ReinterpretChecked(GLuint src_tex, const Common::Rectangle<u32>& src_rect,
                                    GLuint read_fb_handle, GLuint dst_tex,
                                    const Common::Rectangle<u32>& dst_rect, GLuint draw_fb_handle,
                                    u32 src_width, u32 src_height) {
        Reinterpret(src_tex, src_rect, read_fb_handle, dst_tex, dst_rect, draw_fb_handle);
        return true;
    }
    virtual bool EncodeDepthChecked(GLuint, SurfaceParams::PixelFormat,
                                    const Common::Rectangle<u32>&, GLuint,
                                    const Common::Rectangle<u32>&, u32, u32) { return false; }
    virtual bool UploadDepthChecked(GLuint, SurfaceParams::PixelFormat,
                                    const Common::Rectangle<u32>&, const u8*, size_t,
                                    u32, u32, u32) { return false; }
};

bool RunDepthReinterpretSelfTest(std::string& error);

class FormatReinterpreterOpenGL : NonCopyable {
public:
    explicit FormatReinterpreterOpenGL();
    ~FormatReinterpreterOpenGL();

    struct Reinterpreter {
        Reinterpreter(const SurfaceParams::PixelFormat dst_format,
                      const SurfaceParams::PixelFormat src_format,
                      std::unique_ptr<FormatReinterpreterBase> reinterpreter)
            : dst_format(dst_format), src_format(src_format),
              reinterpreter(std::move(reinterpreter)) {}
        const SurfaceParams::PixelFormat dst_format;
        const SurfaceParams::PixelFormat src_format;
        std::unique_ptr<FormatReinterpreterBase> reinterpreter;
    };

    const std::vector<Reinterpreter>& GetReinterpreters();
    // Correct CPU export is independent of optional direct GPU aliasing.
    bool EncodeDepthChecked(GLuint src, SurfaceParams::PixelFormat format,
                            const Common::Rectangle<u32>& src_rect, GLuint dst_rgba,
                            const Common::Rectangle<u32>& dst_rect, u32 src_width, u32 src_height);
    // canonical starts at rect's first pixel; pixels are [S/padding,Zlo,Zmid,Zhi]
    // with stride_pixels between rows. Size bounds every CPU source read.
    bool UploadDepthChecked(GLuint target, SurfaceParams::PixelFormat format,
                            const Common::Rectangle<u32>& rect, const u8* canonical,
                            size_t canonical_bytes, u32 stride_pixels,
                            u32 target_width, u32 target_height);

private:
    std::vector<Reinterpreter> reinterpreters;
    std::unique_ptr<FormatReinterpreterBase> depth_encoder;
};

} // namespace OpenGL
