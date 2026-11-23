// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include "video_core/renderer_opengl/gl_surface_invalidation_context.h"

namespace OpenGL::RemapOriginProfile {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_REMAP_ORIGIN_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
struct Owner {
    SurfaceInvalidationOrigin origin{};
    int vertex_entry{-1};
    std::uint64_t begin{}, end{};
};
// Diagnostic only. A surface can have different writers for different bytes.
using Key = std::tuple<unsigned, int, bool, unsigned, unsigned, bool, bool>;
struct Counts { std::uint64_t calls{}, bytes{}; };
inline thread_local std::map<Key, Counts> rows;
template <typename Surface>
void Observe(const Surface& src, const Surface& dst, std::uint64_t begin, std::uint64_t end) {
    const auto& owner = src.last_remap_owner;
    auto& counts = rows[{unsigned(owner.origin), owner.vertex_entry,
                         owner.begin <= begin && end <= owner.end, src.GetFormatBpp(),
                         dst.GetFormatBpp(), src.is_tiled, dst.is_tiled}];
    ++counts.calls;
    counts.bytes += end - begin;
}
} // namespace OpenGL::RemapOriginProfile
#endif
