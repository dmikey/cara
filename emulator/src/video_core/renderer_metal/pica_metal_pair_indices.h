// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <memory>
#include <iosfwd>
#include <vector>
#include "video_core/renderer_metal/pica_metal_packed_inputs.h"

namespace Pica { struct State; }
namespace Cara {
// Immutable topology only. Guest attributes, shader uniforms and transformed
// outputs are never retained here. Retained-reference commands lease both GPU
// resources after encoding even if the bounded memo evicts this host owner.
struct MetalPairIndexPayload {
    std::vector<u8> source;
    std::vector<u32> pairs;
    std::vector<u16> elements;
    id<MTLTexture> pair_texture = nil;
    id<MTLBuffer> element_buffer = nil;
    u32 pair_count = 0;
    u32 baseline_runs = 0, predicted_runs = 0;
    bool admitted = false;
};

bool UseMetalPairIndices();
std::shared_ptr<const MetalPairIndexPayload> CaptureMetalPairIndices(
    id<MTLDevice> device, const Pica::State& state, bool indexed, u16 input_mask,
    const PicaMetalPackedInputs& captured);
void ReportMetalPairIndices(std::ostream& out);
} // namespace Cara
