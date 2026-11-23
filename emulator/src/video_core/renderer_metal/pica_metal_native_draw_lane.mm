// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_striped_source.hh"
#include "video_core/renderer_metal/exporter-adapter/pica_metal_queued_color_export.hh"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_depth_clear_encoding.hh"

#include <algorithm>
#include <array>
#include <TargetConditionals.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <fmt/format.h>
#import <Foundation/Foundation.h>
#include "core/memory.h"
#include "video_core/pica_state.h"
#include "video_core/renderer_metal/exporter-adapter/pica_metal_visibility_export.h"
#include "video_core/rasterizer_interface.h"

namespace Cara {
id<MTLBuffer> EncodeQueuedCheckedDepthExport(
    id<MTLCommandBuffer> command, id<MTLTexture> texture,
    PicaGuestRange physical_surface, PicaGuestRange requested,
    id<MTLComputePipelineState> tiled_pack, id<MTLComputePipelineState> depth_pack,
    id<MTLBuffer> errors);
}

namespace Cara::MetalTransfer {
std::optional<LinearColorTransferPlan> PlanNativeLinearColorTransfer(DisplayTransferRegisters regs);
}

namespace Cara {
namespace {

// Only the canonical VRAM aperture has complete write coverage here.
constexpr u32 MaxResidentSourceBytes = 1024 * 1024;
constexpr u64 MaxResidentTextureBytes = 128 * 1024 * 1024;
constexpr std::size_t MaxResidentTextures = 1024;

struct ResidentTextureKey {
    PAddr begin, end;
    u32 row_bytes, width, height, format;
    bool disable_alpha;

    auto operator<=>(const ResidentTextureKey&) const = default;
};

struct ResidentTexture {
    id<MTLTexture> texture;
    u64 last_use;
};

struct DecodedTextureResidency {
    id<MTLCommandQueue> queue;
    std::map<ResidentTextureKey, ResidentTexture> entries;
    u64 bytes = 0, time = 0, hits = 0, misses = 0, invalidations = 0, evictions = 0;
    u64 vram_decodes = 0, other_decodes = 0;

    static u64 Cost(const ResidentTextureKey& key) { return u64{key.width} * key.height * 4; }

    void Invalidate(u64 begin, u64 end) {
        if (begin >= end || begin >= Memory::VRAM_PADDR_END || end <= Memory::VRAM_PADDR)
            return;
        const auto first = static_cast<PAddr>(
            std::max<u64>(Memory::VRAM_PADDR, begin > MaxResidentSourceBytes
                ? begin - MaxResidentSourceBytes : 0));
        for (auto it = entries.lower_bound(ResidentTextureKey{.begin = first});
             it != entries.end() && it->first.begin < end;) {
            if (begin < it->first.end) {
                bytes -= Cost(it->first);
                it = entries.erase(it);
                ++invalidations;
            } else {
                ++it;
            }
        }
    }

    void Insert(const ResidentTextureKey& key, id<MTLTexture> texture) {
        const auto cost = Cost(key);
        while (entries.size() >= MaxResidentTextures || cost > MaxResidentTextureBytes - bytes) {
            const auto victim = std::min_element(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
            bytes -= Cost(victim->first);
            entries.erase(victim);
            ++evictions;
        }
        entries.emplace(key, ResidentTexture{texture, ++time});
        bytes += cost;
    }
};

thread_local std::unique_ptr<DecodedTextureResidency> decoded_residency;

bool TextureResidencyEnabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_TEXTURE_RESIDENCY");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

void InvalidateDecodedTextures(id<MTLCommandQueue> queue, u64 begin, u64 end) {
    if (decoded_residency && decoded_residency->queue == queue)
        decoded_residency->Invalidate(begin, end);
}


bool OverlapsAttachment(PAddr begin, PAddr end, const PicaMetalNativeAttachment& attachment) {
    const auto range = attachment.view.authoritative_bytes;
    return begin < range.end && range.begin < end;
}

struct UniformDepthFills {
    struct Fill {
        id<MTLBuffer> backing = nil;
        u32 word = 0;
    };
    id<MTLCommandQueue> queue;
    std::array<Fill, 16> fills{};
    std::size_t next = 0;
};

UniformDepthFills* ActiveDepthFills(id<MTLCommandQueue> queue) {
    if (!NativeMetalCost::ClearUniformDepth())
        return nullptr;
    thread_local std::unique_ptr<UniformDepthFills> history;
    if (!history || history->queue != queue) {
        history = std::make_unique<UniformDepthFills>();
        history->queue = queue;
    }
    return history.get();
}

std::optional<u32> FullDepthFill(id<MTLCommandQueue> queue, MetalGuestAuthority& authority,
                                 MetalTransfer::GuestInterval bytes,
                                 const PicaMetalSubmissionQueue& submissions) {
    auto* history = ActiveDepthFills(queue);
    if (!history)
        return std::nullopt;
    const auto owners = authority.NativeSegments(bytes.begin, bytes.end);
    if (owners.size() != 1 || owners.front().begin != bytes.begin || owners.front().end != bytes.end)
        return std::nullopt;
    const auto* writer = dynamic_cast<const MetalSurfaceWriter*>(owners.front().writer.get());
    if (!writer || writer->Queue() != queue || writer->RawGuestBegin() != bytes.begin ||
        writer->RawGuestBuffer().length != bytes.Size() ||
        (writer->Status() != MetalGuestWriteStatus::Complete &&
         !submissions.IsTrackedProducer(writer->Command())))
        return std::nullopt;
    for (const auto& fill : history->fills)
        if (fill.backing && fill.backing == writer->RawGuestBuffer())
            return fill.word;
    return std::nullopt;
}

struct DepthExportProof {
    PicaGuestRange surface;
    id<MTLTexture> texture;
    u64 generation;
    id<MTLComputePipelineState> tiled_export, depth_pack;
};
struct CheckedDepthExporter {
    MetalGuestExport exporter;
    DepthExportProof proof;
    bool operator()(id<MTLCommandQueue> queue, id<MTLResource> resource, u64 generation,
                    PAddr begin, PAddr end, std::span<u8> bytes) const {
        return exporter(queue, resource, generation, begin, end, bytes);
    }
};
struct DeferredDepthExport {
    MetalGuestExport export_bytes;
    std::shared_ptr<bool> initialized;
    u32 clear_word;
    DepthExportProof proof;

    bool operator()(id<MTLCommandQueue> command_queue, id<MTLResource> resource, u64 generation,
                    PAddr begin, PAddr end, std::span<u8> output) const {
        return *initialized && export_bytes(command_queue, resource, generation, begin, end, output);
    }
};

struct ColorExportContext {
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> tiled_pack;
};
thread_local std::unique_ptr<ColorExportContext> color_export;

// Explicitly registered by the native Apple rasterizer on its owner thread.
// This submission state does not become part of the guest checkpoint.
struct TextureCopyBatch {
    struct Copy {
        id<MTLBuffer> buffer = nil;
        MetalTransfer::Rows destination{};
    };
    PicaMetalNativeDrawLane* owner = nullptr;
    id<MTLCommandBuffer> command = nil;
    std::array<Copy, 2> copies{};
    std::size_t count = 0;
};
thread_local TextureCopyBatch texture_copy_batch;

bool HasTextureCopyBatch(const PicaMetalNativeDrawLane* lane) {
    return texture_copy_batch.count && texture_copy_batch.owner == lane;
}

struct BytePublications {
    MetalGuestWrite dense{};
    std::vector<MetalGuestWrite> striped;

    std::span<const MetalGuestWrite> Writes() const {
        if (dense.writer)
            return {&dense, 1};
        return striped;
    }
};

BytePublications PrepareBytePublications(
    id<MTLCommandBuffer> command, id<MTLBuffer> buffer, const MetalTransfer::Rows& rows,
    u64 generation, std::shared_ptr<const MetalProducerPredecessors> predecessors) {
    const auto bytes = rows.footprint;
    const auto writer = std::make_shared<MetalSurfaceWriter>(
        buffer, command, generation, bytes.begin, bytes.end,
        [buffer, bytes](id<MTLCommandQueue>, id<MTLResource>, u64, PAddr begin, PAddr end,
                        std::span<u8> output) {
            const auto* data = static_cast<const u8*>(buffer.contents);
            if (!data || !bytes.Contains({begin, end}) || output.size() != end - begin)
                return false;
            std::memcpy(output.data(), data + (begin - bytes.begin), output.size());
            return true;
        },
        0, std::move(predecessors), buffer);
    BytePublications published;
    if (NativeMetalCost::ReduceCopyQueueWork() && rows.count == 1) {
        published.dense = {bytes.begin, bytes.end, writer};
    } else {
        published.striped.reserve(rows.count);
        for (u32 i = 0; i < rows.count; ++i) {
            const auto stripe = rows.Stripe(i);
            published.striped.push_back({stripe.begin, stripe.end, writer});
        }
    }
    return published;
}

bool SameBacking(const PicaMetalSurfaceView& a, const PicaMetalSurfaceView& b) {
    return a.texture == b.texture && a.backing_generation == b.backing_generation &&
           a.authoritative_bytes.begin == b.authoritative_bytes.begin &&
           a.authoritative_bytes.end == b.authoritative_bytes.end;
}

bool ValidRgba8(const PicaMetalNativeAttachment& attachment) {
    if (!attachment.rgba8)
        return true;
    const auto& layout = *attachment.rgba8;
    const auto& view = attachment.view;
    return layout.begin < layout.end && layout.begin == view.authoritative_bytes.begin &&
           layout.end == view.authoritative_bytes.end &&
           layout.width == view.texture.width && layout.height == view.texture.height &&
           view.texture.pixelFormat == MTLPixelFormatRGBA8Unorm &&
           layout.width && layout.height &&
           (layout.end - layout.begin) % 4 == 0 &&
           (layout.end - layout.begin) / 4 % layout.width == 0 &&
           (layout.end - layout.begin) / 4 / layout.width == layout.height &&
           (!layout.tiled || ((layout.width | layout.height) & 7u) == 0);
}

} // namespace

void RegisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane& lane) {
    const auto* value = std::getenv("CARA_METAL_BATCH_TEXTURE_COPIES");
    if (!value || std::string_view(value) != "1")
        return;
    if (texture_copy_batch.owner)
        throw std::logic_error("Native Metal texture copy batch already owns this thread");
    texture_copy_batch.owner = &lane;
}

void UnregisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane& lane) noexcept {
    if (texture_copy_batch.owner != &lane)
        return;
    NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchAbandoned, texture_copy_batch.count);
    texture_copy_batch = {};
}

void ReportNativeMetalTextureResidency(const std::string& path) {
    if (!decoded_residency)
        return;
    const auto& c = *decoded_residency;
    std::ofstream out(path + "metal-texture-residency.csv");
    out << "counter,value\n"
        << "hits," << c.hits << '\n' << "misses," << c.misses << '\n'
        << "invalidations," << c.invalidations << '\n' << "evictions," << c.evictions << '\n'
        << "resident_levels," << c.entries.size() << '\n' << "resident_bytes," << c.bytes << '\n'
        << "vram_decodes," << c.vram_decodes << '\n' << "other_decodes," << c.other_decodes << '\n';
}

PicaMetalNativeDrawLane::PicaMetalNativeDrawLane(id<MTLCommandQueue> owner_queue,
                                               NSUInteger upload_page_bytes,
                                               GuestWriteNotice notice, GuestRamView ram_view_)
    : queue(owner_queue), write_notice(std::move(notice)), ram_view(std::move(ram_view_)),
      submissions(queue, [this] { authority.RejectSubmittedWrites(); }),
      group(queue, upload_page_bytes) {}

PicaMetalNativeDrawLane::~PicaMetalNativeDrawLane() { Abort(); }

void PicaMetalNativeDrawLane::Abort() noexcept {
    // Group cleanup enforces the same owner thread as normal encoding. Reject
    // authority before waiting so no failed renderer can resume guest access.
    authority.RejectSubmittedWrites();
    group.Abort();
    pending.reset();
    UnregisterNativeMetalTextureCopyBatch(*this);
    submissions.Abort();
    resident_color.reset();
    resident_depth.reset();
    previous_lcd = {};
    flush_readback.writer.reset();
}

