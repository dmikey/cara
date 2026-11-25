// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_native_renderer.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "cara/native_mac/native_lcd_materialize.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_set>
#include "common/logging/log.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/hw/gpu.h"
#include "video_core/pica_state.h"
#include "video_core/vertex_loader.h"

namespace Cara {

void RegisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane& lane);
void UnregisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane& lane) noexcept;
void ReportNativeMetalBindings(const std::string& path);
namespace {

PAddr EndAddress(PAddr begin, u32 size) {
    return begin + std::min(size, std::numeric_limits<PAddr>::max() - begin);
}

std::array<float, 4> AsArray(const Common::Vec4<Pica::float24>& value) {
    return {value.x.ToFloat32(), value.y.ToFloat32(), value.z.ToFloat32(), value.w.ToFloat32()};
}

PicaMetalStageVertex PackStageVertex(const Pica::Shader::OutputVertex& v,
                                    const std::array<float, 4>& reference) {
    auto q = AsArray(v.quat);
    const auto dot = (reference[0] * q[0] + reference[2] * q[2]) +
                     (reference[1] * q[1] + reference[3] * q[3]);
    if (dot < 0) {
        for (float& x : q)
            x = -x;
    }
    return {AsArray(v.pos), AsArray(v.color),
            {v.tc0.x.ToFloat32(), v.tc0.y.ToFloat32()},
            {v.tc1.x.ToFloat32(), v.tc1.y.ToFloat32()},
            {v.tc2.x.ToFloat32(), v.tc2.y.ToFloat32()}, v.tc0_w.ToFloat32(), q,
            {v.view.x.ToFloat32(), v.view.y.ToFloat32(), v.view.z.ToFloat32()}};
}

class NativeRasterizer final : public VideoCore::RasterizerInterface {
public:
    NativeRasterizer(Memory::MemorySystem& memory_, const PicaMetalResources& resources_)
        : memory(memory_), resources(resources_),
          lane(resources.queue, 2 * 1024 * 1024, [this](auto first, auto last) {
              MarkPages(first, last);
          }, [this](PAddr address, std::size_t size) -> std::span<const u8> {
              const auto ref = memory.GetPhysicalRef(address);
              if (!ref || !ref.GetPtr() || size > ref.GetSize())
                  return {};
              return {ref.GetPtr(), size};
          }, resources.resolution_library, resources.internal_scale),
          bindings(resources),
          read([this](PAddr address, std::span<u8> bytes) {
              const auto ref = memory.GetPhysicalRef(address);
              if (!ref || bytes.size() > ref.GetSize())
                  return false;
              std::memcpy(bytes.data(), ref.GetPtr(), bytes.size());
              return true;
          }) {
        InstallNativePicaDraw([this](bool indexed) {
            NativeMetalCost::NativeDrawScope draw_scope;
            if (ApplyExistingIpadShadowPolicy())
                return true;
            bool interpret_vertices = false;
            auto draw = bindings.Prepare(lane, Pica::g_state, indexed, read, {}, &interpret_vertices);
            if (interpret_vertices) {
                FlushVertexInputs(indexed);
                NativeMetalCost::Count(NativeMetalCost::VertexFallbackDraws, 1);
                return false;
            }
            if (!draw)
                throw std::runtime_error("Gate Metal catalog cannot bind this live PICA draw");
            lane.Record(draw->draw, draw->views, draw->color, draw->depth);
            return true;
        }, true);
        RegisterNativeMetalTextureCopyBatch(lane);
    }

    ~NativeRasterizer() override {
        Stop();
        if (shadow_requests)
            LOG_INFO(Render_OpenGL, "Matched iPad GLES unsupported-shadow requests: {}",
                     shadow_requests);
        ReleasePages();
    }

    bool BeginWarmup() { return bindings.BeginWarmup(); }
    bool FinishWarmup() { return bindings.FinishWarmup(); }
    void ValidateCapture() {
        if (failed)
            throw std::runtime_error("Native Metal capture depends on an aborted renderer");
        lane.Drain();
    }

    void AddTriangle(const Pica::Shader::OutputVertex& a, const Pica::Shader::OutputVertex& b,
                     const Pica::Shader::OutputVertex& c) override {
        // This route receives the original PICA output assembled by the core.
        // A failed Metal binding never triggers CPU vertex evaluation.
        const auto reference = AsArray(a.quat);
        auto first = PackStageVertex(a, reference);
        first.quaternion = reference;
        vertices.push_back(first);
        vertices.push_back(PackStageVertex(b, reference));
        vertices.push_back(PackStageVertex(c, reference));
    }

