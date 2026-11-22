// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_resources.h"
#include "video_core/renderer_metal/pica_metal_pipelines.h"
#include "video_core/renderer_metal/pica_metal_lut.h"
#include "video_core/renderer_metal/pica_metal_lighting_cache.h"

namespace Cara {

class PicaMetalNativeBindings {
public:
    explicit PicaMetalNativeBindings(const PicaMetalResources& resources);
    bool BeginWarmup() { return pipelines.BeginWarmup(); }
    bool FinishWarmup() { return pipelines.FinishWarmup(); }
    void InvalidateLuts() { lut_dirty = true; }
    void InvalidateProceduralLuts() { procedural_lut_dirty = true; }
    std::optional<NativePicaDrawBindings> Prepare(
        PicaMetalNativeDrawLane& lane, const Pica::State& state, bool indexed,
        const MetalGuestAuthority::GuestReader& ram,
        std::span<const PicaMetalStageVertex> stage_in = {}, bool* interpret_vertices = nullptr);

private:
    PicaMetalResources resources;
    PicaMetalPipelines pipelines;
    PicaMetalLightingCache lighting;
    id<MTLTexture> lut = nil;
    bool lut_dirty = true;
    bool procedural_lut_dirty = true;
    bool lut_has_procedural = false;
    u64 generation = 0;
};

} // namespace Cara