void PicaMetalNativeDrawLane::Record(const TexturedMetalDraw& draw, const PicaMetalDrawViews& views,
                                    const PicaMetalNativeAttachment& color,
                                    const PicaMetalNativeAttachment& depth) {
    NativeMetalCost::Scope cost(NativeMetalCost::Record);
    submissions.CheckThread();
    if (HasTextureCopyBatch(this))
        PublishPending();
    {
        NativeMetalCost::Scope retire(NativeMetalCost::RecordRetire);
        submissions.Retire(false);
    }
    if (!color.exporter || !depth.exporter || !SameBacking(color.view, views.color) ||
        !SameBacking(depth.view, views.depth_stencil) || !ValidRgba8(color) || depth.rgba8)
        submissions.Fail("Invalid native Metal attachment ownership");

    if (pending && (!SameBacking(pending->color.view, color.view) ||
                    !SameBacking(pending->depth.view, depth.view) || pending->color.rgba8 != color.rgba8))
        PublishPending();

    static const bool late_reserve = [] {
        const auto* value = std::getenv("CARA_METAL_LATE_DRAW_RESERVE");
        return value && std::string_view(value) == "1";
    }();
    for (;;) {
        // Metadata copying can allocate. Do it before Append records a draw.
        std::optional<Attachments> first_draw;
        if (!pending) {
            NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Draw);
            // The upload arena guards its own pages. Encoding a new command
            // does not need a submission slot until PublishPending commits it.
            if (!late_reserve)
                submissions.Reserve();
            else if (submissions.Full())
                NativeMetalCost::Count(NativeMetalCost::DrawEncodeWhileQueueFull, 1);
            first_draw.emplace(Attachments{color, depth});
        }

        NativeMetalCost::Scope append(NativeMetalCost::RecordAppend);
        const auto* deferred = first_draw ? depth.exporter.target<DeferredDepthExport>() : nullptr;
        std::optional<PicaDepthClearEncoding> clear;
        if (deferred && !*deferred->initialized)
            clear.emplace(PicaDepthClearEncoding{queue, depth.view.texture, deferred->clear_word});
        const auto result = [&] {
            if (!clear)
                return group.Append(draw, views);
            ScopedPicaDepthClearEncoding scope(&*clear);
            return group.Append(draw, views);
        }();
        if (clear && result == PicaMetalDrawGroup::Result::Recorded) {
            if (!clear->encoded)
                submissions.Fail("Deferred native Metal depth clear was not encoded");
            *deferred->initialized = true;
            NativeMetalCost::Count(NativeMetalCost::DepthClearFused, 1);
        }
        append.Stop();
        if (result == PicaMetalDrawGroup::Result::Recorded) {
            static_assert(std::is_nothrow_move_assignable_v<decltype(pending)>);
            if (first_draw)
                pending = std::move(first_draw);
            NativeMetalCost::Scope invalidate(NativeMetalCost::RecordInvalidate);
            InvalidateAttachments(views.color_dirty, &color.view);
            InvalidateAttachments(views.depth_stencil_dirty, &depth.view);
            InvalidateDecodedTextures(queue, views.color_dirty.begin, views.color_dirty.end);
            InvalidateDecodedTextures(queue, views.depth_stencil_dirty.begin,
                                      views.depth_stencil_dirty.end);
            invalidate.Stop();
            NativeMetalCost::Scope notify(NativeMetalCost::RecordNotify);
            if (write_notice) {
                for (const auto dirty : {views.color_dirty, views.depth_stencil_dirty}) {
                    if (dirty.begin < dirty.end) {
                        if (dirty.end > std::numeric_limits<PAddr>::max())
                            submissions.Fail("Native draw address exceeds physical memory range");
                        write_notice(static_cast<PAddr>(dirty.begin), static_cast<PAddr>(dirty.end));
                    }
                }
            }
            return;
        }
        if (result != PicaMetalDrawGroup::Result::BoundaryRequired || !pending)
            submissions.Fail("Native Metal draw could not be admitted");
        PublishPending();
    }
}

void PicaMetalNativeDrawLane::RecordPica(
    const Pica::State& state, bool is_indexed, u16 selected_inputs, TexturedMetalDraw draw,
    const PicaMetalDrawViews& views, const PicaMetalNativeAttachment& color,
    const PicaMetalNativeAttachment& depth, const MetalGuestAuthority::GuestReader& ram) {
    submissions.CheckThread();
    if (HasTextureCopyBatch(this))
        PublishPending();
    submissions.Retire(false);
    const auto read = [&](PAddr first, std::span<u8> bytes) {
        if (bytes.size() > std::numeric_limits<PAddr>::max() - first)
            return false;
        const auto last = first + bytes.size();
        const auto aliases = [&](const PicaMetalNativeAttachment& attachment) {
            const auto region = attachment.view.authoritative_bytes;
            return first < region.end && region.begin < last;
        };
        if (pending && (aliases(pending->color) || aliases(pending->depth)))
            PublishPending();
        return authority.ReadIntoScratch(first, bytes, ram);
    };
    try {
        if (draw.vertex_mode != PicaMetalVertexMode::Packed)
            submissions.Fail("Native PICA draw needs the matching typed vertex producer");
        const auto inputs = CapturePicaMetalPackedInputs(state, is_indexed, selected_inputs, read);
        if (!inputs)
            submissions.Fail("Could not capture native packed PICA inputs");
        draw.packed_vertices = std::as_bytes(std::span(inputs->vertices));
        draw.expanded_indices = std::as_bytes(std::span(inputs->indices));
        draw.layout_uniform = std::as_bytes(std::span(inputs->layout));
        draw.vertex_count = inputs->vertex_count;
        Record(draw, views, color, depth);
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
}

namespace {

constexpr NSUInteger MaxBackingBytes = 16 * 1024 * 1024;

bool RetireCachedInputsAtRecord() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_RETIRE_CACHED_INPUTS_AT_RECORD");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

MetalTransfer::Rows DenseRows(MetalTransfer::GuestInterval bytes) {
    return {bytes, bytes.Size(), bytes.Size(), 1};
}

} // namespace

id<MTLBuffer> PicaMetalNativeDrawLane::AllocateBytes(MetalTransfer::GuestInterval bytes) {
    if (bytes.begin >= bytes.end || bytes.Size() > MaxBackingBytes ||
        bytes.Size() > queue.device.maxBufferLength)
        return nil;
    constexpr MTLResourceOptions options =
        MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;
    return [queue.device newBufferWithLength:bytes.Size() options:options];
}

void PicaMetalNativeDrawLane::Track(id<MTLCommandBuffer> command,
                                  std::shared_ptr<const PicaMetalValidationReceipt> validation) {
    PicaMetalDrawTicket ticket;
    ticket.command = command;
    ticket.validation = std::move(validation);
    submissions.Track(ticket);
}

void PicaMetalNativeDrawLane::SubmitSampledTextureBuild(id<MTLCommandBuffer> command) {
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Sample);
    submissions.CheckThread();
    if (HasTextureCopyBatch(this))
        PublishPending();
    if (!command || command.commandQueue != queue || !command.retainedReferences ||
        command.status != MTLCommandBufferStatusNotEnqueued)
        submissions.Fail("Invalid sampled Metal texture build");
    // Like the checked synchronous path, commit the private mip assembly
    // before the unsubmitted draw group. Its immutable source textures
    // cannot alias that group's attachments. Subsequent draws may sample
    // the new chain on the same queue without splitting the group.
    submissions.Retire(false);
    submissions.Reserve();
    try {
        [command commit];
        Track(command);
        NativeMetalCost::Count(NativeMetalCost::QueuedMipBuild, 1);
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
}

void PicaMetalNativeDrawLane::PublishBytes(id<MTLCommandBuffer> command, id<MTLBuffer> buffer,
                                          const MetalTransfer::Rows& rows) {
    NativeMetalCost::Scope cost(NativeMetalCost::PublishBytesHost);
    if (NativeMetalCost::OverlapTransfers()) {
        NativeMetalCost::Scope reserve(NativeMetalCost::LateTransferReserve);
        // Command creation and encoding need no submitted slot. Keep the
        // original bounded queue and reserve before publishing any ownership.
        submissions.Retire(false);
        submissions.Reserve();
    }
    if (byte_generation == std::numeric_limits<u64>::max())
        submissions.Fail("Native Metal byte backing generation exhausted");

    if (submissions.Full())
        submissions.Fail("Native Metal transfer lacks a reserved submission slot");
    const auto bytes = rows.footprint;
    const auto published = PrepareBytePublications(command, buffer, rows, ++byte_generation,
                                                    submissions.Predecessors());
    const auto writes = published.Writes();

    try {
        [command commit];
        Track(command);
        authority.Publish(writes);
        InvalidateAttachments({bytes.begin, bytes.end});
        for (const auto& write : writes)
            InvalidateDecodedTextures(queue, write.begin, write.end);
        if (write_notice) {
            for (const auto& write : writes)
                write_notice(write.begin, write.end);
        }
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
}

namespace {
bool JoinCheckedDepthImport() {
    static const bool on = [] {
        const auto* value = std::getenv("CARA_METAL_JOIN_CHECKED_DEPTH_IMPORT");
        return value && std::string_view(value) == "1";
    }();
    return on;
}
bool AsyncCheckedDepthImport() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

bool QueuePendingDepthExport() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT");
        return value && std::string_view(value) == "1";
    }();
    return enabled && AsyncCheckedDepthImport();
}

bool ValidatePresentationReceipts(const MetalProducerPredecessors& predecessors) {
    if (predecessors.count > predecessors.commands.size() ||
        predecessors.count > predecessors.validations.size())
        return false;
    for (std::size_t i = 0; i < predecessors.count; ++i) {
        const auto& receipt = predecessors.validations[i];
        if (receipt && (receipt->Command() != predecessors.commands[i] ||
                        receipt->Wait() != PicaMetalCompletion::Complete))
            return false;
    }
    return true;
}

struct JoinedDepthCheck {
    MetalGuestWriterRef producer;
    std::shared_ptr<const MetalSurfaceWriter> pending_source;
    id<MTLBuffer> errors = nil;
};
thread_local JoinedDepthCheck* active_depth_check = nullptr;
struct ScopedJoinedDepthCheck {
    JoinedDepthCheck* previous = active_depth_check;
    explicit ScopedJoinedDepthCheck(JoinedDepthCheck* check) { active_depth_check = check; }
    ~ScopedJoinedDepthCheck() { active_depth_check = previous; }
};
} // namespace

