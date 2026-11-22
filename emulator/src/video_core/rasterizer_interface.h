// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <stdexcept>
#include <utility>
#include "common/common_types.h"
#include "core/hw/gpu.h"

namespace OpenGL {
struct ScreenInfo;
}

namespace Pica::Shader {
struct OutputVertex;
} // namespace Pica::Shader

namespace VideoCore {

class RasterizerInterface {
public:
    virtual ~RasterizerInterface() {}

    /// Queues the primitive formed by the given vertices for rendering
    virtual void AddTriangle(const Pica::Shader::OutputVertex& v0,
                             const Pica::Shader::OutputVertex& v1,
                             const Pica::Shader::OutputVertex& v2) = 0;

    /// Draw the current batch of triangles
    virtual void DrawTriangles() = 0;

    // Immutable per-rasterizer capability, sampled once per logical PICA list.
    // Other backends stay inactive without depending on any OpenGL flags.
    bool PicaWriteHooksActive() const noexcept { return pica_write_hooks_active_; }

    // Hook BEFORE backing registers or special port side effects are modified.
    virtual void BeginPicaCommandList() {}
    virtual void PrePicaRegisterWrite(u32 id, u32 value, u32 mask) {}
    virtual void EndPicaCommandList() {}
    // Explicit visibility/lifecycle fence, before PICA state is reset/restored.
    virtual void FlushDeferredDraws() {}

    /// Notify rasterizer that the specified PICA register has been changed
    virtual void NotifyPicaRegisterChanged(u32 id) = 0;

    /// Notify rasterizer that all caches should be flushed to 3DS memory
    virtual void FlushAll() = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to 3DS memory
    virtual void FlushRegion(PAddr addr, u32 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be invalidated
    virtual void InvalidateRegion(PAddr addr, u32 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to 3DS memory
    /// and invalidated
    virtual void FlushAndInvalidateRegion(PAddr addr, u32 size) = 0;

    /// Attempt to use a faster method to perform a display transfer with is_texture_copy = 0
    virtual bool AccelerateDisplayTransfer(const GPU::Regs::DisplayTransferConfig& config) {
        return false;
    }

    /// Attempt to use a faster method to perform a display transfer with is_texture_copy = 1
    virtual bool AccelerateTextureCopy(const GPU::Regs::DisplayTransferConfig& config) {
        return false;
    }

    /// Attempt to use a faster method to fill a region
    virtual bool AccelerateFill(const GPU::Regs::MemoryFillConfig& config) {
        return false;
    }

    /// Attempt to use a faster method to display the framebuffer to screen
    virtual bool AccelerateDisplay(const GPU::Regs::FramebufferConfig& config,
                                   PAddr framebuffer_addr, u32 pixel_stride,
                                   OpenGL::ScreenInfo& screen_info) {
        return false;
    }

#ifdef CARA_EXPERIMENTAL_METAL_DRAW_BRIDGE
    // Installed once by a native renderer before a guest session begins.
    // A false return explicitly selects the original CPU-produced vertex
    // stream. Binding failures remain errors in the native renderer.
    using NativePicaDraw = std::function<bool(bool indexed)>;
    void InstallNativePicaDraw(NativePicaDraw dispatch, bool accept_original_stage_in = false) {
        if (native_pica_draw_ || !dispatch)
            throw std::logic_error("Invalid native PICA draw installation");
        native_pica_draw_ = std::move(dispatch);
        native_original_stage_in_ = accept_original_stage_in;
    }

    bool DispatchNativePicaDraw(bool indexed, bool hardware_eligible) {
        if (!native_pica_draw_)
            return false;
        if (!hardware_eligible && native_original_stage_in_)
            return false;
        if (!hardware_eligible)
            throw std::runtime_error("PICA draw is unsupported by the native renderer");
        const bool drawn = native_pica_draw_(indexed);
        if (!drawn && !native_original_stage_in_)
            throw std::runtime_error("Native PICA draw declined without a vertex fallback");
        return drawn;
    }

#endif

    /// Attempt to draw using hardware shaders
    virtual bool AccelerateDrawBatch(bool is_indexed) {
        return false;
    }

    /// sync
    virtual void SyncFogLutData() {}
    virtual void SyncLightingLutData() {}
    virtual void SyncProcTexLutData() {}

    /// Handle any config changes, this gets propogated to the backend
    virtual void CheckForConfigChanges() {}
    virtual void OnFrameUpdate() {}

protected:
    void SetPicaWriteHooksActive(bool active) noexcept { pica_write_hooks_active_ = active; }

private:
#ifdef CARA_EXPERIMENTAL_METAL_DRAW_BRIDGE
    NativePicaDraw native_pica_draw_;
    bool native_original_stage_in_ = false;
#endif
    bool pica_write_hooks_active_ = false;
};
} // namespace VideoCore
