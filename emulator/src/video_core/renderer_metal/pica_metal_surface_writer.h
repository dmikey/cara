// Licensed under GPLv2 or any later version
#pragma once

#import <Metal/Metal.h>

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include "video_core/renderer_metal/pica_metal_guest_writer.h"
#include "video_core/renderer_metal/pica_metal_draw_group.h"

#if !__has_feature(objc_arc)
#error Metal writer lifetime requires Objective-C ARC
#endif

namespace Cara {

// Enqueue transfer/conversion on the producer's queue, wait for successful
// completion, and copy the requested guest-encoded bytes into private output.
// The resource generation may be used for ordered later writes. Its storage
// cannot be repurposed while any lease is live.
using MetalGuestExport = std::function<bool(id<MTLCommandQueue>, id<MTLResource>, u64,
                                           PAddr, PAddr, std::span<u8>)>;

// Snapshot the exact earlier submissions on the ordered owning queue. A
// later command succeeding is insufficient evidence for its predecessors.
struct MetalProducerPredecessors {
    std::array<id<MTLCommandBuffer>, 8> commands{};
    std::size_t count = 0;

    MetalGuestWriteStatus Status() const {
        if (count > commands.size())
            return MetalGuestWriteStatus::Failed;
        bool pending = false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto command = commands[i];
            const auto status = command.status;
            if (!command || status < MTLCommandBufferStatusCommitted ||
                status == MTLCommandBufferStatusError || command.error)
                return MetalGuestWriteStatus::Failed;
            pending |= status != MTLCommandBufferStatusCompleted;
        }
        return pending ? MetalGuestWriteStatus::Pending : MetalGuestWriteStatus::Complete;
    }

    bool Wait() const {
        if (count > commands.size())
            return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto command = commands[i];
            if (!command || command.status < MTLCommandBufferStatusCommitted)
                return false;
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted || command.error)
                return false;
        }
        return true;
    }
};

// Describes an exact, unscaled RGBA8 guest surface with no row padding.
struct MetalRgba8Layout {
    PAddr begin, end;
    u32 width, height;
    bool tiled;

    bool operator==(const MetalRgba8Layout&) const = default;
};

class MetalSurfaceWriter final : public MetalGuestWriter {
public:
    MetalSurfaceWriter(id<MTLResource> resource_, id<MTLCommandBuffer> command_, u64 generation_,
                       PAddr begin_, PAddr end_, MetalGuestExport exporter_,
                       u64 residency_revision_ = 0,
                       std::shared_ptr<const MetalProducerPredecessors> predecessors_ = {},
                       id<MTLBuffer> raw_guest_ = nil,
                       std::optional<MetalRgba8Layout> rgba8_ = {},
                       std::optional<PicaGuestRange> packed_color_ = {})
        : resource(resource_), command(command_), generation(generation_), begin(begin_), end(end_),
          exporter(std::move(exporter_)), residency_revision(residency_revision_),
          predecessors(std::move(predecessors_)), raw_guest(raw_guest_), rgba8(rgba8_), packed_color(packed_color_) {}

    MetalGuestWriteStatus Status() const override {
        const auto state = command.status;
        if (!command || state == MTLCommandBufferStatusError || command.error)
            return MetalGuestWriteStatus::Failed;

        const auto before = predecessors ? predecessors->Status() : MetalGuestWriteStatus::Complete;
        if (before == MetalGuestWriteStatus::Failed)
            return before;
        return state == MTLCommandBufferStatusCompleted ? before : MetalGuestWriteStatus::Pending;
    }

    id<MTLResource> Resource() const { return resource; }
    id<MTLCommandQueue> Queue() const { return command.commandQueue; }
    id<MTLCommandBuffer> Command() const { return command; }
    u64 Generation() const { return generation; }
    // Optional immutable guest encoding. Offset zero corresponds to begin.
    id<MTLBuffer> RawGuestBuffer() const { return raw_guest; }
    PAddr RawGuestBegin() const { return begin; }
    const std::optional<MetalRgba8Layout>& Rgba8Layout() const { return rgba8; }
    const std::optional<PicaGuestRange>& PackedColor() const { return packed_color; }
    MetalResidencyStamp Residency() const override {
        return residency_revision ? MetalResidencyStamp{residency_revision, generation}
                                  : MetalResidencyStamp{};
    }

    bool Covers(PAddr first, PAddr last) const override {
        return resource && command && command.commandQueue && generation != 0 && exporter &&
               command.status >= MTLCommandBufferStatusCommitted &&
               first >= begin && first < last && last <= end;
    }

    const MetalGuestExport& Exporter() const { return exporter; }

    bool ValidateProducerCompletion() const {
        if (!command || (predecessors && !predecessors->Wait()))
            return false;
        [command waitUntilCompleted];
        return command.status == MTLCommandBufferStatusCompleted && !command.error;
    }

    bool ExportGuestBytes(PAddr first, PAddr last, std::span<u8> output) const override {
        if (!Covers(first, last) || output.size() != last - first || !ValidateProducerCompletion())
            return false;
        return exporter(command.commandQueue, resource, generation, first, last, output);
    }

private:
    const id<MTLResource> resource;
    const id<MTLCommandBuffer> command;
    const u64 generation;
    const PAddr begin;
    const PAddr end;
    const MetalGuestExport exporter;
    const u64 residency_revision;
    const std::shared_ptr<const MetalProducerPredecessors> predecessors;
    const id<MTLBuffer> raw_guest;
    const std::optional<MetalRgba8Layout> rgba8;
    const std::optional<PicaGuestRange> packed_color;
};

} // namespace Cara