std::optional<MetalTransfer::ByteView> PicaMetalNativeDrawLane::ResolveBytes(
    id<MTLCommandBuffer> command, const MetalTransfer::Rows& rows, NSUInteger alignment,
    const MetalGuestAuthority::GuestReader& ram, ByteUse use) {
    const bool display_transfer = use == ByteUse::DisplayTransfer;
    NativeMetalCost::Scope cost(NativeMetalCost::ResolveBytesHost);
    const auto bytes = rows.footprint;
    if (!command || command.commandQueue != queue || !command.retainedReferences ||
        command.status != MTLCommandBufferStatusNotEnqueued || bytes.begin >= bytes.end ||
        bytes.Size() > MaxBackingBytes || !rows.count || !rows.width_bytes ||
        rows.width_bytes > rows.stride_bytes ||
        u64{rows.count - 1} * rows.stride_bytes + rows.width_bytes != bytes.Size())
        return std::nullopt;

    const auto current = authority.NativeSegments(bytes.begin, bytes.end);
    const auto raw = [&](const MetalGuestWrite& segment) -> const MetalSurfaceWriter* {
        const auto* writer = dynamic_cast<const MetalSurfaceWriter*>(segment.writer.get());
        if (!writer || writer->Queue() != queue || !writer->RawGuestBuffer() ||
            writer->RawGuestBuffer().device != queue.device ||
            writer->RawGuestBuffer().hazardTrackingMode != MTLHazardTrackingModeTracked ||
            segment.begin < writer->RawGuestBegin() ||
            segment.end - writer->RawGuestBegin() > writer->RawGuestBuffer().length)
            return nullptr;
        if (writer->Status() != MetalGuestWriteStatus::Complete &&
            !submissions.IsTrackedProducer(writer->Command()))
            return nullptr;
        return writer;
    };
    const auto queue_texture_export = [&](const MetalGuestWrite& segment, PAddr first, PAddr last)
        -> id<MTLBuffer> {
        const bool packed = display_transfer && packed_color_export &&
                            NativeMetalCost::QueuePackedDisplayExports();
        const bool rgba = NativeMetalCost::QueueColorExports() && color_export &&
                          color_export->queue == queue;
        if (!packed && !rgba)
            return nil;
        const auto* writer = dynamic_cast<const MetalSurfaceWriter*>(segment.writer.get());
        if (!writer || writer->Queue() != queue ||
            (writer->Status() != MetalGuestWriteStatus::Complete &&
             !submissions.IsTrackedProducer(writer->Command())))
            return nil;
        const auto resource = writer->Resource();
        if (![resource conformsToProtocol:@protocol(MTLTexture)])
            return nil;
        if (packed && writer->PackedColor()) {
            const auto buffer = EncodeQueuedPackedColorExport(
                command, packed_color_export, (id<MTLTexture>)resource,
                *writer->PackedColor(), {first, last});
            if (buffer) {
                NativeMetalCost::Count(NativeMetalCost::DisplayQueuedPackedExport, 1);
                NativeMetalCost::Count(NativeMetalCost::DisplayQueuedPackedBytes, last - first);
            }
            return buffer;
        }
        if (!rgba || !writer->Rgba8Layout())
            return nil;
        return EncodeQueuedColorExport(command, color_export->tiled_pack,
                                       (id<MTLTexture>)resource, *writer->Rgba8Layout(), {first,last});
    };
    const auto queue_depth_export = [&](const MetalGuestWrite& segment, PAddr first, PAddr last)
        -> id<MTLBuffer> {
        if (!active_depth_check || active_depth_check->producer)
            return nil;
        const auto* writer = dynamic_cast<const MetalSurfaceWriter*>(segment.writer.get());
        if (!writer || writer->Queue() != queue)
            return nil;
        const auto status = writer->Status();
        const bool pending = status == MetalGuestWriteStatus::Pending && QueuePendingDepthExport() &&
                             submissions.IsTrackedProducer(writer->Command());
        if (status != MetalGuestWriteStatus::Complete && !pending)
            return nil;

        const DepthExportProof* proof = nullptr;
        const auto& exporter = writer->Exporter();
        if (const auto* deferred = exporter.target<DeferredDepthExport>(); deferred && *deferred->initialized)
            proof = &deferred->proof;
        else if (const auto* checked = exporter.target<CheckedDepthExporter>())
            proof = &checked->proof;
        if (!proof || proof->texture != writer->Resource() ||
            proof->generation != writer->Generation() || !writer->Covers(first, last))
            return nil;

        const auto errors = [queue.device newBufferWithLength:sizeof(u32)
                                               options:MTLResourceStorageModeShared];
        if (!errors)
            submissions.Fail("Could not allocate queued depth export validation");
        *static_cast<u32*>(errors.contents) = 0;
        const auto exported = EncodeQueuedCheckedDepthExport(command, proof->texture, proof->surface,
            {first,last}, proof->tiled_export, proof->depth_pack, errors);
        if (!exported)
            submissions.Fail("Could not encode certified queued depth export");
        active_depth_check->producer = segment.writer;
        if (pending) {
            active_depth_check->pending_source =
                std::shared_ptr<const MetalSurfaceWriter>(segment.writer, writer);
            NativeMetalCost::Count(NativeMetalCost::QueuedPendingDepthExports, 1);
        }
        active_depth_check->errors = errors;
        return exported;
    };

    if (use == ByteUse::TextureCopy && rows.count > 1) {
        static const bool borrow_striped_source = [] {
            const auto* value = std::getenv("CARA_METAL_BORROW_STRIPED_SOURCE");
            return value && std::string_view(value) == "1";
        }();
        if (borrow_striped_source && HasOneStripedSourceOwner(rows, current)) {
            if (const auto* writer = raw(current.front())) {
                const auto buffer = writer->RawGuestBuffer();
                const auto offset = bytes.begin - writer->RawGuestBegin();
                if (offset % alignment == 0 && bytes.end - writer->RawGuestBegin() <= buffer.length) {
                    NativeMetalCost::Count(NativeMetalCost::TextureCopyStripedSourceReuse, 1);
                    NativeMetalCost::Count(NativeMetalCost::TextureCopyStagingBytesAvoided, bytes.Size());
                    return MetalTransfer::ByteView{buffer, bytes, offset};
                }
            }
        }
    }

    // Common case: the last transfer produced the complete packed surface.
    if (current.size() == 1 && current.front().begin == bytes.begin &&
        current.front().end == bytes.end) {
        if (const auto* writer = raw(current.front())) {
            const auto offset = bytes.begin - writer->RawGuestBegin();
            if (offset % alignment == 0)
                return MetalTransfer::ByteView{writer->RawGuestBuffer(), bytes, offset};
        }
        if (active_depth_check && QueuePendingDepthExport() &&
            (rows.count == 1 || rows.width_bytes == rows.stride_bytes) && bytes.Size() % 4 == 0) {
            if (const auto exported = queue_depth_export(current.front(), bytes.begin, bytes.end))
                return MetalTransfer::ByteView{exported, bytes, 0};
        }
        static const bool direct_export = [] {
            const auto* value = std::getenv("CARA_METAL_DIRECT_EXPORT_CONSUMPTION");
            return value && std::string_view(value) == "1";
        }();
        // A dense source with one authoritative writer already exports the
        // exact packed interval at offset zero. Bind it directly instead of
        // blitting the result into a second buffer on the same command.
        if (direct_export && (rows.count == 1 || rows.width_bytes == rows.stride_bytes) &&
            bytes.Size() % 4 == 0) {
            if (const auto exported = queue_texture_export(current.front(), bytes.begin, bytes.end)) {
                NativeMetalCost::Count(NativeMetalCost::QueuedColorExportHost, 1);
                NativeMetalCost::Count(NativeMetalCost::DirectQueuedExport, 1);
                NativeMetalCost::Count(NativeMetalCost::DirectQueuedExportBytes, bytes.Size());
                return MetalTransfer::ByteView{exported, bytes, 0};
            }
        }
    }

    const auto buffer = AllocateBytes(bytes);
    auto* data = static_cast<u8*>(buffer.contents);
    if (!data)
        return std::nullopt;

    // Use ordered Metal blits for aligned raw leases. Other pieces must be
    // exported by their authoritative producer into private staging, never
    // reconstructed from underlying RAM or written back into guest memory.
    struct Copy {
        id<MTLBuffer> source;
        NSUInteger from, to, size;
    };
    std::vector<Copy> copies;
    auto owner = current.begin();
    const auto read_gap = [&](PAddr begin, PAddr end) {
        return begin == end || (ram && ram(begin, {data + (begin - bytes.begin), end - begin}));
    };
    for (u32 i = 0; i < rows.count; ++i) {
        const auto row = rows.Stripe(i);
        auto cursor = row.begin;
        while (owner != current.end() && owner->end <= cursor)
            ++owner;
        while (owner != current.end() && owner->begin < row.end) {
            const auto first = std::max(cursor, owner->begin);
            const auto last = std::min(row.end, owner->end);
            if (!read_gap(cursor, first))
                return std::nullopt;
            const auto* writer = raw(*owner);
            const NSUInteger to = first - bytes.begin;
            const NSUInteger size = last - first;
            const NSUInteger from = writer ? first - writer->RawGuestBegin() : 0;
            if (writer && ((from | to | size) & 3u) == 0) {
                copies.push_back({writer->RawGuestBuffer(), from, to, size});
            } else {
                auto exported = ((to | size) & 3u) == 0
                    ? queue_texture_export(*owner, first, last) : nil;
                const bool color_exported = exported != nil;
                if (!exported && ((to | size) & 3u) == 0)
                    exported = queue_depth_export(*owner, first, last);
                if (exported) {
                    if (color_exported)
                        NativeMetalCost::Count(NativeMetalCost::QueuedColorExportHost, 1);
                    copies.push_back({exported, 0, to, size});
                } else {
                    // Unaligned and depth spans retain the certified checked CPU read.
                    NativeMetalCost::Scope cost(NativeMetalCost::ExportSpanHost);
                    if (NativeMetalCost::Enabled()) {
                        const auto* source_writer =
                            dynamic_cast<const MetalSurfaceWriter*>(owner->writer.get());
                        auto kind = NativeMetalCost::ExportFallbackOther;
                        if (source_writer && source_writer->RawGuestBuffer()) {
                            kind = NativeMetalCost::ExportFallbackRawBuffer;
                        } else if (source_writer) {
                            const auto resource = source_writer->Resource();
                            if ([resource conformsToProtocol:@protocol(MTLTexture)]) {
                                const auto format = ((id<MTLTexture>)resource).pixelFormat;
                                kind = format == MTLPixelFormatDepth32Float ||
                                       format == MTLPixelFormatDepth32Float_Stencil8
                                           ? NativeMetalCost::ExportFallbackDepth
                                           : NativeMetalCost::ExportFallbackColor;
                            }
                        }
                        NativeMetalCost::Count(kind, 1);
                    }
                    const auto export_bytes = [&] {
                        return owner->writer->ExportGuestBytes(first, last, {data + to, size});
                    };
                    if (display_transfer && NativeMetalCost::Enabled()) {
                        using namespace NativeMetalCost;
                        const auto* surface = dynamic_cast<const MetalSurfaceWriter*>(owner->writer.get());
                        const auto resource = surface ? surface->Resource() : nil;
                        const auto format = [resource conformsToProtocol:@protocol(MTLTexture)]
                            ? ((id<MTLTexture>)resource).pixelFormat : MTLPixelFormatInvalid;
                        const auto phase = surface && surface->PackedColor() ? DisplayPackedExport :
                            format == MTLPixelFormatRGBA8Unorm ? DisplayRgbaExport :
                            format == MTLPixelFormatDepth32Float_Stencil8 ? DisplayDepthExport :
                            DisplayOtherExport;
                        Scope display_export(phase);
                        if (!export_bytes())
                            return std::nullopt;
                    } else if (!export_bytes()) {
                        return std::nullopt;
                    }
                }
            }
            cursor = last;
            if (owner->end > row.end)
                break;
            ++owner;
        }
        if (!read_gap(cursor, row.end))
            return std::nullopt;
    }
    if (!copies.empty()) {
        const auto blit = [command blitCommandEncoder];
        if (!blit)
            return std::nullopt;
        for (const auto& copy : copies)
            [blit copyFromBuffer:copy.source sourceOffset:copy.from
                       toBuffer:buffer destinationOffset:copy.to size:copy.size];
        [blit endEncoding];
    }
    return MetalTransfer::ByteView{buffer, bytes, 0};
}

id<MTLTexture> PicaMetalNativeDrawLane::ResolveRgba8(const MetalRgba8Layout& layout,
                                                       u64* backing_generation) {
    if (resident_color && resident_color->rgba8 == layout) {
        if (backing_generation)
            *backing_generation = resident_color->view.backing_generation;
        return resident_color->view.texture;
    }
    PAddr cursor = layout.begin;
    u64 generation = 0;
    id<MTLTexture> texture = nil;
    for (const auto& segment : authority.NativeSegments(layout.begin, layout.end)) {
        const auto* writer = dynamic_cast<const MetalSurfaceWriter*>(segment.writer.get());
        if (segment.begin != cursor || !writer || writer->Queue() != queue ||
            writer->Rgba8Layout() != layout ||
            (writer->Status() != MetalGuestWriteStatus::Complete &&
             !submissions.IsTrackedProducer(writer->Command())))
            return nil;
        const auto resource = writer->Resource();
        if (![resource conformsToProtocol:@protocol(MTLTexture)] ||
            (texture && (texture != resource || generation != writer->Generation())))
            return nil;
        texture = (id<MTLTexture>)resource;
        generation = writer->Generation();
        cursor = segment.end;
    }
    if (cursor != layout.end)
        return nil;
    if (backing_generation)
        *backing_generation = generation;
    return texture;
}

#if defined(CARA_PRIVATE_METAL_TRANSFERS)
void PicaMetalNativeDrawLane::Fill(MetalTransfer::FillRegisters registers,
                                  const MetalTransfer::Pipelines& pipelines) {
    NativeMetalCost::Scope cost(NativeMetalCost::Transfer);
    NativeMetalCost::Scope kind(NativeMetalCost::FillHost);
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Fill);
    PublishPending();
    const auto plan = MetalTransfer::PlanFill(registers);
    if (!plan)
        submissions.Fail("Unsupported native Metal fill");
    static const bool reuse_fills = [] {
        const auto* value = std::getenv("CARA_METAL_REUSE_COMPLETED_FILLS");
        return value && std::string_view(value) == "1";
    }();
    // Retain immutable bytes from successful GPU fills. A hit publishes a new
    // byte lease, with current FIFO dependencies, at the original destination.
    struct FillBacking {
        id<MTLBuffer> buffer;
        id<MTLCommandBuffer> producer;
        u32 size, pattern, period;
    };
    struct FillCache {
        id<MTLCommandQueue> queue;
        std::array<FillBacking, 16> entries{};
        std::size_t next = 0;
    };
    constexpr u32 max_cached_fill_size = 1024 * 1024;
    thread_local std::unique_ptr<FillCache> cached_fills;
    FillCache* cache = nullptr;
    FillBacking* hit = nullptr;
    const u32 size = plan->destination.Size();
    if (reuse_fills && size <= max_cached_fill_size) {
        if (!cached_fills || cached_fills->queue != queue) {
            cached_fills = std::make_unique<FillCache>();
            cached_fills->queue = queue;
        }
        cache = cached_fills.get();
        for (auto& entry : cache->entries) {
            if (entry.buffer && entry.size == size && entry.pattern == plan->little_endian_pattern &&
                entry.period == plan->pattern_bytes &&
                entry.producer.status == MTLCommandBufferStatusCompleted && !entry.producer.error) {
                hit = &entry;
                break;
            }
        }
    }

    id<MTLBuffer> buffer = nil;
    if (hit) {
        submissions.Retire(false);
        static const bool fill_lease = [] {
            const auto* value = std::getenv("CARA_METAL_FILL_LEASE_NO_RESERVE");
            return value && std::string_view(value) == "1";
        }();
        if (!fill_lease)
            submissions.Reserve();
        else if (submissions.Full())
            NativeMetalCost::Count(NativeMetalCost::FillLeaseFullQueue, 1);
        if (byte_generation == std::numeric_limits<u64>::max())
            submissions.Fail("Native Metal byte backing generation exhausted");
        const auto before = submissions.Predecessors();
        // The bytes already exist. The latest in-flight command provides the
        // same FIFO visibility and error ordering that a new fill would have.
        const auto completion = before ? before->commands[before->count - 1] : hit->producer;
        buffer = hit->buffer;
        const auto published = PrepareBytePublications(
            completion, buffer, DenseRows(plan->destination), ++byte_generation, before);
        try {
            authority.Publish(published.Writes());
            InvalidateAttachments({plan->destination.begin, plan->destination.end});
            InvalidateDecodedTextures(queue, plan->destination.begin, plan->destination.end);
            if (write_notice)
                write_notice(plan->destination.begin, plan->destination.end);
        } catch (...) {
            submissions.RejectWrites();
            throw;
        }
        NativeMetalCost::Count(NativeMetalCost::FillBackingHits, 1);
    } else {
        if (!NativeMetalCost::OverlapTransfers())
            submissions.Reserve();
        buffer = AllocateBytes(plan->destination);
        const auto command = [queue commandBuffer];
        if (!buffer || !MetalTransfer::EncodeFill(command, pipelines, registers,
                                                  {buffer, plan->destination, 0}))
            submissions.Fail("Could not encode native Metal fill");
        PublishBytes(command, buffer, DenseRows(plan->destination));
        if (cache) {
            cache->entries[cache->next] = {buffer, command, size, plan->little_endian_pattern,
                                          plan->pattern_bytes};
            cache->next = (cache->next + 1) % cache->entries.size();
            NativeMetalCost::Count(NativeMetalCost::FillBackingMisses, 1);
        }
    }
    // Detile reverses the guest little-endian bytes into S,Dhi,Dmid,Dlo.
    // Only depth endpoints can use a clear without any rounding difference.
    const u32 depth = plan->little_endian_pattern & 0x00ffffffu;
    if (plan->pattern_bytes == 4 && (depth == 0 || depth == 0x00ffffffu)) {
        if (auto* history = ActiveDepthFills(queue)) {
            history->fills[history->next] = {buffer, plan->little_endian_pattern};
            history->next = (history->next + 1) % history->fills.size();
        }
    }
}

