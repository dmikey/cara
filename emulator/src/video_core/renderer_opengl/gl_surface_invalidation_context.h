// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include "video_core/renderer_opengl/gl_scissor_damage.h"

namespace OpenGL {
enum class SurfaceInvalidationOrigin : std::uint32_t {
    Unknown = 0, CPU = 1, DrawColor = 2, DrawDepth = 3,
    DisplayTransfer = 4, TextureCopy = 5, Fill = 6,
};
// Diagnostic values only. Coordinates match the actual signed shader uniforms.
struct SurfaceInvalidationContext {
    bool valid = false, shadow = false, color_write = false, depth_write = false;
    bool depth_surface_write = false, damage_narrowing_active = false;
    bool has_stencil = false, stencil_test = false, depth_test = false, depth_mask = false;
    std::uint32_t mode = 0, scale = 0, color_mask = 0, stencil_mask = 0;
    ScissorDamage::Rect viewport{}, actual_damage{}, shader_scissor{};
};
} // namespace OpenGL