    void DrawTriangles() override {
        if (vertices.empty())
            return;
        if (ApplyExistingIpadShadowPolicy()) {
            vertices.clear();
            return;
        }
        auto draw = bindings.Prepare(lane, Pica::g_state, false, read, vertices);
        if (!draw)
            throw std::runtime_error("Gate Metal catalog cannot bind this PICA output stream");
        lane.Record(draw->draw, draw->views, draw->color, draw->depth);
        vertices.clear();
    }

    void NotifyPicaRegisterChanged(u32) override {
        // LUT data writes also have dedicated notifications.
        if (!NativeMetalCost::AvoidRedundantSetup())
            bindings.InvalidateLuts();
    }
    void SyncFogLutData() override { bindings.InvalidateLuts(); }
    void SyncLightingLutData() override { bindings.InvalidateLuts(); }
    void SyncProcTexLutData() override { bindings.InvalidateProceduralLuts(); }

    void FlushDeferredDraws() override {
        if (!failed)
            lane.Drain();
    }

    void FlushVertexInputs(bool indexed) {
        const auto& pipe = Pica::g_state.regs.pipeline;
        const u32 count = pipe.num_vertices;
        if (!count)
            return;
        const u64 base = pipe.vertex_attributes.GetPhysicalBaseAddress();
        u64 first = pipe.vertex_offset;
        u64 last = first + count - 1;
        if (indexed) {
            const u32 width = pipe.index_array.format ? 2 : 1;
            const u64 start = base + pipe.index_array.offset;
            const u64 size = u64(count) * width;
            if (start + size > std::numeric_limits<PAddr>::max())
                return FlushAll();
            FlushRegion(start, size);
            const auto ref = memory.GetPhysicalRef(start);
            if (!ref || size > ref.GetSize())
                return FlushAll();
            const auto* indices = ref.GetPtr();
            first = std::numeric_limits<u16>::max();
            last = 0;
            for (u32 i = 0; i < count; ++i) {
                const auto* bytes = indices + i * width;
                const u64 index = width == 2 ? bytes[0] | (u32(bytes[1]) << 8) : bytes[0];
                first = std::min(first, index);
                last = std::max(last, index);
            }
        }
        const Pica::VertexLoader loader(pipe);
        for (int i = 0; i < loader.GetNumTotalAttributes(); ++i) {
            const auto attr = loader.GetAttributeLayout(i);
            if (!attr.elements)
                continue;
            const u64 start = base + attr.offset + first * attr.stride;
            const u64 end = base + attr.offset + last * attr.stride +
                            attr.elements * pipe.vertex_attributes.GetElementSizeInBytes(i);
            if (end > std::numeric_limits<PAddr>::max())
                return FlushAll();
            FlushRegion(start, end - start);
        }
    }

    void FlushAll() override {
        FlushRegion(0, std::numeric_limits<PAddr>::max());
    }

    void FlushRegion(PAddr addr, u32 size) override {
        if (failed || !size)
            return;
        lane.FlushGuest(addr, EndAddress(addr, size), [&](PAddr address, std::span<const u8> bytes) {
            auto ref = memory.GetPhysicalRef(address);
            if (!ref || bytes.size() > ref.GetSize())
                return false;
            std::memcpy(ref.GetPtr(), bytes.data(), bytes.size());
            return true;
        });
    }

    void InvalidateRegion(PAddr addr, u32 size) override {
        if (failed || !size)
            return;
        lane.InvalidateGuest(addr, EndAddress(addr, size));
        if (!addr && size == std::numeric_limits<PAddr>::max()) {
            bindings.InvalidateLuts();
            ReleasePages();
        } else {
            ReleaseInvalidatedFcramPages(addr, EndAddress(addr, size));
        }
    }

    void FlushAndInvalidateRegion(PAddr addr, u32 size) override {
        FlushRegion(addr, size);
        InvalidateRegion(addr, size);
    }

    bool AccelerateFill(const GPU::Regs::MemoryFillConfig& cfg) override {
        lane.Fill({cfg.GetStartAddress(), cfg.GetEndAddress(), cfg.value_32bit,
                   cfg.fill_24bit.Value() != 0, cfg.fill_32bit.Value() != 0}, resources.transfer);
        return true;
    }