void PicaMetalNativeDrawLane::CopyTexture(
    MetalTransfer::TextureCopyRegisters registers, const MetalTransfer::Pipelines& pipelines,
    const MetalGuestAuthority::GuestReader& ram) {
    NativeMetalCost::Scope cost(NativeMetalCost::Transfer);
    NativeMetalCost::Scope kind(NativeMetalCost::TextureCopyHost);
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Copy);
    const bool had_batch = HasTextureCopyBatch(this);
    const bool may_batch = texture_copy_batch.owner == this && write_notice;
    const auto publish_pending = [&] {
        NativeMetalCost::Scope publishing(NativeMetalCost::CopyPublishPending);
        PublishPending();
    };
    const auto reserve_immediate = [&] {
        if (!NativeMetalCost::OverlapTransfers()) {
            NativeMetalCost::Scope reserve(NativeMetalCost::CopyReserve);
            submissions.Reserve();
        }
    };
    if (!had_batch)
        publish_pending();
    if (!may_batch)
        reserve_immediate();
    NativeMetalCost::Scope planning(NativeMetalCost::CopyPlan);
    const auto plan = MetalTransfer::PlanTextureCopy(registers);
    planning.Stop();
    if (!plan) {
        if (had_batch)
            publish_pending();
        submissions.Fail("Unsupported native Metal texture copy");
    }
    const auto destination = plan->destination.footprint;
    const bool defer = may_batch && destination.begin >= Memory::VRAM_PADDR &&
                       destination.end <= Memory::VRAM_PADDR_END;
    // A second copy can consume a committed producer, never the still-open
    // batch. Using full footprints is conservative even for padded rows.
    const bool hazard = had_batch &&
        (texture_copy_batch.copies[0].destination.footprint.Overlaps(plan->source.footprint) ||
         texture_copy_batch.copies[0].destination.footprint.Overlaps(destination));
    if (hazard)
        NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchHazards, 1);
    if (had_batch && (!defer || hazard))
        publish_pending();
    if (may_batch && !defer)
        reserve_immediate();
    const bool dense = plan->source.count == 1 && plan->destination.count == 1;
    if (dense)
        NativeMetalCost::Count(NativeMetalCost::CopyDense, 1);
    NativeMetalCost::Scope allocate(NativeMetalCost::CopyAllocate);
    const auto command = defer && HasTextureCopyBatch(this) ? texture_copy_batch.command
                                                            : [queue commandBuffer];
    const auto buffer = AllocateBytes(destination);
    allocate.Stop();
    NativeMetalCost::Scope resolve(NativeMetalCost::CopyResolve);
    const auto source = ResolveBytes(command, plan->source, 4, ram, ByteUse::TextureCopy);
    resolve.Stop();
    NativeMetalCost::Scope encode(NativeMetalCost::CopyEncode);
    if (!buffer || !source ||
        !MetalTransfer::EncodeTextureCopy(command, pipelines, registers, *source,
                                          {buffer, destination, 0}))
        submissions.Fail("Could not encode native Metal texture copy");
    encode.Stop();
    NativeMetalCost::Count(NativeMetalCost::TextureCopyCommands, 1);
    NativeMetalCost::Count(NativeMetalCost::TextureCopyPayloadBytes, plan->payload_bytes);
    if (defer) {
        // Mark each exact destination stripe before returning to the existing
        // PPF path. Any guest access through a marked page reaches the original
        // flush/invalidate route, which submits the batch before observing data.
        try {
            for (u32 i = 0; i < plan->destination.count; ++i) {
                const auto stripe = plan->destination.Stripe(i);
                write_notice(stripe.begin, stripe.end);
            }
        } catch (...) {
            submissions.RejectWrites();
            throw;
        }
        auto& batch = texture_copy_batch;
        batch.command = command;
        batch.copies[batch.count++] = {buffer, plan->destination};
        NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchQueued, 1);
        if (batch.count == batch.copies.size()) {
            NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchFull, 1);
            PublishPending();
        }
        return;
    }

    NativeMetalCost::Scope publish(NativeMetalCost::CopyPublish);
    PublishBytes(command, buffer, plan->destination);
}

