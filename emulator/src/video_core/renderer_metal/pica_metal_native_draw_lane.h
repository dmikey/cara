// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_metal/pica_metal_depth_import.h"
#include "video_core/renderer_metal/pica_metal_draw_writer.h"
#include "video_core/renderer_metal/pica_metal_submission_queue.h"
#include "video_core/renderer_metal/pica_metal_lcd_native.h"
#include "video_core/renderer_metal/pica_metal_packed_inputs.h"
#include "video_core/renderer_metal/pica_metal_texture_decode.h"

namespace VideoCore {
class RasterizerInterface;
}

namespace Cara {

// The rasterizer supplies exact register-derived draw inputs, attachment views,
// and their guest-format exports. A view generation has one immutable encoding.
struct PicaMetalNativeAttachment {
    PicaMetalSurfaceView view;
    MetalGuestExport exporter;
    std::optional<MetalRgba8Layout> rgba8;
};

class PicaMetalNativeDrawLane {
public:
    using GuestWriteNotice = std::function<void(PAddr begin, PAddr end)>;
    PicaMetalNativeDrawLane(id<MTLCommandQueue> queue, NSUInteger upload_page_bytes,
                          GuestWriteNotice notice = {});
    void FlushGuest(PAddr begin, PAddr end,
                    const std::function<bool(PAddr, std::span<const u8>)>& write);
    void Record(const TexturedMetalDraw& draw, const PicaMetalDrawViews& views,
                const PicaMetalNativeAttachment& color, const PicaMetalNativeAttachment& depth);
    // Caller supplies shader bindings for the same selected PICA input mask.
    void RecordPica(const Pica::State& state, bool is_indexed, u16 selected_inputs,
                    TexturedMetalDraw draw, const PicaMetalDrawViews& views,
                    const PicaMetalNativeAttachment& color,
                    const PicaMetalNativeAttachment& depth,
                    const MetalGuestAuthority::GuestReader& ram);
    // Requires the private Metal transfer build and a destination validated
    // against the guest physical memory map by the PPF caller.
    void Fill(MetalTransfer::FillRegisters registers, const MetalTransfer::Pipelines& pipelines);
    void CopyTexture(MetalTransfer::TextureCopyRegisters registers,
                     const MetalTransfer::Pipelines& pipelines,
                     const MetalGuestAuthority::GuestReader& ram);
    void TransferDisplay(MetalTransfer::DisplayTransferRegisters registers,
                         const MetalTransfer::Pipelines& pipelines,
                         const PicaMetalTextureDecoder& decoder,
                         const MetalGuestAuthority::GuestReader& ram);
    id<MTLTexture> DecodeTexture(const PicaMetalTextureDecoder& decoder,
                                 MetalTransfer::GuestInterval bytes, u32 tile_row_bytes,
                                 u32 width, u32 height, u32 format, bool disable_alpha,
                                 const MetalGuestAuthority::GuestReader& ram);
    MetalLCD::Receipt Present(CAMetalLayer* layer, const Layout::FramebufferLayout& layout,
                              const MetalLCD::Pipelines& pipelines, MetalLCD::Filter filter,
                              MetalLCD::EdgeMode edge, const MetalGuestAuthority::GuestReader& ram,
                              bool capture, unsigned upscale, bool present_drawable = true);
    PicaMetalNativeAttachment AcquireTiledColor(
        MetalTransfer::GuestInterval bytes, u32 width, u32 height, MTLPixelFormat metal_format,
        const PicaMetalTextureDecoder& decoder, id<MTLComputePipelineState> tiled_export,
        id<MTLComputePipelineState> detile_packed_color,
        const MetalGuestAuthority::GuestReader& ram);
    PicaMetalNativeAttachment AcquireTiledDepthStencil(
        MetalTransfer::GuestInterval bytes, u32 width, u32 height,
        const PicaMetalDepthImportPipelines& pipelines, id<MTLComputePipelineState> tiled_export,
        id<MTLComputePipelineState> depth_pack, const MetalGuestAuthority::GuestReader& ram,
        bool has_stencil = true);
    // Ordered immutable texture initialization on this lane's Metal queue.
    // Submission errors retire through the same checks as draw and transfer work.
    void SubmitSampledTextureBuild(id<MTLCommandBuffer> command);
    void PublishPending();
    bool ReadGuest(PAddr begin, std::span<u8> scratch, const MetalGuestAuthority::GuestReader& ram);
    void InvalidateGuest(PAddr begin, PAddr end);
    void Drain();
    ~PicaMetalNativeDrawLane();
    void Abort() noexcept;

private:
    std::array<std::optional<MetalLCD::Screen>, 2> previous_lcd;
    MetalLCD::LosslessBuffers lossless_lcd{};
    struct Attachments {
        PicaMetalNativeAttachment color, depth;
    };
    // Resolve the bytes sampled by each row. Raw Metal leases may be read
    // directly from the ordered queue. Only guest-owned gaps are read as RAM.
    enum class ByteUse { General, DisplayTransfer, TextureCopy };
    std::optional<MetalTransfer::ByteView> ResolveBytes(
        id<MTLCommandBuffer> command, const MetalTransfer::Rows& rows, NSUInteger alignment,
        const MetalGuestAuthority::GuestReader& ram, ByteUse use = ByteUse::General);
    id<MTLBuffer> AllocateBytes(MetalTransfer::GuestInterval bytes);
    void PublishBytes(id<MTLCommandBuffer> command, id<MTLBuffer> buffer,
                      const MetalTransfer::Rows& rows);
    id<MTLTexture> ResolveRgba8(const MetalRgba8Layout& layout, u64* backing_generation = nullptr);
    void Track(id<MTLCommandBuffer> command,
               std::shared_ptr<const PicaMetalValidationReceipt> validation = {});
    void InvalidateAttachments(PicaGuestRange bytes, const PicaMetalSurfaceView* same_view = nullptr);
    const id<MTLCommandQueue> queue;
    const GuestWriteNotice write_notice;
    u64 byte_generation = 0;
    u64 attachment_generation = 0;
    std::optional<PicaMetalNativeAttachment> resident_color;
    std::optional<PicaMetalNativeAttachment> resident_depth;
    struct FlushReadback {
        static constexpr u32 BlockBytes = 4096;
        std::array<u8, BlockBytes> bytes;
        MetalGuestWriterRef writer;
        PAddr begin = 0;
        PAddr end = 0;
    };
    bool ExportFlushBytes(const MetalGuestWrite& piece, std::span<u8> output);
    FlushReadback flush_readback;
    id<MTLComputePipelineState> packed_color_export = nil;
    MetalGuestAuthority authority;
    PicaMetalSubmissionQueue submissions;
    PicaMetalDrawGroup group;
    std::optional<Attachments> pending;
};

// Prepared on the rasterizer lane. Storage retains uniform spans until
// RecordPica copies them to the upload arena.
struct NativePicaDrawBindings {
    TexturedMetalDraw draw;
    PicaMetalDrawViews views;
    PicaMetalNativeAttachment color;
    PicaMetalNativeAttachment depth;
    u16 selected_inputs;
    std::shared_ptr<const void> storage;
};

using PrepareNativePicaDraw = std::function<std::optional<NativePicaDrawBindings>(
    PicaMetalNativeDrawLane&, const Pica::State&, bool indexed)>;

void ConnectNativePicaDispatch(VideoCore::RasterizerInterface& rasterizer,
                               std::shared_ptr<PicaMetalNativeDrawLane> lane,
                               PrepareNativePicaDraw prepare,
                               MetalGuestAuthority::GuestReader ram);

} // namespace Cara
