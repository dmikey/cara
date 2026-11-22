// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include <cstring>
#include "pica_metal_draw_group.h"
#include "pica_metal_frame_cost.hh"
#include "pica_metal_surface_writer.h"
#include "common/logging/log.h"

#include <algorithm>

namespace Cara {
bool EncodeGroupedMetalDraw(id<MTLCommandBuffer> command, PicaMetalUploadArena& arena,
                           const TexturedMetalDraw& draw);
bool CloseGroupedMetalDraw(id<MTLCommandBuffer> command);
void AbortGroupedMetalDraw(id<MTLCommandBuffer> command) noexcept;

namespace {

bool Valid(PicaGuestRange range) {
    return range.begin < range.end && range.end <= (std::uint64_t{1} << 32);
}

bool Covers(PicaGuestRange outer, PicaGuestRange inner) {
    return (inner.begin == 0 && inner.end == 0) ||
           (Valid(inner) && outer.begin <= inner.begin && inner.end <= outer.end);
}

bool Overlaps(PicaGuestRange a, PicaGuestRange b) {
    return a.begin < b.end && b.begin < a.end;
}

bool CompatibleReadRanges(PicaGuestRange a, PicaGuestRange b) {
    return !Overlaps(a, b) || (a.begin == b.begin && a.end == b.end);
}

bool SameBacking(const PicaMetalSurfaceView& a, const PicaMetalSurfaceView& b) {
    return a.texture == b.texture && a.backing_generation == b.backing_generation;
}

bool CompatibleTexture(id<MTLDevice> device, const PicaMetalSurfaceView& view,
                       id<MTLTexture> used, MTLTextureUsage usage) {
    return view.texture && view.texture == used && used.device == device &&
           used.hazardTrackingMode == MTLHazardTrackingModeTracked &&
           (used.usage & usage) == usage && view.backing_generation;
}

bool CompatibleView(id<MTLDevice> device, const PicaMetalSurfaceView& view,
                    id<MTLTexture> used, MTLTextureUsage usage) {
    return CompatibleTexture(device, view, used, usage) && Valid(view.authoritative_bytes);
}

} // namespace

PicaMetalCompletion PicaMetalValidationReceipt::Poll() const {
    // Metal Completed/Error are final states. Once every shader-side check
    // passes, neither the command nor its immutable validation can change.
    // https://developer.apple.com/documentation/metal/mtlcommandbuffer/status
    const auto terminal = checked.load(std::memory_order_acquire);
    if (terminal != PicaMetalCompletion::Pending)
        return terminal;
    if (!command || command.status < MTLCommandBufferStatusCommitted ||
        command.status == MTLCommandBufferStatusError || command.error) {
        LOG_ERROR(Render_OpenGL, "Metal depth validation command failed (status {}): {}",
                  unsigned(command.status), command.error.localizedDescription.UTF8String ?: "no Metal error");
        checked.store(PicaMetalCompletion::Failed, std::memory_order_release);
        return PicaMetalCompletion::Failed;
    }
    if (command.status != MTLCommandBufferStatusCompleted)
        return PicaMetalCompletion::Pending;
    // Several descendant snapshots can inspect this same receipt concurrently.
    // Serialize only its first completed inspection and one-time ARC releases.
    // No GPU wait is performed while holding this mutex.
    std::lock_guard lock(inspection);
    const auto inspected = checked.load(std::memory_order_relaxed);
    if (inspected != PicaMetalCompletion::Pending)
        return inspected;
    const auto check_source = [&](const auto& writer) {
        const auto status = writer ? writer->Status() : MetalGuestWriteStatus::Complete;
        if (status == MetalGuestWriteStatus::Failed) {
            NativeMetalCost::Count(NativeMetalCost::AsyncDepthChecksFailed, 1);
            checked.store(PicaMetalCompletion::Failed, std::memory_order_release);
            return PicaMetalCompletion::Failed;
        }
        return status == MetalGuestWriteStatus::Pending ? PicaMetalCompletion::Pending
                                                       : PicaMetalCompletion::Complete;
    };
    const auto source_state = check_source(source);
    if (source_state != PicaMetalCompletion::Complete)
        return source_state;
    for (const auto& writer : more_sources) {
        const auto writer_state = check_source(writer);
        if (writer_state != PicaMetalCompletion::Complete)
            return writer_state;
    }
    {
        for (std::size_t i = 0; i < error_count; ++i) {
            const auto& buffer = errors[i];
            const bool valid_buffer = buffer && buffer.device == command.device &&
                buffer.length >= sizeof(u32) && buffer.storageMode == MTLStorageModeShared &&
                buffer.contents;
            const auto error = valid_buffer ? *static_cast<const u32*>(buffer.contents) : 0;
            if (!valid_buffer || error) {
                LOG_ERROR(Render_OpenGL,
                          "Metal depth validation failed at check {}/{} (buffer={}, bits=0x{:08x})",
                          i + 1, error_count, valid_buffer, error);
                NativeMetalCost::Count(NativeMetalCost::AsyncDepthChecksFailed, 1);
                checked.store(PicaMetalCompletion::Failed, std::memory_order_release);
                return PicaMetalCompletion::Failed;
            }
        }
        NativeMetalCost::Count(NativeMetalCost::AsyncDepthChecksPassed, 1);
        errors = {}; // Completed checks no longer retain their staging allocations.
        source.reset();
        more_sources.clear();
        checked.store(PicaMetalCompletion::Complete, std::memory_order_release);
    }
    return PicaMetalCompletion::Complete;
}

PicaMetalCompletion PicaMetalValidationReceipt::Wait() const {
    auto status = Poll();
    if (status != PicaMetalCompletion::Pending)
        return status;
    [command waitUntilCompleted];
    status = Poll();
    if (status != PicaMetalCompletion::Pending)
        return status;
    // The GPU dependency precedes this command. Its host completion or
    // validation receipt may still need resolving at a guest-read boundary.
    std::shared_ptr<const MetalSurfaceWriter> pending_source;
    Sources other_sources;
    {
        std::lock_guard lock(inspection);
        pending_source = source;
        other_sources = more_sources;
    }
    if ((pending_source && !pending_source->ValidateProducerCompletion()) ||
        std::ranges::any_of(other_sources, [](const auto& writer) {
            return !writer->ValidateProducerCompletion();
        })) {
        checked.store(PicaMetalCompletion::Failed, std::memory_order_release);
        return PicaMetalCompletion::Failed;
    }
    return Poll();
}

PicaMetalCompletion PicaMetalDrawTicket::Poll() const {
    if (!command) {
        return PicaMetalCompletion::Failed;
    }
    const auto status = command.status;
    if (status < MTLCommandBufferStatusCommitted || status == MTLCommandBufferStatusError || command.error) {
        return PicaMetalCompletion::Failed;
    }
    if (validation) {
        if (validation->Command() != command)
            return PicaMetalCompletion::Failed;
        const auto checked = validation->Poll();
        if (checked != PicaMetalCompletion::Complete)
            return checked;
    }
    return status == MTLCommandBufferStatusCompleted ? PicaMetalCompletion::Complete
                                                    : PicaMetalCompletion::Pending;
}

PicaMetalCompletion PicaMetalDrawTicket::WaitUntilGuestRead() const {
    if (Poll() == PicaMetalCompletion::Pending) {
        if (validation)
            return validation->Wait();
        [command waitUntilCompleted];
    }
    return Poll();
}

PicaMetalDrawGroup::PicaMetalDrawGroup(id<MTLCommandQueue> owner_queue, NSUInteger upload_page_bytes,
                                     id<MTLLibrary> library, unsigned scale)
    : queue(owner_queue), upload(owner_queue.device, upload_page_bytes), resolution(library, scale) {}

PicaMetalDrawGroup::~PicaMetalDrawGroup() { Abort(); }

void PicaMetalDrawGroup::Abort() noexcept {
    if (std::this_thread::get_id() != owner)
        std::terminate(); // Metal encoding and its thread-local packets have one owner.
    faulted = true;
    AbortGroupedMetalDraw(pending.command);
    upload.Cancel();
    // A failed commit may already have submitted the prefix. Do not release its
    // local backing before completion, and never publish its rejected writes.
    if (pending.command.status >= MTLCommandBufferStatusCommitted &&
        pending.command.status < MTLCommandBufferStatusCompleted)
        [pending.command waitUntilCompleted];
    pending = {};
    color = {};
    depth_stencil = {};
}

PicaMetalDrawGroup::Result PicaMetalDrawGroup::Append(const TexturedMetalDraw& draw,
                                                     const PicaMetalDrawViews& views) {
    if (!queue || faulted || upload.Failed()) {
        return Result::Fault;
    }
    const auto boundary = HasRecordedDraws() ? Result::BoundaryRequired : Result::Unsupported;
    const auto decline = [&](const char* stage) {
        if (boundary == Result::Unsupported)
            LOG_ERROR(Render_OpenGL, "Native Metal draw group {} mode={} vertices={} color=[{:x},{:x}) "
                      "depth=[{:x},{:x}) primary=[{:x},{:x}) secondary=[{:x},{:x})",
                      stage, unsigned(draw.vertex_mode), draw.vertex_count, views.color.authoritative_bytes.begin,
                      views.color.authoritative_bytes.end, views.depth_stencil.authoritative_bytes.begin,
                      views.depth_stencil.authoritative_bytes.end, views.sampled.authoritative_bytes.begin,
                      views.sampled.authoritative_bytes.end,
                      views.secondary_sampled ? views.secondary_sampled->authoritative_bytes.begin : 0,
                      views.secondary_sampled ? views.secondary_sampled->authoritative_bytes.end : 0);
        return boundary;
    };
    if (draw.primary_material_cube != views.primary_cube_faces.has_value())
        return decline("primary cube presence");
    if (views.primary_color_snapshot &&
        (views.primary_cube_faces || !draw.texture0 || draw.texture0 == draw.color ||
         (draw.texture0.usage & MTLTextureUsageRenderTarget) ||
         !Overlaps(views.sampled.authoritative_bytes, views.color.authoritative_bytes)))
        return decline("primary color snapshot");
    const auto compatible_depth_read = [&](const PicaMetalSurfaceView& sampled, unsigned slot) {
        const bool alias = Overlaps(sampled.authoritative_bytes, views.depth_stencil.authoritative_bytes);
        if (!(views.depth_snapshot_slots & (1u << slot)))
            return !alias;
        return alias && sampled.texture && sampled.texture != draw.depth_stencil &&
               !(sampled.texture.usage & MTLTextureUsageRenderTarget);
    };
    const auto compatible_cube_reads = [&](PicaGuestRange range) {
        return !views.primary_cube_faces ||
               std::all_of(views.primary_cube_faces->begin(), views.primary_cube_faces->end(),
                           [&](PicaGuestRange face) { return CompatibleReadRanges(range, face); });
    };
    if (draw.secondary_material.has_value() != views.secondary_sampled.has_value())
        return decline("secondary presence");
    if (const auto& s=views.secondary_sampled) {
        const auto& range=s->authoritative_bytes;
        // Decoded 2D inputs are immutable snapshots. Their guest ranges may
        // overlap, for example when one material reads more mip levels.
        if (!CompatibleView(queue.device,*s,draw.secondary_material->texture,MTLTextureUsageShaderRead) ||
            Overlaps(range,views.color.authoritative_bytes) ||
            !compatible_depth_read(*s, 1) ||
            !compatible_cube_reads(range))
            return decline("secondary view or alias");
    }
    if (draw.tertiary_material.has_value() != views.tertiary_sampled.has_value())
        return decline("tertiary presence");
    if (const auto& third = views.tertiary_sampled) {
        const auto range = third->authoritative_bytes;
        if (!views.secondary_sampled ||
            !CompatibleView(queue.device, *third, draw.tertiary_material->texture, MTLTextureUsageShaderRead) ||
            Overlaps(range, views.color.authoritative_bytes) ||
            !compatible_depth_read(*third, 2) ||
            !compatible_cube_reads(range))
            return decline("tertiary view or alias");
    }
    if (views.primary_cube_faces) {
        const auto& primary = views.sampled;
        if (!CompatibleTexture(queue.device, primary, draw.texture0, MTLTextureUsageShaderRead) ||
            primary.authoritative_bytes.begin || primary.authoritative_bytes.end ||
            !std::all_of(views.primary_cube_faces->begin(), views.primary_cube_faces->end(),
                [&](PicaGuestRange range) {
                    return Valid(range) && !Overlaps(range, views.color.authoritative_bytes) &&
                           !Overlaps(range, views.depth_stencil.authoritative_bytes);
                }))
            return decline("primary cube views or aliases");
    } else if (draw.texture0) {
        if (!CompatibleView(queue.device, views.sampled, draw.texture0, MTLTextureUsageShaderRead) ||
            (!views.primary_color_snapshot &&
             Overlaps(views.sampled.authoritative_bytes, views.color.authoritative_bytes)) ||
            !compatible_depth_read(views.sampled, 0))
            return decline("primary view or alias");
    } else if (views.sampled.texture || views.sampled.backing_generation ||
               views.sampled.authoritative_bytes.begin || views.sampled.authoritative_bytes.end) {
        return decline("unexpected primary view");
    }
    if (draw.lighting_lut && (draw.lighting_lut.device != queue.device ||
        !(draw.lighting_lut.usage & MTLTextureUsageShaderRead) ||
        draw.lighting_lut.hazardTrackingMode != MTLHazardTrackingModeTracked))
        return decline("lighting view");
    const auto depth_range = views.depth_stencil.authoritative_bytes;
    const bool depth_coverage = draw.unbacked_depth
        ? depth_range.begin == 0 && depth_range.end == 0
        : Valid(depth_range);
    if (!CompatibleView(queue.device, views.color, draw.color, MTLTextureUsageRenderTarget) ||
        !depth_coverage || !CompatibleTexture(queue.device, views.depth_stencil, draw.depth_stencil,
                                              MTLTextureUsageRenderTarget) ||
        !Covers(views.color.authoritative_bytes, views.color_dirty) ||
        !Covers(views.depth_stencil.authoritative_bytes, views.depth_stencil_dirty) ||
        Overlaps(views.color.authoritative_bytes, views.depth_stencil.authoritative_bytes)) {
        return decline("attachment coverage or alias");
    }
    if (HasRecordedDraws()) {
        if (pending.count == NativeMetalCost::DrawGroupLimit()) {
            NativeMetalCost::Count(NativeMetalCost::DrawBoundaryCapacity, 1);
            return Result::BoundaryRequired;
        }
        if (!SameBacking(color, views.color) || !SameBacking(depth_stencil, views.depth_stencil)) {
            NativeMetalCost::Count(NativeMetalCost::DrawBoundaryBacking, 1);
            return Result::BoundaryRequired;
        }
    } else {
        const auto limit = NativeMetalCost::DrawGroupLimit();
        if (limit > pending.publications.size())
            pending.extended_publications =
                std::make_shared<std::vector<PicaMetalDrawPublication>>(limit);
        pending.command = [queue commandBuffer];
        if (!upload.Begin(pending.command)) {
            pending.command = nil;
            LOG_ERROR(Render_OpenGL, "Native Metal upload arena failed admission (fault={})",
                      upload.Failed());
            return upload.Failed() ? Result::Fault : Result::Unsupported;
        }
        color = views.color;
        depth_stencil = views.depth_stencil;
    }
    if (!HasRecordedDraws())
        scaled_group = resolution.Factor() > 1 && resolution.Begin(
            pending.command, draw.color, draw.depth_stencil, !draw.unbacked_depth);
    const auto encode = [&] {
        if (!scaled_group)
            return EncodeGroupedMetalDraw(pending.command, upload, draw);
        auto scaled = draw;
        const unsigned factor = resolution.Factor();
        scaled.color = resolution.Target().color;
        scaled.depth_stencil = resolution.Target().depth;
        scaled.viewport.originX *= factor; scaled.viewport.originY *= factor;
        scaled.viewport.width *= factor; scaled.viewport.height *= factor;
        scaled.scissor.x *= factor; scaled.scissor.y *= factor;
        scaled.scissor.width *= factor; scaled.scissor.height *= factor;
        OpenGL::UniformData common{};
        if (draw.common_uniform.size_bytes() != sizeof(common))
            return false;
        std::memcpy(&common, draw.common_uniform.data(), sizeof(common));
        common.scissor_x1 *= factor; common.scissor_y1 *= factor;
        common.scissor_x2 *= factor; common.scissor_y2 *= factor;
        scaled.common_uniform = std::as_bytes(std::span(&common, 1));
        return EncodeGroupedMetalDraw(pending.command, upload, scaled);
    };
    if (!encode()) {
        // The shared encoder returns false before encoding any draw. Its
        // consumed upload ranges may be dropped with an empty command; bytes
        // leased to an already recorded prefix must survive through commit.
        if (!HasRecordedDraws()) {
            resolution.Invalidate();
            AbortGroupedMetalDraw(pending.command);
            upload.Cancel();
            pending.command = nil;
            color = {};
            depth_stencil = {};
        }
        return decline("draw encoder");
    }
    pending.Publications()[pending.count++] = {
        {views.color.backing_generation, views.color_dirty},
        {views.depth_stencil.backing_generation, views.depth_stencil_dirty},
    };
    return Result::Recorded;
}

std::optional<PicaMetalDrawTicket> PicaMetalDrawGroup::Commit() {
    if (!HasRecordedDraws()) {
        return std::nullopt;
    }
    if (!CloseGroupedMetalDraw(pending.command) ||
        (scaled_group && !resolution.Resolve(pending.command)) || !upload.Submit()) {
        // The pre-encoded prefix has no valid writer ticket and may not be
        // silently discarded or marked guest-visible.
        faulted = true;
        return std::nullopt;
    }
    const PicaMetalDrawTicket committed = pending;
    pending = {};
    color = {};
    depth_stencil = {};
    return committed;
}

} // namespace Cara