void PicaMetalNativeDrawLane::TransferDisplay(
    MetalTransfer::DisplayTransferRegisters registers, const MetalTransfer::Pipelines& pipelines,
    const PicaMetalTextureDecoder& decoder, const MetalGuestAuthority::GuestReader& ram) {
    NativeMetalCost::Scope cost(NativeMetalCost::Transfer);
    NativeMetalCost::Scope kind(NativeMetalCost::DisplayHost);
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Display);
    PublishPending();
    if (!NativeMetalCost::OverlapTransfers())
        submissions.Reserve();
    // Raw backing transfers include the inverse of the retile. Prefer a
    // certified texture for the ordinary tiled-to-linear path when available.
    const u32 layout_flags = registers.flags & ~1u;
    const bool linear_to_tiles = layout_flags == 2u;
    const auto rgb8_retile = MetalTransfer::PlanLinearToTiledRgb8(registers);
    const bool rgb565 = layout_flags == 0x2000u;
    // The accelerated display transfer uses the ordinary packed tile layout
    // for the unscaled RGB565 variant with the block_32 register bit set.
    const u32 packed_layout = layout_flags & ~0x10000u;
    const bool packed565_crop = packed_layout == 0x2204u;
    const bool packed16 = layout_flags == 0x4400u || packed_layout == 0x2200u || packed565_crop;
    const bool block32_rgb8 = layout_flags == 0x11000u;
    const bool cropped = layout_flags == 0x2004u || layout_flags == 0x1004u ||
                         packed565_crop;
    const bool valid_extent = registers.output_width && registers.output_height &&
        (cropped ? registers.output_width <= registers.input_width &&
                   registers.output_height <= registers.input_height
                 : registers.output_width == registers.input_width &&
                   registers.output_height == registers.input_height);
    std::optional<MetalTransfer::LinearColorTransferPlan> detile_plan;
    const auto scaled_crop = MetalTransfer::PlanScaledColorCrop(registers);
    static const bool fuse_crop = [] {
        const auto* value = std::getenv("CARA_METAL_FUSE_TEXTURE_CROP");
        return value && std::string_view(value) == "1";
    }();
    static const bool linear_bytes_only = [] {
        const auto* value = std::getenv("CARA_METAL_LINEAR_COLOR_BYTES_ONLY");
        return value && std::string_view(value) == "1";
    }();
    // Optional kernels use per-device caches; failed admission retains the
    // existing byte pipeline and its complete mixed-owner/padding behavior.
    const auto optional_kernel = [&](bool crop) -> id<MTLComputePipelineState> {
        static std::mutex lock;
        static id<MTLDevice> device;
        static id<MTLComputePipelineState> crop_pipeline, bytes_pipeline;
        const std::lock_guard guard(lock);
        if (device != queue.device) {
            device = queue.device;
            crop_pipeline = nil;
            bytes_pipeline = nil;
        }
        auto& pipeline = crop ? crop_pipeline : bytes_pipeline;
        if (!pipeline) {
            const auto url = [NSBundle.mainBundle URLForResource:crop ? @"cara-scaled-crop-rgb8" : @"cara-pica"
                                                 withExtension:@"metallib"];
            const auto library = url ? [queue.device newLibraryWithURL:url error:nullptr] : nil;
            const auto function = [library newFunctionWithName:crop ? @"cara_pica_display_texture_scaled_crop"
                                                                 : @"cara_pica_display_to_linear_bytes"];
            if (function)
                pipeline = [queue.device newComputePipelineStateWithFunction:function error:nullptr];
        }
        return pipeline;
    };
    const auto valid_rgba_source = [&](id<MTLTexture> texture, u32 width, u32 height) {
        return texture && texture.device == queue.device && texture.width == width &&
            texture.height == height && texture.textureType == MTLTextureType2D &&
            texture.pixelFormat == MTLPixelFormatRGBA8Unorm && texture.sampleCount == 1 &&
            texture.mipmapLevelCount == 1 && texture.arrayLength == 1 &&
            texture.hazardTrackingMode == MTLHazardTrackingModeTracked &&
            texture.storageMode != MTLStorageModeMemoryless && !texture.framebufferOnly &&
            !texture.heap && !texture.buffer && !texture.parentTexture && !texture.iosurface &&
            (texture.usage & MTLTextureUsageShaderRead);
    };
    if (scaled_crop && !scaled_crop->scale_xy_tiled && scaled_crop->source_bpp == 4u &&
        (NativeMetalCost::Enabled() || fuse_crop)) {
        NativeMetalCost::Count(scaled_crop->destination_bpp == 3u ?
            NativeMetalCost::DisplayCropRgb8Candidates : NativeMetalCost::DisplayCrop565Candidates, 1);
        const auto texture = ResolveRgba8({scaled_crop->source.begin, scaled_crop->source.end,
            registers.input_width, registers.input_height, true});
        const bool owned = valid_rgba_source(texture, registers.input_width, registers.input_height);
        NativeMetalCost::Count(owned ? NativeMetalCost::DisplayCropOwnerHits :
                                      NativeMetalCost::DisplayCropOwnerMisses, 1);
        const auto pipeline = fuse_crop && owned ? optional_kernel(true) : nil;
        if (pipeline && pipeline.maxTotalThreadsPerThreadgroup >= 64) {
            NativeMetalCost::Scope route(NativeMetalCost::RawDetileHost);
            const auto command = [queue commandBuffer];
            const auto output = AllocateBytes(scaled_crop->destination);
            const auto encoder = command && output ? [command computeCommandEncoder] : nil;
            if (!encoder)
                submissions.Fail("Could not encode fused native Metal crop");
            const std::array<u32,4> args{scaled_crop->output_width, scaled_crop->output_height,
                scaled_crop->destination_bpp, registers.flags & 1u};
            [encoder setComputePipelineState:pipeline];
            [encoder setTexture:texture atIndex:0];
            [encoder setBuffer:output offset:0 atIndex:0];
            [encoder setBytes:args.data() length:sizeof(args) atIndex:1];
            [encoder dispatchThreadgroups:MTLSizeMake((args[0]+7u)/8u,(args[1]+7u)/8u,1)
                    threadsPerThreadgroup:MTLSizeMake(8,8,1)];
            [encoder endEncoding];
            NativeMetalCost::Count(NativeMetalCost::DisplayCropFused, 1);
            NativeMetalCost::Count(NativeMetalCost::DisplayCropSourceBytesAvoided, scaled_crop->source.Size());
            if (scaled_crop->destination_bpp == 2u)
                NativeMetalCost::Count(NativeMetalCost::DisplayScaledCropRgb565, 1);
            PublishBytes(command, output, DenseRows(scaled_crop->destination));
            return;
        }
    }
    if (rgb8_retile && NativeMetalCost::Enabled()) {
        NativeMetalCost::Count(NativeMetalCost::DisplayRgb8RetileCandidates, 1);
        const auto texture = ResolveRgba8({rgb8_retile->source.begin, rgb8_retile->source.end,
            registers.input_width, registers.input_height, true});
        if (valid_rgba_source(texture, registers.input_width, registers.input_height))
            NativeMetalCost::Count(NativeMetalCost::DisplayRgb8RetileOwnerHits, 1);
    }
    if (valid_extent && !registers.address_override_enabled &&
        ((registers.input_width | registers.input_height) & 7u) == 0) {
        if (rgb565 || packed16 || block32_rgb8 || cropped) {
            // Validate the real intervals. A temporary RGBA8 plan rejects
            // valid neighboring packed surfaces because it doubles their span.
            const u32 in_bpp = packed16 ? 2u : 4u;
            const u32 out_bpp = packed16 || rgb565 || layout_flags == 0x2004u ? 2u : 3u;
            const auto from = MetalTransfer::AlignedGuestInterval(registers.source_address,
                u64{registers.input_width} * registers.input_height * in_bpp);
            const auto to = MetalTransfer::AlignedGuestInterval(registers.destination_address,
                u64{registers.output_width} * registers.output_height * out_bpp);
            if (from && to && !from->Overlaps(*to))
                detile_plan = MetalTransfer::LinearColorTransferPlan{*from, *to,
                    registers.input_width, registers.input_height, out_bpp, true,
                    bool(registers.flags & 1)};
        } else if (layout_flags == 0u || layout_flags == 0x1000u) {
            detile_plan = MetalTransfer::PlanNativeLinearColorTransfer(registers);
        }
    }
    const bool detile_raw = detile_plan && (rgb565 || packed16 || block32_rgb8 || cropped ||
        !ResolveRgba8({detile_plan->source.begin,
        detile_plan->source.end, detile_plan->width, detile_plan->height, true}));
    if (scaled_crop || rgb8_retile || ((linear_to_tiles || detile_raw) &&
        !registers.address_override_enabled && valid_extent &&
        ((registers.input_width | registers.input_height) & 7u) == 0)) {
        NativeMetalCost::Scope route(linear_to_tiles || rgb8_retile
                                         ? NativeMetalCost::RetileHost
                                         : NativeMetalCost::RawDetileHost);
        using MetalTransfer::AlignedGuestInterval;
        const u32 source_bpp = scaled_crop ? scaled_crop->source_bpp : packed16 ? 2u : 4u;
        const u64 size = u64{registers.input_width} * registers.input_height * source_bpp;
        std::optional<MetalTransfer::GuestInterval> src, dst;
        if (rgb8_retile) {
            src = rgb8_retile->source;
            dst = rgb8_retile->destination;
        } else if (scaled_crop) {
            src = scaled_crop->source;
            dst = scaled_crop->destination;
        } else {
            src = AlignedGuestInterval(registers.source_address, size);
            dst = linear_to_tiles ? AlignedGuestInterval(registers.destination_address, size)
                                 : std::optional{detile_plan->destination};
        }
        if (!src || !dst || src->Overlaps(*dst) || size > MaxBackingBytes)
            submissions.Fail("Invalid linear-to-tiled display transfer ranges");

        static std::mutex pipeline_lock;
        static id<MTLComputePipelineState> cached_retile;
        static id<MTLComputePipelineState> cached_scaled_crop_rgb8;
        static id<MTLComputePipelineState> cached_rgb8_retile;
        const bool scaled_crop_rgb8 = scaled_crop && scaled_crop->destination_bpp == 3u;
        id<MTLComputePipelineState> retile;
        {
            const std::lock_guard guard(pipeline_lock);
            auto& cached = rgb8_retile ? cached_rgb8_retile
                          : scaled_crop_rgb8 ? cached_scaled_crop_rgb8 : cached_retile;
            if (!cached || cached.device != queue.device) {
                cached = nil;
                NSString* resource = scaled_crop_rgb8 || rgb8_retile
                                         ? @"cara-scaled-crop-rgb8" : @"cara-pica";
                NSString* function = rgb8_retile ? @"cara_pica_display_linear_to_tiled_rgb8"
                                    : scaled_crop_rgb8 ? @"cara_pica_display_scaled_crop_rgb8"
                                                       : @"cara_pica_display_raw_rgba_layout";
                const auto url = [NSBundle.mainBundle URLForResource:resource withExtension:@"metallib"];
                const auto library = url ? [queue.device newLibraryWithURL:url error:nullptr] : nil;
                const auto kernel = [library newFunctionWithName:function];
                if (kernel)
                    cached = [queue.device newComputePipelineStateWithFunction:kernel error:nullptr];
            }
            retile = cached;
        }
        const auto command = [queue commandBuffer];
        const auto output = AllocateBytes(*dst);
        auto source = ResolveBytes(command, DenseRows(*src), 4, ram, ByteUse::DisplayTransfer);
        if (command && !source) {
            // A linear view can reinterpret bytes just written in a tiled
            // render target. Materialize that exact byte range before tiling.
            const auto staging = AllocateBytes(*src);
            auto* data = static_cast<u8*>(staging.contents);
            if (data && authority.ReadIntoScratch(src->begin, {data, src->Size()}, ram))
                source = MetalTransfer::ByteView{staging, *src, 0};
        }
        if (!retile || retile.maxTotalThreadsPerThreadgroup < 64 || !source || !output)
            submissions.Fail("Could not resolve linear-to-tiled Metal transfer");
        const auto encoder = [command computeCommandEncoder];
        if (!encoder)
            submissions.Fail("Could not encode linear-to-tiled Metal transfer");
        u32 mode = 0;
        if (scaled_crop && scaled_crop->scale_xy_tiled)
            mode = source_bpp == 2u ? 7u : 8u;
        else if (scaled_crop)
            mode = source_bpp == 2u ? 6u : 5u;
        else if (packed16)
            mode = 1;
        else if (!linear_to_tiles && !rgb8_retile)
            mode = detile_plan->destination_bpp;
        const u32 output_width = scaled_crop ? scaled_crop->output_width : registers.output_width;
        const u32 output_height = scaled_crop ? scaled_crop->output_height : registers.output_height;
        const std::array<u32, 6> arguments{registers.input_width, registers.input_height,
                                          registers.flags & 1u, mode,
                                          output_width, output_height};
        [encoder setComputePipelineState:retile];
        [encoder setBuffer:source->buffer offset:source->buffer_offset atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBytes:arguments.data() length:sizeof(arguments) atIndex:2];
        [encoder dispatchThreadgroups:MTLSizeMake((output_width + 7u) / 8,
                                                 (output_height + 7u) / 8, 1)
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        if (scaled_crop && scaled_crop->destination_bpp == 2u)
            NativeMetalCost::Count(NativeMetalCost::DisplayScaledCropRgb565, 1);
        [encoder endEncoding];
        PublishBytes(command, output, DenseRows(*dst));
        return;
    }
    if (const auto color = MetalTransfer::PlanNativeLinearColorTransfer(registers)) {
        id<MTLTexture> src = ResolveRgba8({color->source.begin, color->source.end,
                                          color->width, color->height, color->source_tiled});
        const bool scale_x = (registers.flags & 0x03000000u) == 0x01000000u;
        if (src || scale_x) {
            NativeMetalCost::Scope route(NativeMetalCost::NativeColorHost);
            const auto command = [queue commandBuffer];
            const auto make_texture = [&](NSUInteger width) {
                const auto descriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width
                    height:color->height mipmapped:NO];
                descriptor.storageMode = MTLStorageModePrivate;
                descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
                descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                return [queue.device newTextureWithDescriptor:descriptor];
            };
            if (!src) {
                // The blit kernel wants logical RGBA8 texels. Resolve the
                // exact packed source, including native-writer intervals,
                // and detile on the same command before the color blit.
                const auto source = ResolveBytes(command, DenseRows(color->source), 256, ram);
                src = make_texture(color->width);
                constexpr u32 rgba8_format = 0;
                if (!source ||
                    !decoder.Encode(command, source->buffer, source->buffer_offset, color->source.Size(),
                                    color->width * 8 * 4, src, MTLOriginMake(0, 0, 0),
                                    MTLSizeMake(color->width, color->height, 1), rgba8_format, false))
                    submissions.Fail("Could not import scaled native Metal color source");
            }
            const auto width = color->destination.Size() / color->destination_bpp / color->height;
            const auto bytes_pipeline = linear_bytes_only &&
                valid_rgba_source(src, color->width, color->height) ? optional_kernel(false) : nil;
            if (bytes_pipeline && bytes_pipeline.maxTotalThreadsPerThreadgroup >= 64) {
                const auto buffer = AllocateBytes(color->destination);
                const auto encoder = command && buffer ? [command computeCommandEncoder] : nil;
                if (!encoder)
                    submissions.Fail("Could not encode native Metal color bytes");
                const std::array<u32,4> args{color->destination_bpp,
                    unsigned(color->source_tiled != color->flip) | (unsigned(scale_x) << 1u),
                    static_cast<u32>(width), color->height};
                [encoder setComputePipelineState:bytes_pipeline];
                [encoder setTexture:src atIndex:0];
                [encoder setBuffer:buffer offset:0 atIndex:0];
                [encoder setBytes:args.data() length:sizeof(args) atIndex:1];
                [encoder dispatchThreadgroups:MTLSizeMake((width+7u)/8u,(color->height+7u)/8u,1)
                        threadsPerThreadgroup:MTLSizeMake(8,8,1)];
                [encoder endEncoding];
                NativeMetalCost::Count(NativeMetalCost::DisplayLinearBufferOnly, 1);
                NativeMetalCost::Count(NativeMetalCost::DisplayLinearTextureBytesAvoided,
                    u64{width} * color->height * 4u);
                PublishBytes(command, buffer, DenseRows(color->destination));
                return;
            }
            const auto dst = make_texture(width);
            const auto buffer = AllocateBytes(color->destination);
            if (!dst || !buffer ||
                !MetalTransfer::EncodeLinearColorTransfer(command, pipelines, registers, src, dst,
                                                           buffer))
                submissions.Fail("Could not encode native Metal color conversion");
            PublishBytes(command, buffer, DenseRows(color->destination));
            return;
        }
    }
    NativeMetalCost::Scope route(NativeMetalCost::GenericDisplayHost);
    const auto plan = MetalTransfer::PlanDisplayTransfer(registers);
    if (!plan) {
        const auto message = fmt::format(
            "Unsupported native Metal display transfer src=0x{:08x} dst=0x{:08x} "
            "input={}x{} output={}x{} flags=0x{:08x} address_override={}",
            registers.source_address, registers.destination_address, registers.input_width,
            registers.input_height, registers.output_width, registers.output_height,
            registers.flags, registers.address_override_enabled);
        submissions.Fail(message.c_str());
    }
    const auto command = [queue commandBuffer];
    const auto buffer = AllocateBytes(plan->copy.destination.footprint);
    const auto source = ResolveBytes(command, plan->copy.source, 4, ram);
    if (!buffer || !source ||
        !MetalTransfer::EncodeDisplayTransfer(command, pipelines, registers, *source,
                                              {buffer, plan->copy.destination.footprint, 0}))
        submissions.Fail("Could not encode native Metal display transfer");
    PublishBytes(command, buffer, DenseRows(plan->copy.destination.footprint));
}
#else
void PicaMetalNativeDrawLane::Fill(MetalTransfer::FillRegisters,
                                  const MetalTransfer::Pipelines&) {
    submissions.CheckThread();
    submissions.Fail("Native Metal transfer support was not built");
}

void PicaMetalNativeDrawLane::CopyTexture(MetalTransfer::TextureCopyRegisters,
                                         const MetalTransfer::Pipelines&,
                                         const MetalGuestAuthority::GuestReader&) {
    submissions.CheckThread();
    submissions.Fail("Native Metal transfer support was not built");
}

void PicaMetalNativeDrawLane::TransferDisplay(MetalTransfer::DisplayTransferRegisters,
                                             const MetalTransfer::Pipelines&,
                                             const PicaMetalTextureDecoder&,
                                             const MetalGuestAuthority::GuestReader&) {
    submissions.CheckThread();
    submissions.Fail("Native Metal transfer support was not built");
}
#endif

