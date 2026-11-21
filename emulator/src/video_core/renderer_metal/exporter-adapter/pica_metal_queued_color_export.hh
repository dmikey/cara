// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pica_metal_visibility_export.h"

namespace Cara {

// Produce an immutable guest-byte span using the consumer's existing command.
// A declined layout does not mutate the command. An encoding failure throws.
id<MTLBuffer> EncodeQueuedColorExport(id<MTLCommandBuffer> command,
                                     id<MTLComputePipelineState> tiled_pack,
                                     id<MTLTexture> texture, MetalRgba8Layout layout,
                                     PicaGuestRange requested);
// Byte-exact 16-bit packed render-target export on the consumer's queue.
id<MTLBuffer> EncodeQueuedPackedColorExport(id<MTLCommandBuffer> command,
                                           id<MTLComputePipelineState> tiled_pack,
                                           id<MTLTexture> texture, PicaGuestRange surface,
                                           PicaGuestRange requested);
} // namespace Cara
