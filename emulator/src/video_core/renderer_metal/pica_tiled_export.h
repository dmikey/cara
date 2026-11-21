// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pica_metal_draw_group.h"

namespace Cara {

// The canonical input has [R,G,B,A] bytes for RGBA8, [S,Dhi,Dmid,Dlo] for
// native depth, or a little-endian RGB565, RGBA4, or RGB5A1 word. Input and output are disjoint.
bool EncodePicaTiledExport(id<MTLCommandBuffer> command,
                           id<MTLComputePipelineState> pipeline,
                           id<MTLBuffer> canonical, id<MTLBuffer> requested_scratch,
                           std::uint32_t width, std::uint32_t height, std::uint32_t source_row,
                           PicaGuestRange surface, PicaGuestRange requested,
                           unsigned pixel_bytes = 4);

// Detile packed guest bytes before blitting into a renderable packed 16-bit texture.
bool EncodePicaPackedColorImport(id<MTLCommandBuffer> command,
                           id<MTLComputePipelineState> detile,
                           id<MTLBuffer> source, NSUInteger offset, NSUInteger source_bytes,
                           id<MTLTexture> texture);

} // namespace Cara
