// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_metal/pica_metal_surface_writer.h"
#include "../pica_tiled_export.h"

namespace Cara {

enum class PicaExportFormat { Rgba8, Depth24Stencil8, Rgb565, Rgba4, Rgb5A1 };

// Snapshot the exact native lease. A returned callback is valid for this
// resource/generation only and never receives guest RAM memory.
MetalGuestExport MakePicaTiledVisibilityExporter(
    id<MTLTexture> texture, std::uint64_t backing_generation, PicaGuestRange physical_surface,
    PicaExportFormat format, id<MTLComputePipelineState> tiled_pack,
    id<MTLComputePipelineState> depth24_pack);

} // namespace Cara
