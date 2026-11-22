// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

#include "pica_metal_draw.h"
#include "pica_metal_resolution.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace Cara {

// Cache addresses, not Metal staging offsets. Ends are exclusive and may be
// one past the last address representable in PAddr.
struct PicaGuestRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

struct PicaMetalSurfaceView {
    id<MTLTexture> texture = nil;
    PicaGuestRange authoritative_bytes;
    std::uint64_t backing_generation = 0;
};

// The rasterizer cache resolves these views using its existing authority map.
// Each attachment view covers the original cache validation interval. The
// sampled view covers every level read by the chosen sampler.
struct PicaMetalDrawViews {
    PicaMetalSurfaceView color;
    PicaMetalSurfaceView depth_stencil;
    PicaMetalSurfaceView sampled;
    PicaGuestRange color_dirty;
    PicaGuestRange depth_stencil_dirty;
    std::optional<PicaMetalSurfaceView> secondary_sampled{};
    std::optional<PicaMetalSurfaceView> tertiary_sampled{};
    std::optional<std::array<PicaGuestRange, 6>> primary_cube_faces{};
    bool primary_color_snapshot = false;
    unsigned depth_snapshot_slots = 0;
};

struct PicaMetalWriter {
    std::uint64_t backing_generation = 0;
    PicaGuestRange bytes;
};

struct PicaMetalDrawPublication {
    PicaMetalWriter color;
    PicaMetalWriter depth_stencil;
};

enum class PicaMetalCompletion { Pending, Complete, Failed };
class MetalSurfaceWriter;

// A completed GPU command is not usable until its shader-side checks pass.
// Retained by tickets and descendant writers independently of queue retirement.
class PicaMetalValidationReceipt {
public:
    using Sources = std::vector<std::shared_ptr<const MetalSurfaceWriter>>;

    PicaMetalValidationReceipt(id<MTLCommandBuffer> command_,
                              std::array<id<MTLBuffer>, 2> errors_,
                              std::shared_ptr<const MetalSurfaceWriter> source_ = {},
                              Sources more_sources_ = {})
        : command(command_), errors(errors_), source(std::move(source_)),
          more_sources(std::move(more_sources_)), error_count(2) {}
    PicaMetalValidationReceipt(id<MTLCommandBuffer> command_, id<MTLBuffer> error_,
                              std::shared_ptr<const MetalSurfaceWriter> source_ = {},
                              Sources more_sources_ = {})
        : command(command_), errors{error_, nil}, source(std::move(source_)),
          more_sources(std::move(more_sources_)), error_count(1) {}
    PicaMetalCompletion Poll() const;
    PicaMetalCompletion Wait() const;
    id<MTLCommandBuffer> Command() const { return command; }
private:
    const id<MTLCommandBuffer> command;
    mutable std::array<id<MTLBuffer>, 2> errors;
    mutable std::shared_ptr<const MetalSurfaceWriter> source;
    mutable Sources more_sources;
    const std::size_t error_count;
    mutable std::atomic<PicaMetalCompletion> checked{PicaMetalCompletion::Pending};
    mutable std::mutex inspection;
};

struct PicaMetalDrawTicket {
    id<MTLCommandBuffer> command = nil;
    std::array<PicaMetalDrawPublication, 64> publications{};
    // Only wider diagnostic groups allocate additional publication storage.
    // It is filled before commit and immutable while any submitted ticket owns it.
    std::shared_ptr<std::vector<PicaMetalDrawPublication>> extended_publications;
    std::size_t count = 0;
    std::shared_ptr<const PicaMetalValidationReceipt> validation;

    std::span<PicaMetalDrawPublication> Publications() {
        return extended_publications ? std::span(*extended_publications) : std::span(publications);
    }
    std::span<const PicaMetalDrawPublication> Publications() const {
        return extended_publications ? std::span<const PicaMetalDrawPublication>(*extended_publications)
                                     : std::span<const PicaMetalDrawPublication>(publications);
    }

    PicaMetalCompletion Poll() const;
    // Only a real external visibility boundary may wait. Reads/export must
    // still use the same cache intervals and backing generation.
    PicaMetalCompletion WaitUntilGuestRead() const;
};

// Owned by the rasterizer thread. This class records and submits a contiguous
// prefix; it never publishes ownership or exports guest bytes on its own.
class PicaMetalDrawGroup {
public:
    enum class Result { Recorded, BoundaryRequired, Unsupported, Fault };

    PicaMetalDrawGroup(id<MTLCommandQueue> owner_queue, NSUInteger upload_page_bytes,
                       id<MTLLibrary> scale_library = nil, unsigned scale = 1);
    MetalRasterScale& Resolution() { return resolution; }
    ~PicaMetalDrawGroup();
    void Abort() noexcept;
    PicaMetalDrawGroup(const PicaMetalDrawGroup&) = delete;
    PicaMetalDrawGroup& operator=(const PicaMetalDrawGroup&) = delete;

    // For BoundaryRequired, commit the admitted prefix before reconsidering
    // the current draw. If moving to GL, first complete the verified handoff.
    Result Append(const TexturedMetalDraw& draw, const PicaMetalDrawViews& views);
    std::optional<PicaMetalDrawTicket> Commit();
    std::uint64_t TakeUploadWaitNs() { return upload.TakeWaitNs(); }
    bool HasRecordedDraws() const { return pending.count != 0; }

private:
    const std::thread::id owner = std::this_thread::get_id();
    id<MTLCommandQueue> queue;
    PicaMetalUploadArena upload;
    PicaMetalDrawTicket pending;
    PicaMetalSurfaceView color;
    PicaMetalSurfaceView depth_stencil;
    MetalRasterScale resolution;
    bool scaled_group = false;
    bool faulted = false;
};

} // namespace Cara