id<MTLTexture> PicaMetalNativeDrawLane::DecodeTexture(
    const PicaMetalTextureDecoder& decoder, MetalTransfer::GuestInterval bytes, u32 tile_row_bytes,
    u32 width, u32 height, u32 format, bool disable_alpha,
    const MetalGuestAuthority::GuestReader& ram) {
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Sample);
    submissions.CheckThread();
    if (HasTextureCopyBatch(this))
        PublishPending();
    if (!width || !height)
        submissions.Fail("Empty native Metal texture");
    if (TextureResidencyEnabled() && (!decoded_residency || decoded_residency->queue != queue)) {
        decoded_residency = std::make_unique<DecodedTextureResidency>();
        decoded_residency->queue = queue;
    }
    auto* resident = TextureResidencyEnabled() ? decoded_residency.get() : nullptr;
    const ResidentTextureKey key{bytes.begin, bytes.end, tile_row_bytes, width, height, format,
                                 disable_alpha};
    const bool tracked = resident && write_notice && bytes.begin >= Memory::VRAM_PADDR &&
                         bytes.end <= Memory::VRAM_PADDR_END && bytes.begin < bytes.end &&
                         bytes.Size() <= MaxResidentSourceBytes &&
                         DecodedTextureResidency::Cost(key) <= MaxResidentTextureBytes;
    if (tracked) {
        if (const auto it = resident->entries.find(key); it != resident->entries.end()) {
            submissions.Retire(false);
            it->second.last_use = ++resident->time;
            ++resident->hits;
            return it->second.texture;
        }
        ++resident->misses;
        // A scalar store can cross into the first watched page from its predecessor.
        // Mark both so the emulator's existing VRAM write barrier sees that store.
        const PAddr page = bytes.begin & ~Memory::PAGE_MASK;
        write_notice(page == Memory::VRAM_PADDR ? page : page - Memory::PAGE_SIZE, bytes.end);
    }
    // The raw-byte equality path can consult RAM first when the pending
    // draw cannot write this address interval. A miss still publishes before
    // encoding new Metal work, preserving the old queue ordering.
    const bool defer = NativeMetalCost::DeferUnrelatedPublication() && !tracked && pending &&
        bytes.begin < bytes.end &&
        !OverlapsAttachment(bytes.begin, bytes.end, pending->color) &&
        !OverlapsAttachment(bytes.begin, bytes.end, pending->depth);
    if (!defer)
        PublishPending();

    struct Sample {
        PAddr address;
        u32 row_bytes, width, height, format;
        bool disable_alpha;
        std::vector<u8> guest_bytes;
        id<MTLTexture> texture;
    };
    struct LargeSample {
        std::vector<u8> guest_bytes;
        id<MTLTexture> texture;
        u64 last_use;
    };
    constexpr std::size_t max_large_entries = 64;
    constexpr u64 large_budget = 64 * 1024 * 1024;
    const auto large_cost = [](const ResidentTextureKey& entry) {
        return DecodedTextureResidency::Cost(entry) + entry.end - entry.begin;
    };
    // The FIFO can retain several obsolete snapshots of the same texture and
    // evict frequently reused levels. Keep one complete snapshot per key.
    struct KeyedSample {
        std::vector<u8> guest_bytes;
        id<MTLTexture> texture;
        std::list<ResidentTextureKey>::iterator recent;
    };
    constexpr std::size_t max_keyed_entries = 512;
    constexpr u64 keyed_budget = 64 * 1024 * 1024;
    struct SampleCache {
        id<MTLCommandQueue> queue;
        std::array<std::optional<Sample>, 128> samples{};
        std::size_t next = 0;
        std::vector<u8> scratch;
        std::map<ResidentTextureKey, LargeSample> large_samples;
        u64 large_bytes = 0;
        u64 large_time = 0;
        std::list<ResidentTextureKey> recent;
        std::map<ResidentTextureKey, KeyedSample> keyed;
        u64 keyed_bytes = 0;
    };
    thread_local std::unique_ptr<SampleCache> cache;
    if (!cache || cache->queue != queue) {
        cache = std::make_unique<SampleCache>();
        cache->queue = queue;
    }
    auto& samples = cache->samples;
    auto& next_sample = cache->next;
    auto& scratch = cache->scratch;

    // Only cache immutable decoded inputs whose complete source is guest RAM.
    // A pending GPU lease must still go through the ordered ownership resolver.
    const bool small = bytes.begin < bytes.end && bytes.Size() <= 256 * 1024 &&
                       u64{width} * height <= 256 * 256;
    static const bool allow_large = [] {
        const auto* value = std::getenv("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS");
        return value && std::string_view(value) == "1";
    }();
    const bool large = !small && allow_large && bytes.begin < bytes.end &&
                       bytes.Size() <= MaxResidentSourceBytes && width <= 1024 && height <= 1024;
    static const bool allow_keyed = [] {
        const auto* value = std::getenv("CARA_METAL_CACHE_KEYED_TEXTURE_SNAPSHOTS");
        return value && std::string_view(value) == "1";
    }();
    const bool keyed = small && allow_keyed;
    bool snapshot = false;
    std::span<const u8> comparison;
    bool borrowed = false;
    static const bool borrow_compare = [] {
        const auto* value = std::getenv("CARA_METAL_BORROW_RAM_SNAPSHOT_COMPARE");
        return value && std::string_view(value) == "1";
    }();
    if (!tracked && (small || large) && ram) {
        if (authority.NativeSegments(bytes.begin, bytes.end).empty()) {
            if (borrow_compare && ram_view) {
                try {
                    const auto view = ram_view(bytes.begin, bytes.Size());
                    if (view.data() && view.size() == bytes.Size()) {
                        comparison = view;
                        snapshot = borrowed = true;
                    }
                } catch (...) {
                    // An unavailable view does not weaken the original reader.
                }
            }
            if (!snapshot) {
                scratch.resize(bytes.Size());
                snapshot = ram(bytes.begin, scratch);
                if (snapshot)
                    comparison = scratch;
            }
            if (snapshot)
                NativeMetalCost::Count(NativeMetalCost::SnapshotComparedBytes, comparison.size());
        } else {
            NativeMetalCost::Count(NativeMetalCost::SnapshotNativeOwners, 1);
        }
    } else if (!tracked && !small) {
        NativeMetalCost::Count(NativeMetalCost::SnapshotOutsideLimits, 1);
    }

    const auto same_snapshot = [&](const std::vector<u8>& owned) {
        return owned.size() == comparison.size() &&
               std::memcmp(owned.data(), comparison.data(), comparison.size()) == 0;
    };

    const auto reuse = [&](id<MTLTexture> texture) {
        NativeMetalCost::Count(NativeMetalCost::SnapshotHits, 1);
        if (defer) {
            if (!RetireCachedInputsAtRecord())
                submissions.Retire(false);
            NativeMetalCost::Count(NativeMetalCost::SnapshotReadNoPublish, 1);
        } else if (NativeMetalCost::ReduceCopyQueueWork()) {
            // PublishPending already checked the queue and this reuse needs no slot.
            NativeMetalCost::Count(NativeMetalCost::SnapshotReserveAvoided, 1);
        } else {
            submissions.Reserve();
        }
        return texture;
    };
    if (snapshot && large) {
        const auto entry = cache->large_samples.find(key);
        if (entry != cache->large_samples.end() && same_snapshot(entry->second.guest_bytes)) {
            entry->second.last_use = ++cache->large_time;
            NativeMetalCost::Count(NativeMetalCost::LargeSnapshotHits, 1);
            return reuse(entry->second.texture);
        }
        NativeMetalCost::Count(NativeMetalCost::LargeSnapshotMisses, 1);
    } else if (snapshot && keyed) {
        const auto entry = cache->keyed.find(key);
        if (entry == cache->keyed.end()) {
            NativeMetalCost::Count(NativeMetalCost::KeyedSnapshotKeyMisses, 1);
        } else if (same_snapshot(entry->second.guest_bytes)) {
            cache->recent.splice(cache->recent.end(), cache->recent, entry->second.recent);
            NativeMetalCost::Count(NativeMetalCost::KeyedSnapshotHits, 1);
            return reuse(entry->second.texture);
        } else {
            NativeMetalCost::Count(NativeMetalCost::KeyedSnapshotBytesChanged, 1);
        }
    } else if (snapshot) {
        for (const auto& sample : samples) {
            if (sample && sample->address == bytes.begin && sample->row_bytes == tile_row_bytes &&
                sample->width == width && sample->height == height && sample->format == format &&
                sample->disable_alpha == disable_alpha && same_snapshot(sample->guest_bytes))
                return reuse(sample->texture);
        }
    }

    if (borrowed) {
        // Own a miss before PublishPending, allocation, read callbacks or any
        // deferred GPU work. Cached snapshots never borrow the RAM view.
        scratch.assign(comparison.begin(), comparison.end());
        comparison = {};
    }

    if (defer)
        PublishPending();
    submissions.Reserve();
    const auto command = [queue commandBuffer];
    MetalGuestAuthority::GuestReader read = ram;
    if (snapshot) {
        read = [&](PAddr address, std::span<u8> target) {
            if (address != bytes.begin || target.size() != scratch.size())
                return false;
            std::memcpy(target.data(), scratch.data(), scratch.size());
            return true;
        };
    }
    const auto source = ResolveBytes(command, DenseRows(bytes), 256, read);
    if (!source)
        submissions.Fail("Could not resolve native Metal texture bytes");
    const auto descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height
        mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    const auto texture = [queue.device newTextureWithDescriptor:descriptor];
    if (!decoder.Encode(command, source->buffer, source->buffer_offset, bytes.Size(), tile_row_bytes,
                        texture, MTLOriginMake(0, 0, 0), MTLSizeMake(width, height, 1), format,
                        disable_alpha))
        submissions.Fail("Could not decode native Metal texture");
    [command commit];
    Track(command);
    if (tracked) {
        resident->Insert(key, texture);
        ++resident->vram_decodes;
    } else if (resident) {
        ++resident->other_decodes;
    }
    if (snapshot && large) {
        const u64 cost = large_cost(key);
        if (cache->large_samples.erase(key))
            cache->large_bytes -= cost;
        while (cache->large_samples.size() >= max_large_entries ||
               cost > large_budget - cache->large_bytes) {
            const auto victim = std::min_element(cache->large_samples.begin(),
                cache->large_samples.end(),
                [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
            cache->large_bytes -= large_cost(victim->first);
            cache->large_samples.erase(victim);
        }
        cache->large_samples.emplace(key, LargeSample{scratch, texture, ++cache->large_time});
        cache->large_bytes += cost;
    } else if (snapshot && keyed) {
        const auto previous = cache->keyed.find(key);
        if (previous != cache->keyed.end()) {
            auto& sample = previous->second;
            sample.guest_bytes = scratch;
            sample.texture = texture;
            cache->recent.splice(cache->recent.end(), cache->recent, sample.recent);
        } else {
            const u64 cost = large_cost(key);
            while (cache->keyed.size() >= max_keyed_entries ||
                   cost > keyed_budget - cache->keyed_bytes) {
                const auto& oldest = cache->recent.front();
                cache->keyed_bytes -= large_cost(oldest);
                cache->keyed.erase(oldest);
                cache->recent.pop_front();
                NativeMetalCost::Count(NativeMetalCost::KeyedSnapshotEvictions, 1);
            }
            cache->recent.push_back(key);
            cache->keyed.emplace(key, KeyedSample{scratch, texture, std::prev(cache->recent.end())});
            cache->keyed_bytes += cost;
        }
    } else if (snapshot) {
        samples[next_sample].emplace(Sample{bytes.begin, tile_row_bytes, width, height, format,
                                           disable_alpha, scratch, texture});
        next_sample = (next_sample + 1) % samples.size();
    }
    return texture;
}

MetalLCD::Receipt PicaMetalNativeDrawLane::Present(
    CAMetalLayer* layer, const Layout::FramebufferLayout& layout, const MetalLCD::Pipelines& pipelines,
    MetalLCD::Filter filter, MetalLCD::EdgeMode edge, const MetalGuestAuthority::GuestReader& ram,
    bool capture, unsigned upscale, bool present_drawable) {
    NativeMetalCost::Scope cost(NativeMetalCost::Present);
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Present);
    PublishPending();
    submissions.Reserve();
    if ((present_drawable || capture) && AsyncCheckedDepthImport()) {
        // Queued GPU consumers may overlap unchecked imports, but neither a
        // drawable nor a captured image may expose a failed shader-side check.
        // Wait only for validation commands, not unrelated later GPU work.
        const auto predecessors = submissions.Predecessors();
        if (predecessors && !ValidatePresentationReceipts(*predecessors))
            submissions.Fail("Native Metal presentation depends on failed validation");
        submissions.Retire(false);
    }
    auto result = MetalLCD::PresentNative(queue, layer, layout, pipelines, filter, edge,
        [&](id<MTLCommandBuffer> command, const MetalLCD::Scanout& scanout) -> id<MTLTexture> {
            const auto source = ResolveBytes(command, scanout.rows, 1, ram);
            if (!source) {
                LOG_ERROR(Render_OpenGL, "Metal LCD guest bytes unavailable first=0x{:08x} last=0x{:08x}",
                          scanout.rows.footprint.begin, scanout.rows.footprint.end);
                return nil;
            }
            const auto texture = MetalLCD::EncodeLinearScanout(command, pipelines, scanout, *source);
            if (!texture)
                LOG_ERROR(Render_OpenGL, "Metal LCD linear scanout encoding rejected");
            return texture;
        }, capture, &previous_lcd, upscale, &lossless_lcd, present_drawable);
    if (result.status == MetalLCD::Status::Unsupported)
        submissions.Fail("Could not resolve native Metal LCD presentation");
    if (result.status == MetalLCD::Status::Submitted ||
        result.status == MetalLCD::Status::Coalesced)
        Track(result.commands);
    return result;
}

