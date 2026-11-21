// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_draw_group.h"
#include "pica_metal_frame_cost.hh"
#include "common/logging/log.h"

namespace Cara {
bool EncodeGroupedMetalDraw(id<MTLCommandBuffer> command, PicaMetalUploadArena& arena,
                           const TexturedMetalDraw& draw);
bool CloseGroupedMetalDraw(id<MTLCommandBuffer> command);

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

bool CompatibleView(id<MTLDevice> device, const PicaMetalSurfaceView& view,
                    id<MTLTexture> used, MTLTextureUsage usage) {
    return view.texture && view.texture == used && used.device == device &&
           used.hazardTrackingMode == MTLHazardTrackingModeTracked &&
           (used.usage & usage) == usage && view.backing_generation &&
           Valid(view.authoritative_bytes);
}

} // namespace

PicaMetalCompletion PicaMetalDrawTicket::Poll() const {
    if (!command) {
        return PicaMetalCompletion::Failed;
    }
    const auto status = command.status;
    if (status < MTLCommandBufferStatusCommitted || status == MTLCommandBufferStatusError) {
        return PicaMetalCompletion::Failed;
    }
    return status == MTLCommandBufferStatusCompleted ? PicaMetalCompletion::Complete
                                                    : PicaMetalCompletion::Pending;
}

PicaMetalCompletion PicaMetalDrawTicket::WaitUntilGuestRead() const {
    if (Poll() == PicaMetalCompletion::Pending) {
        [command waitUntilCompleted];
    }
    return Poll();
}

PicaMetalDrawGroup::PicaMetalDrawGroup(id<MTLCommandQueue> owner_queue, NSUInteger upload_page_bytes)
    : queue(owner_queue), upload(owner_queue.device, upload_page_bytes) {}

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
    if (draw.secondary_material.has_value() != views.secondary_sampled.has_value())
        return decline("secondary presence");
    if (const auto& s=views.secondary_sampled) {
        const auto& range=s->authoritative_bytes;
        const auto& primary = views.sampled.authoritative_bytes;
        // Separately decoded immutable inputs may read the same complete
        // guest range. Neither input is allowed to alias a write attachment.
        const bool exact_read_alias = range.begin == primary.begin && range.end == primary.end;
        if (!CompatibleView(queue.device,*s,draw.secondary_material->texture,MTLTextureUsageShaderRead) ||
            Overlaps(range,views.color.authoritative_bytes) ||
            Overlaps(range,views.depth_stencil.authoritative_bytes) ||
            (Overlaps(range, primary) && !exact_read_alias))
            return decline("secondary view or alias");
    }
    if (draw.tertiary_material.has_value() != views.tertiary_sampled.has_value())
        return decline("tertiary presence");
    if (const auto& third = views.tertiary_sampled) {
        const auto range = third->authoritative_bytes;
        if (!views.secondary_sampled ||
            !CompatibleView(queue.device, *third, draw.tertiary_material->texture, MTLTextureUsageShaderRead) ||
            Overlaps(range, views.color.authoritative_bytes) ||
            Overlaps(range, views.depth_stencil.authoritative_bytes) ||
            !CompatibleReadRanges(range, views.sampled.authoritative_bytes) ||
            !CompatibleReadRanges(range, views.secondary_sampled->authoritative_bytes))
            return decline("tertiary view or alias");
    }
    if (draw.texture0) {
        if (!CompatibleView(queue.device, views.sampled, draw.texture0, MTLTextureUsageShaderRead) ||
            Overlaps(views.sampled.authoritative_bytes, views.color.authoritative_bytes) ||
            Overlaps(views.sampled.authoritative_bytes, views.depth_stencil.authoritative_bytes))
            return decline("primary view or alias");
    } else if (views.sampled.texture || views.sampled.backing_generation ||
               views.sampled.authoritative_bytes.begin || views.sampled.authoritative_bytes.end) {
        return decline("unexpected primary view");
    }
    if (draw.lighting_lut && (draw.lighting_lut.device != queue.device ||
        !(draw.lighting_lut.usage & MTLTextureUsageShaderRead) ||
        draw.lighting_lut.hazardTrackingMode != MTLHazardTrackingModeTracked))
        return decline("lighting view");
    if (!CompatibleView(queue.device, views.color, draw.color, MTLTextureUsageRenderTarget) ||
        !CompatibleView(queue.device, views.depth_stencil, draw.depth_stencil,
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
    if (!EncodeGroupedMetalDraw(pending.command, upload, draw)) {
        // The shared encoder returns false before encoding any draw. Its
        // consumed upload ranges may be dropped with an empty command; bytes
        // leased to an already recorded prefix must survive through commit.
        if (!HasRecordedDraws()) {
            upload.Cancel();
            pending.command = nil;
            color = {};
            depth_stencil = {};
        }
        return decline("draw encoder");
    }
    pending.publications[pending.count++] = {
        {views.color.backing_generation, views.color_dirty},
        {views.depth_stencil.backing_generation, views.depth_stencil_dirty},
    };
    return Result::Recorded;
}

std::optional<PicaMetalDrawTicket> PicaMetalDrawGroup::Commit() {
    if (!HasRecordedDraws()) {
        return std::nullopt;
    }
    if (!CloseGroupedMetalDraw(pending.command) || !upload.Submit()) {
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
