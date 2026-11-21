// Licensed under GPLv2 or any later version
#pragma once

#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string_view>
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
inline constexpr std::size_t MetalProducerMaxPredecessors = 32;

struct MetalProducerPredecessors {
    using Commands = std::array<id<MTLCommandBuffer>, MetalProducerMaxPredecessors>;
    using Validations = std::array<std::shared_ptr<const PicaMetalValidationReceipt>, MetalProducerMaxPredecessors>;

    MetalProducerPredecessors(Commands commands_, Validations validations_, std::size_t count_)
        : commands(std::move(commands_)), validations(std::move(validations_)), count(count_),
          cache_completed(CacheCompleted()) {}
    MetalProducerPredecessors(const MetalProducerPredecessors&) = delete;
    MetalProducerPredecessors& operator=(const MetalProducerPredecessors&) = delete;

    // Fully assembled before publication. Never clear these resources when
    // caching success: concurrent readers and descendant writers retain them.
    const Commands commands;
    const Validations validations;
    const std::size_t count;

    MetalGuestWriteStatus Status() const {
        if (cache_completed) {
            const auto known = terminal.load(std::memory_order_acquire);
            if (known != MetalGuestWriteStatus::Pending)
                return known;
        }
        if (count > commands.size())
            return Remember(MetalGuestWriteStatus::Failed);
        bool pending = false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto command = commands[i];
            const auto status = command.status;
            if (!command || status < MTLCommandBufferStatusCommitted ||
                status == MTLCommandBufferStatusError || command.error)
                return Remember(MetalGuestWriteStatus::Failed);
            pending |= status != MTLCommandBufferStatusCompleted;
            if (validations[i]) {
                if (validations[i]->Command() != command)
                    return Remember(MetalGuestWriteStatus::Failed);
                const auto checked = validations[i]->Poll();
                if (checked == PicaMetalCompletion::Failed)
                    return Remember(MetalGuestWriteStatus::Failed);
                pending |= checked == PicaMetalCompletion::Pending;
            }
        }
        return Remember(pending ? MetalGuestWriteStatus::Pending : MetalGuestWriteStatus::Complete);
    }

    bool Wait() const {
        if (cache_completed) {
            const auto known = terminal.load(std::memory_order_acquire);
            if (known != MetalGuestWriteStatus::Pending)
                return known == MetalGuestWriteStatus::Complete;
        }
        if (count > commands.size())
            return RememberFailure();
        for (std::size_t i = 0; i < count; ++i) {
            const auto command = commands[i];
            if (!command || command.status < MTLCommandBufferStatusCommitted)
                return RememberFailure();
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted || command.error)
                return RememberFailure();
            if (validations[i] && (validations[i]->Command() != command ||
                                  validations[i]->Wait() != PicaMetalCompletion::Complete))
                return RememberFailure();
        }
        return Remember(MetalGuestWriteStatus::Complete) == MetalGuestWriteStatus::Complete;
    }

private:
    static bool CacheCompleted() {
        static const bool enabled = [] {
            const auto* value = std::getenv("CARA_METAL_CACHE_COMPLETED_PREDECESSORS");
            return value && std::string_view(value) == "1";
        }();
        return enabled;
    }
    MetalGuestWriteStatus Remember(MetalGuestWriteStatus status) const {
        // Pending observations never authorize a later shortcut. Completed
        // command states and fully checked receipts are terminal; immutable
        // snapshots cannot subsequently acquire another producer or receipt.
        if (cache_completed && status != MetalGuestWriteStatus::Pending) {
            auto expected = MetalGuestWriteStatus::Pending;
            if (!terminal.compare_exchange_strong(expected, status, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                return expected;
        }
        return status;
    }
    bool RememberFailure() const {
        Remember(MetalGuestWriteStatus::Failed);
        return false;
    }
    const bool cache_completed;
    mutable std::atomic<MetalGuestWriteStatus> terminal{MetalGuestWriteStatus::Pending};
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
                       std::optional<PicaGuestRange> packed_color_ = {},
                       std::shared_ptr<const PicaMetalValidationReceipt> validation_ = {})
        : resource(resource_), command(command_), generation(generation_), begin(begin_), end(end_),
          exporter(std::move(exporter_)), residency_revision(residency_revision_),
          predecessors(std::move(predecessors_)), raw_guest(raw_guest_), rgba8(rgba8_), packed_color(packed_color_),
          validation(std::move(validation_)) {}

    MetalGuestWriteStatus Status() const override {
        const auto state = command.status;
        if (!command || state == MTLCommandBufferStatusError || command.error)
            return MetalGuestWriteStatus::Failed;

        const auto before = predecessors ? predecessors->Status() : MetalGuestWriteStatus::Complete;
        if (before == MetalGuestWriteStatus::Failed)
            return before;
        if (validation) {
            if (validation->Command() != command)
                return MetalGuestWriteStatus::Failed;
            const auto checked = validation->Poll();
            if (checked == PicaMetalCompletion::Failed)
                return MetalGuestWriteStatus::Failed;
            if (checked == PicaMetalCompletion::Pending)
                return MetalGuestWriteStatus::Pending;
        }
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
        const bool complete = command.status == MTLCommandBufferStatusCompleted && !command.error &&
               (!validation || (validation->Command() == command &&
                                validation->Wait() == PicaMetalCompletion::Complete));
        return complete;
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
    const std::shared_ptr<const PicaMetalValidationReceipt> validation;
};

} // namespace Cara
