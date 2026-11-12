// Cara exact lookup admission helpers; SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <tuple>

namespace OpenGL {
template <typename Params>
bool SameTextureLookupParams(const Params& a, const Params& b) {
    return std::tie(a.addr,a.end,a.size,a.width,a.height,a.stride,a.res_scale,a.is_tiled,a.pixel_format,a.type) ==
           std::tie(b.addr,b.end,b.size,b.width,b.height,b.stride,b.res_scale,b.is_tiled,b.pixel_format,b.type);
}
// One surface can appear in multiple interval-map segments. Distinct identities
// at the highest eligible scale make admission ambiguous, even if one is invalid.
template <typename Identity>
struct ExactLookupSelection {
    Identity first_highest{};
    unsigned highest_scale = 0;
    bool ambiguous = false;
    void Observe(Identity identity, unsigned scale) {
        if (!first_highest || scale > highest_scale) {
            first_highest=identity; highest_scale=scale; ambiguous=false;
        } else if (scale == highest_scale && identity != first_highest) {
            ambiguous=true;
        }
    }
    bool IsUnique(Identity identity) const { return first_highest == identity && identity && !ambiguous; }
};
} // namespace OpenGL
