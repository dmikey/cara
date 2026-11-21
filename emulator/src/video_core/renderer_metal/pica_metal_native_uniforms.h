// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_opengl/gl_shader_manager.h"

namespace Pica {
struct State;
}

namespace Cara {

// Match the exact std140 layout used to compile the retained iPad shaders.
struct PicaMetalNativeUniforms {
    OpenGL::VSUniformData vertex{};
    OpenGL::UniformData common{};
    OpenGL::UniformLightData light{};
};

PicaMetalNativeUniforms CapturePicaMetalUniforms(const Pica::State& state, bool disable_clip_coef);

} // namespace Cara
