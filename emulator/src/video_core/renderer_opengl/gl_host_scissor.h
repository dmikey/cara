// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_opengl/gl_scissor_damage.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace OpenGL::HostScissor {
using State = decltype(OpenGLState::scissor);

// Modify only the tracked host scissor. The normal state Apply performs GL
// updates; the caller restores its original scissor before leaving the draw.
// Unlike dirty-footprint narrowing, this deliberately also clips stencil writes.
inline bool ApplyInclude(State& state, ScissorDamage::Rect shader_bounds, std::uint32_t mode,
                         std::uint32_t scale, bool shadow) {
    if (!state.enabled || state.width < 0 || state.height < 0)
        return false;
    const ScissorDamage::Rect current{state.x, state.y, std::int64_t(state.x) + state.width,
                                      std::int64_t(state.y) + state.height};
    const auto clipped =
        ScissorDamage::IntersectInclude(current, shader_bounds, mode, scale, shadow);
    if (!clipped.eligible)
        return false;
    state.x = static_cast<GLint>(clipped.rect.left);
    state.y = static_cast<GLint>(clipped.rect.bottom);
    state.width = static_cast<GLsizei>(clipped.rect.right - clipped.rect.left);
    state.height = static_cast<GLsizei>(clipped.rect.top - clipped.rect.bottom);
    return true;
}
// Normal cleanup restores desired state only; Draw's existing Apply restores GL.
// On early return or exceptional exit restore the actual clip without changing other GL fields,
// and invalidate the cached raster snapshot rather than claiming it is current.
class ScopedInclude {
public:
    ScopedInclude(OpenGLState& state_, ScissorDamage::Rect bounds, std::uint32_t mode,
                  std::uint32_t scale, bool shadow, bool enabled)
        : state(state_) {
        if (enabled) {
            original = state.scissor;
            active = ApplyInclude(state.scissor, bounds, mode, scale, shadow);
        }
    }
    ~ScopedInclude() {
        if (active) {
            if (original.enabled)
                glEnable(GL_SCISSOR_TEST);
            else
                glDisable(GL_SCISSOR_TEST);
            glScissor(original.x, original.y, original.width, original.height);
            OpenGLState::InvalidateRasterState();
        }
        Restore();
    }
    ScopedInclude(const ScopedInclude&) = delete;
    ScopedInclude& operator=(const ScopedInclude&) = delete;
    bool Active() const {
        return active;
    }
    void Restore() {
        if (active) {
            state.scissor = original;
            active = false;
        }
    }

private:
    OpenGLState& state;
    State original{};
    bool active{};
};
} // namespace OpenGL::HostScissor