    bool AccelerateTextureCopy(const GPU::Regs::DisplayTransferConfig& cfg) override {
        const auto& t = cfg.texture_copy;
        lane.CopyTexture({cfg.GetPhysicalInputAddress(), cfg.GetPhysicalOutputAddress(), t.size,
                         static_cast<u16>(t.input_width.Value()), static_cast<u16>(t.input_gap.Value()),
                         static_cast<u16>(t.output_width.Value()), static_cast<u16>(t.output_gap.Value())},
                         resources.transfer, read);
        return true;
    }

    bool AccelerateDisplayTransfer(const GPU::Regs::DisplayTransferConfig& cfg) override {
        lane.TransferDisplay({cfg.GetPhysicalInputAddress(), cfg.GetPhysicalOutputAddress(),
                              static_cast<u16>(cfg.input_width.Value()),
                              static_cast<u16>(cfg.input_height.Value()),
                              static_cast<u16>(cfg.output_width.Value()),
                              static_cast<u16>(cfg.output_height.Value()), cfg.flags, false},
                             resources.transfer, *resources.draw.textures, read);
        return true;
    }

    MetalLCD::Receipt Present(CAMetalLayer* layer, const NativeLcdTarget& target,
                              bool capture, bool present_drawable) {
        return lane.Present(layer, target.layout, resources.lcd, MetalLCD::Filter::Nearest,
                            MetalLCD::EdgeMode::ClampToEdge, read, capture, target.upscale,
                            present_drawable);
    }

    void OnFrameUpdate() override { lane.EndRefresh(); }

    void Stop() noexcept {
        failed = true;
        lane.Abort();
    }

    void AppendWatchProfile(std::ostream& stream, bool json) const {
        const auto fcram = std::count_if(cached_pages.begin(), cached_pages.end(), [](PAddr page) {
            return page >= Memory::FCRAM_PADDR && page < Memory::FCRAM_N3DS_PADDR_END;
        });
        const auto field = [&](const char* name, u64 value) {
            if (json)
                stream << ",\"" << name << "\":" << value;
            else
                stream << name << ',' << value << ",0\n";
        };
        field("metal_watch_pages_checked", watch_pages_checked);
        field("metal_watch_pages_released", watch_pages_released);
        field("metal_watch_ownerless_queries", watch_ownerless_queries);
        field("metal_watch_ownerless_pages", watch_ownerless_pages);
        field("metal_watch_pages_current", cached_pages.size());
        field("metal_watch_fcram_current", static_cast<u64>(fcram));
    }

private:
    void ReleaseInvalidatedFcramPages(PAddr begin, PAddr end) {
        static const bool enabled = [] {
            const auto* value = std::getenv("CARA_METAL_RELEASE_INVALIDATED_FCRAM_PAGES");
            return value && std::string_view(value) == "1";
        }();
        static const bool partial = [] {
            const auto* value = std::getenv("CARA_METAL_RELEASE_OWNERLESS_FCRAM_PAGES");
            return value && std::string_view(value) == "1";
        }();
        const bool profile = NativeMetalCost::Enabled();
        if (!enabled && !partial && !profile)
            return;
        constexpr PAddr page_size = Memory::PAGE_SIZE;
        if (begin >= end || begin >= Memory::FCRAM_N3DS_PADDR_END || end <= Memory::FCRAM_PADDR)
            return;
        const u64 clipped_begin = std::max<u64>(begin, Memory::FCRAM_PADDR);
        const u64 clipped_end = std::min<u64>(end, Memory::FCRAM_N3DS_PADDR_END);
        const u64 whole_first = (clipped_begin + page_size - 1) & ~u64(Memory::PAGE_MASK);
        const u64 whole_last = clipped_end & ~u64(Memory::PAGE_MASK);
        const u64 first = partial || profile ? clipped_begin & ~u64(Memory::PAGE_MASK) : whole_first;
        const u64 last = partial || profile ? (clipped_end + page_size - 1) & ~u64(Memory::PAGE_MASK) : whole_last;
        if (first >= last)
            return;
        // Complete invalidated pages no longer have authority or residents;
        // partial pages additionally need the lane's full dependency proof.
        // Keep the predecessor of a still-watched page outside a whole run.
        const bool guard = whole_last < Memory::FCRAM_N3DS_PADDR_END &&
                           cached_pages.contains(static_cast<PAddr>(whole_last));
        bool released = false;
        for (u64 page = first; page < last; page += page_size) {
            const auto found = cached_pages.find(static_cast<PAddr>(page));
            if (found == cached_pages.end())
                continue;
            ++watch_pages_checked;
            bool ownerless = false;
            if (partial || profile) {
                ++watch_ownerless_queries;
                ownerless = lane.CanReleaseFcramWatchPage(static_cast<PAddr>(page));
                watch_ownerless_pages += ownerless;
            }
            const bool whole = page >= whole_first && page < whole_last &&
                               !(guard && page + page_size == whole_last);
            if (!(partial && ownerless) && !(enabled && whole))
                continue;
            memory.RasterizerMarkRegionCached(static_cast<PAddr>(page), page_size, false);
            cached_pages.erase(found);
            ++watch_pages_released;
            released = true;
        }
        // A later GPU write must restore every released page's barrier.
        if (released) {
            marked_ranges = {};
            next_marked_range = 0;
        }
    }

