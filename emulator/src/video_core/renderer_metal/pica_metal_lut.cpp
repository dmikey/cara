// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lut.h"

#include "video_core/pica_state.h"

namespace Cara {

std::shared_ptr<const PicaMetalLutData> SnapshotPicaMetalLuts(const Pica::State& state,
                                                            u64 generation) {
    auto snapshot = std::make_shared<PicaMetalLutData>();
    snapshot->generation = generation;
    for (unsigned i = 0; i < Pica::LightingRegs::NumLightingSampler; ++i) {
        const auto& source = state.lighting.luts[i];
        for (unsigned j = 0; j < PicaMetalLutData::entries_per_sampler; ++j) {
            const auto& entry = source[j];
            snapshot->texels[i * PicaMetalLutData::entries_per_sampler + j] = {
                entry.ToFloat(), entry.DiffToFloat()};
        }
    }
    for (unsigned j = 0; j < PicaMetalLutData::fog_entries; ++j) {
        const auto& entry = state.fog.lut[j];
        snapshot->texels[PicaMetalLutData::fog_offset + j] = {entry.ToFloat(), entry.DiffToFloat()};
    }
    return snapshot;
}

} // namespace Cara
