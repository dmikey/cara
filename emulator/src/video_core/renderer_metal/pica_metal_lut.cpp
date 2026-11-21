// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lut.h"

#include "video_core/pica_state.h"

namespace Cara {

std::shared_ptr<const PicaMetalLutData> SnapshotPicaMetalLuts(const Pica::State& state,
                                                            u64 generation, bool procedural) {
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
    if (procedural) {
        using Atlas = PicaMetalLutData;
        const auto& proc = state.proctex;
        const auto load_values = [&](unsigned offset, const auto& table) {
            for (unsigned j = 0; j < 128; ++j) {
                const auto& entry = table[j];
                snapshot->texels[offset + j] = {entry.ToFloat(), entry.DiffToFloat()};
            }
        };
        const auto load_colors = [&](unsigned offset, const auto& table) {
            for (unsigned j = 0; j < 256; ++j) {
                const auto& entry = table[j];
                // Match the GLES upload, including the byte conversion of differences.
                snapshot->texels[2 * (offset + j)] = {u8(entry.r) / 255.0f, u8(entry.g) / 255.0f};
                snapshot->texels[2 * (offset + j) + 1] = {u8(entry.b) / 255.0f, u8(entry.a) / 255.0f};
            }
        };
        load_values(Atlas::noise_offset, proc.noise_table);
        load_values(Atlas::color_map_offset, proc.color_map_table);
        load_values(Atlas::alpha_map_offset, proc.alpha_map_table);
        load_colors(Atlas::color_offset, proc.color_table);
        load_colors(Atlas::diff_offset, proc.color_diff_table);
    }
    return snapshot;
}

} // namespace Cara