    bool ApplyExistingIpadShadowPolicy() {
        if (!Pica::g_state.regs.framebuffer.IsShadowRendering())
            return false;
        // The iPad GLES control declines the whole draw without GL writes
        // because it lacks the desktop ARB image/attachment capabilities.
        // Do not claim Metal shadow support or silently drop enabled shadows.
        if (Settings::values.allow_shadow || Settings::values.shadow_rendering)
            throw std::runtime_error("Enabled PICA shadow rendering requires a native implementation");
        ++shadow_requests;
        return true;
    }

    void MarkPages(PAddr begin, PAddr end) {
        constexpr PAddr PageSize = Memory::PAGE_SIZE;
        const auto first_page = begin & ~Memory::PAGE_MASK;
        const auto cached = std::find_if(marked_ranges.begin(), marked_ranges.end(),
            [&](const auto& range) { return first_page >= range.first && end <= range.second; });
        if (cached != marked_ranges.end()) {
            NativeMetalCost::Count(NativeMetalCost::PageHits, 1);
            return;
        }
        if (!((begin >= Memory::VRAM_PADDR && end <= Memory::VRAM_PADDR_END) ||
              (begin >= Memory::FCRAM_PADDR && end <= Memory::FCRAM_N3DS_PADDR_END)) ||
            !memory.IsValidPhysicalAddress(begin) || !memory.IsValidPhysicalAddress(end - 1))
            throw std::runtime_error("Invalid physical address for native Metal writes");

        PAddr page = first_page;
        for (; page < end; page += PageSize) {
            if (cached_pages.insert(page).second)
                memory.RasterizerMarkRegionCached(page, PageSize, true);
        }
        // Page flags only clear as a group in ReleasePages. A covered interval
        // needs no address validation, page lookup or second page-table write.
        marked_ranges[next_marked_range] = {first_page, page};
        next_marked_range = (next_marked_range + 1) % marked_ranges.size();
    }

    void ReleasePages() {
        constexpr PAddr PageSize = 1 << Memory::PAGE_BITS;
        for (PAddr page : cached_pages)
            memory.RasterizerMarkRegionCached(page, PageSize, false);
        cached_pages.clear();
        marked_ranges = {};
        next_marked_range = 0;
    }

    Memory::MemorySystem& memory;
    PicaMetalResources resources;
    PicaMetalNativeDrawLane lane;
    PicaMetalNativeBindings bindings;
    MetalGuestAuthority::GuestReader read;
    std::unordered_set<PAddr> cached_pages;
    std::array<std::pair<PAddr, PAddr>, 16> marked_ranges{};
    std::size_t next_marked_range = 0;
    u64 watch_pages_checked = 0, watch_pages_released = 0;
    u64 watch_ownerless_queries = 0, watch_ownerless_pages = 0;
    std::vector<PicaMetalStageVertex> vertices;
    u64 shadow_requests = 0;
    bool failed = false;
};

class NativeRenderer final : public VideoCore::RendererBase {
public:
    NativeRenderer(Frontend::EmuWindow& window, NativeRasterizer& rasterizer_, CAMetalLayer* layer_,
                   NativeLcdLayout layout_, NativeLcdPresented presented_,
                   NativeLcdCaptureRequested capture_, NativeLcdPresentPolicy policy_)
        : RendererBase(window), rasterizer(rasterizer_), layer(layer_), layout(std::move(layout_)),
          presented(std::move(presented_)), capture(std::move(capture_)),
          policy(std::move(policy_)) {}