void PicaMetalNativeDrawLane::InvalidateAttachments(PicaGuestRange bytes,
                                                         const PicaMetalSurfaceView* same_view) {
    const auto invalidate = [&](std::optional<PicaMetalNativeAttachment>& attachment) {
        if (!attachment || (same_view && SameBacking(attachment->view, *same_view)))
            return;
        const auto backing = attachment->view.authoritative_bytes;
        if (bytes.begin < bytes.end && bytes.begin < backing.end && backing.begin < bytes.end)
            attachment.reset();
    };
    invalidate(resident_color);
    invalidate(resident_depth);
}

PicaMetalNativeAttachment PicaMetalNativeDrawLane::AcquireTiledColor(
    MetalTransfer::GuestInterval bytes, u32 width, u32 height, MTLPixelFormat metal_format,
    const PicaMetalTextureDecoder& decoder, id<MTLComputePipelineState> tiled_export,
    id<MTLComputePipelineState> detile_packed_color,
    const MetalGuestAuthority::GuestReader& ram) {
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Sample);
    submissions.CheckThread();
    if (HasTextureCopyBatch(this))
        PublishPending();
    const bool retire_at_record = RetireCachedInputsAtRecord();
    if (!retire_at_record)
        submissions.Retire(false);
    if (NativeMetalCost::QueueColorExports() && (!color_export || color_export->queue != queue))
        color_export = std::make_unique<ColorExportContext>(ColorExportContext{queue, tiled_export});
    const bool rgba8 = metal_format == MTLPixelFormatRGBA8Unorm;
    const bool packed_color = metal_format == MTLPixelFormatB5G6R5Unorm ||
                              metal_format == MTLPixelFormatABGR4Unorm ||
                              metal_format == MTLPixelFormatA1BGR5Unorm;
    if (packed_color && NativeMetalCost::QueuePackedDisplayExports())
        packed_color_export = tiled_export;
    const unsigned pixel_bytes = packed_color ? 2 : 4;
    const MetalRgba8Layout layout{bytes.begin, bytes.end, width, height, true};
    if ((!rgba8 && !packed_color) || !width || !height ||
        ((width | height) & 7u) || bytes.begin >= bytes.end ||
        bytes.Size() > MaxBackingBytes || bytes.Size() % pixel_bytes ||
        bytes.Size() / pixel_bytes % width || bytes.Size() / pixel_bytes / width != height ||
        attachment_generation == std::numeric_limits<u64>::max())
        submissions.Fail("Unsupported native Metal color surface");

    if (resident_color) {
        const auto& view = resident_color->view;
        if (view.texture.pixelFormat == metal_format && view.texture.width == width &&
            view.texture.height == height && view.authoritative_bytes.begin == bytes.begin &&
            view.authoritative_bytes.end == bytes.end)
            return *resident_color;
    }

    // Cache hits only hand back the existing lease. Record still retires
    // before encoding, and PublishPending retires before new imports.
    PublishPending();
    static const bool color_lease_no_reserve = [] {
        const auto* value = std::getenv("CARA_METAL_COLOR_LEASE_NO_RESERVE");
        return value && std::string_view(value) == "1";
    }();
    if (!color_lease_no_reserve)
        submissions.Reserve();
    u64 generation = 0;
    id<MTLTexture> texture = rgba8 ? ResolveRgba8(layout, &generation) : nil;
    if (!texture || !(texture.usage & MTLTextureUsageRenderTarget)) {
        // Exact authority recovery above only acquires existing immutable
        // metadata. Reserve a submission slot when an actual import command
        // is needed; all existing owner/status checks and publication stay.
        if (color_lease_no_reserve)
            submissions.Reserve();
        const auto command = [queue commandBuffer];
        auto source = ResolveBytes(command, DenseRows(bytes), 256, ram);
        if (!source)
            submissions.Fail("Could not resolve native Metal color surface bytes");
        const auto descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:metal_format width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        if (rgba8)
            descriptor.usage |= MTLTextureUsageShaderWrite;
        texture = [queue.device newTextureWithDescriptor:descriptor];
        constexpr u32 Rgba8 = 0;
        const bool imported = packed_color
            ? EncodePicaPackedColorImport(command, detile_packed_color, source->buffer,
                                          source->buffer_offset, bytes.Size(), texture)
            : decoder.Encode(command, source->buffer, source->buffer_offset, bytes.Size(),
                             width * 8 * 4, texture, MTLOriginMake(0, 0, 0),
                             MTLSizeMake(width, height, 1), Rgba8, false);
        if (!imported)
            submissions.Fail("Could not import native Metal color surface");
        [command commit];
        Track(command);
        generation = ++attachment_generation;
    }

    const PicaGuestRange range{bytes.begin, bytes.end};
    auto export_format = PicaExportFormat::Rgba8;
    if (metal_format == MTLPixelFormatB5G6R5Unorm)
        export_format = PicaExportFormat::Rgb565;
    else if (metal_format == MTLPixelFormatABGR4Unorm)
        export_format = PicaExportFormat::Rgba4;
    else if (metal_format == MTLPixelFormatA1BGR5Unorm)
        export_format = PicaExportFormat::Rgb5A1;
    auto export_bytes = MakePicaTiledVisibilityExporter(
        texture, generation, range, export_format, tiled_export, nil);
    if (!export_bytes)
        submissions.Fail("Could not certify native Metal color export");
    resident_color = PicaMetalNativeAttachment{{texture, range, generation}, std::move(export_bytes),
                                              rgba8 ? std::optional{layout} : std::nullopt};
    return *resident_color;
}

PicaMetalNativeAttachment PicaMetalNativeDrawLane::AcquireTiledDepthStencil(
    MetalTransfer::GuestInterval bytes, u32 width, u32 height,
    const PicaMetalDepthImportPipelines& pipelines, id<MTLComputePipelineState> tiled_export,
    id<MTLComputePipelineState> depth_pack, const MetalGuestAuthority::GuestReader& ram,
    bool has_stencil) {
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Sample);
    submissions.CheckThread();
    if (HasTextureCopyBatch(this))
        PublishPending();
    if (!RetireCachedInputsAtRecord())
        submissions.Retire(false);
    const u32 pixel_bytes = has_stencil ? 4u : 3u;
    if (!width || !height || ((width | height) & 7u) || bytes.begin >= bytes.end ||
        bytes.Size() > MaxBackingBytes || u64{width} * height * pixel_bytes != bytes.Size() ||
        attachment_generation == std::numeric_limits<u64>::max())
        submissions.Fail("Unsupported native Metal depth surface");

    if (resident_depth) {
        const auto& view = resident_depth->view;
        if (view.authoritative_bytes.begin == bytes.begin &&
            view.authoritative_bytes.end == bytes.end &&
            view.texture.width == width && view.texture.height == height)
            return *resident_depth;
    }

    PublishPending();
    const bool allow_fuse = has_stencil && NativeMetalCost::FuseDepthClear();
    if (!allow_fuse)
        submissions.Reserve();
    const auto clear_word = has_stencil ? FullDepthFill(queue, authority, bytes, submissions)
                                        : std::nullopt;
    const bool deferred_clear = allow_fuse && clear_word.has_value();
    if (allow_fuse && !deferred_clear)
        submissions.Reserve();
    const auto command = deferred_clear ? nil : [queue commandBuffer];
    std::optional<MetalTransfer::ByteView> source;
    JoinedDepthCheck joined;
    if (!clear_word) {
        const ScopedJoinedDepthCheck checked(JoinCheckedDepthImport() ? &joined : nullptr);
        source = ResolveBytes(command, DenseRows(bytes), 4, ram);
        if (joined.producer && !source)
            submissions.Fail("Could not resolve complete queued depth import");
        if (command && !source) {
            // Cross-format aliases still take the exact existing import.
            const auto scratch = AllocateBytes(bytes);
            auto* data = static_cast<u8*>(scratch.contents);
            if (data && authority.ReadIntoScratch(bytes.begin, {data, bytes.Size()}, ram))
                source = MetalTransfer::ByteView{scratch, bytes, 0};
        }
    }
    const auto descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:width
        height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    descriptor.usage = MTLTextureUsageRenderTarget;
    const auto texture = [queue.device newTextureWithDescriptor:descriptor];
    if ((!deferred_clear && !command) || !texture || (!clear_word && !source))
        submissions.Fail("Could not allocate native Metal D24S8 import");

    const auto generation = ++attachment_generation;
    const PicaGuestRange range{bytes.begin, bytes.end};
    auto exporter = MakePicaTiledVisibilityExporter(
        texture, generation, range,
        has_stencil ? PicaExportFormat::Depth24Stencil8 : PicaExportFormat::Depth24,
        tiled_export, depth_pack);
    if (!exporter)
        submissions.Fail("Could not certify native Metal depth/stencil export");

    const DepthExportProof proof{range, texture, generation, tiled_export, depth_pack};
    if (deferred_clear) {
        // The raw guest writer remains authoritative. Prepare the exact
        // attachment on the first real draw, before a texture writer exists.
        exporter = DeferredDepthExport{std::move(exporter), std::make_shared<bool>(false),
                                       *clear_word, proof};
        NativeMetalCost::Count(NativeMetalCost::DepthClearHits, 1);
    } else if (clear_word) {
        // The raw guest writer stays authoritative until the first draw.
        // The clear prepares the exact attachment, on the same ordered queue.
        const auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = texture;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = (*clear_word & 0x00ffffffu) ? 1.0 : 0.0;
        pass.stencilAttachment.texture = texture;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.clearStencil = *clear_word >> 24;
        const auto encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder)
            submissions.Fail("Could not encode exact native Metal depth clear");
        [encoder endEncoding];
        [command commit];
        Track(command);
        NativeMetalCost::Count(NativeMetalCost::DepthClearHits, 1);
    } else {
        const auto errors = [queue.device newBufferWithLength:sizeof(u32)
                                                     options:MTLResourceStorageModeShared];
        if (!errors)
            submissions.Fail("Could not allocate native Metal depth-validation buffer");
        *static_cast<u32*>(errors.contents) = 0;
        if (!EncodeDepthImport(command, pipelines, source->buffer, source->buffer_offset, texture,
                               errors, nullptr, has_stencil))
            submissions.Fail("Could not encode native Metal depth import");
        // Preserve both GPU checks and, for the queued path, the original
        // producer's transitive validity across retirement and guest reads.
        std::shared_ptr<const PicaMetalValidationReceipt> validation;
        if (AsyncCheckedDepthImport() && joined.producer) {
            validation = std::make_shared<PicaMetalValidationReceipt>(command,
                std::array<id<MTLBuffer>, 2>{errors, joined.errors}, std::move(joined.pending_source));
            NativeMetalCost::Count(NativeMetalCost::AsyncDepthImports, 1);
        }
        [command commit];
        Track(command, validation);
        if (!validation) {
            [command waitUntilCompleted];
            submissions.Retire(false);
            if (command.status != MTLCommandBufferStatusCompleted || command.error ||
                *static_cast<const u32*>(errors.contents) != 0)
                submissions.Fail("Native Metal D24S8 import failed validation");
            if (joined.producer) {
                const auto* writer = static_cast<const MetalSurfaceWriter*>(joined.producer.get());
                if (!writer->ValidateProducerCompletion() ||
                    *static_cast<const u32*>(joined.errors.contents) != 0)
                    submissions.Fail("Queued depth import source failed validation");
            }
        }

    }

    if (has_stencil && !deferred_clear && JoinCheckedDepthImport())
        exporter = CheckedDepthExporter{std::move(exporter), proof};
    resident_depth = PicaMetalNativeAttachment{{texture, range, generation}, std::move(exporter),
                                              std::nullopt};
    return *resident_depth;
}

