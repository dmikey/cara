// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace Cara {

// MakeKey's material recipe has 28 fields. Only the first ten contribute to
// the MTLRenderPipelineDescriptor currently emitted by Prepare: shader pair,
// blend enable, color write mask, six blend terms and color pixel format.
// Depth/stencil format is fixed D32F_S8 and raster sample count is fixed at one.
// The rest belongs to separately rebuilt depth/stencil and sampler objects.
// Pair identity also fixes vertex layout, reflected slots and cube interfaces;
// raw-index mode is a process-static policy, and entries are owned by one device.
// A new render-descriptor property must enter this identity before reuse.
inline constexpr std::size_t PicaMetalMaterialKeyFields = 28;
inline constexpr std::size_t PicaMetalRenderKeyFields = 10;

template <std::size_t Fields>
inline bool SamePicaMetalRenderPipeline(
    const std::array<unsigned, Fields>& a, const std::array<unsigned, Fields>& b) {
    static_assert(Fields == PicaMetalMaterialKeyFields,
                  "Review the render-pipeline identity when the material key changes");
    return std::equal(a.begin(), a.begin() + PicaMetalRenderKeyFields, b.begin());
}

} // namespace Cara