    VideoCore::ResultStatus Init() override { return VideoCore::ResultStatus::Success; }
    void SwapBuffers() override {
        const auto target = layout();
        bool submitted = false;
        if (target) {
            const bool capturing = capture && capture();
            const bool present_drawable = !policy || policy(capturing) || capturing;
            const auto result = rasterizer.Present(layer, *target, capturing, present_drawable);
            submitted = result.status == MetalLCD::Status::Submitted;
            if (result.capture.buffer) {
                retained = result;
                retained_layout = target->layout;
                Cara::NativeLCDMaterialize::AtCapture(VideoCore::GetCurrentFrame()+1);
            }
        }
        presented(submitted);
        VideoCore::FrameUpdate();
    }
    bool TryPresent() override { return false; }
    void ResetPresent() override { retained = {}; }
    bool WriteCapture(const std::string& path) {
        // A successful LCD command alone does not validate earlier shader-side
        // depth checks. Drain through the checked owner queue before exporting.
        rasterizer.ValidateCapture();
        ReportNativeMetalBindings(path);
        const auto& pixels = retained.capture;
        if (!pixels.buffer || !retained.commands || pixels.format != MTLPixelFormatBGRA8Unorm)
            return false;
        [retained.commands waitUntilCompleted];
        if (retained.commands.status != MTLCommandBufferStatusCompleted || retained.commands.error)
            return false;
        std::ofstream raw(path + "metal-benchmark-frame.bgra", std::ios::binary);
        raw.write(static_cast<const char*>(pixels.buffer.contents),
                  pixels.row_stride * retained_layout.height);
        std::ofstream meta(path + "metal-benchmark-frame.json");
        const auto rect = [&](const Common::Rectangle<unsigned>& r) {
            meta << '[' << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << ']';
        };
        meta << "{\"format\":\"BGRA8\",\"origin\":\"top_left\",\"width\":"
             << retained_layout.width << ",\"height\":" << retained_layout.height
             << ",\"row_stride\":" << pixels.row_stride << ",\"top\":";
        rect(retained_layout.top_screen);
        meta << ",\"bottom\":";
        rect(retained_layout.bottom_screen);
        meta << "}\n";
        return raw && meta;
    }

private:
    NativeRasterizer& rasterizer;
    CAMetalLayer* layer;
    NativeLcdLayout layout;
    NativeLcdPresented presented;
    NativeLcdCaptureRequested capture;
    NativeLcdPresentPolicy policy;
    MetalLCD::Receipt retained{};
    Layout::FramebufferLayout retained_layout{};
};

} // namespace

void AppendNativeMetalWatchProfile(std::ostream& stream, bool json) {
    if (auto* rasterizer = dynamic_cast<NativeRasterizer*>(VideoCore::Rasterizer()))
        rasterizer->AppendWatchProfile(stream, json);
}

VideoCore::NativeRendererPair MakeNativeMetalRenderer(
    Frontend::EmuWindow& window, Memory::MemorySystem& memory, CAMetalLayer* layer,
    const PicaMetalResources& resources, NativeLcdLayout layout, NativeLcdPresented presented,
    NativeLcdCaptureRequested capture, NativeLcdPresentPolicy policy) {
    if (!layer || resources.queue.device != layer.device || !layout || !presented)
        return {};
    auto rasterizer = std::make_unique<NativeRasterizer>(memory, resources);
    auto renderer = std::make_unique<NativeRenderer>(window, *rasterizer, layer, std::move(layout),
                                                    std::move(presented), std::move(capture),
                                                    std::move(policy));
    return {std::move(renderer), std::move(rasterizer)};
}

bool WriteNativeMetalCapture(const std::string& output_directory) {
    if (auto* renderer = dynamic_cast<NativeRenderer*>(VideoCore::Renderer()))
        return renderer->WriteCapture(output_directory);
    return false;
}

bool BeginNativeMetalWarmup() {
    if (auto* rasterizer = dynamic_cast<NativeRasterizer*>(VideoCore::Rasterizer()))
        return rasterizer->BeginWarmup();
    return false;
}

bool FinishNativeMetalWarmup() {
    if (auto* rasterizer = dynamic_cast<NativeRasterizer*>(VideoCore::Rasterizer()))
        return rasterizer->FinishWarmup();
    return false;
}

void AbortNativeMetalRendering() {
    if (auto* rasterizer = dynamic_cast<NativeRasterizer*>(VideoCore::Rasterizer()))
        rasterizer->Stop();
}

} // namespace Cara
