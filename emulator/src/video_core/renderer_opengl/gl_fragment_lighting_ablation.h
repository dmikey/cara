// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
namespace OpenGL {
// Deliberate visual diagnostic, never a timing or guest-register override.
// Shared process-cached mode value for shader selection and native profile receipts.
inline bool FragmentLightingAblationEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_GPU_ABLATE_FRAGMENT_LIGHTING");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
} // namespace OpenGL
