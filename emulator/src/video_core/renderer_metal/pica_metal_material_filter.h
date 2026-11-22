// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include "cara/metal_texture_filter.h"

namespace Cara {
// Apply only to material sampling. Keep lookup tables and texel fetches exact.
inline void ApplyMaterialFilter(MTLSamplerDescriptor* descriptor, MetalTextureFilter filter) {
    if (filter == MetalTextureFilter::Game)
        return;
    descriptor.minFilter = descriptor.magFilter = filter == MetalTextureFilter::Linear
        ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

} // namespace Cara
