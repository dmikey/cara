// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <limits>
#include <stdexcept>
#include "video_core/renderer_metal/pica_metal_draw_group.h"
#include "video_core/renderer_metal/pica_metal_guest_authority.h"
#include "video_core/renderer_metal/pica_metal_surface_writer.h"

namespace Cara {

inline void ValidateMetalDrawTicket(const PicaMetalDrawTicket& ticket) {
    if (!ticket.command || ticket.command.status < MTLCommandBufferStatusCommitted ||
        ticket.count == 0 || ticket.count > ticket.publications.size())
        throw std::runtime_error("Invalid Metal draw ticket");
}

inline std::optional<MetalGuestWrite> MakeMetalDrawWrite(
    const PicaMetalDrawTicket& ticket, const PicaMetalWriter& writer,
    const PicaMetalSurfaceView& view, const MetalGuestExport& exporter, u64 residency_revision,
    const std::shared_ptr<const MetalProducerPredecessors>& predecessors,
    const std::optional<MetalRgba8Layout>& rgba8 = {}) {
    if (writer.bytes.begin == 0 && writer.bytes.end == 0)
        return std::nullopt;
    if (!view.texture || !exporter || view.texture.device != ticket.command.commandQueue.device ||
        writer.backing_generation != view.backing_generation ||
        view.authoritative_bytes.begin > writer.bytes.begin ||
        view.authoritative_bytes.end < writer.bytes.end || writer.bytes.begin >= writer.bytes.end ||
        writer.bytes.end > std::numeric_limits<PAddr>::max())
        throw std::runtime_error("Metal draw writer does not match its bound surface");

    const auto begin = static_cast<PAddr>(writer.bytes.begin);
    const auto end = static_cast<PAddr>(writer.bytes.end);
    const auto format = view.texture.pixelFormat;
    std::optional<PicaGuestRange> packed_color;
    if (format == MTLPixelFormatB5G6R5Unorm || format == MTLPixelFormatABGR4Unorm ||
        format == MTLPixelFormatA1BGR5Unorm)
        packed_color = view.authoritative_bytes;
    return MetalGuestWrite{begin, end,
        std::make_shared<MetalSurfaceWriter>(view.texture, ticket.command, writer.backing_generation,
                                             begin, end, exporter, residency_revision, predecessors,
                                             nil, rgba8, packed_color)};
}

} // namespace Cara
