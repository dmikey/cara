// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Frontend {
class EmuWindow;
}

namespace Memory {
class MemorySystem;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Video Core namespace

namespace VideoCore {
extern Memory::MemorySystem* g_memory;
template <class Archive> void serialize(Archive& ar, const unsigned int);

class RendererBase;
class RasterizerInterface;

// TODO: Wrap these in a user settings struct along with any other graphics settings (often set from
// qt ui)
extern std::atomic<bool> g_hw_shader_enabled;
extern std::function<void(u32 width, u32 height, const std::vector<u32>& pixels)>
    g_screenshot_complete_callback;

#ifdef CARA_EXPERIMENTAL_METAL_DRAW_BRIDGE
struct NativeRendererPair {
    std::unique_ptr<RendererBase> renderer;
    std::unique_ptr<RasterizerInterface> rasterizer;
};

// Set and reset by the emulation thread for the lifetime of its frontend.
using NativeRendererFactory = std::function<NativeRendererPair(
    Frontend::EmuWindow&, Memory::MemorySystem&)>;
void SetNativeRendererFactory(NativeRendererFactory factory);

#endif

enum class ResultStatus {
    Success,
    ErrorWindow,
    ErrorGenericDrivers,
    ErrorBelowGL33,
};

/// Initialize the video core
ResultStatus Init(Frontend::EmuWindow& window, Memory::MemorySystem& memory);

RendererBase* Renderer();
Memory::MemorySystem* Memory();
RasterizerInterface* Rasterizer();
#ifdef CARA_ANDROID_GLES_ATLAS
bool RunGLESDepthCoherenceSelfTest(std::string& detail);
#endif
u16 GetResolutionScaleFactor();
u32 GetCurrentFrame();
void SetBackgroundImage(u32* pixels, u32 width, u32 height);

void FrameUpdate();
void SettingUpdate();

/// Shutdown the video core
void Shutdown();

} // namespace VideoCore
