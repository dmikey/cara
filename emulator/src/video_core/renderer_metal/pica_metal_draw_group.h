// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

#include "pica_metal_draw.h"

#include <array>
#include <cstdint>

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
    PicaMetalValidationReceipt(id<MTLCommandBuffer> command_,
                              std::array<id<MTLBuffer>, 2> errors_,
                              std::shared_ptr<const MetalSurfaceWriter> source_ = {})
        : command(command_), errors(errors_), source(std::move(source_)) {}
    PicaMetalCompletion Poll() const;
    PicaMetalCompletion Wait() const;
    id<MTLCommandBuffer> Command() const { return command; }
private:
    const id<MTLCommandBuffer> command;
    mutable std::array<id<MTLBuffer>, 2> errors;
    mutable std::shared_ptr<const MetalSurfaceWriter> source;
    mutable std::atomic<PicaMetalCompletion> checked{PicaMetalCompletion::Pending};
    mutable std::mutex inspection;
};

struct PicaMetalDrawTicket {
    id<MTLCommandBuffer> command = nil;
    std::array<PicaMetalDrawPublication, 64> publications{};
    std::size_t count = 0;
    std::shared_ptr<const PicaMetalValidationReceipt> validation;

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

    PicaMetalDrawGroup(id<MTLCommandQueue> owner_queue, NSUInteger upload_page_bytes);
    ~PicaMetalDrawGroup();
    void Abort() noexcept;
    PicaMetalDrawGroup(const PicaMetalDrawGroup&) = delete;
    PicaMetalDrawGroup& operator=(const PicaMetalDrawGroup&) = delete;

    // For BoundaryRequired, commit the admitted prefix before reconsidering
    // the current draw. If moving to GL, first complete the verified handoff.
    Result Append(const TexturedMetalDraw& draw, const PicaMetalDrawViews& views);
    std::optional<PicaMetalDrawTicket> Commit();
    bool HasRecordedDraws() const { return pending.count != 0; }

private:
    const std::thread::id owner = std::this_thread::get_id();
    id<MTLCommandQueue> queue;
    PicaMetalUploadArena upload;
    PicaMetalDrawTicket pending;
    PicaMetalSurfaceView color;
    PicaMetalSurfaceView depth_stencil;
    bool faulted = false;
};

} // namespace Cara