void PicaMetalNativeDrawLane::PublishPending() {
    NativeMetalCost::Scope cost(NativeMetalCost::Publish);
    submissions.CheckThread();
    submissions.Retire(false);
    if (HasTextureCopyBatch(this)) {
        if (pending)
            submissions.Fail("Native Metal draw group and texture copy batch both pending");
        NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Copy);
        NativeMetalCost::Scope publish(NativeMetalCost::CopyPublish);
        NativeMetalCost::Scope cost(NativeMetalCost::PublishBytesHost);
        {
            NativeMetalCost::Scope reserve(NativeMetalCost::LateTransferReserve);
            submissions.Reserve();
        }
        auto& batch = texture_copy_batch;
        if (byte_generation > std::numeric_limits<u64>::max() - batch.count)
            submissions.Fail("Native Metal byte backing generation exhausted");
        try {
            if (submissions.Full())
                submissions.Fail("Native Metal copy batch lacks a reserved submission slot");
            const auto predecessors = submissions.Predecessors();
            std::array<BytePublications, 2> published;
            for (std::size_t i = 0; i < batch.count; ++i) {
                const auto& copy = batch.copies[i];
                published[i] = PrepareBytePublications(batch.command, copy.buffer, copy.destination,
                                                       ++byte_generation, predecessors);
            }
            [batch.command commit];
            Track(batch.command);
            for (std::size_t i = 0; i < batch.count; ++i) {
                const auto bytes = batch.copies[i].destination.footprint;
                const auto writes = published[i].Writes();
                authority.Publish(writes);
                InvalidateAttachments({bytes.begin, bytes.end});
                for (const auto& write : writes)
                    InvalidateDecodedTextures(queue, write.begin, write.end);
            }
        } catch (...) {
            submissions.RejectWrites();
            throw;
        }
        NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchCommits, 1);
        NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchCopiesCommitted, batch.count);
        NativeMetalCost::Count(NativeMetalCost::TextureCopyBatchCommandBuffersSaved, batch.count - 1);
        batch = {.owner = this};
        return;
    }
    if (!pending)
        return;

    const auto trigger = NativeMetalCost::queue_request;
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Draw);
    submissions.Reserve();
    if (submissions.Full())
        submissions.Fail("Native Metal draw group lacks a reserved submission slot");
    auto predecessors = submissions.Predecessors();
    auto ticket = group.Commit();
    if (!ticket)
        submissions.Fail("Native Metal draw group did not submit");
    auto& submitted = submissions.Track(*ticket);
    NativeMetalCost::Count(NativeMetalCost::Submits, 1);
    NativeMetalCost::Count(NativeMetalCost::PublishDraws, submitted.count);
    constexpr std::array trigger_counts{
        NativeMetalCost::DrawPublishOther, NativeMetalCost::DrawPublishFromDraw,
        NativeMetalCost::DrawPublishFromCopy, NativeMetalCost::DrawPublishFromFill,
        NativeMetalCost::DrawPublishFromDisplay, NativeMetalCost::DrawPublishFromSample,
        NativeMetalCost::DrawPublishFromPresent, NativeMetalCost::DrawPublishFromDrain,
    };
    NativeMetalCost::Count(trigger_counts[static_cast<std::size_t>(trigger)], 1);
    if (submitted.count == NativeMetalCost::DrawGroupLimit())
        NativeMetalCost::Count(NativeMetalCost::DrawGroupsAtCapacity, 1);
    try {
        NativeMetalCost::Scope ownership(NativeMetalCost::DrawPublishAuthority);
        ValidateMetalDrawTicket(submitted);
        const auto make_write = [&](const PicaMetalWriter& writer,
                                    const PicaMetalNativeAttachment& attachment) {
            return MakeMetalDrawWrite(submitted, writer, attachment.view, attachment.exporter,
                                      0, predecessors, attachment.rgba8);
        };
        static const bool dedup = [] {
            const auto* value = std::getenv("CARA_METAL_DEDUP_DRAW_PUBLICATIONS");
            return value && std::string_view(value) == "1";
        }();
        const auto same_write = [](const PicaMetalWriter& a, const PicaMetalWriter& b) {
            return a.backing_generation == b.backing_generation &&
                   a.bytes.begin == b.bytes.begin && a.bytes.end == b.bytes.end;
        };
        const auto publications = submitted.Publications();
        const auto& first = publications.front();
        auto count = submitted.count;
        // Publications are visible only after this command's completed output.
        // Identical records for each of its disjoint attachments have the same
        // bytes, generation, resource, export callback and predecessor chain.
        if (dedup && count > 1 &&
            std::all_of(publications.begin() + 1,
                        publications.begin() + count, [&](const auto& draw) {
                return same_write(first.color, draw.color) &&
                       same_write(first.depth_stencil, draw.depth_stencil);
            })) {
            const auto dirty = [](const PicaMetalWriter& write) {
                return write.bytes.begin != 0 || write.bytes.end != 0;
            };
            const auto eliminated = (count - 1) *
                (unsigned(dirty(first.color)) + unsigned(dirty(first.depth_stencil)));
            NativeMetalCost::Count(NativeMetalCost::DrawWritesElided, eliminated);
            count = 1;
        }
        static const bool reuse_publications = [] {
            const char* value = std::getenv("CARA_METAL_REUSE_OWNERSHIP_PUBLICATIONS");
            return value && std::string_view(value) == "1";
        }();
        if (reuse_publications && count == 1) {
            std::array<MetalGuestWrite, 2> writes;
            std::size_t size = 0;
            if (auto color = make_write(first.color, pending->color))
                writes[size++] = std::move(*color);
            if (auto depth = make_write(first.depth_stencil, pending->depth))
                writes[size++] = std::move(*depth);
            authority.Publish(std::span{writes}.first(size));
        } else {
            std::vector<MetalGuestWrite> writes;
            writes.reserve(submitted.count * 2);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& draw = publications[i];
                if (auto color = make_write(draw.color, pending->color))
                    writes.push_back(std::move(*color));
                if (auto depth = make_write(draw.depth_stencil, pending->depth))
                    writes.push_back(std::move(*depth));
            }
            authority.Publish(writes);
        }
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
    pending.reset();
}

bool PicaMetalNativeDrawLane::ReadGuest(PAddr begin, std::span<u8> scratch,
                                      const MetalGuestAuthority::GuestReader& ram) {
    NativeMetalCost::Scope cost(NativeMetalCost::VertexRead);
    // Pending GPU writes only need publishing before an overlapping read.
    // Match the ownership guard already used by RecordPica.
    if (HasTextureCopyBatch(this) || !NativeMetalCost::AvoidRedundantSetup() || !pending ||
        scratch.size() > std::numeric_limits<PAddr>::max() - begin) {
        PublishPending();
    } else {
        const auto end = begin + scratch.size();
        const auto aliases = [&](const PicaMetalNativeAttachment& attachment) {
            const auto range = attachment.view.authoritative_bytes;
            return begin < range.end && range.begin < end;
        };
        if (aliases(pending->color) || aliases(pending->depth))
            PublishPending();
        else
            submissions.Retire(false);
    }
    try {
        if (authority.ReadIntoScratch(begin, scratch, ram))
            return true;
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
    submissions.RejectWrites();
    return false;
}

void PicaMetalNativeDrawLane::InvalidateGuest(PAddr begin, PAddr end) {
    if (!HasTextureCopyBatch(this) && NativeMetalCost::DeferUnrelatedPublication() &&
        pending && begin <= end &&
        !OverlapsAttachment(begin, end, pending->color) &&
        !OverlapsAttachment(begin, end, pending->depth)) {
        submissions.CheckThread();
        submissions.Retire(false);
        NativeMetalCost::Count(NativeMetalCost::InvalidationNoPublish, 1);
    } else {
        PublishPending();
    }
    try {
        authority.Invalidate(begin, end);
        InvalidateAttachments({begin, end});
        InvalidateDecodedTextures(queue, begin, end);
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
}

bool PicaMetalNativeDrawLane::CanReleaseFcramWatchPage(PAddr page) {
    submissions.CheckThread();
    if (page < Memory::FCRAM_PADDR || page >= Memory::FCRAM_N3DS_PADDR_END ||
        (page & Memory::PAGE_MASK) || HasTextureCopyBatch(this))
        return false;
    const auto free = [&](PAddr first) {
        const auto last = first + Memory::PAGE_SIZE;
        if ((resident_color && OverlapsAttachment(first, last, *resident_color)) ||
            (resident_depth && OverlapsAttachment(first, last, *resident_depth)) ||
            (pending && (OverlapsAttachment(first, last, pending->color) ||
                         OverlapsAttachment(first, last, pending->depth))))
            return false;
        // Also validates any surviving producer's ownership and fault status.
        return authority.NativeSegments(first, last).empty();
    };
    if (!free(page))
        return false;
    const auto next = page + Memory::PAGE_SIZE;
    return next == Memory::FCRAM_N3DS_PADDR_END || free(next);
}

bool PicaMetalNativeDrawLane::ExportFlushBytes(const MetalGuestWrite& piece,
                                                    std::span<u8> output) {
    const auto exact = [&] {
        return piece.writer->ExportGuestBytes(piece.begin, piece.end, output);
    };
    if (!NativeMetalCost::CoalesceFlushReadbacks() || output.size() > 64)
        return exact();
    const auto* native = dynamic_cast<const MetalSurfaceWriter*>(piece.writer.get());
    // Shared buffer producers already export with a memcpy after completion.
    if (!native || native->RawGuestBuffer())
        return exact();

    auto& cache = flush_readback;
    if (cache.writer == piece.writer && cache.begin <= piece.begin && cache.end >= piece.end) {
        std::memcpy(output.data(), cache.bytes.data() + piece.begin - cache.begin, output.size());
        NativeMetalCost::Count(NativeMetalCost::FlushReadbackHit, 1);
        return true;
    }

    const auto owner = authority.OwningExtent(piece);
    const auto block = piece.begin & ~(FlushReadback::BlockBytes - 1u);
    const auto begin = std::max(owner.begin, block);
    const auto end = static_cast<PAddr>(
        std::min<u64>(owner.end, u64(block) + FlushReadback::BlockBytes));
    if (end < piece.end || end - begin <= output.size())
        return exact();

    // Publish no neighboring bytes to guest RAM. Reuse is limited to this
    // producer object and to bytes still validated by the authority on read.
    cache.writer.reset();
    const auto bytes = std::span(cache.bytes).first(end - begin);
    if (!piece.writer->ExportGuestBytes(begin, end, bytes))
        return exact();
    cache.writer = piece.writer;
    cache.begin = begin;
    cache.end = end;
    std::memcpy(output.data(), bytes.data() + piece.begin - begin, output.size());
    NativeMetalCost::Count(NativeMetalCost::FlushReadbackFill, 1);
    NativeMetalCost::Count(NativeMetalCost::FlushReadbackBytes, bytes.size());
    return true;
}

void PicaMetalNativeDrawLane::FlushGuest(
    PAddr begin, PAddr end, const std::function<bool(PAddr, std::span<const u8>)>& write) {
    using namespace NativeMetalCost;
    Scope cost(Flush);
    const bool profile = Enabled();
    const auto start = profile ? Scope::Clock::now() : Scope::Clock::time_point{};
    u64 native_bytes = 0;
    u64 export_bytes = 0;
    u64 exports = 0;
    {
        Scope publish(FlushPublish);
        if (!HasTextureCopyBatch(this) && DeferUnrelatedPublication() &&
            pending && begin <= end &&
            !OverlapsAttachment(begin, end, pending->color) &&
            !OverlapsAttachment(begin, end, pending->depth)) {
            submissions.CheckThread();
            submissions.Retire(false);
            Count(FlushNoPublish, 1);
        } else {
            PublishPending();
        }
    }
    try {
        const auto native = [&] {
            Scope lookup(FlushFindOwners);
            return authority.NativeSegments(begin, end);
        }();
        const bool reuse = ReuseMaterializedGuestFlush();
        const auto copy = [&](const MetalGuestWrite& piece) {
            std::vector<u8> scratch(piece.end - piece.begin);
            {
                Scope export_guest(FlushExportGuest);
                if (!ExportFlushBytes(piece, scratch))
                    submissions.Fail("Could not export native Metal guest memory");
            }
            {
                Scope write_ram(FlushWriteRam);
                if (!write(piece.begin, scratch))
                    submissions.Fail("Could not write native Metal guest memory");
            }
            if (reuse)
                authority.MarkMaterialized(piece.begin, piece.end);
            export_bytes += scratch.size();
            ++exports;
        };
        for (const auto& piece : native) {
            native_bytes += piece.end - piece.begin;
            if (reuse) {
                for (const auto& missing : authority.UnmaterializedSegments(piece))
                    copy(missing);
            } else {
                copy(piece);
            }
        }
        if (profile) {
            Count(FlushNativeBytes, native_bytes);
            Count(FlushExportBytes, export_bytes);
            Count(FlushRamReuseBytes, native_bytes - export_bytes);
            const auto wall_ms = std::chrono::duration<double, std::milli>(
                Scope::Clock::now() - start).count();
            if (wall_ms >= 20)
                LOG_INFO(Render_OpenGL,
                         "Slow Metal guest flush [0x{:08x},0x{:08x}) ms={} native_regions={} "
                         "native_bytes={} exports={} exported_bytes={}",
                         begin, end, wall_ms, native.size(), native_bytes, exports, export_bytes);
        }
    } catch (...) {
        submissions.RejectWrites();
        throw;
    }
}

void PicaMetalNativeDrawLane::Drain() {
    PublishPending();
    submissions.Drain();
}

void ConnectNativePicaDispatch(VideoCore::RasterizerInterface& rasterizer,
                               std::shared_ptr<PicaMetalNativeDrawLane> lane,
                               PrepareNativePicaDraw prepare,
                               MetalGuestAuthority::GuestReader ram) {
    if (!lane || !prepare || !ram)
        throw std::invalid_argument("Native PICA dispatch requires render, binding and RAM owners");
    rasterizer.InstallNativePicaDraw(
        [lane = std::move(lane), prepare = std::move(prepare), ram = std::move(ram)](bool indexed) {
            const auto& state = Pica::g_state;
            const auto bindings = prepare(*lane, state, indexed);
            if (!bindings)
                throw std::runtime_error("Could not prepare native PICA draw bindings");
            if (bindings->draw.vertex_mode == PicaMetalVertexMode::Packed) {
                lane->RecordPica(state, indexed, bindings->selected_inputs, bindings->draw,
                                 bindings->views, bindings->color, bindings->depth, ram);
            } else {
                lane->Record(bindings->draw, bindings->views, bindings->color, bindings->depth);
            }
        });
}

} // namespace Cara
