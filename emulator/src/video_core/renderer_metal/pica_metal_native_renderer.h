// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_metal/pica_metal_native_bindings.h"
#include "video_core/renderer_base.h"

namespace Cara {

struct NativeLcdTarget {
    Layout::FramebufferLayout layout;
    unsigned upscale = 1;
};

// Null layout means the frontend has no visible drawable for this refresh.
using NativeLcdLayout = std::function<std::optional<NativeLcdTarget>()>;
using NativeLcdPresented = std::function<void(bool submitted)>;
using NativeLcdCaptureRequested = std::function<bool()>;
// Controls host drawable acquisition only. Captures always acquire a drawable.
using NativeLcdPresentPolicy = std::function<bool(bool capturing)>;

VideoCore::NativeRendererPair MakeNativeMetalRenderer(
    Frontend::EmuWindow& window, Memory::MemorySystem& memory, CAMetalLayer* layer,
    const PicaMetalResources& resources, NativeLcdLayout layout, NativeLcdPresented presented,
    NativeLcdCaptureRequested capture, NativeLcdPresentPolicy policy = {});
bool WriteNativeMetalCapture(const std::string& output_directory);

bool BeginNativeMetalWarmup();
bool FinishNativeMetalWarmup();
void AbortNativeMetalRendering();

} // namespace Cara
