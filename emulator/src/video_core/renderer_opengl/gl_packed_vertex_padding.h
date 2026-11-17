// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
namespace OpenGL {
// One process-cached flag shared by the renderer and its native mode report.
inline bool PackedVertexPaddingOnlyEnabled() {
    static const bool enabled=[] {
        const auto* value=std::getenv("CARA_PACKED_PADDING_ONLY_ZERO");
        return value && std::strcmp(value,"1")==0;
    }();
    return enabled;
}
} // namespace OpenGL
